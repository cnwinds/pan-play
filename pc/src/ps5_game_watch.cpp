#if defined(PANPLAY_PS5)

#include "ps5_game_watch.hpp"

#include "panplay/game_launch.hpp"
#include "panplay/trace.hpp"
#include "ps5_appinst.hpp"
#include "ps5_notify.hpp"
#include "ps5_titles.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kSandboxRoot = "/mnt/sandbox";
// 进程刚执行时 AppInfo 里可能还没有 Title ID（同 ShadowMountPlus），隔一会儿再查。
constexpr auto kAppInfoRetry = std::chrono::milliseconds(20);
constexpr auto kAppInfoGiveUp = std::chrono::seconds(3);
constexpr auto kUnmountRetry = std::chrono::milliseconds(500);
constexpr auto kUnmountGiveUp = std::chrono::seconds(30);

struct AppInfo {
  std::uint32_t app_id;
  std::uint64_t unknown1;
  char title_id[14];
  char unknown2[0x3c];
};
using GetAppInfo = int (*)(int, AppInfo*);
GetAppInfo get_app_info = nullptr;

struct PendingExec {
  Clock::time_point seen;
};

struct Overlay {
  std::string title_id;
  std::string lib_dir;
};

struct PendingUnmount {
  std::string title_id;
  std::string lib_dir;
  Clock::time_point give_up;
};

std::map<int, PendingExec> pending_execs;
// 已经处理过的盘游游戏进程，进程号到 Title ID。
std::map<int, std::string> games;
std::map<int, Overlay> overlays;
std::vector<PendingUnmount> pending_unmounts;

std::int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string errno_text() { return std::strerror(errno); }

bool process_alive(int pid) { return kill(pid, 0) == 0 || errno != ESRCH; }

// 进程的 Title ID；AppInfo 还没准备好时为空。
std::string title_of(int pid) {
  AppInfo info{};
  if (get_app_info(pid, &info) != 0) return "";
  return std::string(info.title_id, strnlen(info.title_id, sizeof(info.title_id)));
}

// unionfs 的参数照 ShadowMountPlus，不接受时退回不带参数（同 BackPork）。
int nmount_unionfs(const std::string& from, const std::string& to, bool options) {
  const auto entry = [](const char* text) { return iovec{const_cast<char*>(text), std::strlen(text) + 1}; };
  std::vector<iovec> items = {entry("fstype"), entry("unionfs"), entry("from"), entry(from.c_str()),
                              entry("fspath"), entry(to.c_str())};
  if (options) {
    items.insert(items.end(), {entry("copymode"), entry("transparent"), entry("notime"), iovec{nullptr, 0},
                               entry("fnodup"), iovec{nullptr, 0}});
  }
  return nmount(items.data(), static_cast<unsigned>(items.size()), 0);
}

std::string mount_overlay(const std::string& from, const std::string& to) {
  int failure = 0;
  const auto attempt = [&]() -> std::string {
    if (nmount_unionfs(from, to, true) == 0) return "";
    const int with_options = errno;
    if (nmount_unionfs(from, to, false) == 0) {
      PANPLAY_DEBUG("unionfs 不接受 ShadowMountPlus 的参数（" + std::string(std::strerror(with_options)) + "），已不带参数挂上");
      return "";
    }
    failure = errno;
    return "把 " + from + " 叠到 " + to + " 失败：" + std::strerror(failure);
  };
  std::string error = attempt();
  if (!error.empty() && (failure == EPERM || failure == EACCES)) {
    panplay::log_info(error + "，换特权身份再试");
    error = with_privileged_auth(attempt);
  }
  return error;
}

// fakelib 里的 libkernel.sprx 不叠（同 ShadowMountPlus），改名留着，不删用户的文件。
void skip_libkernel(const fs::path& fakelib) {
  std::error_code ec;
  const fs::path kernel = fakelib / "libkernel.sprx";
  if (!fs::exists(kernel, ec)) return;
  fs::rename(kernel, fakelib / "libkernel.sprx.panplay-skip", ec);
  if (ec) panplay::log_warn("没能把 " + kernel.u8string() + " 改名，照样叠上去：" + ec.message());
  else panplay::log_info("fakelib 里的 libkernel.sprx 不叠，已改名为 libkernel.sprx.panplay-skip：" + fakelib.u8string());
}

