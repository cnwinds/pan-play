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

struct RarExtract {
  std::string staging_root;
  std::string journal_path;
  std::int64_t* written = nullptr;
  std::int64_t stop_after_output_bytes = -1;
  const CancelToken* cancel = nullptr;
  PrefetchOptions prefetch;
  // 只解压这个目录（带结尾斜杠）里的内容，并去掉这层前缀；为空表示全部原样解压。
  std::string game_root;
  std::function<void(const std::vector<FinishedFile>& done)> on_file_done;
};

std::string list_rar_entries(DrivePlugin& drive, const std::vector<DriveEntry>& parts, const CancelToken* cancel,
                             std::vector<ListedRarEntry>* entries);
std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const RarExtract& options, std::vector<FinishedFile>* done);

InstallResult install_game(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request);
bool parse_destination(const std::string& text, Destination* destination, std::string* error);
std::unique_ptr<InstallSink> open_destination(const Destination& destination, std::string* error);

int run_scan_cli(DrivePlugin& drive, const std::string& dir);
int run_install_cli(DrivePlugin& drive, int argc, char** argv);
int run_install_pipeline_self_test();
int run_prefetch_bench();

}  // namespace panplay
