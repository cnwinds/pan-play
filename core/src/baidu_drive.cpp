#include "panplay/drive.hpp"

#include "panplay/http.hpp"
#include "panplay/json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace panplay {
namespace {

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// 下载地址约 8 小时失效，提前换。
constexpr std::int64_t kDlinkLifetimeSeconds = 6 * 3600;
constexpr int kListPageSize = 1000;

bool write_file_atomic(const std::string& path, const std::string& text) {
  const std::filesystem::path target = std::filesystem::u8path(path);
  std::filesystem::path temp = target;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  return MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool link_expired_status(int status) { return status == 403 || status == 404 || status == 410; }

class BaiduStream final : public ByteStream {
 public:
  BaiduStream(std::unique_ptr<HttpStream> http, std::int64_t expected) : http_(std::move(http)), left_(expected) {}

  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) override {
    if (left_ == 0) return 0;
    if (left_ > 0) size = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(size), left_));
    const std::size_t got = http_->read(dst, size, error);
    if (got == 0 && error->empty() && left_ > 0) *error = "下载提前结束";
    if (left_ > 0) left_ -= static_cast<std::int64_t>(got);
    return got;
  }

  void abort() override { http_->abort(); }

 private:
  std::unique_ptr<HttpStream> http_;
  std::int64_t left_;
};

