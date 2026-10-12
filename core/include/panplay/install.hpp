#pragma once

#include "panplay/drive.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/sink.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

struct InstallResult {
  std::string status;
  std::string message;
  int files_done = 0;
  int files_total = 0;
  // 按压缩包读到的位置算的进度。RAR 要解压完才知道有多少文件，安装中 files_total 可能一直是 0。
  std::int64_t bytes_done = 0;
  std::int64_t bytes_total = 0;
  // 失败原因在压缩包本身（比如是 PKG 安装包）时为 false，再装一次结果也一样。
  bool retryable = true;
};

struct InstallRequest {
  std::string source_dir;
  std::string dest_prefix = "/mnt/usb0/homebrew";
  std::int64_t stop_after_output_bytes = -1;
  // 扫描和逐个文件完成时回调。安装结束前就会调用，方便界面不必等到整包结束。
  std::function<void(const InstallResult&)> on_progress;
  const CancelToken* cancel = nullptr;
  PrefetchOptions prefetch;
};

struct Destination {
  bool ftp = false;
  std::string local_root;
  FtpEndpoint endpoint;
  std::string prefix = "/mnt/usb0/homebrew";
};

struct ListedRarEntry {
  std::string name;
  bool directory = false;
  bool solid = false;
  std::uint32_t crc = 0;
  std::uint64_t size = 0;
};

struct FinishedFile {
  std::string name;
  std::int64_t size = 0;
  std::uint32_t crc = 0;
};

class JournalWriter;

struct RarExtract {
  std::string staging_root;
  JournalWriter* journal = nullptr;
  std::int64_t* written = nullptr;
  std::int64_t stop_after_output_bytes = -1;
  const CancelToken* cancel = nullptr;
  PrefetchOptions prefetch;
  // 解压到暂存区时加在压缩包内路径前面（带结尾斜杠）；进度日志里记的也是加了前缀的名字。
  std::string staging_prefix;
  std::function<void(const std::vector<FinishedFile>& done)> on_file_done;
  // 已读到整组分卷的第几个字节。
  std::function<void(std::int64_t archive_bytes)> on_bytes;
};

// 一遍读完整组分卷：全部解压到暂存区，同时把压缩包里的条目按原路径记到 listed。
// 没有单独列目录这一步，因为没有快速打开记录的 RAR 列目录要逐个文件跳着读，小文件多时比解压还慢。
std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const RarExtract& options, std::vector<FinishedFile>* done,
                                std::vector<ListedRarEntry>* listed);

InstallResult install_game(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request);
bool parse_destination(const std::string& text, Destination* destination, std::string* error);
std::unique_ptr<InstallSink> open_destination(const Destination& destination, std::string* error);

int run_scan_cli(DrivePlugin& drive, const std::string& dir);
int run_install_cli(DrivePlugin& drive, int argc, char** argv);
int run_install_pipeline_self_test();
int run_prefetch_bench();

}  // namespace panplay
