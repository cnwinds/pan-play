#if defined(PANPLAY_PS5)

#include "ps5_appinst.hpp"

#include "panplay/trace.hpp"

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

struct RunningApp {
  bool resolved = false;
  int (*app_id)() = nullptr;
  int (*title_id)(int, char*) = nullptr;
};

RunningApp running;

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
  if (!running.resolved) {
    running.resolved = true;
    if (void* lib = dlopen("libSceSystemService.sprx", RTLD_LAZY); lib != nullptr) {
      running.app_id = reinterpret_cast<int (*)()>(dlsym(lib, "sceSystemServiceGetAppIdOfRunningBigApp"));
      running.title_id = reinterpret_cast<int (*)(int, char*)>(dlsym(lib, "sceSystemServiceGetAppTitleId"));
      // 同一个库里的 LncUtil 版本（ps5-unified-autoloader 用的），上面的找不到时用。
      if (running.app_id == nullptr) {
        running.app_id = reinterpret_cast<int (*)()>(dlsym(lib, "sceLncUtilGetAppIdOfRunningBigApp"));
      }
      if (running.title_id == nullptr) {
        running.title_id = reinterpret_cast<int (*)(int, char*)>(dlsym(lib, "sceLncUtilGetAppTitleId"));
      }
    }
    if (running.app_id == nullptr || running.title_id == nullptr) {
      panplay::log_warn("这个固件上查不了正在运行的游戏，删除前不检查游戏开没开着");
    }
  }
  if (running.app_id == nullptr || running.title_id == nullptr) return "";
  const int app = running.app_id();
  if (app <= 0) return "";
  char id[32] = {};
  if (running.title_id(app, id) != 0) return "";
  return std::string(id, strnlen(id, sizeof(id)));
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
