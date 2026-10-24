#if !defined(_WIN32)

#include "panplay/http.hpp"
#include "panplay/socket_util.hpp"
#include "panplay/trace.hpp"

#include <fcntl.h>
#include <netdb.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/gcm.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if defined(PANPLAY_EMBEDDED_CA)
// 生成的文件，定义 kCaBundle（PEM，末尾带 0）。
#include "panplay_ca_bundle.inc"
#endif

namespace panplay {
namespace {

constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 20000;
constexpr int kMaxRedirects = 10;
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kIdlePerHost = 8;
constexpr auto kIdleLifetime = std::chrono::seconds(20);

const char* const kAborted = "下载已中止";

std::string lower(std::string value) {
  for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

#if defined(PANPLAY_PS5)
// 单条连接最快约是接收窗口 ÷ 往返时间：PS5 默认的接收缓冲小，到百度往返 25 ms 时一条连接只有 2.5 MB/s 左右，
// 同一网络下 PC 能到 25 MB/s。窗口在建连接时就和服务器商定了，要在 connect 之前设；系统上限更小时设不上，逐级减半。
// Linux 不设：手动设了会关掉系统的自动调大。
void widen_receive_buffer(SOCKET fd) {
  constexpr int kWanted = 8 << 20;
  int size = kWanted;
  while (size >= (256 << 10) && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) size /= 2;
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) return;
  int got = 0;
  socklen_t length = sizeof(got);
  getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &length);
  log_info("下载连接的 TCP 接收缓冲：要 " + std::to_string(kWanted >> 20) + " MB，系统给了 " +
           std::to_string(got >> 10) + " KB");
}
#endif

// 下载全走 HTTPS，解密吃的是主机 CPU。每个服务器第一次连上时记一行用的什么加密，
// 第一次时再量一下本机 AES-GCM 解密多快，看得出解密会不会拖慢下载、有没有用上 CPU 的 AES 指令。
void measure_aes_gcm() {
  std::vector<unsigned char> data(16u << 20);
  unsigned char key[16]{};
  unsigned char iv[12]{};
  unsigned char tag[16];
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
    mbedtls_gcm_free(&gcm);
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  const int code = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_DECRYPT, data.size(), iv, sizeof(iv), nullptr, 0, data.data(),
                                             data.data(), sizeof(tag), tag);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  mbedtls_gcm_free(&gcm);
  if (code != 0 || seconds <= 0) return;
  log_info("本机 AES-128-GCM 解密 " + std::to_string(static_cast<long long>(data.size() / seconds / (1 << 20))) +
           " MB/s（单核；低于 200 MB/s 多半没用上 CPU 的 AES 指令）");
}

void note_tls_suite(const std::string& host, const mbedtls_ssl_context& ssl) {
  static std::mutex mu;
  static std::set<std::string> seen;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!seen.insert(host).second) return;
    if (seen.size() > 32) return;
  }
  log_info("HTTPS " + host + "：" + mbedtls_ssl_get_version(&ssl) + "，" + mbedtls_ssl_get_ciphersuite(&ssl));
  static std::once_flag measured;
  std::call_once(measured, [] { std::thread(measure_aes_gcm).detach(); });
}

std::string tls_error_text(int code) {
  char text[160]{};
  mbedtls_strerror(code, text, sizeof(text));
  return text;
}

struct Url {
  bool tls = false;
  std::string host;
  std::uint16_t port = 0;
  std::string target = "/";

  std::string origin() const { return std::string(tls ? "https://" : "http://") + host + ":" + std::to_string(port); }
};

