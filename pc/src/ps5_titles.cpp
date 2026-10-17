#if defined(PANPLAY_PS5)

#include "ps5_titles.hpp"

#include "panplay/json.hpp"
#include "panplay/trace.hpp"
#include "ps5_appinst.hpp"

#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppBase = "/user/app";
constexpr const char* kAppMetaBase = "/user/appmeta";
constexpr const char* kTrophyMetaBase = "/system_data/priv/appmeta";
constexpr const char* kSystemExBase = "/system_ex/app";

// PS5 上 kinfo_proc 里进程号和线程名的位置（同 ShadowMountPlus）。
constexpr std::size_t kKinfoPidOffset = 72;
constexpr std::size_t kKinfoThreadNameOffset = 447;

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

std::string errno_text() { return std::strerror(errno); }

bool valid_title_id(const std::string& id) {
  if (id.size() != 9) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    if (!std::isupper(static_cast<unsigned char>(id[i]))) return false;
  }
  for (std::size_t i = 4; i < 9; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(id[i]))) return false;
  }
  return true;
}

// 返回 Title ID，不能注册时把原因写进 *problem。
std::string read_title_id(const std::string& game_dir, std::string* problem) {
  const fs::path sce_sys = fs::u8path(game_dir) / "sce_sys";
  std::error_code ec;
  if (!fs::exists(sce_sys / "param.json", ec)) {
    *problem = fs::exists(sce_sys / "param.sfo", ec) ? "PS4 游戏暂时不能注册到主机桌面" : "游戏目录里没有 sce_sys/param.json";
    return "";
  }
  std::string error;
  const panplay::Json json = panplay::Json::parse(read_text(sce_sys / "param.json"), &error);
  const std::string id = json.str("titleId");
  if (!error.empty() || !valid_title_id(id)) {
    *problem = "sce_sys/param.json 里没有有效的 titleId";
    return "";
  }
  if (!fs::exists(fs::u8path(game_dir) / "eboot.bin", ec)) {
    *problem = "游戏目录里没有 eboot.bin";
    return "";
  }
  return id;
}

std::string link_path(const std::string& id) { return std::string(kAppBase) + "/" + id + "/mount.lnk"; }
std::string system_ex_path(const std::string& id) { return std::string(kSystemExBase) + "/" + id; }

std::string read_link(const std::string& id) {
  std::string text = read_text(link_path(id));
  const auto end = text.find_first_of("\r\n");
  if (end != std::string::npos) text.resize(end);
  return text;
}

bool same_dir(const std::string& a, const std::string& b) {
  return fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal();
}

// 游戏目录正以 nullfs 挂在 /system_ex/app/<ID>。
bool mounted_from(const std::string& id, const std::string& game_dir) {
  struct statfs st {};
  const std::string target = system_ex_path(id);
  if (statfs(target.c_str(), &st) != 0) return false;
  return std::strcmp(st.f_fstypename, "nullfs") == 0 && same_dir(st.f_mntonname, target) &&
         same_dir(st.f_mntfromname, game_dir);
}

bool is_mount_point(const std::string& path) {
  struct statfs st {};
  return statfs(path.c_str(), &st) == 0 && same_dir(st.f_mntonname, path);
}

std::string unmount_target(const std::string& target) {
  if (!is_mount_point(target)) return "";
  if (unmount(target.c_str(), 0) == 0 || unmount(target.c_str(), MNT_FORCE) == 0) return "";
  return "卸不掉 " + target + " 上的挂载：" + errno_text();
}

std::string mount_game(const std::string& id, const std::string& game_dir) {
  if (mounted_from(id, game_dir)) return "";
  const std::string target = system_ex_path(id);
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec) return "建不了 " + target + "：" + ec.message();
  if (auto error = unmount_target(target); !error.empty()) return error;
  const auto entry = [](const std::string& text) {
    return iovec{const_cast<char*>(text.c_str()), text.size() + 1};
  };
  const std::string fstype = "fstype", nullfs = "nullfs", from = "from", fspath = "fspath";
  iovec items[] = {entry(fstype), entry(nullfs), entry(from), entry(game_dir), entry(fspath), entry(target)};
  if (nmount(items, sizeof(items) / sizeof(items[0]), 0) != 0) {
    return "把 " + game_dir + " 挂到 " + target + " 失败：" + errno_text();
  }
  if (!fs::exists(target + "/eboot.bin", ec)) {
    unmount_target(target);
    return "挂载后 " + target + " 里看不到 eboot.bin";
  }
  PANPLAY_DEBUG("已挂载 " + game_dir + " -> " + target);
  return "";
}

