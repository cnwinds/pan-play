#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
#ifndef SD_SEND
#define SD_SEND SHUT_WR
#endif
#ifndef SD_BOTH
#define SD_BOTH SHUT_RDWR
#endif
inline int closesocket(SOCKET socket) { return ::close(socket); }
#endif

#include <cstdint>
#include <string>

namespace panplay {

#if defined(_WIN32)
using socklen = int;
#else
using socklen = socklen_t;
#endif

bool net_startup(std::string* error);
void set_socket_timeout(SOCKET socket, int milliseconds);
// 从别的线程关掉套接字时用：POSIX 上只 close 叫不醒阻塞在 accept/recv 里的线程，要先 shutdown。
void close_socket_now(SOCKET socket);
// 等套接字可读，超时或出错返回 false。
bool wait_readable(SOCKET socket, int milliseconds);
bool send_all(SOCKET socket, const char* data, std::size_t size);
bool send_line(SOCKET socket, const std::string& line);
bool recv_some(SOCKET socket, std::string* pending);

struct LineReader {
  SOCKET socket = INVALID_SOCKET;
  std::string pending;

  bool recv_line(std::string* line);
  bool read_reply(std::string* text, int* code);
};

SOCKET connect_tcp(const std::string& host, std::uint16_t port, std::string* error);
bool parse_pasv(const std::string& reply, std::string* host, std::uint16_t* port);

}  // namespace panplay
