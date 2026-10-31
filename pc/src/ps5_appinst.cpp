#if defined(PANPLAY_PS5)

#include "ps5_appinst.hpp"

#include "panplay/trace.hpp"
#include "ps5_dashboard.hpp"

#include <dlfcn.h>
#include <ps5/kernel.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

// 注册应用要用有特权的进程身份，值同 ShadowMountPlus。
constexpr std::uint64_t kPrivilegedAuthId = 0x4800000000000006ULL;
// 注册结果：已经有这个应用，按现有目录恢复了。
constexpr int kAppRestored = static_cast<int>(0x80990002u);

struct AppInstUtil {
  int (*install_all)(void*) = nullptr;
  int (*install_title_dir)(const char*, const char*, void*) = nullptr;
  int (*uninstall)(const char*) = nullptr;
};

AppInstUtil util;
std::atomic<bool> util_ready{false};
std::mutex auth_mu;

// sceSystemServiceLaunchApp 的参数，布局同 websrv。
struct LaunchParam {
  std::uint32_t size;
  std::uint32_t user_id;
  std::uint32_t app_opt;
  std::uint64_t crash_report;
  std::uint32_t check_flag;
};

struct SystemService {
  bool resolved = false;
  int (*app_id)() = nullptr;
  int (*mini_app_id)() = nullptr;
  int (*title_id)(int, char*) = nullptr;
  int (*kill)(int, int, int, int) = nullptr;
  int (*lnc_kill)(std::uint32_t) = nullptr;
  int (*launch)(const char*, char**, LaunchParam*) = nullptr;
  int (*foreground_user)(std::int32_t*) = nullptr;
};

SystemService running;

// 要拿着 auth_mu 调用。
void resolve_system_service() {
  if (running.resolved) return;
  running.resolved = true;
  if (void* lib = dlopen("libSceSystemService.sprx", RTLD_LAZY); lib != nullptr) {
    running.app_id = reinterpret_cast<int (*)()>(dlsym(lib, "sceSystemServiceGetAppIdOfRunningBigApp"));
    running.title_id = reinterpret_cast<int (*)(int, char*)>(dlsym(lib, "sceSystemServiceGetAppTitleId"));
    running.kill = reinterpret_cast<int (*)(int, int, int, int)>(dlsym(lib, "sceSystemServiceKillApp"));
    running.launch =
        reinterpret_cast<int (*)(const char*, char**, LaunchParam*)>(dlsym(lib, "sceSystemServiceLaunchApp"));
    // 同一个库里的 LncUtil 版本（ps5-unified-autoloader 用的），上面的找不到时用。
    if (running.app_id == nullptr) {
      running.app_id = reinterpret_cast<int (*)()>(dlsym(lib, "sceLncUtilGetAppIdOfRunningBigApp"));
    }
    if (running.title_id == nullptr) {
      running.title_id = reinterpret_cast<int (*)(int, char*)>(dlsym(lib, "sceLncUtilGetAppTitleId"));
    }
    running.lnc_kill = reinterpret_cast<int (*)(std::uint32_t)>(dlsym(lib, "sceLncUtilKillApp"));
    // 主机浏览器不是全屏应用，是盖在上面的小应用。
    running.mini_app_id = reinterpret_cast<int (*)()>(dlsym(lib, "sceLncUtilGetAppIdOfMiniApp"));
  }
  if (void* lib = dlopen("libSceUserService.sprx", RTLD_LAZY); lib != nullptr) {
    running.foreground_user = reinterpret_cast<int (*)(std::int32_t*)>(dlsym(lib, "sceUserServiceGetForegroundUser"));
  }
  const auto found = [](const void* function) { return function != nullptr ? "有" : "无"; };
  panplay::log_info(std::string("系统服务：查运行中的应用 ") + found(reinterpret_cast<const void*>(running.app_id)) +
                    "、查小应用 " + found(reinterpret_cast<const void*>(running.mini_app_id)) + "、查 Title ID " +
                    found(reinterpret_cast<const void*>(running.title_id)) + "、关应用 " +
                    found(reinterpret_cast<const void*>(running.kill)) + "/" +
                    found(reinterpret_cast<const void*>(running.lnc_kill)) + "、启动 " +
                    found(reinterpret_cast<const void*>(running.launch)) + "、前台用户 " +
                    found(reinterpret_cast<const void*>(running.foreground_user)));
  if (running.app_id == nullptr || running.title_id == nullptr) {
    panplay::log_warn("这个固件上查不了正在运行的游戏，删除前不检查游戏开没开着");
  }
}

// 要拿着 auth_mu 调用。查不到时为空。
std::string app_title_locked(int app) {
  if (running.title_id == nullptr) return "";
  char title[32] = {};
  if (running.title_id(app, title) != 0) return "";
  return std::string(title, strnlen(title, sizeof(title)));
}

