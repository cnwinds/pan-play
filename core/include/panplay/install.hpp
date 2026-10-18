#pragma once

#include "panplay/drive.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/sink.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
  // 续装时核对上次装好的文件：核对过几个、上次一共装好几个。
  int files_checked = 0;
  int files_to_check = 0;
  // 失败原因在压缩包本身（比如是 PKG 安装包）时为 false，再装一次结果也一样。
  bool retryable = true;
  // 压缩包里是 PKG 安装包、盘游装不了时，是哪一组压缩包（archive_set_id）。
  // 是主游戏那组时，安装队列自动把这个版本记成有问题；是补丁时跳过那组还能装，不记。
  std::string install_package_set;
  // 意外停下（网络断开、主机休眠唤醒、存放位置连接断了），等一会儿再装多半能接着装，安装队列会自动继续。
  // 用户取消、压缩包有问题、字典超过上限、解压内存不够、网盘没登录这些，自动再装也一样，为 false。
  bool auto_resume = false;
};

struct InstallRequest {
  std::string source_dir;
  std::string dest_prefix = "/data/homebrew";
  std::int64_t stop_after_output_bytes = -1;
  // 扫描和逐个文件完成时回调。安装结束前就会调用，方便界面不必等到整包结束。
  std::function<void(const InstallResult&)> on_progress;
  const CancelToken* cancel = nullptr;
  PrefetchOptions prefetch;
  // 用户选了不装的压缩包组（按 archive_set_id）。是否已安装、有没有更新也只看没跳过的。
  std::set<std::string> skip_sets;
  // RAR / 7z 的字典超过这么多字节就拒绝安装，0 是不限制。
  std::uint64_t max_dictionary = 0;
};

struct Destination {
  bool ftp = false;
  std::string local_root;
  FtpEndpoint endpoint;
  std::string prefix = "/data/homebrew";
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
  // RAR 的续装点：这个文件写完后下一个文件头在第几卷（volume）、卷内第几字节（next），以及那一卷的大小。
  // 非固实 RAR 续装时直接从这里接着读，不用从第一卷走一遍。别的格式不记，volume 为 -1。
  int volume = -1;
  std::int64_t next = -1;
  std::int64_t volume_size = -1;
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
  // 每读到一个文件调用一次；already 表示上次已经装好，这次只核对后跳过。
  std::function<void(bool already)> on_entry;
  // 已读到整组分卷的第几个字节。
  std::function<void(std::int64_t archive_bytes)> on_bytes;
  // 字典超过这么多字节就在解压前停下，0 是不限制。
  std::uint64_t max_dictionary = 0;
  // 上次装到一半时已经建好的目录（带暂存前缀），从续装点接着读时用来补全条目清单。
  const std::vector<std::string>* finished_dirs = nullptr;
};

// 一遍读完整组分卷：全部解压到暂存区，同时把压缩包里的条目按原路径记到 listed。
// 没有单独列目录这一步，因为没有快速打开记录的 RAR 列目录要逐个文件跳着读，小文件多时比解压还慢。
std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const RarExtract& options, std::vector<FinishedFile>* done,
                                std::vector<ListedRarEntry>* listed);

struct SevenZipFile {
  // 已换成安全的相对路径，目录带结尾斜杠。
  std::string name;
  bool directory = false;
  // 符号链接：照常解码，但不装。
  bool link = false;
  bool has_crc = false;
  std::uint32_t crc = 0;
  std::uint64_t size = 0;
  // 数据在哪个块里；-1 表示没有数据（目录或空文件）。
  int folder = -1;
};

// 7z 的一个块：一段压缩数据解出一串首尾相接的文件。固实压缩时整包可能只有一块。
struct SevenZipFolder {
  // 压缩数据在整组分卷拼接后的位置。
  std::int64_t pack_offset = 0;
  std::uint64_t pack_size = 0;
  std::uint64_t unpack_size = 0;
  std::uint32_t method = 0;
  std::vector<std::uint8_t> props;
  // 解压后再过一遍的过滤器，0 表示没有。
  std::uint32_t filter = 0;
  std::vector<std::uint8_t> filter_props;
  bool has_crc = false;
  std::uint32_t crc = 0;
  // 块里依次是哪些文件（SevenZipArchive::files 的下标）。
  std::vector<std::size_t> files;
};

struct SevenZipArchive {
  std::vector<SevenZipFile> files;
  std::vector<SevenZipFolder> folders;
};

// 只读包头和包尾的目录。用了这一版不支持的压缩方法、加了密码、字典超过 max_dictionary 字节（0 不限制）时
// 直接报错，不开始下载。
std::string read_7z_archive(DrivePlugin& drive, const std::vector<DriveEntry>& parts, SevenZipArchive* archive,
                            std::uint64_t max_dictionary = 0);

struct SevenZipExtract {
  std::string staging_root;
  JournalWriter* journal = nullptr;
  std::int64_t* written = nullptr;
  std::int64_t stop_after_output_bytes = -1;
  const CancelToken* cancel = nullptr;
  PrefetchOptions prefetch;
  // 按 SevenZipArchive::files 的下标给出暂存区里的路径；空的不装。
  std::vector<std::string> targets;
  // 上次已装好的文件（暂存区路径 -> 大小和 CRC），这次只核对后跳过。
  const std::map<std::string, std::pair<std::int64_t, std::uint32_t>>* finished_before = nullptr;
  std::function<void(const std::vector<FinishedFile>& done)> on_file_done;
  std::function<void(bool already)> on_entry;
  std::function<void(std::int64_t archive_bytes)> on_bytes;
};

// 按块顺序边下载边解压：块里不需要的文件解码后丢掉，块里最后一个要装的文件解完就不再往下读。
std::string extract_7z_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                               const SevenZipArchive& archive, const SevenZipExtract& options,
                               std::vector<FinishedFile>* done);

InstallResult install_game(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request);
// 游戏已经装好，但网盘目录里（除去 skip_sets）的压缩包和装的时候不一样了（比如多了补丁）。没装好时返回 false。
bool install_outdated(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir,
                      const std::vector<ArchiveSet>& sets, const std::set<std::string>& skip_sets = {});
// 已装好的游戏按顺序叠了哪些压缩包组（archive_set_id）。没装好或旧版本装的返回空。
std::vector<std::string> installed_sets(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir);
// 删掉没装完的游戏留在暂存区的全部文件，之后只能从头装。已经装好的游戏不受影响。
std::string discard_install(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir);
// 从存放位置删掉这个游戏的整个文件夹，连同没装完时留下的暂存文件。
std::string uninstall_game(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir);
// 游戏装在存放位置下的哪个文件夹（Title ID，没有时用目录名）。目录名不能用作文件夹时为空。
std::string install_folder_name(const std::string& source_dir);
bool parse_destination(const std::string& text, Destination* destination, std::string* error);
std::unique_ptr<InstallSink> open_destination(const Destination& destination, std::string* error);

int run_scan_cli(DrivePlugin& drive, const std::string& dir);
int run_install_cli(DrivePlugin& drive, int argc, char** argv);
int run_install_pipeline_self_test();
int run_prefetch_bench();

}  // namespace panplay
