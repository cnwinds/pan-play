#include "panplay/http.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>

namespace panplay {
namespace {

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
  return out;
}

std::string narrow(const std::wstring& value) {
  if (value.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
  return out;
}

class WinHttpStream final : public HttpStream {
 public:
  WinHttpStream(HINTERNET connect, HINTERNET request) : connect_(connect), request_(request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    status_ = static_cast<int>(status);
    wchar_t length[32]{};
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
      content_length_ = _wcstoi64(length, nullptr, 10);
    }
    DWORD url_size = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &url_size);
    if (url_size > 0) {
      std::wstring url(url_size / sizeof(wchar_t), L'\0');
      if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &url_size)) {
        url.resize(url_size / sizeof(wchar_t));
        while (!url.empty() && url.back() == L'\0') url.pop_back();
        final_url_ = narrow(url);
      }
    }
  }

  ~WinHttpStream() override {
    abort();
    WinHttpCloseHandle(connect_);
  }

  int status() const override { return status_; }
  std::int64_t content_length() const override { return content_length_; }
  std::string final_url() const override { return final_url_; }

  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) override {
    HINTERNET request = request_.load();
    if (request == nullptr) {
      *error = "下载已中止";
      return 0;
    }
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
    if (!WinHttpReadData(request, dst, want, &got)) {
      *error = request_.load() == nullptr ? "下载已中止" : "下载连接中断";
      return 0;
    }
    return got;
  }

  void abort() override {
    HINTERNET request = request_.exchange(nullptr);
    if (request != nullptr) WinHttpCloseHandle(request);
  }

 private:
  HINTERNET connect_ = nullptr;
  std::atomic<HINTERNET> request_{nullptr};
  int status_ = 0;
  std::int64_t content_length_ = -1;
  std::string final_url_;
};

}  // namespace

std::string url_encode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

HttpClient::HttpClient() {
  session_ = WinHttpOpen(L"panplay/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
}

HttpClient::~HttpClient() {
  if (session_ != nullptr) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

std::unique_ptr<HttpStream> HttpClient::open(const HttpRequest& request, std::string* error) {
  if (session_ == nullptr) {
    *error = "winhttp open failed";
    return nullptr;
  }
  const std::wstring url = widen(request.url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[512]{};
  wchar_t path[4096]{};
  wchar_t extra[8192]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 512;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 8192;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    *error = "invalid url";
    return nullptr;
  }
  HINTERNET connect = WinHttpConnect(static_cast<HINTERNET>(session_), host, parts.nPort, 0);
  if (!connect) {
    *error = "winhttp connect failed";
    return nullptr;
  }
  std::wstring target = path;
  target += extra;
  const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  const std::wstring method = widen(request.method);
  HINTERNET handle = WinHttpOpenRequest(connect, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!handle) {
    WinHttpCloseHandle(connect);
    *error = "winhttp request failed";
    return nullptr;
  }
  const int timeout = request.timeout_ms > 0 ? request.timeout_ms : 30000;
  WinHttpSetTimeouts(handle, 10000, 10000, 20000, timeout);
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

  std::wstring header_block;
  for (const auto& header : request.headers) {
    header_block += widen(header.first);
    header_block += L": ";
    header_block += widen(header.second);
    header_block += L"\r\n";
  }
  BOOL sent = WinHttpSendRequest(handle, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
                                 header_block.empty() ? 0 : static_cast<DWORD>(-1),
                                 request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                                 static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
  if (!sent || !WinHttpReceiveResponse(handle, nullptr)) {
    WinHttpCloseHandle(handle);
    WinHttpCloseHandle(connect);
    *error = "request failed";
    return nullptr;
  }
  return std::make_unique<WinHttpStream>(connect, handle);
}

HttpResponse HttpClient::send(const HttpRequest& request) {
  HttpResponse response;
  auto stream = open(request, &response.error);
  if (!stream) return response;
  response.status = stream->status();
  response.final_url = stream->final_url();
  std::uint8_t buffer[65536];
  for (;;) {
    std::string error;
    const std::size_t got = stream->read(buffer, sizeof(buffer), &error);
    if (!error.empty()) {
      response.error = error;
      break;
    }
    if (got == 0) break;
    response.body.append(reinterpret_cast<const char*>(buffer), got);
  }
  return response;
}

HttpResponse http_send(const HttpRequest& request) {
  static HttpClient client;
  return client.send(request);
}

}  // namespace panplay