bool parse_url(const std::string& text, Url* url) {
  std::string rest;
  if (text.rfind("https://", 0) == 0) {
    url->tls = true;
    rest = text.substr(8);
  } else if (text.rfind("http://", 0) == 0) {
    url->tls = false;
    rest = text.substr(7);
  } else {
    return false;
  }
  const std::size_t path = rest.find_first_of("/?#");
  std::string authority = rest.substr(0, path);
  url->target = path == std::string::npos ? "/" : rest.substr(path);
  if (!url->target.empty() && url->target.front() != '/') url->target.insert(url->target.begin(), '/');
  const std::size_t hash = url->target.find('#');
  if (hash != std::string::npos) url->target.resize(hash);
  const std::size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  url->port = url->tls ? 443 : 80;
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    try {
      url->port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon + 1)));
    } catch (const std::exception&) {
      return false;
    }
    authority.resize(colon);
  }
  url->host = authority;
  return !url->host.empty();
}

// 把 Location 头（可能是相对地址）换成完整地址。
std::string resolve_location(const Url& base, const std::string& location) {
  if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;
  const std::string scheme = base.tls ? "https:" : "http:";
  if (location.rfind("//", 0) == 0) return scheme + location;
  const bool default_port = base.port == (base.tls ? 443 : 80);
  const std::string origin = scheme + "//" + base.host + (default_port ? "" : ":" + std::to_string(base.port));
  if (!location.empty() && location.front() == '/') return origin + location;
  std::string dir = base.target.substr(0, base.target.find('?'));
  dir = dir.substr(0, dir.rfind('/') + 1);
  return origin + dir + location;
}

// 所有连接共用的 TLS 配置。mbedTLS 编译时打开了线程支持，多个连接可以同时握手。
struct TlsShared {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt ca;
  mbedtls_ssl_config conf;
  std::string error;
};

TlsShared& tls_shared() {
  static TlsShared* shared = [] {
    auto* tls = new TlsShared;
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_x509_crt_init(&tls->ca);
    mbedtls_ssl_config_init(&tls->conf);
    PANPLAY_DEBUG("TLS 初始化：PSA");
    if (psa_crypto_init() != PSA_SUCCESS) {
      tls->error = "TLS 初始化失败（PSA）";
      log_warn(tls->error);
      return tls;
    }
    PANPLAY_DEBUG("TLS 初始化：随机数");
    static const unsigned char personal[] = "panplay";
    if (int code = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy, personal, sizeof(personal) - 1);
        code != 0) {
      tls->error = "TLS 随机数初始化失败: " + tls_error_text(code);
      log_warn(tls->error);
      return tls;
    }
    int loaded = -1;
#if defined(PANPLAY_EMBEDDED_CA)
    PANPLAY_DEBUG("TLS 初始化：解析根证书");
    loaded = mbedtls_x509_crt_parse(&tls->ca, kCaBundle, sizeof(kCaBundle));
    PANPLAY_DEBUG("TLS 初始化：根证书解析结果 " + std::to_string(loaded));
#else
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
      loaded = mbedtls_x509_crt_parse_file(&tls->ca, path);
      if (loaded >= 0) break;
    }
#endif
    if (loaded < 0 || tls->ca.version == 0) {
      tls->error = "没有可用的根证书";
      return tls;
    }
    if (int code = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                               MBEDTLS_SSL_PRESET_DEFAULT);
        code != 0) {
      tls->error = "TLS 配置失败: " + tls_error_text(code);
      return tls;
    }
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->ca, nullptr);
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);
    PANPLAY_DEBUG("TLS 初始化完成");
    return tls;
  }();
  return *shared;
}

