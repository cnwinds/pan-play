#include "socket_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace panplay {
namespace {

bool trace_enabled() {
  const char* value = std::getenv("PANPLAY_FTP_TRACE");
  return value && value[0] == '1';
}

}  // namespace

bool net_startup(std::string* error) {
  static bool ready = false;
  if (ready) return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    *error = "Winsock 初始化失败";
    return false;
  }
  ready = true;
  return true;
}

void set_socket_timeout(SOCKET socket, int milliseconds) {
  const DWORD timeout = static_cast<DWORD>(milliseconds);
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

bool send_all(SOCKET socket, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const int n = ::send(socket, data + sent, static_cast<int>(size - sent), 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_line(SOCKET socket, const std::string& line) {
  if (trace_enabled()) std::cerr << "ftp > " << line << "\n";
  const std::string framed = line + "\r\n";
  return send_all(socket, framed.data(), framed.size());
}

bool recv_some(SOCKET socket, std::string* pending) {
  char buffer[4096];
  const int n = ::recv(socket, buffer, sizeof(buffer), 0);
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
      if (trace_enabled()) std::cerr << "ftp < " << *line << "\n";
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
