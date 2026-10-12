#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

namespace panplay {

bool net_startup(std::string* error);
void set_socket_timeout(SOCKET socket, int milliseconds);
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