void watch_exit(int kq, int pid) {
  struct kevent change;
  EV_SET(&change, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_EXIT | NOTE_EXEC, 0,
         nullptr);
  if (kevent(kq, &change, 1, nullptr, 0, nullptr) != 0 && errno != ESRCH) {
    panplay::log_warn("监视进程 " + std::to_string(pid) + " 的退出失败：" + errno_text());
  }
}

void mount_fakelib(int pid, const std::string& title_id, const std::string& source, Clock::time_point seen) {
  const std::string kind = panplay::pick_fakelib(fs::u8path(source));
  if (kind.empty()) return;
  const std::string by = fakelib_mounter();
  if (by != "panplay") {
    panplay::game_launches().fakelib(pid, kind, by, "", -1);
    panplay::log_info(title_id + " 的 " + kind + " 交给 " + by + " 挂");
    return;
  }
  // 游戏自己换执行文件、新进程比旧进程的退出先到：沙盒还是那个，叠好的接着用。
  for (auto it = overlays.begin(); it != overlays.end(); ++it) {
    if (it->second.title_id != title_id) continue;
    const Overlay kept = it->second;
    overlays.erase(it);
    overlays[pid] = kept;
    panplay::game_launches().fakelib(pid, kind, by, "", 0);
    panplay::log_info(title_id + " 换了新进程 " + std::to_string(pid) + "，沿用已叠好的 " + kind);
    return;
  }
  const fs::path from = fs::u8path(source) / kind;
  skip_libkernel(from);
  std::string error;
  const std::string lib_dir = panplay::sandbox_lib_dir(kSandboxRoot, title_id, &error);
  if (!lib_dir.empty()) error = mount_overlay(from.u8string(), lib_dir);
  const long long ms = panplay::ms_since(seen);
  panplay::game_launches().fakelib(pid, kind, by, error, ms);
  if (!error.empty()) {
    panplay::log_warn("没能给 " + title_id + " 挂上 " + kind + "：" + error);
    notify("盘游没能挂上 " + title_id + " 的 backport 库：" + error);
    return;
  }
  overlays[pid] = {title_id, lib_dir};
  panplay::log_info("已把 " + from.u8string() + " 叠到 " + lib_dir + "，从进程执行算起用时 " + std::to_string(ms) +
                    " 毫秒");
}

// 返回 false 表示 AppInfo 还没准备好，要再查。
bool handle_exec(int kq, int pid, Clock::time_point seen) {
  if (games.count(pid) != 0) return true;
  const std::string title_id = title_of(pid);
  if (title_id.empty()) return false;
  const std::string source = managed_game_source(title_id);
  if (source.empty()) return true;
  games[pid] = title_id;
  panplay::log_info("主机上启动了 " + title_id + "，进程 " + std::to_string(pid));
  panplay::game_launches().exec(title_id, pid, unix_ms());
  watch_exit(kq, pid);
  mount_fakelib(pid, title_id, source, seen);
  return true;
}

void retry_unmounts() {
  const auto now = Clock::now();
  for (auto it = pending_unmounts.begin(); it != pending_unmounts.end();) {
    struct statfs st {};
    const bool ours = statfs(it->lib_dir.c_str(), &st) == 0 && it->lib_dir == st.f_mntonname &&
                      std::strcmp(st.f_fstypename, "unionfs") == 0;
    if (!ours || unmount(it->lib_dir.c_str(), 0) == 0 || errno == ENOENT || errno == EINVAL) {
      PANPLAY_DEBUG("已卸掉 " + it->title_id + " 的 fakelib：" + it->lib_dir);
      it = pending_unmounts.erase(it);
    } else if (errno == EBUSY && now < it->give_up) {
      ++it;
    } else {
      panplay::log_warn("卸不掉 " + it->title_id + " 的 fakelib（" + it->lib_dir + "）：" + errno_text());
      it = pending_unmounts.erase(it);
    }
  }
}

