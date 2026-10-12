#include "panplay/dummy_ftp.hpp"

#include "panplay/storage_path.hpp"
#include "socket_util.hpp"

#include <atomic>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <cstdio>
#include <thread>

namespace panplay {
namespace {

std::string upper_copy(std::string value) {
  for (char& ch : value) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return value;
}

std::string command_name(const std::string& line, std::string* argument) {
  const std::size_t space = line.find(' ');
  if (space == std::string::npos) {
    *argument = "";
    return upper_copy(line);
  }
  *argument = line.substr(space + 1);
  while (!argument->empty() && argument->front() == ' ') argument->erase(argument->begin());
  return upper_copy(line.substr(0, space));
}

SOCKET listen_on(std::uint16_t port, std::uint16_t* bound, std::string* error) {
  if (!net_startup(error)) return INVALID_SOCKET;
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    *error = "无法创建套接字";
    return INVALID_SOCKET;
  }
  BOOL reuse = TRUE;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    closesocket(socket);
    *error = "绑定 127.0.0.1 失败";
    return INVALID_SOCKET;
  }
  if (listen(socket, 4) != 0) {
    closesocket(socket);
    *error = "监听失败";
    return INVALID_SOCKET;
  }
  sockaddr_in actual{};
  int length = sizeof(actual);
  getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &length);
  *bound = ntohs(actual.sin_port);
  return socket;
}

}  // namespace

struct DummyFtp::Impl {
  std::filesystem::path root;
  std::uint16_t requested = 0;
  std::atomic<std::uint16_t> bound{0};
  std::atomic<bool> running{false};
  std::atomic<int> data_timeout_ms{10000};
  std::mutex sockets_mu;
  SOCKET listen_socket = INVALID_SOCKET;
  SOCKET active_socket = INVALID_SOCKET;
  std::thread worker;

  std::string resolve(const std::string& cwd, const std::string& path, std::filesystem::path* out) const {
    std::string logical = path;
    if (logical.empty() || logical.front() != '/') logical = cwd + (cwd == "/" ? "" : "/") + logical;
    std::string error;
    if (!storage_path(root, logical, out, &error)) return error;
    return "";
  }

