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
  // 数据连接空闲多久就断开，默认 10 秒。自测里调短，模拟主机掐掉慢连接。
  void set_data_timeout_ms(int ms);
  // 上传结束后过多久才关文件、写出缓冲。自测里用来模拟主机磁盘慢，旧传输还没写完续传就开始了。
  void set_close_delay_ms(int ms);

 private:
  struct Impl;
  Impl* impl_;
};

int run_dummy_ftp(std::uint16_t port, const std::string& root);

}  // namespace panplay
