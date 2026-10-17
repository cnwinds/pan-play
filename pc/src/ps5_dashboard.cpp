#if defined(PANPLAY_PS5)

#include "ps5_dashboard.hpp"

#include <ps5/kernel.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "panplay_icon0.inc"

extern "C" {
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void* reserved);
}

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

// 按目录注册单个应用。这个函数没有导出名字，只能按 NID 找；找不到时退回重新扫描全部应用目录。
int register_title() {
  using InstallTitleDir = int (*)(const char*, const char*, void*);
  InstallTitleDir install_title_dir = nullptr;
  std::uint32_t handle = 0;
  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    install_title_dir = reinterpret_cast<InstallTitleDir>(kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
  }
  if (install_title_dir != nullptr) return install_title_dir(kTitleId, "/user/app/", nullptr);
  return sceAppInstUtilAppInstallAll(nullptr);
}

std::string hex_code(int code) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(code));
  return text;
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

  const std::uint64_t own_auth_id = kernel_get_ucred_authid(-1);
  kernel_set_ucred_authid(-1, kShellCoreAuthId);
  std::string failure;
  std::filesystem::create_directories(app_dir + "/sce_sys", ec);
  if (ec) {
    failure = "建不了 " + app_dir + "/sce_sys：" + ec.message();
  } else if (!write_file(param_path, param) || !write_file(icon_path, icon)) {
    failure = "写不进 " + app_dir + "/sce_sys";
  } else if (const int init_code = sceAppInstUtilInitialize(); init_code != 0) {
    failure = "sceAppInstUtilInitialize 返回 " + hex_code(init_code);
  } else if (const int install_code = register_title(); install_code != 0) {
    failure = "注册桌面图标返回 " + hex_code(install_code);
  }
  kernel_set_ucred_authid(-1, own_auth_id);

  if (failure.empty()) return DashboardIcon::installed;
  *error = failure;
  return DashboardIcon::failed;
}

#endif
