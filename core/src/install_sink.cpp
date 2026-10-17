#include "panplay/sink.hpp"

#include "panplay/dummy_ftp.hpp"
#include "panplay/storage_path.hpp"
#include "panplay/socket_util.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
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

class LocalWriter final : public FileWriter {
 public:
  explicit LocalWriter(const std::filesystem::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
  bool ok() const { return static_cast<bool>(out_); }

  std::string write(const std::uint8_t* data, std::size_t size) override {
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out_ ? "" : "写入中断";
  }

  std::string finish() override {
    out_.close();
    return out_ ? "" : "写入中断";
  }

 private:
  std::ofstream out_;
};

class LocalSink : public InstallSink {
 public:
  explicit LocalSink(std::filesystem::path root) : root_(std::filesystem::absolute(std::move(root))) {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
  }

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t, std::string* error) override {
    std::filesystem::path local;
    if (!storage_path(root_, path, &local, error)) return nullptr;
    auto writer = std::make_unique<LocalWriter>(local);
    if (!writer->ok()) {
      *error = "无法写入";
      return nullptr;
    }
    return writer;
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

  std::string remove_tree(const std::string& path) override {
    std::filesystem::path local;
    std::string error;
    if (!storage_path(root_, path, &local, &error)) return error;
    const std::filesystem::path inside = local.lexically_normal().lexically_relative(root_.lexically_normal());
    if (inside.empty() || inside == "." || *inside.begin() == "..") return "不能删除存放位置以外或整个存放位置";
    return remove_recursive(local);
  }

 private:
  // 不用 remove_all：它删不掉时说不出是哪个文件、什么原因；新版 libc++ 里它还改用 openat/unlinkat 一类调用，
  // 主机上不一定都支持。这里只用列目录、删文件、删空目录。
  std::string remove_recursive(const std::filesystem::path& path) {
    const auto shown = [&] { return path.lexically_normal().lexically_relative(root_.lexically_normal()).u8string(); };
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) return "";
    if (ec) return "读不了 " + shown() + "：" + ec.message();
    if (std::filesystem::is_directory(status)) {
      std::vector<std::filesystem::path> children;
      for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        children.push_back(it->path());
      }
      if (ec) return "列不出 " + shown() + " 里的文件：" + ec.message();
      for (const auto& child : children) {
        if (auto error = remove_recursive(child); !error.empty()) return error;
      }
    }
    if (!std::filesystem::remove(path, ec) && ec) return "删不掉 " + shown() + "：" + ec.message();
    return "";
  }

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

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) override;

  // 主机答复 452/552 是存储空间不够，重试也没用。
  static bool out_of_space(int code) { return code == 452 || code == 552; }
  static std::string out_of_space_error(const std::string& reply) { return "主机存储空间不足（" + reply + "）"; }

  // 写入流结束时调用，读回传输结果。replied 表示收到了主机的答复，也就是主机已经关掉了文件。
  std::string end_upload(SOCKET data_socket, bool finished, bool* replied = nullptr, bool* fatal = nullptr) {
    shutdown(data_socket, SD_SEND);
    closesocket(data_socket);
    std::string reply;
    int code = 0;
    const bool got = reader_.read_reply(&reply, &code);
    if (replied != nullptr) *replied = got;
    if (!got) return "没有收到传输结果";
    if (out_of_space(code)) {
      if (fatal != nullptr) *fatal = true;
      return out_of_space_error(reply);
    }
    if (finished && code != 226) return reply;
    return "";
  }

  // 上传中途断了：问主机已经存下多少字节，再从那里续传。
  // 主机可能还在把断掉那次传输收到的数据写进文件，这时问到的大小偏小，续传会把文件写坏。
  // 所以先等主机答复断掉的那次传输（答复了说明文件已关闭）；控制连接也断了只能重连时，连问两次大小一致才算数。
  // fatal 表示不用再试了（主机空间不足）。append_only 时主机上即使没有这个文件也用 APPE。
  std::string resume_upload(const std::string& path, bool reply_pending, bool append_only, std::int64_t* stored,
                            SOCKET* data_socket, bool* fatal) {
    std::string error;
    std::string reply;
    int code = 0;
    bool settled = !reply_pending && control_ != INVALID_SOCKET;
    if (reply_pending && control_ != INVALID_SOCKET) settled = reader_.read_reply(&reply, &code);
    if (settled && out_of_space(code)) {
      *fatal = true;
      return out_of_space_error(reply);
    }
    if (!settled && !reconnect_unlocked(&error)) return error.empty() ? "FTP 重新连接失败" : error;
    std::int64_t previous = -1;
    for (int check = 0;; ++check) {
      if (!exchange("SIZE " + path, &reply, &code)) {
        settled = false;
        if (!reconnect_unlocked(&error)) return error.empty() ? "FTP 重新连接失败" : error;
        if (!exchange("SIZE " + path, &reply, &code)) return "FTP 控制连接中断";
      }
      if (code == 550) {
        *stored = 0;
      } else if (code == 213 && reply.find(' ') != std::string::npos) {
        *stored = std::stoll(reply.substr(reply.find(' ') + 1));
      } else {
        return reply;
      }
      if (settled || *stored == previous) break;
      if (check >= 10) return "主机上的文件一直在变，没法续传";
      previous = *stored;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return begin_upload(path, data_socket, *stored > 0 || append_only ? "APPE" : "STOR");
  }

  // 先删掉旧文件，之后只用 APPE 写（不存在时 APPE 会新建）。zftpd 上从头写的 STOR 中途断了会把半截文件删掉，
  // APPE 断了会留着，大文件才能从主机已存下的位置续传。
  std::string begin_append_upload(const std::string& path, SOCKET* data_socket) {
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip("DELE " + path, &reply, &code, &error)) return error;
    if (code != 250) {
      if (!roundtrip("SIZE " + path, &reply, &code, &error)) return error;
      if (code == 213) return "无法覆盖主机上的旧文件（" + reply + "）";
    }
    return begin_upload(path, data_socket, "APPE");
  }

  std::string begin_upload(const std::string& path, SOCKET* data_socket, const char* verb = "STOR") {
    std::string reply;
    int code = 0;
    std::string error;
    if (!roundtrip("PASV", &reply, &code, &error)) return error;
    if (code != 227) return reply;
    std::string host;
    std::uint16_t port = 0;
    if (!parse_pasv(reply, &host, &port)) return "无法解析 PASV";
    SOCKET socket = connect_tcp(host, port, &error);
    if (socket == INVALID_SOCKET) return error;
    if (!roundtrip(std::string(verb) + " " + path, &reply, &code, &error)) {
      closesocket(socket);
      return error;
    }
    if (code != 150 && code != 125) {
      closesocket(socket);
      return reply;
    }
    *data_socket = socket;
    return "";
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

  // FTP 列目录分不出文件和子目录：先当文件删，删不掉再当目录往里删。
  std::string remove_tree(const std::string& path) override {
    std::string error;
    const auto slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
    const auto siblings = list_children(parent, &error);
    if (!error.empty()) return error;
    if (std::find(siblings.begin(), siblings.end(), basename_of(path)) == siblings.end()) return "";
    return remove_tree_at(path, 0);
  }

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
  std::string remove_tree_at(const std::string& path, int depth) {
    if (depth > 64) return "目录层级太深";
    std::string error;
    const auto names = list_children(path, &error);
    if (!error.empty()) return error;
    for (const auto& name : names) {
      if (name.empty() || name == "." || name == "..") continue;
      const std::string child = (!path.empty() && path.back() == '/' ? path : path + "/") + name;
      if (remove_file(child).empty()) continue;
      if (auto child_error = remove_tree_at(child, depth + 1); !child_error.empty()) return child_error;
    }
    return remove_dir(path);
  }

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

// 一个文件一条数据连接。写入期间持有控制连接的锁，保活线程不会插入命令。
// 网盘读得慢时数据连接可能空闲太久被主机断开，所以留着最近发出的一段数据，断了就续传。
class FtpWriter final : public FileWriter {
 public:
  // 最近发出的数据留这么多，断了用来补发。比这长的文件断了只能从主机已存下的位置接着写，
  // 所以用 APPE 写，主机才不会删掉半截文件。
  static constexpr std::size_t kKeepBytes = 32u << 20;

  FtpWriter(FtpSink& sink, std::unique_lock<std::mutex> lock, std::string path, SOCKET data_socket, bool append_only)
      : sink_(sink), lock_(std::move(lock)), path_(std::move(path)), data_(data_socket), append_only_(append_only) {}

  ~FtpWriter() override {
    if (data_ != INVALID_SOCKET) sink_.end_upload(data_, false);
  }

  std::string write(const std::uint8_t* data, std::size_t size) override {
    if (finished_) return "写入已结束";
    if (size == 0) return "";
    remember(data, size);
    sent_ += static_cast<std::int64_t>(size);
    if (data_ != INVALID_SOCKET && send_all(data_, reinterpret_cast<const char*>(data), size)) return "";
    return recover();
  }

  std::string finish() override {
    if (finished_) return "写入已结束";
    for (int attempt = 0;; ++attempt) {
      std::string error = "发送数据失败";
      bool fatal = false;
      if (data_ != INVALID_SOCKET) {
        bool replied = false;
        error = sink_.end_upload(data_, true, &replied, &fatal);
        awaiting_reply_ = !replied;
      }
      data_ = INVALID_SOCKET;
      if (error.empty() || fatal || attempt >= kRetries) {
        finished_ = true;
        lock_.unlock();
        return error;
      }
      if (!(error = recover()).empty()) {
        finished_ = true;
        lock_.unlock();
        return error;
      }
    }
  }

 private:
  static constexpr int kRetries = 5;

  // 环形缓冲：文件第 pos 个字节放在 pos % kKeepBytes，小文件只占自己那么大。
  void remember(const std::uint8_t* data, std::size_t size) {
    std::int64_t pos = sent_;
    if (size > kKeepBytes) {
      pos += static_cast<std::int64_t>(size - kKeepBytes);
      data += size - kKeepBytes;
      size = kKeepBytes;
    }
    while (size > 0) {
      const auto at = static_cast<std::size_t>(pos % static_cast<std::int64_t>(kKeepBytes));
      const std::size_t n = std::min(size, kKeepBytes - at);
      if (ring_.size() < at + n) ring_.resize(at + n);
      std::memcpy(ring_.data() + at, data, n);
      data += n;
      pos += static_cast<std::int64_t>(n);
      size -= n;
    }
  }

  bool resend_from(std::int64_t pos) {
    while (pos < sent_) {
      const auto at = static_cast<std::size_t>(pos % static_cast<std::int64_t>(kKeepBytes));
      const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(sent_ - pos, static_cast<std::int64_t>(kKeepBytes - at)));
      if (!send_all(data_, reinterpret_cast<const char*>(ring_.data() + at), n)) return false;
      pos += static_cast<std::int64_t>(n);
    }
    return true;
  }

  // 主机存下的字节数要落在留着的这段里，才能把缺的部分补发过去。主机一时忙不过来时，隔一会儿再试，间隔逐次翻倍。
  std::string recover() {
    std::string error = "发送数据失败";
    const std::int64_t kept_from = std::max<std::int64_t>(0, sent_ - static_cast<std::int64_t>(kKeepBytes));
    for (int attempt = 0; attempt < kRetries; ++attempt) {
      if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
      if (data_ != INVALID_SOCKET) {
        closesocket(data_);
        data_ = INVALID_SOCKET;
      }
      std::int64_t stored = 0;
      SOCKET socket = INVALID_SOCKET;
      bool fatal = false;
      error = sink_.resume_upload(path_, awaiting_reply_, append_only_, &stored, &socket, &fatal);
      awaiting_reply_ = true;
      if (fatal) return error;
      if (!error.empty()) continue;
      data_ = socket;
      if (stored < kept_from || stored > sent_) return "发送数据失败，主机上的文件大小对不上";
      if (resend_from(stored)) return "";
      error = "发送数据失败";
    }
    return error;
  }

  FtpSink& sink_;
  std::unique_lock<std::mutex> lock_;
  std::string path_;
  SOCKET data_ = INVALID_SOCKET;
  std::int64_t sent_ = 0;
  std::vector<std::uint8_t> ring_;
  // 当前这次传输的结果主机还没答复（续传前要先读掉，确认主机已关掉文件）。
  bool awaiting_reply_ = true;
  bool finished_ = false;
  bool append_only_ = false;
};