std::string copy_one_file(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  if (!in) return "读不了 " + from.u8string();
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  out << in.rdbuf();
  if (!out) return "写不进 " + to.u8string();
  return "";
}

// 不用 fs::copy：主机上的 libc++ 复制文件可能用到这里没有的系统调用。
std::string copy_tree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return "建不了 " + to.u8string() + "：" + ec.message();
  for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path target = to / it->path().filename();
    std::error_code entry_ec;
    if (it->is_directory(entry_ec)) {
      if (it->is_symlink(entry_ec)) continue;
      if (auto error = copy_tree(it->path(), target); !error.empty()) return error;
    } else if (it->is_regular_file(entry_ec)) {
      if (auto error = copy_one_file(it->path(), target); !error.empty()) return error;
    }
  }
  if (ec) return "列不出 " + from.u8string() + "：" + ec.message();
  return "";
}

bool appmeta_file(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  if (lower == "param.json" || lower == "param.sfo") return true;
  const auto dot = lower.rfind('.');
  if (dot == std::string::npos) return false;
  const std::string ext = lower.substr(dot);
  return ext == ".png" || ext == ".dds" || ext == ".at9";
}

// 照 ShadowMountPlus：sce_sys 整个放到 /user/app/<ID>，icon0.png 再放一份在外层；
// 主机列表显示用的几种文件放到 /user/appmeta/<ID>；奖杯资料放到 /system_data/priv/appmeta/<ID>（已有的不覆盖）。
std::string stage_metadata(const std::string& id, const std::string& game_dir) {
  const fs::path sce_sys = fs::u8path(game_dir) / "sce_sys";
  const fs::path app_dir = fs::path(kAppBase) / id;
  if (auto error = copy_tree(sce_sys, app_dir / "sce_sys"); !error.empty()) return error;
  std::error_code ec;
  if (fs::exists(sce_sys / "icon0.png", ec)) {
    if (auto error = copy_one_file(sce_sys / "icon0.png", app_dir / "icon0.png"); !error.empty()) return error;
  }

  const fs::path appmeta = fs::path(kAppMetaBase) / id;
  fs::create_directories(appmeta, ec);
  if (ec) return "建不了 " + appmeta.u8string() + "：" + ec.message();
  for (fs::directory_iterator it(sce_sys, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || !appmeta_file(it->path().filename().u8string())) continue;
    if (auto error = copy_one_file(it->path(), appmeta / it->path().filename()); !error.empty()) return error;
  }

  const fs::path trophy = fs::path(kTrophyMetaBase) / id;
  for (const char* sub : {"trophy2", "uds"}) fs::create_directories(trophy / sub, ec);
  for (const char* file : {"trophy2/npbind.dat", "uds/npbind.dat", "param.json"}) {
    if (!fs::exists(sce_sys / file, ec) || fs::exists(trophy / file, ec)) continue;
    if (auto error = copy_one_file(sce_sys / file, trophy / file); !error.empty()) {
      panplay::log_warn("奖杯资料没复制过去：" + error);
    }
  }
  return "";
}

}  // namespace

bool shadowmount_running() {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  std::size_t size = 0;
  if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) return false;
  std::vector<std::uint8_t> buffer(size + size / 4);
  size = buffer.size();
  if (sysctl(mib, 4, buffer.data(), &size, nullptr, 0) != 0) return false;
  for (std::size_t at = 0; at + kKinfoThreadNameOffset < size;) {
    int struct_size = 0;
    std::memcpy(&struct_size, buffer.data() + at, sizeof(struct_size));
    if (struct_size <= static_cast<int>(kKinfoThreadNameOffset)) break;
    const char* name = reinterpret_cast<const char*>(buffer.data() + at + kKinfoThreadNameOffset);
    const std::string thread(name, strnlen(name, static_cast<std::size_t>(struct_size) - kKinfoThreadNameOffset));
    if (thread == "shadowmountplus.elf" || thread == "shadowmount.elf") return true;
    at += static_cast<std::size_t>(struct_size);
  }
  return false;
}