int bio_send(void* context, const unsigned char* data, std::size_t size) {
  const SOCKET fd = *static_cast<SOCKET*>(context);
#if defined(MSG_NOSIGNAL)
  const auto n = ::send(fd, data, size, MSG_NOSIGNAL);
#else
  const auto n = ::send(fd, data, size, 0);
#endif
  if (n >= 0) return static_cast<int>(n);
  return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

int bio_recv(void* context, unsigned char* data, std::size_t size) {
  const SOCKET fd = *static_cast<SOCKET*>(context);
  const auto n = ::recv(fd, data, size, 0);
  if (n > 0) return static_cast<int>(n);
  if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
  return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

struct Connection {
  SOCKET fd = INVALID_SOCKET;
  bool tls = false;
  mbedtls_ssl_context ssl;
  std::string key;
  std::vector<std::uint8_t> buffer;
  std::size_t begin = 0;
  std::size_t end = 0;
  std::chrono::steady_clock::time_point idle_since;
  std::chrono::steady_clock::time_point opened_at = std::chrono::steady_clock::now();
  int uses = 0;

  // 日志用：这条连接发过几个请求、连了多久。看得出服务器是按次数还是按时间关连接。
  std::string story() const {
    const auto alive = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - opened_at);
    return "用了 " + std::to_string(uses) + " 次，连了 " + std::to_string(alive.count()) + " 秒";
  }

  Connection() { mbedtls_ssl_init(&ssl); }
  ~Connection() {
    mbedtls_ssl_free(&ssl);
    if (fd != INVALID_SOCKET) closesocket(fd);
  }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // 返回读到的字节数，0 表示对方关了连接，负数表示出错。
  long raw_read(std::uint8_t* dst, std::size_t size) {
    if (!tls) {
      for (;;) {
        const auto n = ::recv(fd, dst, size, 0);
        if (n < 0 && errno == EINTR) continue;
        return static_cast<long>(n);
      }
    }
    for (;;) {
      const int n = mbedtls_ssl_read(&ssl, dst, size);
      if (n > 0) return n;
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
      if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_NET_CONN_RESET) return 0;
      return -1;
    }
  }

  bool write_all(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      long n = 0;
      if (tls) {
        const int r = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(data.data()) + sent, data.size() - sent);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        n = r;
      } else {
        n = static_cast<long>(::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
        if (n < 0 && errno == EINTR) continue;
      }
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // 先吐缓冲里剩下的，缓冲空了直接读进 dst。
  long read_some(std::uint8_t* dst, std::size_t size) {
    if (begin < end) {
      const std::size_t n = std::min(size, end - begin);
      std::memcpy(dst, buffer.data() + begin, n);
      begin += n;
      return static_cast<long>(n);
    }
    return raw_read(dst, size);
  }

  bool fill() {
    if (begin == end) begin = end = 0;
    if (buffer.size() < end + 16384) buffer.resize(end + 16384);
    const long n = raw_read(buffer.data() + end, buffer.size() - end);
    if (n <= 0) return false;
    end += static_cast<std::size_t>(n);
    return true;
  }

  bool read_line(std::string* line) {
    for (;;) {
      const auto* start = buffer.data() + begin;
      const auto* found =
          begin < end ? static_cast<const std::uint8_t*>(std::memchr(start, '\n', end - begin)) : nullptr;
      if (found != nullptr) {
        line->assign(reinterpret_cast<const char*>(start), static_cast<std::size_t>(found - start));
        if (!line->empty() && line->back() == '\r') line->pop_back();
        begin += static_cast<std::size_t>(found - start) + 1;
        return true;
      }
      if (end - begin > kMaxHeaderBytes || !fill()) return false;
    }
  }
};

// 同一主机的空闲连接，复用时省掉 TCP 和 TLS 握手。
struct Pool {
  std::mutex mu;
  std::multimap<std::string, std::unique_ptr<Connection>> idle;

  std::unique_ptr<Connection> take(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = idle.begin(); it != idle.end();) {
      if (now - it->second->idle_since > kIdleLifetime) {
        PANPLAY_DEBUG("连接 " + it->first + " 空闲超过 20 秒，关掉（" + it->second->story() + "）");
        it = idle.erase(it);
      } else {
        ++it;
      }
    }
    const auto found = idle.find(key);
    if (found == idle.end()) return nullptr;
    auto conn = std::move(found->second);
    idle.erase(found);
    return conn;
  }

  void put(std::unique_ptr<Connection> conn) {
    std::lock_guard<std::mutex> lock(mu);
    if (idle.count(conn->key) >= kIdlePerHost) {
      PANPLAY_DEBUG("连接 " + conn->key + " 空闲的已有 " + std::to_string(kIdlePerHost) + " 条，关掉这条（" + conn->story() +
                    "）");
      return;
    }
    conn->idle_since = std::chrono::steady_clock::now();
    const std::string key = conn->key;
    idle.emplace(key, std::move(conn));
  }
};

