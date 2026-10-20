#include "panplay/drive.hpp"

#include "panplay/http.hpp"
#include "panplay/json.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace panplay {
namespace {

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string now_ms_text() {
  return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// 下载地址约 8 小时失效，提前换。
constexpr std::int64_t kDlinkLifetimeSeconds = 6 * 3600;
constexpr int kListPageSize = 1000;

const std::string kPassport = "https://passport.baidu.com";
const std::string kPanHome = "https://pan.baidu.com/disk/home";
const std::string kBrowserUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
const std::string kPanUa = "pan.baidu.com";
constexpr int kQrLifetimeSeconds = 300;
// passport 的长轮询最多挂 30 秒左右才回 errno=1。超时要比它长，不然手机确认的消息可能送到一半被我们断开。
constexpr int kLongPollTimeoutMs = 40000;
// 百度返回 -6 表示登录态失效。
constexpr int kErrnoNotLoggedIn = -6;

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
#if defined(_WIN32)
  return MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  std::error_code ec;
  std::filesystem::rename(temp, target, ec);
  return !ec;
#endif
}

bool link_expired_status(int status) { return status == 403 || status == 404 || status == 410; }

// "bytes 100-199/1000" 里的 100；格式不对时返回 -1。
std::int64_t range_start(const std::string& content_range) {
  const auto space = content_range.find(' ');
  const auto dash = content_range.find('-');
  if (space == std::string::npos || dash == std::string::npos || dash <= space + 1) return -1;
  std::int64_t value = 0;
  for (std::size_t i = space + 1; i < dash; ++i) {
    const char ch = content_range[i];
    if (ch < '0' || ch > '9' || value > (INT64_MAX - 9) / 10) return -1;
    value = value * 10 + (ch - '0');
  }
  return value;
}

std::string lower(std::string value) {
  for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t");
  if (begin == std::string::npos) return "";
  return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}

std::string url_host(const std::string& url) {
  const auto scheme = url.find("://");
  const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  const auto end = url.find_first_of(":/?#", start);
  return lower(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

std::string response_header(const HttpResponse& response, const std::string& name) {
  for (const auto& header : response.headers) {
    if (header.first == name) return header.second;
  }
  return {};
}

// 把 Location 头（可能是相对地址）换成完整地址。
std::string absolute_url(const std::string& base, const std::string& location) {
  if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;
  const auto scheme_end = base.find("://");
  const std::string scheme = scheme_end == std::string::npos ? "https" : base.substr(0, scheme_end);
  if (location.rfind("//", 0) == 0) return scheme + ":" + location;
  const auto path_start = base.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
  const std::string origin = base.substr(0, path_start);
  if (!location.empty() && location.front() == '/') return origin + location;
  std::string dir = path_start == std::string::npos ? "/" : base.substr(path_start, base.find('?', path_start) - path_start);
  return origin + dir.substr(0, dir.rfind('/') + 1) + location;
}

// passport 的接口回 JSONP：callback({...})，也可能直接是 JSON。
Json parse_jsonp(const std::string& text) {
  const auto open = text.find_first_of("({");
  if (open == std::string::npos) return {};
  if (text[open] == '{') return Json::parse(text.substr(open), nullptr);
  const auto close = text.rfind(')');
  if (close == std::string::npos || close <= open) return {};
  return Json::parse(text.substr(open + 1, close - open - 1), nullptr);
}

std::string random_gid() {
  std::random_device device;
  std::mt19937 engine(device());
  std::uniform_int_distribution<int> digit(0, 15);
  static const char* hex = "0123456789ABCDEF";
  std::string gid;
  for (const int group : {8, 4, 4, 4, 12}) {
    if (!gid.empty()) gid += '-';
    for (int i = 0; i < group; ++i) gid += hex[digit(engine)];
  }
  return gid;
}

// 登录过程要跨 passport.baidu.com 和 pan.baidu.com 收集 cookie，两边各有一个 STOKEN，按域名分开存。
class CookieJar {
 public:
  void absorb(const HttpResponse& response, const std::string& url) {
    const std::string host = url_host(url);
    for (const auto& [name, line] : response.headers) {
      if (name != "set-cookie") continue;
      std::stringstream parts(line);
      std::string pair;
      std::getline(parts, pair, ';');
      const auto eq = pair.find('=');
      if (eq == std::string::npos) continue;
      Cookie cookie{trim(pair.substr(0, eq)), trim(pair.substr(eq + 1)), host};
      std::string attr;
      while (std::getline(parts, attr, ';')) {
        attr = trim(attr);
        if (lower(attr.substr(0, 7)) != "domain=") continue;
        cookie.domain = lower(attr.substr(7));
        if (!cookie.domain.empty() && cookie.domain.front() == '.') cookie.domain.erase(0, 1);
      }
      cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                    [&](const Cookie& c) { return c.name == cookie.name && c.domain == cookie.domain; }),
                     cookies_.end());
      if (!cookie.value.empty() && lower(cookie.value) != "deleted") cookies_.push_back(std::move(cookie));
    }
  }

  std::string header(const std::string& url) const {
    const std::string host = url_host(url);
    std::string out;
    for (const Cookie& cookie : cookies_) {
      if (!matches(host, cookie.domain)) continue;
      if (!out.empty()) out += "; ";
      out += cookie.name + "=" + cookie.value;
    }
    return out;
  }

  // 对 host 生效的同名 cookie 里取域名最具体的那个。
  std::string get(const std::string& name, const std::string& host) const {
    const Cookie* best = nullptr;
    for (const Cookie& cookie : cookies_) {
      if (cookie.name != name || !matches(host, cookie.domain)) continue;
      if (best == nullptr || cookie.domain.size() > best->domain.size()) best = &cookie;
    }
    return best == nullptr ? "" : best->value;
  }

 private:
  struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
  };