// 要拿着 auth_mu 调用。返回系统的结果码，接口都没有时返回 -1。
int kill_app_locked(int app) {
  if (running.kill != nullptr) return running.kill(app, -1, 0, 0);
  if (running.lnc_kill != nullptr) return running.lnc_kill(static_cast<std::uint32_t>(app));
  return -1;
}

// 要拿着 auth_mu 调用。没有游戏在运行时 app 为 0。
std::string running_title_locked(int* app) {
  *app = 0;
  if (running.app_id == nullptr || running.title_id == nullptr) return "";
  const int id = running.app_id();
  if (id <= 0) return "";
  *app = id;
  char title[32] = {};
  if (running.title_id(id, title) != 0) return "";
  return std::string(title, strnlen(title, sizeof(title)));
}

unsigned bcd(unsigned value) { return ((value >> 4) & 0xFu) * 10u + (value & 0xFu); }

void* open_lib(const char* name, std::string* error) {
  panplay::log_info(std::string("安装库：加载 ") + name);
  void* lib = dlopen(name, RTLD_LAZY);
  if (lib == nullptr) {
    const char* reason = dlerror();
    *error = std::string("加载 ") + name + " 失败：" + (reason != nullptr ? reason : "未知原因");
  }
  return lib;
}

// 库不在链接时依赖：加载不了时载荷在进入 main 之前就退出，什么记录都留不下。
// 顺序照 websrv 和 ShadowMountPlus：先加载并初始化用户服务，再加载安装库并初始化，都用进程原来的身份。
std::string load() {
  std::string error;
  if (open_lib("libSceSystemService.sprx", &error) == nullptr) return error;
  void* user_service = open_lib("libSceUserService.sprx", &error);
  if (user_service == nullptr) return error;
  const auto user_init = reinterpret_cast<int (*)(void*)>(dlsym(user_service, "sceUserServiceInitialize"));
  if (user_init == nullptr) return "找不到 sceUserServiceInitialize";
  panplay::log_info("安装库：sceUserServiceInitialize 返回 " + hex_code(user_init(nullptr)));

  void* lib = open_lib("libSceAppInstUtil.sprx", &error);
  if (lib == nullptr) return error;
  const auto initialize = reinterpret_cast<int (*)()>(dlsym(lib, "sceAppInstUtilInitialize"));
  if (initialize == nullptr) return "找不到 sceAppInstUtilInitialize";
  if (const int code = initialize(); code != 0) return "sceAppInstUtilInitialize 返回 " + hex_code(code);
  panplay::log_info("安装库：sceAppInstUtilInitialize 成功，固件 " + firmware_text());

  util.install_all = reinterpret_cast<int (*)(void*)>(dlsym(lib, "sceAppInstUtilAppInstallAll"));
  util.uninstall = reinterpret_cast<int (*)(const char*)>(dlsym(lib, "sceAppInstUtilAppUnInstall"));
  // 按目录注册单个应用的函数没有导出名字，只能按 NID 找。
  std::uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    util.install_title_dir =
        reinterpret_cast<int (*)(const char*, const char*, void*)>(kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
  }
  if (util.install_all == nullptr && util.install_title_dir == nullptr) return "找不到注册应用的函数";
  return "";
}

}  // namespace

unsigned firmware_major() { return bcd((kernel_get_fw_version() >> 24) & 0xFFu); }

std::string firmware_text() {
  const std::uint32_t fw = kernel_get_fw_version();
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%02u", bcd((fw >> 24) & 0xFFu), bcd((fw >> 16) & 0xFFu));
  return text;
}

std::string hex_code(int code) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(code));
  return text;
}

// 也拿 auth_mu：身份是整个进程共用的，别的线程正顶着特权身份时在这里 dlopen 一样会卡死。
// 失败不缓存，下次再试。
std::string load_app_inst_util() {
  if (util_ready) return "";
  std::lock_guard<std::mutex> lock(auth_mu);
  if (util_ready) return "";
  const std::string error = load();
  util_ready = error.empty();
  return error;
}

std::string running_app_title_id() {
  std::lock_guard<std::mutex> lock(auth_mu);
  resolve_system_service();
  int app = 0;
  return running_title_locked(&app);
}

const char* const kOtherAppRunning = "主机上正在运行别的游戏";

namespace {

// 等 current() 不再是 app，最多等 limit。等到了返回 true。
bool wait_closed(const std::function<int()>& current, int app, std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int still = 0;
    {
      std::lock_guard<std::mutex> lock(auth_mu);
      still = current();
    }
    if (still != app) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
  }
}

}  // namespace