// 中止动作可能在任何阶段到来：解析域名、连接、握手、等响应头。记下当前套接字，到时 shutdown 它。
struct AbortState {
  std::mutex mu;
  bool aborted = false;
  SOCKET fd = INVALID_SOCKET;

  void abort() {
    std::lock_guard<std::mutex> lock(mu);
    aborted = true;
    if (fd != INVALID_SOCKET) shutdown(fd, SD_BOTH);
  }
  // 返回 false 表示已经中止了。
  bool watch(SOCKET socket) {
    std::lock_guard<std::mutex> lock(mu);
    fd = socket;
    if (aborted && fd != INVALID_SOCKET) shutdown(fd, SD_BOTH);
    return !aborted;
  }
  bool is_aborted() {
    std::lock_guard<std::mutex> lock(mu);
    return aborted;
  }
};

bool connect_with_timeout(SOCKET fd, const sockaddr* address, socklen_t length) {
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int result = ::connect(fd, address, length);
  if (result != 0 && errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval wait{kConnectTimeoutMs / 1000, 0};
    result = -1;
    if (select(fd + 1, nullptr, &writable, nullptr, &wait) > 0) {
      int error = 0;
      socklen_t size = sizeof(error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) result = 0;
    }
  }
  fcntl(fd, F_SETFL, flags);
  return result == 0;
}

std::unique_ptr<Connection> open_connection(const Url& url, int receive_timeout_ms, AbortState& abort, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const auto open_start = std::chrono::steady_clock::now();
  PANPLAY_DEBUG("连接 " + url.host + ":" + std::to_string(url.port) + "：解析域名");
  if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 || found == nullptr) {
    *error = "无法解析域名 " + url.host;
    PANPLAY_DEBUG(*error);
    return nullptr;
  }
  auto conn = std::make_unique<Connection>();
  conn->key = url.origin();
  conn->tls = url.tls;
  for (addrinfo* it = found; it != nullptr; it = it->ai_next) {
    SOCKET fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd == INVALID_SOCKET) continue;
#if defined(PANPLAY_PS5)
    widen_receive_buffer(fd);
#endif
    if (!abort.watch(fd)) {
      closesocket(fd);
      break;
    }
    if (connect_with_timeout(fd, it->ai_addr, it->ai_addrlen)) {
      conn->fd = fd;
      break;
    }
    abort.watch(INVALID_SOCKET);
    closesocket(fd);
  }
  freeaddrinfo(found);
  if (conn->fd == INVALID_SOCKET) {
    *error = abort.is_aborted() ? kAborted : "连接 " + url.host + " 失败";
    PANPLAY_DEBUG(*error);
    return nullptr;
  }
  const int one = 1;
  setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
  setsockopt(conn->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  set_socket_timeout(conn->fd, kSendTimeoutMs);
  timeval receive{};
  receive.tv_sec = receive_timeout_ms / 1000;
  receive.tv_usec = (receive_timeout_ms % 1000) * 1000;
  setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive));
  if (!url.tls) {
    PANPLAY_DEBUG("连接 " + url.host + " 已建立，耗时 " + std::to_string(ms_since(open_start)) + " ms");
    return conn;
  }

  TlsShared& tls = tls_shared();
  if (!tls.error.empty()) {
    *error = tls.error;
    return nullptr;
  }
  PANPLAY_DEBUG("连接 " + url.host + "：TLS 握手");
  if (int code = mbedtls_ssl_setup(&conn->ssl, &tls.conf); code != 0) {
    *error = "TLS 建立失败: " + tls_error_text(code);
    return nullptr;
  }
  mbedtls_ssl_set_hostname(&conn->ssl, url.host.c_str());
  mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, bio_send, bio_recv, nullptr);
  for (;;) {
    const int code = mbedtls_ssl_handshake(&conn->ssl);
    if (code == 0) break;
    if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (abort.is_aborted()) {
      *error = kAborted;
    } else if (code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
      *error = "证书校验失败: " + url.host;
    } else {
      *error = "TLS 握手失败: " + tls_error_text(code);
    }
    PANPLAY_DEBUG("连接 " + url.host + "：" + *error);
    return nullptr;
  }
  PANPLAY_DEBUG("连接 " + url.host + " 已建立（TLS），耗时 " + std::to_string(ms_since(open_start)) + " ms");
  note_tls_suite(url.host, conn->ssl);
  return conn;
}

