#if defined(PANPLAY_PS5)

#include "ps5_dashboard.hpp"

#include "panplay/trace.hpp"

#include <dlfcn.h>
#include <ps5/kernel.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "panplay_icon0.inc"

namespace {

constexpr const char* kTitleId = "PANP00001";
// 注册应用要用有特权的进程身份，装完换回原来的。值同 ShadowMountPlus。
constexpr std::uint64_t kPrivilegedAuthId = 0x4800000000000006ULL;
// 注册结果：已经有这个应用，按现有目录恢复了。
constexpr int kAppRestored = static_cast<int>(0x80990002u);

std::string param_json(int port) {
  return std::string("{\n") +
         "  \"titleId\": \"" + kTitleId + "\",\n"
         "  \"deeplinkUri\": \"http://127.0.0.1:" + std::to_string(port) + "\",\n"
         "  \"localizedParameters\": {\n"
         "    \"defaultLanguage\": \"en-US\",\n"
         "    \"en-US\": {\n"
         "      \"titleName\": \"盘游\"\n"
         "    }\n"
         "  }\n"
         "}\n";
}

bool file_equals(const std::string& path, const std::string& content) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return got == content;
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

std::string hex_code(int code) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(code));
  return text;
}

unsigned bcd(unsigned value) { return ((value >> 4) & 0xFu) * 10u + (value & 0xFu); }

// 固件版本按 BCD 存在高两个字节里，0x12020000 是 12.02。
unsigned firmware_major() { return bcd((kernel_get_fw_version() >> 24) & 0xFFu); }

std::string firmware_text() {
  const std::uint32_t fw = kernel_get_fw_version();
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%02u", bcd((fw >> 24) & 0xFFu), bcd((fw >> 16) & 0xFFu));
  return text;
}

void* open_lib(const char* name, std::string* error) {
  panplay::trace(std::string("桌面图标：加载 ") + name);
  void* lib = dlopen(name, RTLD_LAZY);
  if (lib == nullptr) {
    const char* reason = dlerror();
    *error = std::string("加载 ") + name + " 失败：" + (reason != nullptr ? reason : "未知原因");
  }
  return lib;
}

template <typename Fn>
Fn find_symbol(void* lib, const char* name, std::string* error) {
  const auto fn = reinterpret_cast<Fn>(dlsym(lib, name));
  if (fn == nullptr) *error = std::string("找不到 ") + name;
  return fn;
}

struct AppInstUtil {
  int (*install_all)(void*) = nullptr;
  int (*install_title_dir)(const char*, const char*, void*) = nullptr;
};

// 照 websrv 和 ShadowMountPlus 的顺序：先加载并初始化用户服务，再加载安装库并初始化，都用进程原来的身份。
// 库不在链接时依赖：加载不了时载荷在进入 main 之前就退出，什么记录都留不下。
bool load_app_inst_util(AppInstUtil* out, std::string* error) {
  if (open_lib("libSceSystemService.sprx", error) == nullptr) return false;
  void* user_service = open_lib("libSceUserService.sprx", error);
  if (user_service == nullptr) return false;
  const auto user_init = find_symbol<int (*)(void*)>(user_service, "sceUserServiceInitialize", error);
  if (user_init == nullptr) return false;
  panplay::trace("桌面图标：sceUserServiceInitialize 返回 " + hex_code(user_init(nullptr)));

  void* lib = open_lib("libSceAppInstUtil.sprx", error);
  if (lib == nullptr) return false;
  const auto initialize = find_symbol<int (*)()>(lib, "sceAppInstUtilInitialize", error);
  if (initialize == nullptr) return false;
  if (const int code = initialize(); code != 0) {
    *error = "sceAppInstUtilInitialize 返回 " + hex_code(code);
    return false;
  }
  panplay::trace("桌面图标：sceAppInstUtilInitialize 成功");

  out->install_all = find_symbol<int (*)(void*)>(lib, "sceAppInstUtilAppInstallAll", error);
  // 按目录注册单个应用的函数没有导出名字，只能按 NID 找。
  std::uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    out->install_title_dir = reinterpret_cast<int (*)(const char*, const char*, void*)>(
        kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
  }
  if (out->install_all == nullptr && out->install_title_dir == nullptr) {
    *error = "找不到注册应用的函数";
    return false;
  }
  error->clear();
  return true;
}

// 固件 12 起按目录注册单个应用在载荷里用不了（ShadowMountPlus 同样改用全部重新扫描）。
std::string register_title(const AppInstUtil& util) {
  int code = 0;
  if (util.install_title_dir != nullptr && firmware_major() < 12) {
    panplay::trace("桌面图标：调用 sceAppInstUtilAppInstallTitleDir");
    code = util.install_title_dir(kTitleId, "/user/app/", nullptr);
  } else if (util.install_all != nullptr) {
    panplay::trace("桌面图标：调用 sceAppInstUtilAppInstallAll");
    code = util.install_all(nullptr);
  } else {
    return "这个固件上找不到 sceAppInstUtilAppInstallAll";
  }
  panplay::trace("桌面图标：注册返回 " + hex_code(code));
  if (code != 0 && code != kAppRestored) return "注册桌面图标返回 " + hex_code(code);
  return "";
}

}  // namespace

DashboardIcon install_dashboard_icon(int port, std::string* error) {
  const std::string app_dir = std::string("/user/app/") + kTitleId;
  const std::string param_path = app_dir + "/sce_sys/param.json";
  const std::string icon_path = app_dir + "/sce_sys/icon0.png";
  const std::string param = param_json(port);
  const std::string icon(reinterpret_cast<const char*>(kIcon0Png), sizeof(kIcon0Png));

  std::error_code ec;
  const bool registered = std::filesystem::exists(std::string("/user/appmeta/") + kTitleId + "/param.json", ec);
  if (registered && file_equals(param_path, param) && file_equals(icon_path, icon)) return DashboardIcon::unchanged;
  panplay::trace(std::string("桌面图标：固件 ") + firmware_text() + "，" + (registered ? "内容有变，重装" : "还没注册，开始安装"));

  AppInstUtil util;
  if (!load_app_inst_util(&util, error)) return DashboardIcon::failed;

  const std::uint64_t own_auth_id = kernel_get_ucred_authid(-1);
  std::string failure;
  if (kernel_set_ucred_authid(-1, kPrivilegedAuthId) != 0) {
    failure = "切换进程身份失败";
  } else {
    std::filesystem::create_directories(app_dir + "/sce_sys", ec);
    if (ec) {
      failure = "建不了 " + app_dir + "/sce_sys：" + ec.message();
    } else if (!write_file(param_path, param) || !write_file(icon_path, icon)) {
      failure = "写不进 " + app_dir + "/sce_sys";
    } else {
      panplay::trace("桌面图标：已写入 " + app_dir + "/sce_sys");
      failure = register_title(util);
    }
    kernel_set_ucred_authid(-1, own_auth_id);
  }

  if (failure.empty()) return DashboardIcon::installed;
  *error = failure;
  return DashboardIcon::failed;
}

#endif