class BaiduDrive final : public DrivePlugin {
 public:
  explicit BaiduDrive(BaiduConfig config) : config_(std::move(config)) { load_session_locked(); }

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
    HttpResponse response = client_.send({url});
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
    HttpResponse response = client_.send({url});
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
    Session fresh;
    fresh.access_token = json.str("access_token");
    fresh.refresh_token = json.str("refresh_token");
    fresh.expires_at = now_seconds() + static_cast<std::int64_t>(json.num("expires_in")) - 60;
    if (fresh.access_token.empty()) {
      poll.error = "百度没有返回访问令牌";
      return poll;
    }
    std::lock_guard<std::mutex> lock(mu_);
    session_ = fresh;
    dlinks_.clear();
    save_session_locked();
    poll.done = true;
    return poll;
  }

  bool session_valid() override { return !access_token().empty(); }

  void logout() override {
    std::lock_guard<std::mutex> lock(mu_);
    session_ = {};
    dlinks_.clear();
    save_session_locked();
  }

  std::vector<DriveEntry> list(const std::string& path, std::string* error) override {
    std::vector<DriveEntry> entries;
    const std::string token = access_token();
    if (token.empty()) {
      if (error) *error = "百度未登录";
      return entries;
    }
    const std::string dir = path.empty() ? "/" : path;
    for (int start = 0;; start += kListPageSize) {
      const std::string url = "https://pan.baidu.com/rest/2.0/xpan/file?method=list&access_token=" + url_encode(token) +
                              "&dir=" + url_encode(dir) + "&start=" + std::to_string(start) +
                              "&limit=" + std::to_string(kListPageSize);
      HttpResponse response = client_.send({url, "GET", "", {{"User-Agent", "pan.baidu.com"}}});
      if (!response.error.empty()) {
        if (error) *error = response.error;
        return {};
      }
      std::string parse_error;
      Json json = Json::parse(response.body, &parse_error);
      if (!parse_error.empty()) {
        if (error) *error = parse_error;
        return {};
      }
      if (json.num("errno") != 0) {
        if (error) *error = "百度列目录失败 errno=" + std::to_string(static_cast<int>(json.num("errno")));
        return {};
      }
      const Json* list = json.get("list");
      if (!list || list->type != Json::Type::array) break;
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
      if (static_cast<int>(list->array.size()) < kListPageSize) break;
    }
    return entries;
  }

  ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) override {
    ReadResult result;
    if (offset < 0 || length == 0) {
      result.error = "读取范围无效";
      return result;
    }
    const std::string range =
        "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + static_cast<std::int64_t>(length) - 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string url;
      bool resolved = false;
      if (auto error = download_url(file_id, attempt > 0, &url, &resolved); !error.empty()) {
        result.error = error;
        return result;
      }
      HttpResponse body = client_.send({url, "GET", "", {{"User-Agent", "pan.baidu.com"}, {"Range", range}}});
      result.status = body.status;
      if (!body.error.empty()) {
        if (resolved && attempt == 0) continue;
        result.error = body.error;
        return result;
      }
      if ((link_expired_status(body.status) || (resolved && body.status != 206)) && attempt == 0) continue;
      if (body.status != 206) {
        result.error = "下载没有按偏移返回，状态 " + std::to_string(body.status);
        return result;
      }
      remember_resolved(file_id, url, body.final_url);
      result.bytes.assign(body.body.begin(), body.body.end());
      return result;
    }
    result.error = "下载地址失效";
    return result;
  }

  std::unique_ptr<ByteStream> open_stream(const std::string& file_id, std::int64_t offset, std::string* error) override {
    return open_bytes(file_id, offset, -1, error);
  }

  std::unique_ptr<ByteStream> open_range(const std::string& file_id, std::int64_t offset, std::int64_t length,
                                         std::string* error) override {
    return open_bytes(file_id, offset, length, error);
  }

 private:
  std::unique_ptr<ByteStream> open_bytes(const std::string& file_id, std::int64_t offset, std::int64_t length,
                                         std::string* error) {
    std::string range = "bytes=" + std::to_string(offset) + "-";
    if (length > 0) range += std::to_string(offset + length - 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string url;
      bool resolved = false;
      if (auto link_error = download_url(file_id, attempt > 0, &url, &resolved); !link_error.empty()) {
        *error = link_error;
        return nullptr;
      }
      HttpRequest request{url, "GET", "", {{"User-Agent", "pan.baidu.com"}, {"Range", range}}};
      auto http = client_.open(request, error);
      if (!http) {
        if (resolved && attempt == 0) continue;
        return nullptr;
      }
      const int status = http->status();
      if ((link_expired_status(status) || (resolved && status != 206)) && attempt == 0) continue;
      if (status != 206 && !(status == 200 && offset == 0)) {
        *error = "下载没有按偏移返回，状态 " + std::to_string(status);
        return nullptr;
      }
      remember_resolved(file_id, url, http->final_url());
      const std::int64_t content_length = http->content_length();
      return std::make_unique<BaiduStream>(std::move(http), content_length);
    }
    *error = "下载地址失效";
    return nullptr;
  }

  struct Session {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0;
  };

  struct Dlink {
    std::string url;
    // dlink 每次都会重定向到下载节点，记下节点地址，后面的请求直接发过去，省一次往返。
    std::string resolved;
    std::int64_t fetched_at = 0;
  };

  void remember_resolved(const std::string& file_id, const std::string& requested, const std::string& final_url) {
    if (final_url.empty() || final_url == requested || final_url.rfind("https://", 0) != 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = dlinks_.find(file_id);
    if (it != dlinks_.end() && it->second.url == requested) it->second.resolved = final_url;
  }

  std::string access_token() {
    std::lock_guard<std::mutex> lock(mu_);
    if (session_.access_token.empty()) return {};
    if (session_.expires_at > now_seconds()) return session_.access_token;
    if (!refresh_locked()) return {};
    return session_.access_token;
  }

  std::string download_url(const std::string& file_id, bool force, std::string* url, bool* resolved) {
    *resolved = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = dlinks_.find(file_id);
      if (!force && it != dlinks_.end() && now_seconds() - it->second.fetched_at < kDlinkLifetimeSeconds) {
        *resolved = !it->second.resolved.empty();
        *url = *resolved ? it->second.resolved : it->second.url;
        return "";
      }
    }
    const std::string token = access_token();
    if (token.empty()) return "百度未登录";
    const std::string meta_url = "https://pan.baidu.com/rest/2.0/xpan/multimedia?method=filemetas&access_token=" +
                                 url_encode(token) + "&fsids=" + url_encode("[" + file_id + "]") + "&dlink=1";
    HttpResponse meta = client_.send({meta_url, "GET", "", {{"User-Agent", "pan.baidu.com"}}});
    if (!meta.error.empty()) return meta.error;
    std::string parse_error;
    Json json = Json::parse(meta.body, &parse_error);
    if (!parse_error.empty()) return parse_error;
    const Json* list = json.get("list");
    if (!list || list->type != Json::Type::array || list->array.empty()) return "没有拿到下载地址";
    std::string dlink = list->array.front().str("dlink");
    if (dlink.empty()) return "下载地址为空";
    if (dlink.find("access_token=") == std::string::npos) {
      dlink += (dlink.find('?') == std::string::npos ? "?" : "&");
      dlink += "access_token=" + url_encode(token);
    }
    std::lock_guard<std::mutex> lock(mu_);
    dlinks_[file_id] = Dlink{dlink, "", now_seconds()};
    *url = dlink;
    return "";
  }

  // 百度的 refresh_token 只能用一次。别的进程或旧实例可能已经换过，先看会话文件里是不是有更新的。
  bool refresh_locked() {
    Session saved = session_;
    load_session_locked();
    if (session_.refresh_token != saved.refresh_token && session_.expires_at > now_seconds()) return true;
    if (session_.refresh_token.empty() || config_.secret_key.empty()) return false;
    const std::string url = "https://openapi.baidu.com/oauth/2.0/token?grant_type=refresh_token&refresh_token=" +
                            url_encode(session_.refresh_token) + "&client_id=" + url_encode(config_.app_key) +
                            "&client_secret=" + url_encode(config_.secret_key);
    HttpResponse response = client_.send({url});
    if (!response.error.empty()) return false;
    Json json = Json::parse(response.body, nullptr);
    const std::string token = json.str("access_token");
    if (token.empty()) return false;
    session_.access_token = token;
    if (!json.str("refresh_token").empty()) session_.refresh_token = json.str("refresh_token");
    session_.expires_at = now_seconds() + static_cast<std::int64_t>(json.num("expires_in")) - 60;
    dlinks_.clear();
    save_session_locked();
    return true;
  }

  void load_session_locked() {
    std::ifstream in(std::filesystem::u8path(config_.session_path));
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    Json json = Json::parse(buffer.str(), nullptr);
    if (json.str("access_token").empty()) return;
    session_.access_token = json.str("access_token");
    session_.refresh_token = json.str("refresh_token");
    session_.expires_at = static_cast<std::int64_t>(json.num("expires_at"));
  }

  void save_session_locked() const {
    write_file_atomic(config_.session_path, "{\"access_token\":\"" + json_escape(session_.access_token) +
                                                "\",\"refresh_token\":\"" + json_escape(session_.refresh_token) +
                                                "\",\"expires_at\":" + std::to_string(session_.expires_at) + "}");
  }

  BaiduConfig config_;
  HttpClient client_;
  std::mutex mu_;
  Session session_;
  std::map<std::string, Dlink> dlinks_;
};

}  // namespace

std::unique_ptr<DrivePlugin> make_baidu_drive(BaiduConfig config) {
  return std::make_unique<BaiduDrive>(std::move(config));
}

}  // namespace panplay
