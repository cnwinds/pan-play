#pragma once

#include "panplay/install.hpp"
#include "panplay/sink.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

inline constexpr const char* kInterrupted = "interrupted";

std::string parent_path(const std::string& path);

// 解压出的一个文件：第一次写入时才建目录、开写入流，同时算 CRC。
// stop >= 0 时，累计写满 stop 字节就返回 kInterrupted，供自测模拟中断。
class OutputFile {
 public:
  OutputFile(InstallSink& sink, std::string path, std::int64_t stop, std::int64_t* written);
  std::string write(const std::uint8_t* data, std::size_t size);
  // 空文件也会在这里建出来。
  std::string finish();
  std::uint32_t crc() const { return crc_; }
  std::int64_t size() const { return committed_; }

 private:
  std::string open();

  InstallSink& sink_;
  std::string path_;
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
std::string append_journal(InstallSink& sink, const std::string& path, const FinishedFile& file);

}  // namespace panplay