ConsoleTitle inspect_console_title(const std::string& game_dir) {
  ConsoleTitle title;
  title.title_id = read_title_id(game_dir, &title.problem);
  if (title.title_id.empty()) return title;
  std::error_code ec;
  const bool known = fs::exists(std::string(kAppMetaBase) + "/" + title.title_id + "/param.json", ec);
  title.registered = known && same_dir(read_link(title.title_id), game_dir);
  title.mounted = mounted_from(title.title_id, game_dir);
  if (shadowmount_running()) title.problem = "ShadowMountPlus 在运行，注册交给它；想让盘游来管，先停掉它";
  return title;
}

std::string register_console_title(const std::string& game_dir) {
  std::string problem;
  const std::string id = read_title_id(game_dir, &problem);
  if (id.empty()) return problem;
  if (shadowmount_running()) return "ShadowMountPlus 在运行，先停掉它再让盘游注册";
  panplay::log_info("注册到主机：" + id + "，" + game_dir);
  if (auto error = load_app_inst_util(); !error.empty()) return error;
  const std::string error = with_privileged_auth([&]() -> std::string {
    if (auto staged = stage_metadata(id, game_dir); !staged.empty()) return staged;
    if (auto mounted = mount_game(id, game_dir); !mounted.empty()) return mounted;
    if (!write_text(link_path(id), game_dir)) return "写不进 " + link_path(id);
    return register_app(id);
  });
  if (error.empty()) panplay::log_info("已注册到主机：" + id);
  else panplay::log_warn("注册到主机失败 " + id + "：" + error);
  return error;
}

std::string unregister_console_title(const std::string& game_dir) {
  std::string problem;
  const std::string id = read_title_id(game_dir, &problem);
  if (id.empty() || !same_dir(read_link(id), game_dir)) return "";
  if (shadowmount_running()) {
    panplay::log_info("ShadowMountPlus 在运行，" + id + " 的注销交给它");
    return "";
  }
  panplay::log_info("从主机注销：" + id);
  if (auto error = load_app_inst_util(); !error.empty()) return error;
  const std::string error = with_privileged_auth([&]() -> std::string {
    if (auto unmounted = unmount_target(system_ex_path(id)); !unmounted.empty()) return unmounted;
    std::error_code ec;
    fs::remove(link_path(id), ec);
    if (auto uninstalled = uninstall_app(id); !uninstalled.empty()) panplay::log_warn("注销 " + id + "：" + uninstalled);
    return "";
  });
  if (!error.empty()) panplay::log_warn("从主机注销失败 " + id + "：" + error);
  return error;
}

void restore_console_mounts() {
  if (shadowmount_running()) {
    panplay::log_info("ShadowMountPlus 在运行，游戏挂载交给它");
    return;
  }
  std::vector<std::pair<std::string, std::string>> pending;
  std::error_code ec;
  for (fs::directory_iterator it(kAppBase, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string id = it->path().filename().u8string();
    if (!valid_title_id(id)) continue;
    const std::string source = read_link(id);
    if (source.empty()) continue;
    std::error_code entry_ec;
    if (!fs::exists(fs::u8path(source) / "eboot.bin", entry_ec)) {
      PANPLAY_DEBUG("恢复挂载：" + id + " 的源目录不在了 " + source);
      continue;
    }
    if (!mounted_from(id, source)) pending.emplace_back(id, source);
  }
  if (pending.empty()) return;
  int restored = 0;
  with_privileged_auth([&]() -> std::string {
    for (const auto& [id, source] : pending) {
      if (auto error = mount_game(id, source); !error.empty()) {
        panplay::log_warn("恢复挂载 " + id + " 失败：" + error);
      } else {
        ++restored;
      }
    }
    return "";
  });
  panplay::log_info("恢复挂载 " + std::to_string(restored) + " / " + std::to_string(pending.size()) + " 个游戏");
}

#endif
