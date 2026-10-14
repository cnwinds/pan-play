#pragma once

#include "panplay/abort.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace panplay {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string error;
  // 跟随重定向后实际请求的地址。
  std::string final_url;
  // 全部响应头，名字小写，同名的（如 set-cookie）各占一项。
  std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpRequest {
  std::string url;
  std::string method = "GET";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  int timeout_ms = 0;  // 0 = default (30s receive)
  // 可选。中止后 open 立即失败返回；open 成功后由 HttpStream::abort 接管。
  AbortSignal* abort = nullptr;
  // false 时原样返回 3xx 响应，由调用方自己处理 Location 和中间的 Set-Cookie。
  bool follow_redirects = true;
};

// 边收边读的响应体。
class HttpStream {
 public:
  virtual ~HttpStream() = default;
  virtual int status() const = 0;
  // 服务器没给长度时返回 -1。
  virtual std::int64_t content_length() const = 0;
  // 跟随重定向后实际请求的地址。
  virtual std::string final_url() const = 0;
  // 响应头的值，没有时返回空串。
  virtual std::string header(const std::string& name) const = 0;
  // 全部响应头，名字小写。
  virtual std::vector<std::pair<std::string, std::string>> headers() const = 0;
  // 返回读到的字节数，0 表示响应体结束。
  virtual std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) = 0;
  // 可以从其他线程调用，让阻塞中的 read 立刻失败返回。
  virtual void abort() = 0;
};

// 持有一个长期会话，同一主机的 TCP 和 TLS 连接会被复用。可以多线程同时使用。
class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpResponse send(const HttpRequest& request);
  std::unique_ptr<HttpStream> open(const HttpRequest& request, std::string* error);

 private:
  void* session_ = nullptr;
};

// 走进程内共享的 HttpClient。
HttpResponse http_send(const HttpRequest& request);

std::string url_encode(const std::string& value);

}  // namespace panplay
