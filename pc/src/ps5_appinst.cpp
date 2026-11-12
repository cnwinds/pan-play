#if defined(PANPLAY_PS5)

#include "ps5_appinst.hpp"

#include "panplay/trace.hpp"

#include <dlfcn.h>
#include <ps5/kernel.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace {

// 注册应用要用有特权的进程身份，值同 ShadowMountPlus。
constexpr std::uint64_t kPrivilegedAuthId = 0x4800000000000006ULL;
// 从安装包安装要用 ShellCore 的身份（etaHEN 的 PS5 PKG 安装说明）。
constexpr std::uint64_t kShellCoreAuthId = 0x3800000000000010ULL;
// 注册结果：已经有这个应用，按现有目录恢复了。
constexpr int kAppRestored = static_cast<int>(0x80990002u);
// 安装包安装的结果（SCE_APP_INSTALLER_ERROR_*）。
constexpr int kAddcontAlreadyInstalled = static_cast<int>(0x80A20022u);

// sceAppInstUtilInstallByPackage 的三个参数，照 etaHEN「PS5 Package Installation」说明。
struct PackageMetaInfo {
  const char* uri;
  const char* ex_uri;
  const char* playgo_scenario_id;
  const char* content_id;
  const char* content_name;
  const char* icon_url;
};
struct PackageInfo {
  char content_id[0x30];
  int content_type;
  int content_platform;
};
struct PlayGoInfo {
  char languages[30][8];
  char playgo_scenario_ids[64][3];
  char content_ids[64][0x30];
  unsigned char unknown[6480];
};
struct InstallErrorInfo {
  std::int32_t error_code;
  std::int32_t version;
  char description[512];
  char type[9];
};
struct InstallStatus {
  char status[16];
  char src_type[8];
  std::uint32_t remain_time;
  std::uint64_t downloaded_size;
  std::uint64_t initial_chunk_size;
  std::uint64_t total_size;
  std::uint32_t promote_progress;
  InstallErrorInfo error_info;
  std::int32_t local_copy_percent;
  bool is_copy_only;
};

struct AppInstUtil {
  int (*install_all)(void*) = nullptr;
  int (*install_title_dir)(const char*, const char*, void*) = nullptr;
  int (*uninstall)(const char*) = nullptr;
  int (*install_by_package)(PackageMetaInfo*, PackageInfo*, PlayGoInfo*) = nullptr;
  int (*install_status)(const char*, InstallStatus*) = nullptr;
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
  util.install_by_package =
      reinterpret_cast<int (*)(PackageMetaInfo*, PackageInfo*, PlayGoInfo*)>(dlsym(lib, "sceAppInstUtilInstallByPackage"));
  util.install_status = reinterpret_cast<int (*)(const char*, InstallStatus*)>(dlsym(lib, "sceAppInstUtilGetInstallStatus"));
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

namespace {

std::string with_auth(std::uint64_t auth_id, const std::function<std::string()>& action) {
  std::lock_guard<std::mutex> lock(auth_mu);
  const std::uint64_t own_auth_id = kernel_get_ucred_authid(-1);
  if (kernel_set_ucred_authid(-1, auth_id) != 0) return "切换进程身份失败";
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

}  // namespace

std::string with_privileged_auth(const std::function<std::string()>& action) {
  return with_auth(kPrivilegedAuthId, action);
}

std::string with_shellcore_auth(const std::function<std::string()>& action) { return with_auth(kShellCoreAuthId, action); }

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

std::string installer_error_text(int code) {
  switch (static_cast<unsigned>(code)) {
    case 0x80A20002u: return "主机存储空间不够";
    case 0x80A20004u: return "主机上没有这款游戏（游戏要先注册到主机）";
    case 0x80A20006u: return "主机不认这个包的签名，破解环境要支持安装自制 PKG";
    case 0x80A20009u: return "包的类型不对";
    case 0x80A2000Bu: return "附加内容损坏";
    case 0x80A2000Cu: return "游戏正在运行，先关掉游戏";
    case 0x80A2000Du: return "主机固件版本不够";
    case 0x80A2000Fu: return "包和游戏的内容 ID 对不上";
    case 0x80A20015u: return "主机正忙，稍后再试";
    case 0x80A20021u: return "这个附加内容正在安装";
    default: return "安装接口返回 " + hex_code(code);
  }
}

std::string install_package(const std::string& file, const std::string& name, std::string* content_id) {
  if (!util_ready) return "安装库还没加载";
  if (util.install_by_package == nullptr) return "这个固件上找不到 sceAppInstUtilInstallByPackage";
  PackageMetaInfo meta{file.c_str(), "", "", "", name.c_str(), ""};
  PackageInfo info{};
  auto playgo = std::make_unique<PlayGoInfo>();
  std::memset(playgo.get(), 0, sizeof(PlayGoInfo));
  const int code = util.install_by_package(&meta, &info, playgo.get());
  *content_id = std::string(info.content_id, strnlen(info.content_id, sizeof(info.content_id)));
  panplay::log_info("安装包 " + file + " 返回 " + hex_code(code) + "，内容 ID " + *content_id);
  if (code == kAddcontAlreadyInstalled) return "";
  return code == 0 ? "" : installer_error_text(code);
}

std::string package_install_status(const std::string& content_id, std::string* status, int* error_code) {
  if (!util_ready) return "安装库还没加载";
  if (util.install_status == nullptr) return "这个固件上找不到 sceAppInstUtilGetInstallStatus";
  // 结构比说明里写的大时别让系统写出界。
  alignas(8) unsigned char buffer[4096] = {};
  auto* installed = reinterpret_cast<InstallStatus*>(buffer);
  const int code = util.install_status(content_id.c_str(), installed);
  if (code != 0) return "查安装进度返回 " + hex_code(code);
  *status = std::string(installed->status, strnlen(installed->status, sizeof(installed->status)));
  *error_code = installed->error_info.error_code;
  return "";
}

#endif
