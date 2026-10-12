#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

// 安装写入目标。PC 上可以是本机目录，也可以是模拟 PS5 存放目录的 FTP。
class InstallSink {
 public:
  virtual ~InstallSink() = default;
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
};

// 远端大小必须等于已提交字节才追加。不一致时不写入，调用方应从头 store。
std::string write_continuing(InstallSink& sink, const std::string& path, std::int64_t committed,
                             const std::uint8_t* data, std::size_t size);

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