  static bool matches(const std::string& host, const std::string& domain) {
    return host == domain ||
           (host.size() > domain.size() && host.compare(host.size() - domain.size() - 1, std::string::npos, "." + domain) == 0);
  }

  std::vector<Cookie> cookies_;
};

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

// 登录走百度网盘网页版的扫码流程，拿到 BDUSS（和 STOKEN）后，列目录、取下载地址都带这组 cookie，
// 不需要开放平台的应用和密钥。
class BaiduDrive final : public DrivePlugin {
 public:
  explicit BaiduDrive(BaiduConfig config) : config_(std::move(config)) { load_session_locked(); }

  std::string id() const override { return "baidu"; }
  std::string display_name() const override { return "百度网盘"; }

  LoginChallenge start_login() override {
    LoginChallenge challenge;
    const std::string gid = random_gid();
    const std::string t = now_ms_text();
    HttpResponse response = client_.send({kPassport + "/v2/api/getqrcode?lp=pc&qrloginfrom=pc&gid=" + gid +
                                              "&apiver=v3&tt=" + t + "&tpl=netdisk&_=" + t,
                                          "GET",
                                          "",
                                          {{"User-Agent", kBrowserUa}, {"Referer", "https://pan.baidu.com/"}}});
    if (!response.error.empty()) {
      challenge.error = response.error;
      return challenge;
    }
    const Json json = parse_jsonp(response.body);
    const std::string sign = json.str("sign");
    std::string image = json.str("imgurl");
    if (sign.empty() || image.empty()) {
      challenge.error = "百度没有返回登录二维码";
      return challenge;
    }
    if (const auto scheme = image.find("://"); scheme != std::string::npos) image = image.substr(scheme + 3);
    challenge.qrcode_url = "https://" + image;
    // 轮询时要用到 sign、gid 和过期时间，一起放进 device_code，服务重启后也能接着等。
    challenge.device_code = sign + ":" + gid + ":" + std::to_string(now_seconds() + kQrLifetimeSeconds);
    challenge.expires_in = kQrLifetimeSeconds;
    challenge.interval_seconds = 1;
    return challenge;
  }