  void handle(SOCKET client) {
    // 10 秒无命令会断开。安装读网盘时客户端会发 NOOP，避免控制连接在扫描期间被掐掉。
    set_socket_timeout(client, 10000);
    LineReader reader;
    reader.socket = client;
    if (!send_line(client, "220 panplay dummy ftp ready")) return;

    bool authed = false;
    std::string cwd = "/";
    std::filesystem::path rename_from;
    SOCKET passive = INVALID_SOCKET;
    auto close_passive = [&]() {
      if (passive != INVALID_SOCKET) {
        closesocket(passive);
        passive = INVALID_SOCKET;
      }
    };

    std::string line;
    while (running.load() && reader.recv_line(&line)) {
      std::string argument;
      const std::string name = command_name(line, &argument);
      if (!authed && name != "USER" && name != "PASS" && name != "QUIT") {
        send_line(client, "530 login required");
        continue;
      }
      if (name == "QUIT") {
        send_line(client, "221 bye");
        break;
      }
      if (name == "USER") {
        send_line(client, "331 password required");
        continue;
      }
      if (name == "PASS") {
        authed = true;
        send_line(client, "230 login ok");
        continue;
      }
      if (name == "NOOP" || name == "SYST") {
        send_line(client, name == "SYST" ? "215 UNIX Type: L8" : "200 ok");
        continue;
      }
      if (name == "TYPE") {
        send_line(client, argument == "I" ? "200 type set" : "504 binary only");
        continue;
      }
      if (name == "PWD") {
        send_line(client, "257 \"" + cwd + "\" is current directory");
        continue;
      }
      if (name == "PASV") {
        close_passive();
        std::string error;
        std::uint16_t data_port = 0;
        passive = listen_on(0, &data_port, &error);
        if (passive == INVALID_SOCKET) {
          send_line(client, "425 " + error);
          continue;
        }
        const int high = data_port / 256;
        const int low = data_port % 256;
        send_line(client, "227 Entering Passive Mode (127,0,0,1," + std::to_string(high) + "," + std::to_string(low) + ")");
        continue;
      }

      std::filesystem::path local;
      if (name == "CWD" || name == "MKD" || name == "SIZE" || name == "STOR" || name == "APPE" || name == "DELE" ||
          name == "RMD" || name == "RNFR" || name == "RNTO" || name == "RETR" || name == "NLST") {
        if (name != "RNTO") {
          const std::string error = resolve(cwd, argument, &local);
          if (!error.empty()) {
            send_line(client, "550 " + error);
            continue;
          }
        }
      }

      try {
        if (name == "CWD") {
          if (argument.empty() || argument.front() != '/') {
            send_line(client, "550 absolute path required");
          } else if (!std::filesystem::is_directory(local)) {
            send_line(client, "550 not a directory");
          } else {
            cwd = argument;
            while (cwd.size() > 1 && cwd.back() == '/') cwd.pop_back();
            send_line(client, "250 cwd ok");
          }
        } else if (name == "MKD") {
          if (std::filesystem::is_directory(local)) {
            send_line(client, "257 \"" + argument + "\" exists");
          } else if (std::filesystem::create_directory(local)) {
            send_line(client, "257 \"" + argument + "\" created");
          } else {
            send_line(client, "550 cannot create directory");
          }
        } else if (name == "SIZE") {
          if (!std::filesystem::is_regular_file(local)) {
            send_line(client, "550 not found");
          } else {
            send_line(client, "213 " + std::to_string(std::filesystem::file_size(local)));
          }
        } else if (name == "DELE") {
          if (!std::filesystem::is_regular_file(local) || !std::filesystem::remove(local)) {
            send_line(client, "550 cannot delete");
          } else {
            send_line(client, "250 deleted");
          }
        } else if (name == "RMD") {
          std::error_code ec;
          if (!std::filesystem::is_directory(local) || !std::filesystem::is_empty(local, ec) ||
              !std::filesystem::remove(local, ec)) {
            send_line(client, "550 cannot remove directory");
          } else {
            send_line(client, "250 removed");
          }
        } else if (name == "RNFR") {
          if (!std::filesystem::exists(local)) {
            send_line(client, "550 not found");
          } else {
            rename_from = local;
            send_line(client, "350 ready for rnto");
          }
        } else if (name == "RNTO") {
          std::filesystem::path target;
          const std::string error = resolve(cwd, argument, &target);
          if (rename_from.empty()) {
            send_line(client, "503 rnfr first");
          } else if (!error.empty()) {
            send_line(client, "550 " + error);
          } else if (std::filesystem::exists(target)) {
            send_line(client, "550 target exists");
          } else {
            std::filesystem::rename(rename_from, target);
            rename_from.clear();
            send_line(client, "250 renamed");
          }
        } else if (name == "NLST") {
          if (passive == INVALID_SOCKET) {
            send_line(client, "425 use pasv first");
            continue;
          }
          if (!std::filesystem::is_directory(local)) {
            close_passive();
            send_line(client, "550 not a directory");
            continue;
          }
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(passive, &fds);
          timeval wait{10, 0};
          if (select(0, &fds, nullptr, nullptr, &wait) <= 0) {
            close_passive();
            send_line(client, "425 data connection timeout");
            continue;
          }
          SOCKET data = accept(passive, nullptr, nullptr);
          close_passive();
          if (data == INVALID_SOCKET) {
            send_line(client, "425 data connection failed");
            continue;
          }
          set_socket_timeout(data, 10000);
          send_line(client, "150 opening data connection");
          bool failed = false;
          for (const auto& entry : std::filesystem::directory_iterator(local)) {
            const std::string line = entry.path().filename().u8string() + "\r\n";
            if (!send_all(data, line.data(), line.size())) {
              failed = true;
              break;
            }
          }
          closesocket(data);
          send_line(client, failed ? "426 transfer aborted" : "226 transfer complete");
        } else if (name == "RETR") {
          if (passive == INVALID_SOCKET) {
            send_line(client, "425 use pasv first");
            continue;
          }
          if (!std::filesystem::is_regular_file(local)) {
            close_passive();
            send_line(client, "550 not found");
            continue;
          }
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(passive, &fds);
          timeval wait{10, 0};
          if (select(0, &fds, nullptr, nullptr, &wait) <= 0) {
            close_passive();
            send_line(client, "425 data connection timeout");
            continue;
          }
          SOCKET data = accept(passive, nullptr, nullptr);
          close_passive();
          if (data == INVALID_SOCKET) {
            send_line(client, "425 data connection failed");
            continue;
          }
          set_socket_timeout(data, 10000);
          FILE* file = _wfopen(local.wstring().c_str(), L"rb");
          if (!file) {
            closesocket(data);
            send_line(client, "550 cannot open file");
            continue;
          }
          send_line(client, "150 opening data connection");
          char buffer[64 * 1024];
          bool failed = false;
          while (true) {
            const std::size_t n = std::fread(buffer, 1, sizeof(buffer), file);
            if (n > 0 && !send_all(data, buffer, n)) {
              failed = true;
              break;
            }
            if (n < sizeof(buffer)) {
              if (std::ferror(file)) failed = true;
              break;
            }
          }
          std::fclose(file);
          closesocket(data);
          send_line(client, failed ? "426 transfer aborted" : "226 transfer complete");
        } else if (name == "STOR" || name == "APPE") {
          if (passive == INVALID_SOCKET) {
            send_line(client, "425 use pasv first");
            continue;
          }
          fd_set fds;
          FD_ZERO(&fds);
          FD_SET(passive, &fds);
          timeval wait{10, 0};
          if (select(0, &fds, nullptr, nullptr, &wait) <= 0) {
            close_passive();
            send_line(client, "425 data connection timeout");
            continue;
          }
          SOCKET data = accept(passive, nullptr, nullptr);
          close_passive();
          if (data == INVALID_SOCKET) {
            send_line(client, "425 data connection failed");
            continue;
          }
          set_socket_timeout(data, data_timeout_ms.load());
          if (!std::filesystem::exists(local.parent_path())) {
            closesocket(data);
            send_line(client, "550 parent directory missing");
            continue;
          }
          // 直接写目标文件。中断后 SIZE 就是已落盘字节，和已提交字节不一致时由客户端改为 STOR 覆盖。
          const bool append = name == "APPE";
          FILE* file = _wfopen(local.wstring().c_str(), append ? L"ab" : L"wb");
          if (!file) {
            closesocket(data);
            send_line(client, "550 cannot open file");
            continue;
          }
          send_line(client, "150 opening data connection");
          char buffer[64 * 1024];
          bool failed = false;
          for (;;) {
            const int n = recv(data, buffer, sizeof(buffer), 0);
            if (n == 0) break;
            if (n < 0 || std::fwrite(buffer, 1, static_cast<std::size_t>(n), file) != static_cast<std::size_t>(n)) {
              failed = true;
              break;
            }
          }
          if (std::fclose(file) != 0) failed = true;
          closesocket(data);
          if (failed) {
            send_line(client, "426 transfer aborted");
            continue;
          }
          send_line(client, "226 transfer complete");
        } else {
          send_line(client, "502 command not implemented");
        }
      } catch (const std::exception& ex) {
        send_line(client, std::string("550 ") + ex.what());
      }
    }
    close_passive();
  }
};

