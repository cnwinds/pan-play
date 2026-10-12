#include "panplay/sink.hpp"

#include "panplay/dummy_ftp.hpp"
#include "panplay/storage_path.hpp"
#include "socket_util.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace panplay {

bool login_ftp(SOCKET control, LineReader* reader, const FtpEndpoint& endpoint, std::string* error);

namespace {

class LocalSink : public InstallSink {
 public:
  explicit LocalSink(std::filesystem::path root) : root_(std::filesystem::absolute(std::move(root))) {
    std::filesystem::create_directories(root_);
  }

  std::string ensure_dir(const std::string& path) override {
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    std::filesystem::create_directories(local);
    if (!std::filesystem::is_directory(local)) return "无法创建目录";
    return "";
  }

  std::int64_t file_size(const std::string& path, std::string* error) override {
    std::filesystem::path local;
    if (!storage_path(root_, path, &local, error)) return -1;
    if (!std::filesystem::exists(local)) return -1;
    if (!std::filesystem::is_regular_file(local)) {
      *error = "不是文件";
      return -1;
    }
    return static_cast<std::int64_t>(std::filesystem::file_size(local));
  }

  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    return write_bytes(path, data, size, false);
  }

  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    return write_bytes(path, data, size, true);
  }

  std::string rename_path(const std::string& from, const std::string& to) override {
    std::filesystem::path source;
    std::filesystem::path target;
    std::string error;
    if (!storage_path(root_, from, &source, &error)) return error;
    if (!storage_path(root_, to, &target, &error)) return error;
    if (std::filesystem::exists(target)) return "目标已存在";
    try {
      std::filesystem::rename(source, target);
    } catch (const std::exception& ex) {
      return ex.what();
    }
    return "";
  }

  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    out->clear();
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    if (!std::filesystem::is_regular_file(local)) return "missing";
    const auto size = std::filesystem::file_size(local);
    if (size > max_bytes) return "文件过大";
    std::ifstream in(local, std::ios::binary);
    if (!in) return "无法读取";
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return "";
  }

  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    std::vector<std::string> names;
    std::filesystem::path local;
    if (!storage_path(root_, path, &local, error)) return names;
    if (!std::filesystem::exists(local)) return names;
    if (!std::filesystem::is_directory(local)) {
      *error = "不是目录";
      return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(local)) {
      if (entry.is_directory()) names.push_back(entry.path().filename().u8string());
    }
    return names;
  }

  std::string remove_file(const std::string& path) override {
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(local, ec) || !std::filesystem::remove(local, ec)) return "无法删除文件";
    return "";
  }

  std::string remove_dir(const std::string& path) override {
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    std::error_code ec;
    if (!std::filesystem::is_directory(local, ec) || !std::filesystem::is_empty(local, ec) ||
        !std::filesystem::remove(local, ec)) {
      return "无法删除目录";
    }
    return "";
  }

 private:
  std::string write_bytes(const std::string& path, const std::uint8_t* data, std::size_t size, bool append) {
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    std::ofstream out(local, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) return "无法写入";
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) return "写入中断";
    return "";
  }

  std::filesystem::path root_;
};

class FtpSink : public InstallSink {
 public:
  FtpSink(SOCKET control, LineReader reader, FtpEndpoint endpoint)
      : control_(control), reader_(std::move(reader)), endpoint_(std::move(endpoint)) {
    keepalive_ = std::thread([this] { keepalive_loop(); });
  }

  ~FtpSink() override {
    {
      std::lock_guard<std::mutex> lock(io_);
      stop_ = true;
    }
    cv_.notify_all();
    if (keepalive_.joinable()) keepalive_.join();
    if (control_ != INVALID_SOCKET) {
      send_line(control_, "QUIT");
      closesocket(control_);
      control_ = INVALID_SOCKET;
    }
  }

  std::string ensure_dir(const std::string& path) override {
    std::lock_guard<std::mutex> lock(io_);
    std::string current;
    std::size_t index = 0;
    while (index < path.size()) {
      while (index < path.size() && path[index] == '/') ++index;
      if (index >= path.size()) break;
      const std::size_t slash = path.find('/', index);
      const std::size_t end = slash == std::string::npos ? path.size() : slash;
      current += "/";
      current += path.substr(index, end - index);
      index = end;
      std::string reply;
      int code = 0;
      std::string error;
      if (!roundtrip("MKD " + current, &reply, &code, &error)) return error;
      if (code != 257) return reply;
    }
    return "";
  }

