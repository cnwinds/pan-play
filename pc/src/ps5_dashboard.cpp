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
// 系统界面进程的身份。注册应用的接口只接受它，装完换回原来的身份。
constexpr std::uint64_t kShellCoreAuthId = 0x4801000000000013ULL;

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

std::string dl_error() {
  const char* text = dlerror();
  return text != nullptr ? text : "未知原因";
}

// 加载安装库并注册应用。库在这里才加载：链接时依赖它的话，加载不了时载荷在进入 main 之前就退出，什么记录都留不下。
std::string register_title() {
  void* lib = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
  if (lib == nullptr) return "加载 libSceAppInstUtil.sprx 失败：" + dl_error();
  panplay::trace("桌面图标：已加载 libSceAppInstUtil.sprx");

  using Initialize = int (*)();
  using InstallAll = int (*)(void*);
  using InstallTitleDir = int (*)(const char*, const char*, void*);
  const auto initialize = reinterpret_cast<Initialize>(dlsym(lib, "sceAppInstUtilInitialize"));
  if (initialize == nullptr) return "找不到 sceAppInstUtilInitialize：" + dl_error();
  if (const int code = initialize(); code != 0) return "sceAppInstUtilInitialize 返回 " + hex_code(code);
  panplay::trace("桌面图标：sceAppInstUtilInitialize 成功");

  // 按目录注册单个应用的函数没有导出名字，只能按 NID 找；找不到时退回重新扫描全部应用目录。
  InstallTitleDir install_title_dir = nullptr;
  std::uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    install_title_dir = reinterpret_cast<InstallTitleDir>(kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
  }
  int code = 0;
  if (install_title_dir != nullptr) {
    panplay::trace("桌面图标：调用 sceAppInstUtilAppInstallTitleDir");
    code = install_title_dir(kTitleId, "/user/app/", nullptr);
  } else {
    const auto install_all = reinterpret_cast<InstallAll>(dlsym(lib, "sceAppInstUtilAppInstallAll"));
    if (install_all == nullptr) return "找不到注册应用的函数：" + dl_error();
    panplay::trace("桌面图标：调用 sceAppInstUtilAppInstallAll");
    code = install_all(nullptr);
  }
  if (code != 0) return "注册桌面图标返回 " + hex_code(code);
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
  panplay::trace(std::string("桌面图标：") + (registered ? "内容有变，重装" : "还没注册，开始安装"));

  const std::uint64_t own_auth_id = kernel_get_ucred_authid(-1);
  std::string failure;
  if (kernel_set_ucred_authid(-1, kShellCoreAuthId) != 0) {
    failure = "切换进程身份失败";
  } else {
    std::filesystem::create_directories(app_dir + "/sce_sys", ec);
    if (ec) {
      failure = "建不了 " + app_dir + "/sce_sys：" + ec.message();
    } else if (!write_file(param_path, param) || !write_file(icon_path, icon)) {
      failure = "写不进 " + app_dir + "/sce_sys";
    } else {
      panplay::trace("桌面图标：已写入 " + app_dir + "/sce_sys");
      failure = register_title();
    }
    kernel_set_ucred_authid(-1, own_auth_id);
  }

  if (failure.empty()) return DashboardIcon::installed;
  *error = failure;
  return DashboardIcon::failed;
}

#endif
