#include "panplay/http.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

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

HttpResponse http_send(const HttpRequest& request) {
  HttpResponse response;
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
    response.error = "invalid url";
    return response;
  }

  HINTERNET session = WinHttpOpen(L"panplay/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    response.error = "winhttp open failed";
    return response;
  }
  const int timeout = request.timeout_ms > 0 ? request.timeout_ms : 30000;
  WinHttpSetTimeouts(session, 10000, 10000, 20000, timeout);
  HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    response.error = "winhttp connect failed";
    return response;
  }
  std::wstring target = path;
  target += extra;
  const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  const std::wstring method = widen(request.method);
  HINTERNET handle = WinHttpOpenRequest(connect, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!handle) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    response.error = "winhttp request failed";
    return response;
  }

  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
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
    response.error = "request failed";
    WinHttpCloseHandle(handle);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &status_size, WINHTTP_NO_HEADER_INDEX);
  response.status = static_cast<int>(status);

  std::string body;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(handle, &available)) break;
    if (available == 0) break;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(handle, chunk.data(), available, &read)) break;
    chunk.resize(read);
    body += chunk;
  }
  response.body = std::move(body);

  WinHttpCloseHandle(handle);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return response;
}

}  // namespace panplay
