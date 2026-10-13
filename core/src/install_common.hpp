#pragma once

#include "panplay/install.hpp"
#include "panplay/sink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

inline constexpr const char* kInterrupted = "interrupted";

std::string parent_path(const std::string& path);

// 索尼的 .pkg 安装包要在主机上用安装工具安装，解压到 homebrew 里主机和 ShadowMountPlus 都不认。
// 压缩包里没有 sce_sys、却有这么大的 .pkg，就当作安装包拒绝；小的 .pkg 可能是游戏自己的数据文件。
inline constexpr std::int64_t kInstallPackageMinBytes = 64ll << 20;
bool is_install_package(const std::string& name, std::int64_t size);
bool in_sce_sys(const std::string& name);
std::string install_package_error(const std::string& name);
bool is_install_package_error(const std::string& message);

// 解压出的一个文件：第一次写入时才建目录、开写入流，同时算 CRC。
// stop >= 0 时，累计写满 stop 字节就返回 kInterrupted，供自测模拟中断。expected 是压缩包里记的解压后大小。
class OutputFile {
 public:
  OutputFile(InstallSink& sink, std::string path, std::int64_t expected, std::int64_t stop, std::int64_t* written);
  std::string write(const std::uint8_t* data, std::size_t size);
  // 空文件也会在这里建出来。
  std::string finish();
  std::uint32_t crc() const { return crc_; }
  std::int64_t size() const { return committed_; }

 private:
  std::string open();

  InstallSink& sink_;
  std::string path_;
  std::int64_t expected_;
  std::int64_t stop_;
  std::int64_t* written_;
  std::unique_ptr<FileWriter> writer_;
  std::uint32_t crc_;
  std::int64_t committed_ = 0;
};

// 进度日志每行一个已校验的文件。读不懂的最后一行是中断留下的半行，忽略；
// 中间有坏行时整份当作空的，已完成的文件会重新解压校验。
std::vector<FinishedFile> load_journal(InstallSink& sink, const std::string& path);
// 每次开始安装时整份重写一次，去掉上次中断留下的半行，之后只追加。
std::string write_journal(InstallSink& sink, const std::string& path, const std::vector<FinishedFile>& done);

// 往进度日志追加已校验的文件。在 FTP 上每次追加都要开一条数据连接，所以攒够一批
// 或隔一秒才写一次；析构时写掉剩下的。中断时最多丢最近一批，那几个文件续装时重新解压。
class JournalWriter {
 public:
  JournalWriter(InstallSink& sink, std::string path);
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  std::string add(const FinishedFile& file);
  std::string flush();

 private:
  static constexpr int kBatchFiles = 100;
  InstallSink& sink_;
  std::string path_;
  std::string pending_;
  int pending_count_ = 0;
  bool dir_ready_ = false;
  std::chrono::steady_clock::time_point last_flush_ = std::chrono::steady_clock::now();
};

}  // namespace panplay
