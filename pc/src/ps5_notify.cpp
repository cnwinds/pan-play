#if defined(PANPLAY_PS5)

#include "ps5_notify.hpp"

#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

extern "C" int sceNotificationSend(int user_id, bool is_logged, const char* payload);
extern "C" int sceNotificationSendById(int user_id, bool is_logged, const char* use_case_id, const char* params);

namespace {

constexpr int kSystemUser = 0xFE;

// 每条通知要有自己的编号，编号相同的会被当成同一条，后发的可能不显示。
std::string notification_id() {
  static std::atomic<unsigned> counter{0};
  return std::to_string(static_cast<unsigned>(std::time(nullptr)) * 16u + (counter++ % 16u));
}

std::string created_at() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
  return text;
}

std::string icon_json(const std::string& icon_path) {
  struct stat st {};
  if (!icon_path.empty() && ::stat(icon_path.c_str(), &st) == 0 && st.st_size > 0) {
    return "{\"type\":\"Url\",\"parameters\":{\"url\":\"" + panplay::json_escape(icon_path) + "\"}}";
  }
  return "{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}}";
}

int send_toast(const std::string& text, const std::string& sub_text, const std::string& icon_path) {
  const std::string payload =
      "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
      "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"InQueue\",\"isImmediate\":true,\"priority\":100,"
      "\"viewData\":{\"icon\":" + icon_json(icon_path) + ",\"message\":{\"body\":\"" + panplay::json_escape(text) + "\"}" +
      (sub_text.empty() ? std::string() : ",\"subMessage\":{\"body\":\"" + panplay::json_escape(sub_text) + "\"}") +
      "}},\"createdDateTime\":\"" + created_at() + "\",\"localNotificationId\":\"" + notification_id() + "\"}";
  return sceNotificationSend(kSystemUser, true, payload.c_str());
}

}  // namespace

void notify(const std::string& text) { send_toast(text, "", ""); }

void notify_game_installed(const std::string& title_id, bool registered, const std::string& name,
                           const std::string& icon_path) {
  // NUC240 是系统「游戏已可以玩」的通知，按 npTitleId 找已注册的应用取图标和名字（同 ShadowMountPlus）。
  if (registered && !title_id.empty()) {
    const std::string params = "{\"npTitleId\":\"" + panplay::json_escape(title_id) + "_00\"}";
    const int code = sceNotificationSendById(kSystemUser, true, "NUC240", params.c_str());
    if (code == 0) return;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08x", static_cast<unsigned>(code));
    panplay::log_info(std::string("系统的已安装通知没发出去（") + hex + "），改发盘游自己的");
  }
  const std::string shown = name.empty() ? title_id : name;
  send_toast("《" + shown + "》已安装完成", registered ? "可以在主机桌面启动" : "盘游", icon_path);
}

#endif