  std::int64_t file_size(const std::string& path, std::string* error) override {
    std::lock_guard<std::mutex> lock(io_);
    std::string reply;
    int code = 0;
    if (!roundtrip("SIZE " + path, &reply, &code, error)) return -1;
    if (code == 550) return -1;
    if (code != 213) {
      *error = reply;
      return -1;
    }
    const std::size_t space = reply.find(' ');
    if (space == std::string::npos) {
      *error = reply;
      return -1;
    }
    return std::stoll(reply.substr(space + 1));
  }

  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    std::lock_guard<std::mutex> lock(io_);
    return transfer("STOR", path, data, size);
  }

  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    std::lock_guard<std::mutex> lock(io_);
    return transfer("APPE", path, data, size);
  }

  std::string rename_path(const std::string& from, const std::string& to) override {
    std::lock_guard<std::mutex> lock(io_);
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip("RNFR " + from, &reply, &code, &error)) return error;
    if (code != 350) return reply;
    if (!roundtrip("RNTO " + to, &reply, &code, &error)) return error;
    if (code != 250) return reply;
    return "";
  }

  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    std::lock_guard<std::mutex> lock(io_);
    return fetch_names("NLST", path, error);
  }

  std::string remove_file(const std::string& path) override { return simple_command("DELE " + path); }

  std::string remove_dir(const std::string& path) override { return simple_command("RMD " + path); }

  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    std::lock_guard<std::mutex> lock(io_);
    out->clear();
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip("PASV", &reply, &code, &error)) return error;
    if (code != 227) return reply;
    std::string host;
    std::uint16_t port = 0;
    if (!parse_pasv(reply, &host, &port)) return "无法解析 PASV";
    SOCKET data_socket = connect_tcp(host, port, &error);
    if (data_socket == INVALID_SOCKET) return error;
    if (!roundtrip("RETR " + path, &reply, &code, &error)) {
      closesocket(data_socket);
      return error;
    }
    if (code == 550) {
      closesocket(data_socket);
      return "missing";
    }
    if (code != 150) {
      closesocket(data_socket);
      return reply;
    }
    while (out->size() <= max_bytes) {
      char buffer[8192];
      const int n = recv(data_socket, buffer, sizeof(buffer), 0);
      if (n == 0) break;
      if (n < 0) {
        closesocket(data_socket);
        return "读取中断";
      }
      out->insert(out->end(), buffer, buffer + n);
    }
    closesocket(data_socket);
    if (out->size() > max_bytes) return "文件过大";
    if (!reader_.read_reply(&reply, &code)) return "没有收到传输结果";
    if (code != 226) return reply;
    return "";
  }

 private:
  std::string simple_command(const std::string& line) {
    std::lock_guard<std::mutex> lock(io_);
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip(line, &reply, &code, &error)) return error;
    if (code != 250) return reply;
    return "";
  }

  bool exchange(const std::string& line, std::string* reply, int* code) {
    return send_line(control_, line) && reader_.read_reply(reply, code);
  }

  bool reconnect_unlocked(std::string* error) {
    if (control_ != INVALID_SOCKET) {
      closesocket(control_);
      control_ = INVALID_SOCKET;
    }
    SOCKET control = connect_tcp(endpoint_.host, endpoint_.port, error);
    if (control == INVALID_SOCKET) return false;
    LineReader reader;
    reader.socket = control;
    if (!login_ftp(control, &reader, endpoint_, error)) {
      closesocket(control);
      return false;
    }
    control_ = control;
    reader_ = std::move(reader);
    return true;
  }

  bool roundtrip(const std::string& line, std::string* reply, int* code, std::string* error) {
    if (line.find_first_of("\r\n") != std::string::npos) {
      *error = "FTP 路径含有换行符";
      return false;
    }
    if (exchange(line, reply, code)) return true;
    if (!reconnect_unlocked(error)) {
      if (error->empty()) *error = "FTP 控制连接中断";
      return false;
    }
    if (!exchange(line, reply, code)) {
      *error = "FTP 控制连接中断";
      return false;
    }
    return true;
  }

  void keepalive_loop() {
    std::unique_lock<std::mutex> lock(io_);
    while (!stop_) {
      if (cv_.wait_for(lock, std::chrono::seconds(3), [this] { return stop_; })) break;
      if (stop_ || control_ == INVALID_SOCKET) continue;
      std::string reply;
      int code = 0;
      std::string error;
      try {
        if (!exchange("NOOP", &reply, &code) || code >= 400) reconnect_unlocked(&error);
      } catch (const std::exception&) {
        reconnect_unlocked(&error);
      }
    }
  }

  static std::string basename_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  std::vector<std::string> fetch_names(const char* verb, const std::string& path, std::string* error) {
    std::vector<std::string> names;
    std::string reply;
    int code = 0;
    if (!roundtrip("PASV", &reply, &code, error)) return names;
    if (code != 227) {
      *error = reply;
      return names;
    }
    std::string host;
    std::uint16_t port = 0;
    if (!parse_pasv(reply, &host, &port)) {
      *error = "无法解析 PASV";
      return names;
    }
    SOCKET data_socket = connect_tcp(host, port, error);
    if (data_socket == INVALID_SOCKET) return names;
    if (!roundtrip(std::string(verb) + " " + path, &reply, &code, error)) {
      closesocket(data_socket);
      return names;
    }
    if (code == 550) {
      closesocket(data_socket);
      return names;
    }
    if (code != 150) {
      closesocket(data_socket);
      *error = reply;
      return names;
    }
    std::string body;
    for (;;) {
      char buffer[8192];
      const int n = recv(data_socket, buffer, sizeof(buffer), 0);
      if (n == 0) break;
      if (n < 0) {
        closesocket(data_socket);
        *error = "读取目录列表中断";
        return names;
      }
      body.append(buffer, buffer + n);
    }
    closesocket(data_socket);
    if (!reader_.read_reply(&reply, &code)) {
      *error = "没有收到目录列表结果";
      return names;
    }
    if (code != 226) {
      *error = reply;
      return names;
    }
    std::size_t start = 0;
    while (start < body.size()) {
      const std::size_t end = body.find_first_of("\r\n", start);
      const std::size_t line_end = end == std::string::npos ? body.size() : end;
      const std::string line = body.substr(start, line_end - start);
      if (!line.empty()) names.push_back(basename_of(line));
      if (end == std::string::npos) break;
      start = body.find_first_not_of("\r\n", end);
      if (start == std::string::npos) break;
    }
    return names;
  }

  std::string transfer(const char* verb, const std::string& path, const std::uint8_t* data, std::size_t size) {
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip("PASV", &reply, &code, &error)) return error;
    if (code != 227) return reply;
    std::string host;
    std::uint16_t port = 0;
    if (!parse_pasv(reply, &host, &port)) return "无法解析 PASV";
    SOCKET data_socket = connect_tcp(host, port, &error);
    if (data_socket == INVALID_SOCKET) return error;
    if (!roundtrip(std::string(verb) + " " + path, &reply, &code, &error)) {
      closesocket(data_socket);
      return error;
    }
    if (code != 150) {
      closesocket(data_socket);
      return reply;
    }
    const bool sent = size == 0 || send_all(data_socket, reinterpret_cast<const char*>(data), size);
    shutdown(data_socket, SD_SEND);
    if (!sent || !reader_.read_reply(&reply, &code)) {
      closesocket(data_socket);
      return sent ? "没有收到传输结果" : "发送数据失败";
    }
    closesocket(data_socket);
    if (code != 226) return reply;
    return "";
  }

  SOCKET control_ = INVALID_SOCKET;
  LineReader reader_;
  FtpEndpoint endpoint_;
  std::mutex io_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread keepalive_;
};

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(seed + i);
  return bytes;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int fail(const std::string& label, const std::string& message) {
  std::cerr << label << ": " << message << "\n";
  return 1;
}