void handle_exit(int pid) {
  pending_execs.erase(pid);
  if (games.erase(pid) == 0) return;
  panplay::game_launches().exited(pid, unix_ms());
  const auto it = overlays.find(pid);
  if (it == overlays.end()) return;
  panplay::log_info(it->second.title_id + " 的进程 " + std::to_string(pid) + " 退出了，卸掉 fakelib");
  pending_unmounts.push_back({it->second.title_id, it->second.lib_dir, Clock::now() + kUnmountGiveUp});
  overlays.erase(it);
  retry_unmounts();
}

void retry_execs(int kq) {
  const auto now = Clock::now();
  for (auto it = pending_execs.begin(); it != pending_execs.end();) {
    if (handle_exec(kq, it->first, it->second.seen) || now - it->second.seen > kAppInfoGiveUp ||
        !process_alive(it->first)) {
      it = pending_execs.erase(it);
    } else {
      ++it;
    }
  }
}

void watch_loop(int kq, int syscore) {
  for (;;) {
    timespec wait{};
    timespec* timeout = nullptr;
    if (!pending_execs.empty() || !pending_unmounts.empty()) {
      const auto step = !pending_execs.empty() ? kAppInfoRetry : kUnmountRetry;
      wait.tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(step).count());
      timeout = &wait;
    }
    struct kevent events[16];
    const int count = kevent(kq, nullptr, 0, events, 16, timeout);
    if (count < 0 && errno != EINTR) {
      panplay::log_error("监视游戏启动出错，停止监视：" + errno_text());
      return;
    }
    const auto seen = Clock::now();
    for (int i = 0; i < count; ++i) {
      if (events[i].filter != EVFILT_PROC) continue;
      const int pid = static_cast<int>(events[i].ident);
      if ((events[i].fflags & NOTE_TRACKERR) != 0) panplay::log_warn("没能跟踪 SceSysCore 新开的进程");
      if (pid == syscore) continue;
      if ((events[i].fflags & NOTE_EXEC) != 0 && !handle_exec(kq, pid, seen) && process_alive(pid)) {
        pending_execs.emplace(pid, PendingExec{seen});
      }
      if ((events[i].fflags & NOTE_EXIT) != 0) handle_exit(pid);
    }
    if (!pending_execs.empty()) retry_execs(kq);
    if (!pending_unmounts.empty()) retry_unmounts();
  }
}

}  // namespace

void start_game_watch() {
  for (const char* lib : {"libkernel_sys.sprx", "libkernel.sprx"}) {
    void* handle = dlopen(lib, RTLD_LAZY);
    if (handle == nullptr) continue;
    get_app_info = reinterpret_cast<GetAppInfo>(dlsym(handle, "sceKernelGetAppInfo"));
    if (get_app_info != nullptr) break;
  }
  if (get_app_info == nullptr) {
    panplay::log_warn("找不到 sceKernelGetAppInfo，不监视游戏启动，backport 的 fakelib 挂不上");
    return;
  }
  const int syscore = find_process({"SceSysCore.elf"});
  if (syscore <= 0) {
    panplay::log_warn("找不到 SceSysCore.elf，不监视游戏启动，backport 的 fakelib 挂不上");
    return;
  }
  const int kq = kqueue();
  if (kq < 0) {
    panplay::log_warn("kqueue 失败，不监视游戏启动：" + errno_text());
    return;
  }
  struct kevent change;
  EV_SET(&change, static_cast<uintptr_t>(syscore), EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, nullptr);
  if (kevent(kq, &change, 1, nullptr, 0, nullptr) != 0) {
    panplay::log_warn("监视 SceSysCore.elf 失败，不监视游戏启动：" + errno_text());
    close(kq);
    return;
  }
  std::thread([kq, syscore] { watch_loop(kq, syscore); }).detach();
  panplay::log_info("开始监视游戏启动（SceSysCore.elf 进程 " + std::to_string(syscore) + "）");
}

#endif
