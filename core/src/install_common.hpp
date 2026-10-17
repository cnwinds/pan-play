#pragma once

#include "panplay/drive.hpp"
#include "panplay/install.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/sink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

inline constexpr const char* kInterrupted = "interrupted";

std::string parent_path(const std::string& path);

// 压缩包里的路径换成安全的相对路径：反斜杠换成斜杠，去掉 "." 和多余斜杠，目录保留结尾斜杠。
// 绝对路径、".."、带冒号或控制字符的返回 false。
bool safe_archive_name(const std::string& name, std::string* normalized, bool* directory);

// 一组分卷里的一卷。start 是它在整组拼接后的起点。
struct ArchiveVolume {
  std::string file_id;
  std::int64_t size = 0;
  std::int64_t start = 0;
};

std::vector<ArchiveVolume> archive_volumes(const std::vector<DriveEntry>& parts);

// 随机读一小段，可以跨卷。只用于读目录。
std::string read_span(DrivePlugin& drive, const std::vector<ArchiveVolume>& volumes, std::int64_t position, std::size_t size,
                      std::vector<std::uint8_t>* out);

// 在整组分卷上顺序读，每卷一个预读器。
class VolumeStream {
 public:
  VolumeStream(DrivePlugin& drive, std::vector<ArchiveVolume> volumes, const CancelToken* cancel, PrefetchOptions options);

  std::int64_t position() const { return position_; }
  std::string seek(std::int64_t position);
  // 读满 size 字节，读不满算出错。
  std::string read(std::uint8_t* dst, std::size_t size);
  std::string skip(std::int64_t size);

 private:
  void open_volume(std::size_t index);

  DrivePlugin& drive_;
  std::vector<ArchiveVolume> volumes_;
  const CancelToken* cancel_;
  PrefetchOptions options_;
  std::size_t index_ = 0;
  std::unique_ptr<PrefetchReader> reader_;
  std::int64_t position_ = 0;
};

// 索尼的 .pkg 安装包要在主机上用安装工具安装，解压到 homebrew 里主机和 ShadowMountPlus 都不认。
// 压缩包里没有 sce_sys、却有这么大的 .pkg，就当作安装包拒绝；小的 .pkg 可能是游戏自己的数据文件。
inline constexpr std::int64_t kInstallPackageMinBytes = 64ll << 20;
bool is_install_package(const std::string& name, std::int64_t size);
bool in_sce_sys(const std::string& name);
std::string install_package_error(const std::string& name);
bool is_install_package_error(const std::string& message);
// 7z 用了不支持的方法或加了密码：压缩包本身的问题，再装一次也一样。
bool is_unsupported_7z_error(const std::string& message);
// 字典超过设置里的上限时拒绝安装的说明。调高上限后可以重试，所以不算压缩包本身的问题。
std::string dictionary_refusal(const std::string& format, std::uint64_t dictionary, std::uint64_t limit);
bool is_dictionary_refusal(const std::string& message);

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
