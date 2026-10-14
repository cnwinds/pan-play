#include "panplay/drive.hpp"

namespace panplay {

const std::vector<DriveKind>& drive_kinds() {
  static const std::vector<DriveKind> kinds = {
      {"baidu",
       "百度网盘",
       "用百度网盘 App 扫码登录，不用填任何密钥。",
       {}},
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
  (void)values;
  if (id == "baidu") {
    BaiduConfig config;
    config.session_path = session_path;
    return make_baidu_drive(std::move(config));
  }
  return nullptr;
}

}  // namespace panplay
