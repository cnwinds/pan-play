#include "panplay/socket_util.hpp"
#include "panplay/trace.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace panplay {
namespace {

#if defined(_WIN32)
// Windows 10 起的 SO_REUSE_UNICASTPORT，MinGW 头文件里没有。
constexpr int kReuseUnicastPort = 0x3007;
#endif

// 每条 FTP 命令都记，装一个游戏就是几万行，所以调试日志之外还要单独设 PANPLAY_FTP_TRACE=1。
bool ftp_trace_enabled() {
  static const bool requested = [] {
    const char* value = std::getenv("PANPLAY_FTP_TRACE");
    return value && value[0] == '1';
  }();
  return requested && log_enabled(LogLevel::debug);
}

}  // namespace

bool net_startup(std::string* error) {
#if defined(_WIN32)
  static bool ready = false;
  if (ready) return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    *error = "Winsock 初始化失败";
    return false;
  }
  ready = true;
#else
  (void)error;
#endif
  return true;
}

void set_listen_exclusive(SOCKET socket) {
  const int on = 1;
#if defined(_WIN32)
  // Windows 的 SO_REUSEADDR 允许别的进程绑同一个端口，请求会被随机一方接走；要用独占。
  setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
  // POSIX 的 SO_REUSEADDR 只是让 TIME_WAIT 里的端口能马上重新监听，不允许两个进程同时监听。
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
}

void set_socket_timeout(SOCKET socket, int milliseconds) {
#if defined(_WIN32)
  const DWORD timeout = static_cast<DWORD>(milliseconds);
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

void close_socket_now(SOCKET socket) {
  if (socket == INVALID_SOCKET) return;
  shutdown(socket, SD_BOTH);
  closesocket(socket);
}

bool wait_readable(SOCKET socket, int milliseconds) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(socket, &fds);
  timeval wait{};
  wait.tv_sec = milliseconds / 1000;
  wait.tv_usec = (milliseconds % 1000) * 1000;
  return select(static_cast<int>(socket) + 1, &fds, nullptr, nullptr, &wait) > 0;
}

bool send_all(SOCKET socket, const char* data, std::size_t size) {
#if defined(MSG_NOSIGNAL)
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  std::size_t sent = 0;
  while (sent < size) {
    const int n = static_cast<int>(::send(socket, data + sent, static_cast<int>(size - sent), flags));
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_line(SOCKET socket, const std::string& line) {
  if (ftp_trace_enabled()) log_line(LogLevel::debug, "ftp > " + line);
  const std::string framed = line + "\r\n";
  return send_all(socket, framed.data(), framed.size());
}

bool recv_some(SOCKET socket, std::string* pending) {
  char buffer[4096];
  const int n = static_cast<int>(::recv(socket, buffer, sizeof(buffer), 0));
  if (n <= 0) return false;
  pending->append(buffer, static_cast<std::size_t>(n));
  return pending->size() <= 1024 * 1024;
}

bool LineReader::recv_line(std::string* line) {
  for (;;) {
    const std::size_t pos = pending.find('\n');
    if (pos != std::string::npos) {
      *line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      if (!line->empty() && line->back() == '\r') line->pop_back();
      if (ftp_trace_enabled()) log_line(LogLevel::debug, "ftp < " + *line);
      return true;
    }
    if (!recv_some(socket, &pending)) return false;
  }
}

bool LineReader::read_reply(std::string* text, int* code) {
  std::string line;
  if (!recv_line(&line) || line.size() < 3) return false;
  const int first = std::atoi(line.substr(0, 3).c_str());
  if (line.size() >= 4 && line[3] == '-') {
    std::string all = line;
    for (;;) {
      if (!recv_line(&line)) return false;
      all += "\n";
      all += line;
      if (line.size() >= 4 && std::atoi(line.substr(0, 3).c_str()) == first && line[3] == ' ') break;
    }
    *text = all;
    *code = first;
    return true;
  }
  *text = line;
  *code = first;
  return true;
}

SOCKET connect_tcp(const std::string& host, std::uint16_t port, std::string* error) {
  if (!net_startup(error)) return INVALID_SOCKET;
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    *error = "无法创建套接字";
    return INVALID_SOCKET;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    closesocket(socket);
    *error = "地址无效";
    return INVALID_SOCKET;
  }
#if defined(_WIN32)
  // FTP 每传一个文件开一条数据连接，且由本机先关，关掉的连接在 TIME_WAIT 里占着本机端口约两分钟；
  // 小文件多时一两分钟就能把临时端口用光。打开这个选项并绑端口 0，系统改在 connect 时按完整的
  // 四元组选端口，还在 TIME_WAIT 的端口也能连别的地址。系统不支持这个选项时照旧。
  const DWORD reuse = 1;
  if (setsockopt(socket, SOL_SOCKET, kReuseUnicastPort, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == 0) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local));
  }
#endif
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    closesocket(socket);
    *error = "连接失败";
    return INVALID_SOCKET;
  }
  set_socket_timeout(socket, 10000);
  return socket;
}

bool parse_pasv(const std::string& reply, std::string* host, std::uint16_t* port) {
  const std::size_t open = reply.rfind('(');
  const std::size_t close = reply.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return false;
  int h1 = 0, h2 = 0, h3 = 0, h4 = 0, p1 = 0, p2 = 0;
  if (std::sscanf(reply.c_str() + open, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return false;
  *host = std::to_string(h1) + "." + std::to_string(h2) + "." + std::to_string(h3) + "." + std::to_string(h4);
  *port = static_cast<std::uint16_t>(p1 * 256 + p2);
  return true;
}

}  // namespace panplay