DummyFtp::DummyFtp(std::filesystem::path root, std::uint16_t port) : impl_(new Impl) {
  impl_->root = std::filesystem::absolute(root);
  impl_->requested = port;
}

DummyFtp::~DummyFtp() {
  stop();
  delete impl_;
}

bool DummyFtp::start(std::string* error) {
  std::filesystem::create_directories(impl_->root);
  std::uint16_t bound = 0;
  impl_->listen_socket = listen_on(impl_->requested, &bound, error);
  if (impl_->listen_socket == INVALID_SOCKET) return false;
  impl_->bound = bound;
  impl_->running = true;
  impl_->worker = std::thread([this]() {
    while (impl_->running.load()) {
      SOCKET client = accept(impl_->listen_socket, nullptr, nullptr);
      if (client == INVALID_SOCKET) break;
      {
        std::lock_guard<std::mutex> lock(impl_->sockets_mu);
        impl_->active_socket = client;
      }
      impl_->handle(client);
      SOCKET to_close = INVALID_SOCKET;
      {
        std::lock_guard<std::mutex> lock(impl_->sockets_mu);
        if (impl_->active_socket == client) {
          impl_->active_socket = INVALID_SOCKET;
          to_close = client;
        }
      }
      if (to_close != INVALID_SOCKET) closesocket(to_close);
    }
  });
  return true;
}

void DummyFtp::stop() {
  if (!impl_->running.exchange(false) && impl_->listen_socket == INVALID_SOCKET) return;
  SOCKET listen_socket = INVALID_SOCKET;
  SOCKET active = INVALID_SOCKET;
  {
    std::lock_guard<std::mutex> lock(impl_->sockets_mu);
    listen_socket = impl_->listen_socket;
    impl_->listen_socket = INVALID_SOCKET;
    active = impl_->active_socket;
    impl_->active_socket = INVALID_SOCKET;
  }
  if (listen_socket != INVALID_SOCKET) closesocket(listen_socket);
  if (active != INVALID_SOCKET) closesocket(active);
  if (impl_->worker.joinable()) impl_->worker.join();
}

std::uint16_t DummyFtp::port() const { return impl_->bound.load(); }

void DummyFtp::set_data_timeout_ms(int ms) { impl_->data_timeout_ms = ms; }

int run_dummy_ftp(std::uint16_t port, const std::string& root) {
  std::string error;
  DummyFtp server(root, port);
  if (!server.start(&error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "dummy ftp 127.0.0.1:" << server.port() << "\n根目录 " << std::filesystem::absolute(root).string()
            << "\n按回车停止\n";
  std::string line;
  std::getline(std::cin, line);
  server.stop();
  return 0;
}

}  // namespace panplay