  // 一次长轮询最多挂 30 秒左右，有扫码或确认的动静会立刻返回。
  LoginPoll poll_login(const LoginChallenge& challenge) override {
    LoginPoll poll;
    poll.retry_after_seconds = 1;
    const auto first = challenge.device_code.find(':');
    const auto second = first == std::string::npos ? std::string::npos : challenge.device_code.find(':', first + 1);
    if (second == std::string::npos) {
      poll.error = "登录二维码无效，请重新获取";
      return poll;
    }
    const std::string sign = challenge.device_code.substr(0, first);
    const std::string gid = challenge.device_code.substr(first + 1, second - first - 1);
    std::int64_t deadline = 0;
    try {
      deadline = std::stoll(challenge.device_code.substr(second + 1));
    } catch (const std::exception&) {
    }
    if (now_seconds() > deadline) {
      poll.expired = true;
      poll.error = "二维码已过期";
      return poll;
    }

    const std::string t = now_ms_text();
    HttpRequest request{kPassport + "/channel/unicast?channel_id=" + sign + "&tpl=netdisk&gid=" + gid +
                            "&apiver=v3&tt=" + t + "&_=" + t,
                        "GET",
                        "",
                        {{"User-Agent", kBrowserUa}, {"Referer", "https://pan.baidu.com/"}}};
    request.timeout_ms = kLongPollTimeoutMs;
    const HttpResponse response = client_.send(request);
    if (!response.error.empty()) return poll;
    const Json json = parse_jsonp(response.body);
    const std::string channel = json.str("channel_v");
    if (json.num("errno") != 0 || channel.empty()) return poll;
    const Json state = Json::parse(channel, nullptr);
    const int status = static_cast<int>(state.num("status"));
    if (status == 1) {
      poll.scanned = true;
      return poll;
    }
    if (status == 2) {
      poll.error = "已在手机上取消登录";
      return poll;
    }
    const std::string ticket = state.str("v");
    if (status != 0 || ticket.empty()) return poll;

    Session fresh;
    if (std::string error = exchange_ticket(ticket, &fresh); !error.empty()) {
      poll.error = error;
      return poll;
    }
    std::lock_guard<std::mutex> lock(mu_);
    session_ = fresh;
    dlinks_.clear();
    save_session_locked();
    poll.done = true;
    return poll;
  }

  bool session_valid() override {
    std::lock_guard<std::mutex> lock(mu_);
    return !session_.bduss.empty();
  }

  void logout() override {
    std::lock_guard<std::mutex> lock(mu_);
    session_ = {};
    dlinks_.clear();
    save_session_locked();
  }