int exercise(InstallSink& sink, const std::filesystem::path& disk_root, const std::string& label) {
  const std::string staging = "/mnt/usb0/homebrew/.panplay-staging/CUSA00001/eboot.bin";
  const std::string final_path = "/mnt/usb0/homebrew/CUSA00001/eboot.bin";
  const std::vector<std::uint8_t> first = pattern(10000, 1);
  const std::vector<std::uint8_t> second = pattern(30000, 9);
  std::vector<std::uint8_t> whole = first;
  whole.insert(whole.end(), second.begin(), second.end());

  std::string error = sink.ensure_dir("/mnt/usb0/homebrew/.panplay-staging/CUSA00001");
  if (!error.empty()) return fail(label, error);
  error = write_continuing(sink, staging, 0, first.data(), first.size());
  if (!error.empty()) return fail(label, error);
  if (sink.file_size(staging, &error) != 10000 || !error.empty()) return fail(label, "STOR 之后大小不是 10000");
  error = write_continuing(sink, staging, 10000, second.data(), second.size());
  if (!error.empty()) return fail(label, error);

  std::filesystem::path on_disk;
  if (!storage_path(disk_root, staging, &on_disk, &error)) return fail(label, error);
  if (read_file(on_disk) != whole) return fail(label, "续写后的文件内容不一致");

  {
    std::ofstream torn(on_disk, std::ios::binary | std::ios::app);
    const char extra[] = {'x', 'x', 'x', 'x', 'x'};
    torn.write(extra, 5);
  }
  error = write_continuing(sink, staging, static_cast<std::int64_t>(whole.size()), second.data(), 1);
  if (error.empty()) return fail(label, "远端大小和已提交字节不一致时仍然写入了");
  error.clear();
  if (sink.file_size(staging, &error) != static_cast<std::int64_t>(whole.size()) + 5 || !error.empty()) {
    return fail(label, "撕坏的文件大小变了");
  }

  error = sink.store(staging, whole.data(), whole.size());
  if (!error.empty()) return fail(label, error);
  if (read_file(on_disk) != whole) return fail(label, "覆盖后的文件内容不一致");

  error = sink.ensure_dir("/mnt/usb0/homebrew/CUSA00001");
  if (!error.empty()) return fail(label, error);
  error = sink.rename_path(staging, final_path);
  if (!error.empty()) return fail(label, error);
  if (sink.file_size(staging, &error) != -1 || !error.empty()) return fail(label, "暂存文件还在");
  std::filesystem::path final_disk;
  if (!storage_path(disk_root, final_path, &final_disk, &error)) return fail(label, error);
  if (read_file(final_disk) != whole) return fail(label, "搬到 PS5 扫描目录后内容不一致");
  return 0;
}

}  // namespace