std::string launch_app(const std::string& title_id, bool close_running, bool from_console, std::string* running_title) {
  // 用户服务要先初始化，前台用户才取得到。
  if (auto error = load_app_inst_util(); !error.empty()) return error;
  int app = 0;
  int mini = 0;
  {
    std::lock_guard<std::mutex> lock(auth_mu);
    resolve_system_service();
    if (running.launch == nullptr || running.foreground_user == nullptr) return "这个固件上找不到启动游戏的接口";
    const std::string current = running_title_locked(&app);
    panplay::log_info("启动 " + title_id + " 前正在运行的全屏应用：" +
                      (app <= 0 ? std::string("没有") : (current.empty() ? "查不到 Title ID" : current) + "（" + hex_code(app) + "）"));
    if (app > 0) {
      if (current == title_id) return "这个游戏已经在主机上运行了";
      // 盘游页面不用问，关掉它游戏才会到前台（同 websrv）。
      if (!close_running && current != kDashboardTitleId) {
        *running_title = current;
        return kOtherAppRunning;
      }
      const int code = kill_app_locked(app);
      panplay::log_info("关掉正在运行的 " + (current.empty() ? hex_code(app) : current) + " 返回 " + hex_code(code));
      if (code != 0) return "关掉正在运行的应用失败，系统返回 " + hex_code(code);
    }
    // 主机浏览器是盖在全屏应用上面的小应用：不关掉它，游戏起来了也在浏览器下面。
    // 盘游页面关掉；在主机浏览器里点的启动，开着的小应用就是这个浏览器，也关掉。
    if (running.mini_app_id != nullptr && (mini = running.mini_app_id()) > 0) {
      const std::string mini_title = app_title_locked(mini);
      const bool ours = mini_title == kDashboardTitleId || from_console;
      panplay::log_info("启动 " + title_id + " 前开着的小应用：" + (mini_title.empty() ? "查不到 Title ID" : mini_title) +
                        "（" + hex_code(mini) + "）" + (ours ? "，先关掉" : "，不是盘游页面，不动"));
      if (ours) {
        const int code = kill_app_locked(mini);
        panplay::log_info("关掉小应用 " + hex_code(mini) + " 返回 " + hex_code(code));
        if (code != 0) mini = 0;
      } else {
        mini = 0;
      }
    } else {
      mini = 0;
    }
  }
  if (app > 0 && !wait_closed([] { int id = 0; running_title_locked(&id); return id; }, app, std::chrono::seconds(10))) {
    return "正在运行的游戏 10 秒内没关掉，稍后再试";
  }
  if (mini > 0 && !wait_closed([] { return running.mini_app_id(); }, mini, std::chrono::seconds(5))) {
    panplay::log_warn("小应用 " + hex_code(mini) + " 5 秒内没关掉，照样启动");
  }
  std::lock_guard<std::mutex> lock(auth_mu);
  std::int32_t user = -1;
  if (running.foreground_user(&user) != 0 || user <= 0) return "主机上还没有登录的用户，先在主机上选好用户";
  LaunchParam param{};
  param.user_id = static_cast<std::uint32_t>(user);
  char* argv[] = {nullptr};
  const int code = running.launch(title_id.c_str(), argv, &param);
  panplay::log_info("启动 " + title_id + " 返回 " + hex_code(code));
  if (code < 0) return "启动失败，系统返回 " + hex_code(code);
  return "";
}

std::string with_privileged_auth(const std::function<std::string()>& action) {
  std::lock_guard<std::mutex> lock(auth_mu);
  const std::uint64_t own_auth_id = kernel_get_ucred_authid(-1);
  if (kernel_set_ucred_authid(-1, kPrivilegedAuthId) != 0) return "切换进程身份失败";
  std::string error;
  try {
    error = action();
  } catch (const std::exception& ex) {
    error = ex.what();
  } catch (...) {
    error = "执行时出错";
  }
  if (kernel_set_ucred_authid(-1, own_auth_id) != 0) panplay::log_error("换回进程原来的身份失败");
  return error;
}

bool hold_privileged_ops(std::chrono::milliseconds wait) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  while (!auth_mu.try_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

// 固件 12 起按目录注册单个应用在载荷里用不了（ShadowMountPlus 同样改用整体重新扫描）。
std::string register_app(const std::string& title_id) {
  if (!util_ready) return "安装库还没加载";
  int code = 0;
  if (util.install_title_dir != nullptr && firmware_major() < 12) {
    PANPLAY_DEBUG("注册应用 " + title_id + "：sceAppInstUtilAppInstallTitleDir");
    code = util.install_title_dir(title_id.c_str(), "/user/app/", nullptr);
  } else if (util.install_all != nullptr) {
    PANPLAY_DEBUG("注册应用 " + title_id + "：sceAppInstUtilAppInstallAll");
    code = util.install_all(nullptr);
  } else {
    return "这个固件上找不到 sceAppInstUtilAppInstallAll";
  }
  panplay::log_info("注册应用 " + title_id + " 返回 " + hex_code(code));
  if (code != 0 && code != kAppRestored) return "注册返回 " + hex_code(code);
  return "";
}

std::string uninstall_app(const std::string& title_id) {
  if (!util_ready) return "安装库还没加载";
  if (util.uninstall == nullptr) return "找不到 sceAppInstUtilAppUnInstall";
  const int code = util.uninstall(title_id.c_str());
  panplay::log_info("注销应用 " + title_id + " 返回 " + hex_code(code));
  return code == 0 ? "" : "注销返回 " + hex_code(code);
}

#endif
