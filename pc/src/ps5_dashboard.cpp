#if defined(PANPLAY_PS5)

#include "ps5_dashboard.hpp"

#include "panplay/trace.hpp"
#include "ps5_appinst.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "panplay_icon0.inc"

namespace {

constexpr const char* kTitleId = "PANP00001";

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

}  // namespace

DashboardIcon install_dashboard_icon(int port, std::string* error) {
  const std::string app_dir = std::string("/user/app/") + kTitleId;
  const std::string param_path = app_dir + "/sce_sys/param.json";
  const std::string icon_path = app_dir + "/sce_sys/icon0.png";
  // 注册成功后盘游自己留的标记。固件 13 上注册成功也不生成 /user/appmeta/<ID>，只看它会每次启动都整体重新扫描一遍。
  // 在主机上删掉图标时整个应用目录连同标记一起删掉，下次启动会重装。
  const std::string marker_path = app_dir + "/panplay.registered";
  const std::string param = param_json(port);
  const std::string icon(reinterpret_cast<const char*>(kIcon0Png), sizeof(kIcon0Png));

  std::error_code ec;
  const bool registered = std::filesystem::exists(marker_path, ec) ||
                          std::filesystem::exists(std::string("/user/appmeta/") + kTitleId + "/param.json", ec);
  if (registered && file_equals(param_path, param) && file_equals(icon_path, icon)) return DashboardIcon::unchanged;
  panplay::log_info(std::string("桌面图标：") + (registered ? "内容有变，重装" : "还没注册，开始安装"));

  if (auto load_error = load_app_inst_util(); !load_error.empty()) {
    *error = load_error;
    return DashboardIcon::failed;
  }
  const std::string failure = with_privileged_auth([&]() -> std::string {
    std::error_code dir_error;
    std::filesystem::create_directories(app_dir + "/sce_sys", dir_error);
    if (dir_error) return "建不了 " + app_dir + "/sce_sys：" + dir_error.message();
    if (!write_file(param_path, param) || !write_file(icon_path, icon)) return "写不进 " + app_dir + "/sce_sys";
    panplay::log_info("桌面图标：已写入 " + app_dir + "/sce_sys");
    std::filesystem::remove(marker_path, dir_error);
    if (std::string registered_error = register_app(kTitleId); !registered_error.empty()) return registered_error;
    if (!write_file(marker_path, "1")) panplay::log_warn("桌面图标：注册标记写不进 " + marker_path + "，下次启动会再注册一次");
    return std::string();
  });
  if (failure.empty()) return DashboardIcon::installed;
  *error = failure;
  return DashboardIcon::failed;
}

#endif