  std::vector<DriveEntry> list(const std::string& path, std::string* error) override {
    std::vector<DriveEntry> entries;
    const std::string cookie = pan_cookie();
    if (cookie.empty()) {
      if (error) *error = "百度未登录";
      return entries;
    }
    const std::string dir = path.empty() ? "/" : path;
    for (int start = 0;; start += kListPageSize) {
      const std::string url = "https://pan.baidu.com/rest/2.0/xpan/file?method=list&dir=" + url_encode(dir) +
                              "&start=" + std::to_string(start) + "&limit=" + std::to_string(kListPageSize);
      HttpResponse response = client_.send({url, "GET", "", {{"User-Agent", kPanUa}, {"Cookie", cookie}}});
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
      const int code = static_cast<int>(json.num("errno"));
      if (code != 0) {
        if (error) *error = code == kErrnoNotLoggedIn ? signed_out() : "百度列目录失败 errno=" + std::to_string(code);
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
        entry.content_hash = item.str("md5");
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
      HttpResponse body = client_.send({url, "GET", "", download_headers(resolved, range)});
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

  std::unique_ptr<ByteStream> open_stream(const std::string& file_id, std::int64_t offset, std::string* error,
                                          AbortSignal* abort) override {
    return open_bytes(file_id, offset, -1, error, abort);
  }

  std::unique_ptr<ByteStream> open_range(const std::string& file_id, std::int64_t offset, std::int64_t length,
                                         std::string* error, AbortSignal* abort) override {
    return open_bytes(file_id, offset, length, error, abort);
  }

  // 小文件一次 multipart 传完，带登录 cookie 就行；ondup=overwrite 覆盖同名文件，上级目录没有时自动建。
  std::string write_small_file(const std::string& path, const std::string& bytes) override {
    const std::string cookie = pan_cookie();
    if (cookie.empty()) return "百度未登录";
    const std::string boundary = "----panplay" + now_ms_text();
    const std::string name = path.substr(path.rfind('/') + 1);
    const std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
                             "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary +
                             "--\r\n";
    const HttpResponse response =
        client_.send({"https://d.pcs.baidu.com/rest/2.0/pcs/file?method=upload&app_id=250528&ondup=overwrite&path=" +
                          url_encode(path),
                      "POST",
                      body,
                      {{"User-Agent", kPanUa},
                       {"Cookie", cookie},
                       {"Content-Type", "multipart/form-data; boundary=" + boundary}}});
    if (!response.error.empty()) return response.error;
    const Json json = Json::parse(response.body, nullptr);
    if (response.status == 200 && !json.str("fs_id").empty()) return "";
    const int code = static_cast<int>(json.num("error_code"));
    if (code == 110 || code == 111 || json.num("errno") == kErrnoNotLoggedIn) return signed_out();
    return "上传失败，状态 " + std::to_string(response.status) + (code != 0 ? " error_code=" + std::to_string(code) : "");
  }

 private:
  std::unique_ptr<ByteStream> open_bytes(const std::string& file_id, std::int64_t offset, std::int64_t length,
                                         std::string* error, AbortSignal* abort) {
    std::string range = "bytes=" + std::to_string(offset) + "-";
    if (length > 0) range += std::to_string(offset + length - 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string url;
      bool resolved = false;
      if (auto link_error = download_url(file_id, attempt > 0, &url, &resolved); !link_error.empty()) {
        *error = link_error;
        return nullptr;
      }
      HttpRequest request{url, "GET", "", download_headers(resolved, range)};
      request.abort = abort;
      auto http = client_.open(request, error);
      if (!http) {
        if (abort != nullptr && abort->aborted()) return nullptr;
        if (resolved && attempt == 0) continue;
        return nullptr;
      }
      const int status = http->status();
      const bool misplaced = status == 206 && range_start(http->header("Content-Range")) != offset;
      if ((link_expired_status(status) || (resolved && (status != 206 || misplaced))) && attempt == 0) continue;
      if (status != 206 && !(status == 200 && offset == 0)) {
        *error = "下载没有按偏移返回，状态 " + std::to_string(status);
        return nullptr;
      }
      if (misplaced) {
        *error = "下载返回的数据位置不对";
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
    std::string bduss;
    std::string stoken;
  };

  struct Dlink {
    std::string url;
    // dlink 每次都会重定向到下载节点，记下节点地址，后面的请求直接发过去，省一次往返。
    std::string resolved;
    std::int64_t fetched_at = 0;
  };

  // 跟着跳转一路走，把每一跳的 Set-Cookie 都收进 jar。
  HttpResponse visit(CookieJar& jar, std::string url) {
    HttpResponse response;
    for (int hop = 0; hop <= 8; ++hop) {
      HttpRequest request{url, "GET", "", {{"User-Agent", kBrowserUa}, {"Referer", "https://pan.baidu.com/"}}};
      if (const std::string cookie = jar.header(url); !cookie.empty()) request.headers.emplace_back("Cookie", cookie);
      request.follow_redirects = false;
      response = client_.send(request);
      if (!response.error.empty()) return response;
      jar.absorb(response, url);
      const std::string location = response_header(response, "location");
      if (response.status < 300 || response.status >= 400 || location.empty()) {
        response.final_url = url;
        return response;
      }
      url = absolute_url(url, location);
    }
    response.error = "跳转次数过多";
    return response;
  }

  // 手机确认后拿到的一次性凭据换成网盘域下的 BDUSS 和 STOKEN，再用列目录确认这组 cookie 真能用。
  std::string exchange_ticket(const std::string& ticket, Session* out) {
    CookieJar jar;
    const std::string t = now_ms_text();
    const HttpResponse login =
        visit(jar, kPassport + "/v3/login/main/qrbdusslogin?v=" + t + "&bduss=" + url_encode(ticket) +
                       "&u=" + url_encode(kPanHome) + "&loginVersion=v4&qrcode=1&tpl=netdisk&apiver=v3&tt=" + t +
                       "&time=" + std::to_string(now_seconds()) + "&alg=v3");
    if (!login.error.empty()) return login.error;
    out->bduss = jar.get("BDUSS", "pan.baidu.com");
    if (out->bduss.empty()) return "换取百度登录态失败，请重新扫码";

    visit(jar, kPassport + "/v3/login/api/auth/?return_type=5&tpl=netdisk&u=" + url_encode(kPanHome));
    out->stoken = jar.get("STOKEN", "pan.baidu.com");
    if (out->stoken.empty()) {
      visit(jar, kPanHome);
      out->stoken = jar.get("STOKEN", "pan.baidu.com");
    }

    const HttpResponse check = client_.send({"https://pan.baidu.com/rest/2.0/xpan/file?method=list&dir=%2F&start=0&limit=1",
                                             "GET",
                                             "",
                                             {{"User-Agent", kPanUa}, {"Cookie", cookie_of(*out)}}});
    if (!check.error.empty()) return check.error;
    std::string parse_error;
    const Json listed = Json::parse(check.body, &parse_error);
    if (!parse_error.empty() || listed.get("errno") == nullptr || listed.num("errno") != 0) {
      return "已拿到登录态，但网盘校验没有通过，请重新扫码";
    }
    return "";
  }

  static std::string cookie_of(const Session& session) {
    if (session.bduss.empty()) return "";
    return "BDUSS=" + session.bduss + (session.stoken.empty() ? "" : "; STOKEN=" + session.stoken);
  }

  std::string pan_cookie() {
    std::lock_guard<std::mutex> lock(mu_);
    return cookie_of(session_);
  }

  // 百度说登录态失效了：清掉会话，界面会回到扫码登录。
  std::string signed_out() {
    logout();
    return "百度未登录";
  }

  // 下载地址要带登录 cookie 才给跳转；跳到的下载节点地址本身就带签名，不再带 cookie。
  std::vector<std::pair<std::string, std::string>> download_headers(bool resolved, const std::string& range) {
    std::vector<std::pair<std::string, std::string>> headers{{"User-Agent", kPanUa}, {"Range", range}};
    if (!resolved) headers.emplace_back("Cookie", pan_cookie());
    return headers;
  }

  void remember_resolved(const std::string& file_id, const std::string& requested, const std::string& final_url) {
    if (final_url.empty() || final_url == requested || final_url.rfind("https://", 0) != 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = dlinks_.find(file_id);
    if (it != dlinks_.end() && it->second.url == requested) it->second.resolved = final_url;
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
    const std::string cookie = pan_cookie();
    if (cookie.empty()) return "百度未登录";
    const std::string meta_url = "https://pan.baidu.com/api/filemetas?dlink=1&web=1&clienttype=0&fsids=" +
                                 url_encode("[" + file_id + "]");
    HttpResponse meta = client_.send({meta_url, "GET", "", {{"User-Agent", kPanUa}, {"Cookie", cookie}}});
    if (!meta.error.empty()) return meta.error;
    std::string parse_error;
    Json json = Json::parse(meta.body, &parse_error);
    if (!parse_error.empty()) return parse_error;
    const int code = static_cast<int>(json.num("errno"));
    if (code == kErrnoNotLoggedIn) return signed_out();
    if (code != 0) return "没有拿到下载地址 errno=" + std::to_string(code);
    const Json* info = json.get("info");
    if (!info || info->type != Json::Type::array || info->array.empty()) return "没有拿到下载地址";
    const std::string dlink = info->array.front().str("dlink");
    if (dlink.empty()) return "下载地址为空";
    std::lock_guard<std::mutex> lock(mu_);
    dlinks_[file_id] = Dlink{dlink, "", now_seconds()};
    *url = dlink;
    return "";
  }

  void load_session_locked() {
    std::ifstream in(std::filesystem::u8path(config_.session_path));
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    Json json = Json::parse(buffer.str(), nullptr);
    session_.bduss = json.str("bduss");
    session_.stoken = json.str("stoken");
  }

  void save_session_locked() const {
    write_file_atomic(config_.session_path, "{\"bduss\":\"" + json_escape(session_.bduss) + "\",\"stoken\":\"" +
                                                json_escape(session_.stoken) + "\"}");
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
