#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace panplay {

// 只监听 127.0.0.1，用本机目录模拟 PS5 FTP 根。支持安装需要的 SIZE、STOR、APPE、MKD、RNFR/RNTO。
class DummyFtp {
 public:
  DummyFtp(std::filesystem::path root, std::uint16_t port);
  ~DummyFtp();

  DummyFtp(const DummyFtp&) = delete;
  DummyFtp& operator=(const DummyFtp&) = delete;

  bool start(std::string* error);
  void stop();
  std::uint16_t port() const;

 private:
  struct Impl;
  Impl* impl_;
};

int run_dummy_ftp(std::uint16_t port, const std::string& root);

}  // namespace panplay
