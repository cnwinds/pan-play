#include "panplay/drive.hpp"

namespace panplay {

const std::vector<DriveKind>& drive_kinds() {
  static const std::vector<DriveKind> kinds = {
      {"baidu",
       "百度网盘",
       "用百度网盘开放平台的应用登录，扫码授权。",
       {{"app_key", "AppKey", false, "在百度网盘开放平台创建应用后得到。"},
        {"secret_key", "SecretKey", true, "只保存在这台设备上。"}}},
  };
  return kinds;
}

const DriveKind* find_drive_kind(const std::string& id) {
  for (const DriveKind& kind : drive_kinds()) {
    if (kind.id == id) return &kind;
  }
  return nullptr;
}

std::unique_ptr<DrivePlugin> make_drive(const std::string& id, const std::map<std::string, std::string>& values,
                                        const std::string& session_path) {
  const auto value = [&](const char* key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
  };
  if (id == "baidu") {
    BaiduConfig config;
    config.app_key = value("app_key");
    config.secret_key = value("secret_key");
    config.session_path = session_path;
    return make_baidu_drive(std::move(config));
  }
  return nullptr;
}

}  // namespace panplay