struct ResponseHead {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  bool keep_alive = true;
  bool chunked = false;
  std::int64_t content_length = -1;

  std::string get(const std::string& name) const {
    const std::string wanted = lower(name);
    for (const auto& header : headers) {
      if (header.first == wanted) return header.second;
    }
    return {};
  }
};

bool read_head(Connection& conn, ResponseHead* head) {
  std::string line;
  // 跳过 100 Continue 之类的临时响应。
  for (;;) {
    if (!conn.read_line(&line)) return false;
    if (line.rfind("HTTP/", 0) != 0) return false;
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) return false;
    head->status = std::atoi(line.c_str() + space + 1);
    head->keep_alive = line.rfind("HTTP/1.1", 0) == 0;
    head->headers.clear();
    std::size_t total = 0;
    for (;;) {
      if (!conn.read_line(&line)) return false;
      total += line.size();
      if (total > kMaxHeaderBytes) return false;
      if (line.empty()) break;
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string value = line.substr(colon + 1);
      const std::size_t first = value.find_first_not_of(" \t");
      value = first == std::string::npos ? "" : value.substr(first);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      head->headers.emplace_back(lower(line.substr(0, colon)), value);
    }
    if (head->status >= 200 || head->status == 101) break;
  }
  const std::string connection = lower(head->get("Connection"));
  if (connection.find("close") != std::string::npos) head->keep_alive = false;
  if (connection.find("keep-alive") != std::string::npos) head->keep_alive = true;
  head->chunked = lower(head->get("Transfer-Encoding")).find("chunked") != std::string::npos;
  const std::string length = head->get("Content-Length");
  if (!head->chunked && !length.empty()) head->content_length = std::strtoll(length.c_str(), nullptr, 10);
  return true;
}

class PosixStream final : public HttpStream {
 public:
  PosixStream(std::shared_ptr<Pool> pool, std::unique_ptr<Connection> conn, ResponseHead head, std::string final_url,
              bool head_request)
      : pool_(std::move(pool)), conn_(std::move(conn)), head_(std::move(head)), final_url_(std::move(final_url)) {
    fd_ = conn_->fd;
    if (head_.chunked) {
      mode_ = Mode::kChunked;
    } else if (head_.content_length >= 0) {
      mode_ = Mode::kLength;
      remaining_ = head_.content_length;
    } else {
      mode_ = Mode::kUntilClose;
      head_.keep_alive = false;
    }
    if (head_request || head_.status == 204 || head_.status == 304) {
      mode_ = Mode::kLength;
      remaining_ = 0;
    }
  }

  // 连接没还回池就在这里关掉。正常读完还回池的不记，中止、出错、没读完的记下原因。
  ~PosixStream() override {
    if (!conn_ || closed_noted_ || !log_enabled(LogLevel::debug)) return;
    std::string reason;
    if (aborted_.load()) {
      reason = "被中止";
    } else if (failed_) {
      reason = "读的时候出错";
    } else if (done_) {
      reason = "响应没给长度，读到服务器关闭为止";
    } else {
      reason = "响应没读完";
      if (mode_ == Mode::kLength) reason += "，还剩 " + std::to_string(remaining_) + " 字节";
    }
    PANPLAY_DEBUG("连接 " + conn_->key + " " + reason + "，关掉（" + conn_->story() + "）");
  }

