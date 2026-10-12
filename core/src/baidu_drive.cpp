#include "panplay/drive.hpp"

#include "panplay/http.hpp"
#include "panplay/json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace panplay {
namespace {

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class BaiduDrive final : public DrivePlugin {
 public:
  explicit BaiduDrive(BaiduConfig config) : config_(std::move(config)) { load_session(); }

  std::string id() const override { return "baidu"; }
  std::string display_name() const override { return "百度网盘"; }

  LoginChallenge start_login() override {
    LoginChallenge challenge;
    if (config_.app_key.empty()) {
      challenge.error = "请先打开设置，填写 AppKey 和 SecretKey 并保存。";
      return challenge;
    }
    const std::string url = "https://openapi.baidu.com/oauth/2.0/device/code?response_type=device_code&client_id=" +
                            url_encode(config_.app_key) + "&scope=" + url_encode("basic,netdisk");
    HttpResponse response = http_send({url});
    if (!response.error.empty()) {
      challenge.error = response.error;
      return challenge;
    }
    std::string parse_error;
    Json json = Json::parse(response.body, &parse_error);
    if (!parse_error.empty()) {
      challenge.error = parse_error;
      return challenge;
    }
    if (const Json* err = json.get("error")) {
      if (err->type == Json::Type::string) challenge.error = err->text;
    }
    challenge.device_code = json.str("device_code");
    challenge.user_code = json.str("user_code");
    challenge.verification_url = json.str("verification_url");
    challenge.qrcode_url = json.str("qrcode_url");
    challenge.expires_in = static_cast<int>(json.num("expires_in"));
    challenge.interval_seconds = static_cast<int>(json.num("interval"));
    if (challenge.interval_seconds < 5) challenge.interval_seconds = 5;
    if (challenge.device_code.empty() && challenge.error.empty()) challenge.error = "百度没有返回设备码";
    return challenge;
  }

  LoginPoll poll_login(const LoginChallenge& challenge) override {
    LoginPoll poll;
    if (config_.secret_key.empty()) {
      poll.error = "请先打开设置，填写 AppKey 和 SecretKey 并保存。";
      return poll;
    }
    const std::string url = "https://openapi.baidu.com/oauth/2.0/token?grant_type=device_token&code=" +
                            url_encode(challenge.device_code) + "&client_id=" + url_encode(config_.app_key) +
                            "&client_secret=" + url_encode(config_.secret_key);
    HttpResponse response = http_send({url});
    if (!response.error.empty()) {
      poll.error = response.error;
      return poll;
    }
    std::string parse_error;
    Json json = Json::parse(response.body, &parse_error);
    if (!parse_error.empty()) {
      poll.error = parse_error;
      return poll;
    }
    const std::string error = json.str("error");
    if (error == "authorization_pending") {
      poll.retry_after_seconds = std::max(challenge.interval_seconds, 5);
      return poll;
    }
    if (error == "slow_down") {
      poll.retry_after_seconds = std::max(challenge.interval_seconds + 5, 10);
      return poll;
    }
    if (error == "expired_token" || error == "authorization_expired") {
      poll.expired = true;
      poll.error = "二维码已过期";
      return poll;
    }
    if (!error.empty()) {
      poll.error = error;
      return poll;
    }
    session_.access_token = json.str("access_token");
    session_.refresh_token = json.str("refresh_token");
    session_.expires_at = now_seconds() + static_cast<std::int64_t>(json.num("expires_in")) - 60;
    if (session_.access_token.empty()) {
      poll.error = "百度没有返回访问令牌";
      return poll;
    }
    save_session();
    poll.done = true;
    return poll;
  }

  bool session_valid() override {
    if (session_.access_token.empty()) return false;
    if (session_.expires_at > now_seconds()) return true;
    return refresh();
  }

  void logout() override {
    session_ = {};
    save_session();
  }

  std::vector<DriveEntry> list(const std::string& path, std::string* error) override {
    std::vector<DriveEntry> entries;
    if (!session_valid()) {
      if (error) *error = "百度未登录";
      return entries;
    }
    const std::string dir = path.empty() ? "/" : path;
    const std::string url = "https://pan.baidu.com/rest/2.0/xpan/file?method=list&access_token=" +
                            url_encode(session_.access_token) + "&dir=" + url_encode(dir) + "&limit=1000";
    HttpResponse response = http_send({url, "GET", "", {{"User-Agent", "pan.baidu.com"}}});
    if (!response.error.empty()) {
      if (error) *error = response.error;
      return entries;
    }
    std::string parse_error;
    Json json = Json::parse(response.body, &parse_error);
    if (!parse_error.empty()) {
      if (error) *error = parse_error;
      return entries;
    }
    if (json.num("errno") != 0) {
      if (error) *error = "百度列目录失败 errno=" + std::to_string(static_cast<int>(json.num("errno")));
      return entries;
    }
    const Json* list = json.get("list");
    if (!list || list->type != Json::Type::array) return entries;
    for (const Json& item : list->array) {
      DriveEntry entry;
      entry.file_id = item.str("fs_id");
      entry.name = item.str("server_filename");
      entry.path = item.str("path");
      entry.is_dir = item.num("isdir") != 0;
      const std::string size_text = item.str("size");
      try {
        entry.size = size_text.empty() ? 0 : std::stoll(size_text);
      } catch (const std::exception&) {
        entry.size = 0;
      }
      entries.push_back(std::move(entry));
    }
    return entries;
  }

  ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) override {
    ReadResult result;
    if (!session_valid()) {
      result.error = "百度未登录";
      return result;
    }
    if (offset < 0 || length == 0) {
      result.error = "读取范围无效";
      return result;
    }
    const std::string meta_url =
        "https://pan.baidu.com/rest/2.0/xpan/multimedia?method=filemetas&access_token=" +
        url_encode(session_.access_token) + "&fsids=" + url_encode("[" + file_id + "]") + "&dlink=1";
    HttpResponse meta = http_send({meta_url, "GET", "", {{"User-Agent", "pan.baidu.com"}}});
    if (!meta.error.empty()) {
      result.error = meta.error;
      return result;
    }
    std::string parse_error;
    Json json = Json::parse(meta.body, &parse_error);
    if (!parse_error.empty()) {
      result.error = parse_error;
      return result;
    }
    const Json* list = json.get("list");
    if (!list || list->type != Json::Type::array || list->array.empty()) {
      result.error = "没有拿到下载地址";
      return result;
    }
    std::string dlink = list->array.front().str("dlink");
    if (dlink.empty()) {
      result.error = "下载地址为空";
      return result;
    }
    if (dlink.find("access_token=") == std::string::npos) {
      dlink += (dlink.find('?') == std::string::npos ? "?" : "&");
      dlink += "access_token=" + url_encode(session_.access_token);
    }
    const std::string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + static_cast<std::int64_t>(length) - 1);
    HttpResponse body = http_send({dlink, "GET", "", {{"User-Agent", "pan.baidu.com"}, {"Range", range}}});
    result.status = body.status;
    if (!body.error.empty()) {
      result.error = body.error;
      return result;
    }
    if (body.status != 206) {
      result.error = "下载没有按偏移返回，状态 " + std::to_string(body.status);
      return result;
    }
    result.bytes.assign(body.body.begin(), body.body.end());
    return result;
  }

 private:
  struct Session {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0;
  };

  bool refresh() {
    if (session_.refresh_token.empty() || config_.secret_key.empty()) return false;
    const std::string url = "https://openapi.baidu.com/oauth/2.0/token?grant_type=refresh_token&refresh_token=" +
                            url_encode(session_.refresh_token) + "&client_id=" + url_encode(config_.app_key) +
                            "&client_secret=" + url_encode(config_.secret_key);
    HttpResponse response = http_send({url});
    if (!response.error.empty()) return false;
    Json json = Json::parse(response.body, nullptr);
    const std::string token = json.str("access_token");
    if (token.empty()) return false;
    session_.access_token = token;
    if (!json.str("refresh_token").empty()) session_.refresh_token = json.str("refresh_token");
    session_.expires_at = now_seconds() + static_cast<std::int64_t>(json.num("expires_in")) - 60;
    save_session();
    return true;
  }

  void load_session() {
    std::ifstream in(config_.session_path);
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    Json json = Json::parse(buffer.str(), nullptr);
    session_.access_token = json.str("access_token");
    session_.refresh_token = json.str("refresh_token");
    session_.expires_at = static_cast<std::int64_t>(json.num("expires_at"));
  }

  void save_session() const {
    std::ofstream out(config_.session_path, std::ios::trunc);
    if (!out) return;
    out << "{\"access_token\":\"" << json_escape(session_.access_token) << "\",\"refresh_token\":\""
        << json_escape(session_.refresh_token) << "\",\"expires_at\":" << session_.expires_at << "}";
  }

  BaiduConfig config_;
  Session session_;
};

}  // namespace

std::unique_ptr<DrivePlugin> make_baidu_drive(BaiduConfig config) {
  return std::make_unique<BaiduDrive>(std::move(config));
}

}  // namespace panplay
