#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

// 连续写一个文件。没调用 finish 就销毁时，已写的数据保留，但不保证完整。
class FileWriter {
 public:
  virtual ~FileWriter() = default;
  virtual std::string write(const std::uint8_t* data, std::size_t size) = 0;
  virtual std::string finish() = 0;
};

// 安装写入目标。PC 上可以是本机目录，也可以是模拟 PS5 存放目录的 FTP。
class InstallSink {
 public:
  virtual ~InstallSink() = default;
  // 新建或覆盖文件。写入期间不能对同一个 sink 做别的操作。size 是预计写入的字节数，不知道时为 -1。
  virtual std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) = 0;
  virtual std::string ensure_dir(const std::string& path) = 0;
  // 文件不存在时返回 -1，且不设置 error。
  virtual std::int64_t file_size(const std::string& path, std::string* error) = 0;
  virtual std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) = 0;
  virtual std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) = 0;
  virtual std::string rename_path(const std::string& from, const std::string& to) = 0;
  // 文件不存在时返回 "missing"。
  virtual std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) = 0;
  // 列出目录下一层子目录名。目录不存在时返回空列表。
  virtual std::vector<std::string> list_children(const std::string& path, std::string* error) = 0;
  virtual std::string remove_file(const std::string& path) = 0;
  // 只删除空目录。
  virtual std::string remove_dir(const std::string& path) = 0;
  // 连同里面的文件和子目录一起删。目录不存在也算删掉了。
  virtual std::string remove_tree(const std::string& path) = 0;
};

struct FtpEndpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 21;
  std::string user = "anonymous";
  std::string password = "panplay";
};

std::unique_ptr<InstallSink> make_local_sink(std::string root);
std::unique_ptr<InstallSink> make_ftp_sink(FtpEndpoint endpoint, std::string* error);

int run_install_sink_self_test();

}  // namespace panplay