std::string write_continuing(InstallSink& sink, const std::string& path, std::int64_t committed, const std::uint8_t* data,
                             std::size_t size) {
  std::string error;
  const std::int64_t remote = sink.file_size(path, &error);
  if (!error.empty()) return error;
  if (remote < 0) {
    if (committed != 0) return "远端没有该文件，但已提交字节不是 0";
    return sink.store(path, data, size);
  }
  if (remote != committed) {
    return "远端大小 " + std::to_string(remote) + " 与已提交 " + std::to_string(committed) + " 不一致，需要从头覆盖";
  }
  return sink.append(path, data, size);
}

std::unique_ptr<InstallSink> make_local_sink(std::string root) {
  return std::make_unique<LocalSink>(std::filesystem::u8path(root));
}

bool login_ftp(SOCKET control, LineReader* reader, const FtpEndpoint& endpoint, std::string* error) {
  std::string reply;
  int code = 0;
  if (!reader->read_reply(&reply, &code) || code != 220) {
    *error = reply.empty() ? "没有收到 FTP 欢迎语" : reply;
    return false;
  }
  if (!send_line(control, "USER " + endpoint.user) || !reader->read_reply(&reply, &code) || code != 331) {
    *error = reply.empty() ? "登录用户名被拒绝" : reply;
    return false;
  }
  if (!send_line(control, "PASS " + endpoint.password) || !reader->read_reply(&reply, &code) || code != 230) {
    *error = reply.empty() ? "登录被拒绝" : reply;
    return false;
  }
  if (!send_line(control, "TYPE I") || !reader->read_reply(&reply, &code) || code != 200) {
    *error = reply.empty() ? "无法切换到二进制" : reply;
    return false;
  }
  return true;
}

std::unique_ptr<InstallSink> make_ftp_sink(FtpEndpoint endpoint, std::string* error) {
  SOCKET control = connect_tcp(endpoint.host, endpoint.port, error);
  if (control == INVALID_SOCKET) return nullptr;
  LineReader reader;
  reader.socket = control;
  auto fail = [&](const std::string& message) {
    *error = message;
    closesocket(control);
    return std::unique_ptr<InstallSink>();
  };
  if (!login_ftp(control, &reader, endpoint, error)) return fail(error->empty() ? "FTP 登录失败" : *error);
  return std::make_unique<FtpSink>(control, std::move(reader), endpoint);
}

int run_install_sink_self_test() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "panplay-sink-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};

  {
    const std::filesystem::path local_root = root / "local";
    auto sink = make_local_sink(local_root.u8string());
    if (const int code = exercise(*sink, local_root, "local")) return code;
  }
  {
    const std::filesystem::path ftp_root = root / "ftp";
    std::string error;
    DummyFtp server(ftp_root, 0);
    if (!server.start(&error)) {
      std::cerr << error << "\n";
      return 1;
    }
    FtpEndpoint endpoint;
    endpoint.port = server.port();
    auto sink = make_ftp_sink(endpoint, &error);
    if (!sink) {
      std::cerr << error << "\n";
      return 1;
    }
    if (const int code = exercise(*sink, ftp_root, "ftp")) return code;
    // dummy-ftp 的控制连接 10 秒收不到命令就会断开。读网盘期间必须靠 NOOP 撑过这段空闲。
    std::this_thread::sleep_for(std::chrono::seconds(12));
    std::string idle_error;
    if (sink->file_size("/no-such-file", &idle_error) != -1 || !idle_error.empty()) {
      std::cerr << "ftp: " << (idle_error.empty() ? "空闲后 SIZE 行为变了" : idle_error) << "\n";
      return 1;
    }
  }
  return 0;
}

}  // namespace panplay