// 小文件照旧一条 STOR：断了也能从留着的数据从头重发，不用多一次 DELE 往返。
std::unique_ptr<FileWriter> FtpSink::open_writer(const std::string& path, std::int64_t size, std::string* error) {
  std::unique_lock<std::mutex> lock(io_);
  SOCKET data_socket = INVALID_SOCKET;
  const bool append_only = size < 0 || size > static_cast<std::int64_t>(FtpWriter::kKeepBytes);
  *error = append_only ? begin_append_upload(path, &data_socket) : begin_upload(path, &data_socket);
  if (!error->empty()) return nullptr;
  return std::make_unique<FtpWriter>(*this, std::move(lock), path, data_socket, append_only);
}

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
  const std::string staging = "/data/homebrew/.panplay-staging/CUSA00001/eboot.bin";
  const std::string final_path = "/data/homebrew/CUSA00001/eboot.bin";
  const std::vector<std::uint8_t> first = pattern(10000, 1);
  const std::vector<std::uint8_t> second = pattern(30000, 9);
  std::vector<std::uint8_t> whole = first;
  whole.insert(whole.end(), second.begin(), second.end());

  std::string error = sink.ensure_dir("/data/homebrew/.panplay-staging/CUSA00001");
  if (!error.empty()) return fail(label, error);
  {
    auto writer = sink.open_writer(staging, static_cast<std::int64_t>(whole.size()), &error);
    if (!writer) return fail(label, error);
    error = writer->write(first.data(), first.size());
    if (error.empty()) error = writer->write(second.data(), second.size());
    if (error.empty()) error = writer->finish();
    if (!error.empty()) return fail(label, error);
  }
  if (sink.file_size(staging, &error) != static_cast<std::int64_t>(whole.size()) || !error.empty()) {
    return fail(label, "连续写入后大小不对");
  }

  std::filesystem::path on_disk;
  if (!storage_path(disk_root, staging, &on_disk, &error)) return fail(label, error);
  if (read_file(on_disk) != whole) return fail(label, "连续写入后的文件内容不一致");

  // 不知道大小时按大文件写：先删旧文件再 APPE，旧内容不能留在前面。
  {
    auto writer = sink.open_writer(staging, -1, &error);
    if (!writer) return fail(label, error);
    error = writer->write(first.data(), 10);
    if (!error.empty()) return fail(label, error);
  }
  if (sink.file_size(staging, &error) != 10 || !error.empty()) return fail(label, "没有 finish 的写入没有留下已写数据");

  error = sink.store(staging, whole.data(), whole.size());
  if (!error.empty()) return fail(label, error);
  if (read_file(on_disk) != whole) return fail(label, "覆盖后的文件内容不一致");

  error = sink.ensure_dir("/data/homebrew/CUSA00001");
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
  // zftpd 这类主机上的 FTP 不要密码，USER 就直接答 230。
  if (!send_line(control, "USER " + endpoint.user) || !reader->read_reply(&reply, &code) || (code != 331 && code != 230)) {
    *error = reply.empty() ? "登录用户名被拒绝" : reply;
    return false;
  }
  if (code == 331 &&
      (!send_line(control, "PASS " + endpoint.password) || !reader->read_reply(&reply, &code) || code != 230)) {
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
  for (const bool zftpd : {false, true}) {
    const std::string label = zftpd ? "ftp-zftpd" : "ftp";
    const std::filesystem::path ftp_root = root / label;
    std::string error;
    DummyFtp server(ftp_root, 0);
    server.set_zftpd_style(zftpd);
    if (!server.start(&error)) {
      std::cerr << error << "\n";
      return 1;
    }
    FtpEndpoint endpoint;
    endpoint.port = server.port();
    auto sink = make_ftp_sink(endpoint, &error);
    if (!sink) {
      std::cerr << label << ": " << error << "\n";
      return 1;
    }
    if (const int code = exercise(*sink, ftp_root, label)) return code;
    if (zftpd) continue;
    // dummy-ftp 的控制连接 10 秒收不到命令就会断开。读网盘期间必须靠 NOOP 撑过这段空闲。
    std::this_thread::sleep_for(std::chrono::seconds(12));
    std::string idle_error;
    if (sink->file_size("/no-such-file", &idle_error) != -1 || !idle_error.empty()) {
      std::cerr << "ftp: " << (idle_error.empty() ? "空闲后 SIZE 行为变了" : idle_error) << "\n";
      return 1;
    }
  }
  // 网盘读得慢时，主机会掐掉空闲的数据连接；写入方要重新连上，从主机已存下的位置续传。
  // 打开后先停 gap_first 毫秒，每段写完再停 700 毫秒，都比主机的数据超时长，会被掐断。
  // 返回写入的错误，内容不对时返回说明。
  const auto slow_write = [&](const std::string& label, bool zftpd, int close_delay, std::int64_t size_hint,
                              const std::vector<std::vector<std::uint8_t>>& parts, int gap_first) -> std::string {
    const std::filesystem::path ftp_root = root / label;
    std::string error;
    DummyFtp server(ftp_root, 0);
    server.set_data_timeout_ms(300);
    server.set_close_delay_ms(close_delay);
    server.set_zftpd_style(zftpd);
    if (!server.start(&error)) return error;
    FtpEndpoint endpoint;
    endpoint.port = server.port();
    auto sink = make_ftp_sink(endpoint, &error);
    if (!sink) return error;
    if (auto made = sink->ensure_dir("/slow"); !made.empty()) return made;
    std::vector<std::uint8_t> whole;
    auto writer = sink->open_writer("/slow/file.bin", size_hint, &error);
    if (!writer) return error;
    if (gap_first > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gap_first));
    for (const auto& part : parts) {
      if (error.empty()) error = writer->write(part.data(), part.size());
      whole.insert(whole.end(), part.begin(), part.end());
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    if (error.empty()) error = writer->finish();
    writer.reset();
    if (!error.empty()) return error;
    if (read_file(ftp_root / "slow" / "file.bin") != whole) return "续传后的文件内容不对";
    return "";
  };
  // 第二遍让主机断开后过一秒才把缓冲写进文件：续传要等它写完再问大小，否则会把文件写坏。
  // zftpd 会删掉断掉的 STOR 写了一半的文件，小文件要能从留着的数据从头重发。
  const std::vector<std::vector<std::uint8_t>> small = {pattern(10000, 3), pattern(200000, 4), pattern(30000, 5)};
  for (const bool zftpd : {false, true}) {
    for (const int close_delay : {0, 1000}) {
      const std::string label = std::string(zftpd ? "ftp-zftpd-idle-" : "ftp-idle-") + std::to_string(close_delay);
      if (auto error = slow_write(label, zftpd, close_delay, 240000, small, 700); !error.empty()) {
        std::cerr << label << ": 断开后没有续传: " << error << "\n";
        return 1;
      }
    }
  }
  // 比留着的数据长的文件：写过 32 MB 后被掐断。按大文件写（APPE）时 zftpd 留着半截，能接着写；
  // 当成小文件用 STOR 写时半截被删，接不上，说明上面那条确实是靠 APPE 才过的。
  const std::vector<std::vector<std::uint8_t>> large = {pattern(40u << 20, 6), pattern(1u << 20, 7)};
  const std::int64_t large_size = (41 << 20);
  if (auto error = slow_write("ftp-zftpd-large", true, 0, large_size, large, 0); !error.empty()) {
    std::cerr << "ftp-zftpd-large: 大文件断开后没有续传: " << error << "\n";
    return 1;
  }
  if (slow_write("ftp-zftpd-large-stor", true, 0, 1, large, 0).empty()) {
    std::cerr << "ftp-zftpd-large-stor: 模拟的 zftpd 没有删掉断掉的 STOR 文件\n";
    return 1;
  }
  return 0;
}

}  // namespace panplay