  int status() const override { return head_.status; }
  std::int64_t content_length() const override { return head_.chunked ? -1 : head_.content_length; }
  std::string final_url() const override { return final_url_; }
  std::string header(const std::string& name) const override { return head_.get(name); }
  std::vector<std::pair<std::string, std::string>> headers() const override { return head_.headers; }

  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) override {
    if (aborted_.load()) {
      *error = kAborted;
      return 0;
    }
    if (done_ || size == 0) return 0;
    switch (mode_) {
      case Mode::kLength: {
        if (remaining_ == 0) {
          finish();
          return 0;
        }
        const long n = conn_->read_some(dst, static_cast<std::size_t>(std::min<std::int64_t>(remaining_, static_cast<std::int64_t>(size))));
        if (n <= 0) return fail(error);
        remaining_ -= n;
        if (remaining_ == 0) finish();
        return static_cast<std::size_t>(n);
      }
      case Mode::kUntilClose: {
        const long n = conn_->read_some(dst, size);
        if (n < 0) return fail(error);
        if (n == 0) done_ = true;
        return static_cast<std::size_t>(n);
      }
      case Mode::kChunked: {
        if (chunk_left_ == 0) {
          std::string line;
          if (chunk_started_ && (!conn_->read_line(&line) || !line.empty())) return fail(error);
          chunk_started_ = true;
          if (!conn_->read_line(&line)) return fail(error);
          chunk_left_ = std::strtoll(line.c_str(), nullptr, 16);
          if (chunk_left_ < 0) return fail(error);
          if (chunk_left_ == 0) {
            do {
              if (!conn_->read_line(&line)) return fail(error);
            } while (!line.empty());
            finish();
            return 0;
          }
        }
        const long n = conn_->read_some(dst, static_cast<std::size_t>(std::min<std::int64_t>(chunk_left_, static_cast<std::int64_t>(size))));
        if (n <= 0) return fail(error);
        chunk_left_ -= n;
        return static_cast<std::size_t>(n);
      }
    }
    return 0;
  }

  void abort() override {
    std::lock_guard<std::mutex> lock(mu_);
    aborted_ = true;
    if (fd_ != INVALID_SOCKET) shutdown(fd_, SD_BOTH);
  }

 private:
  enum class Mode { kLength, kChunked, kUntilClose };

  std::size_t fail(std::string* error) {
    *error = aborted_.load() ? kAborted : "下载连接中断";
    done_ = true;
    failed_ = true;
    return 0;
  }

  // 响应体读完了，连接还能用就还回连接池。之后 abort 不能再碰这个套接字。
  void finish() {
    done_ = true;
    std::lock_guard<std::mutex> lock(mu_);
    if (aborted_.load() || !conn_) return;
    if (!head_.keep_alive) {
      closed_noted_ = true;
      PANPLAY_DEBUG("连接 " + conn_->key + " 服务器要求读完就关（Connection: close 或 HTTP/1.0），" + conn_->story());
      return;
    }
    fd_ = INVALID_SOCKET;
    pool_->put(std::move(conn_));
  }

  std::shared_ptr<Pool> pool_;
  std::unique_ptr<Connection> conn_;
  ResponseHead head_;
  std::string final_url_;
  std::mutex mu_;
  SOCKET fd_ = INVALID_SOCKET;
  std::atomic<bool> aborted_{false};
  bool done_ = false;
  bool failed_ = false;
  bool closed_noted_ = false;
  Mode mode_ = Mode::kLength;
  std::int64_t remaining_ = 0;
  std::int64_t chunk_left_ = 0;
  bool chunk_started_ = false;
};

std::string build_request(const HttpRequest& request, const std::string& method, const std::string& body, const Url& url) {
  const bool default_port = url.port == (url.tls ? 443 : 80);
  std::string text = method + " " + url.target + " HTTP/1.1\r\nHost: " + url.host +
                     (default_port ? "" : ":" + std::to_string(url.port)) + "\r\n";
  bool has_agent = false;
  for (const auto& header : request.headers) {
    if (lower(header.first) == "user-agent") has_agent = true;
    text += header.first + ": " + header.second + "\r\n";
  }
  if (!has_agent) text += "User-Agent: panplay/0.1\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  text += "Connection: keep-alive\r\n\r\n";
  text += body;
  return text;
}

bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

}  // namespace

HttpClient::HttpClient() { session_ = new std::shared_ptr<Pool>(std::make_shared<Pool>()); }

HttpClient::~HttpClient() { delete static_cast<std::shared_ptr<Pool>*>(session_); }

std::unique_ptr<HttpStream> HttpClient::open(const HttpRequest& request, std::string* error) {
  const std::shared_ptr<Pool> pool = *static_cast<std::shared_ptr<Pool>*>(session_);
  auto abort_state = std::make_shared<AbortState>();
  if (request.abort != nullptr && !request.abort->arm([abort_state] { abort_state->abort(); })) {
    *error = kAborted;
    return nullptr;
  }
  struct Disarm {
    AbortSignal* signal;
    ~Disarm() {
      if (signal != nullptr) signal->disarm();
    }
  } disarm{request.abort};

  const int receive_timeout = request.timeout_ms > 0 ? request.timeout_ms : 30000;
  std::string url_text = request.url;
  std::string method = request.method;
  std::string body = request.body;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    Url url;
    if (!parse_url(url_text, &url)) {
      *error = "invalid url";
      return nullptr;
    }
    const std::string payload = build_request(request, method, body, url);
    std::unique_ptr<Connection> conn;
    ResponseHead head;
    bool ok = false;
    // 复用的空闲连接可能已被服务器关掉，失败时换一条新连接再试一次。
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
      conn = attempt == 0 ? pool->take(url.origin()) : nullptr;
      const bool reused = conn != nullptr;
      if (!conn) {
        conn = open_connection(url, receive_timeout, *abort_state, error);
        if (!conn) return nullptr;
      } else {
        timeval receive{};
        receive.tv_sec = receive_timeout / 1000;
        receive.tv_usec = (receive_timeout % 1000) * 1000;
        setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive));
      }
      if (!abort_state->watch(conn->fd)) {
        *error = kAborted;
        return nullptr;
      }
      ++conn->uses;
      ok = conn->write_all(payload) && read_head(*conn, &head);
      if (!ok && (!reused || abort_state->is_aborted())) {
        *error = abort_state->is_aborted() ? kAborted : "request failed";
        return nullptr;
      }
      if (!ok) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - conn->idle_since);
        PANPLAY_DEBUG("连接 " + conn->key + " 复用失败，应该是服务器已经关了（空闲 " + std::to_string(idle.count()) + " 秒，" +
                      conn->story() + "），换新连接重发");
      }
    }
    if (!ok) {
      *error = "request failed";
      return nullptr;
    }
    const std::string location = head.get("Location");
    if (request.follow_redirects && is_redirect(head.status) && !location.empty()) {
      const std::string next = resolve_location(url, location);
      // 和 WinHTTP 的默认策略一样：不从 https 跳到 http，原样返回跳转响应。
      if (!(url.tls && next.rfind("http://", 0) == 0)) {
        PANPLAY_DEBUG("连接 " + conn->key + " 收到跳转，响应体不读，关掉（" + conn->story() + "）");
        url_text = next;
        if (head.status == 303 || ((head.status == 301 || head.status == 302) && method == "POST")) {
          method = "GET";
          body.clear();
        }
        continue;
      }
    }
    abort_state->watch(INVALID_SOCKET);
    if (request.abort != nullptr && request.abort->disarm()) {
      *error = kAborted;
      return nullptr;
    }
    disarm.signal = nullptr;
    return std::make_unique<PosixStream>(pool, std::move(conn), std::move(head), url_text, method == "HEAD");
  }
  *error = "重定向次数过多";
  return nullptr;
}

}  // namespace panplay

#endif
