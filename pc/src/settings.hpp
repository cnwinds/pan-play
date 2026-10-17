#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 每个网盘插件各存一份，切换插件不会丢。
struct DriveSettings {
  std::string library_path = "/apps";
  std::map<std::string, std::string> values;
};

// RAR / 7z 解压时整个字典要放进内存，字典超过这个大小（MB）的压缩包不装。0 是不限制。
#if defined(PANPLAY_PS5)
constexpr std::uint64_t kDefaultMaxDictionaryMb = 512;
#else
constexpr std::uint64_t kDefaultMaxDictionaryMb = 4096;
#endif

struct AppSettings {
  std::string drive;
  std::map<std::string, DriveSettings> drives;
  // "local" 写本机文件夹，"ftp" 发到主机的 FTP。
  std::string target;
  std::string local_dir;
  std::string ftp_host;
  std::string ftp_port = "2121";
  std::string ftp_dir = "/data/homebrew";
  // 局域网里别的设备能不能打开界面，以及要不要访问口令。家里内网用，默认开着、不要口令。
  bool lan_access = true;
  bool require_code = false;
  std::uint64_t max_dictionary_mb = kDefaultMaxDictionaryMb;

  DriveSettings current_drive() const;
  std::string library_path() const { return current_drive().library_path; }
  // 交给 parse_destination 的地址，还没填好时为空。
  std::string dest() const;
  // 游戏装在存放位置下的这个目录里。
  std::string prefix() const;
};

// 本机能用的保存方式。PS5 上只有本机文件夹。
std::vector<std::string> target_modes();

AppSettings load_settings();
std::string save_settings(const AppSettings& settings);
std::filesystem::path settings_file_path();
std::string drive_session_path(const std::string& drive_id);

// 键是 Title ID，目录名里没有 Title ID 时用目录名。
std::map<std::string, std::string> load_name_overrides();
std::string save_name_override(const std::string& key, const std::string& name);

// 用户选了不装的压缩包组（archive_set_id），按游戏目录记。没选过时为空，即全装。
std::set<std::string> load_skipped_sets(const std::string& game_path);
std::string save_skipped_sets(const std::string& game_path, const std::set<std::string>& skipped);

// 没装完的游戏上次装到哪里，按游戏目录记。字节按压缩包算，和安装时的进度条一致。
struct InstallProgress {
  std::int64_t bytes_done = 0;
  std::int64_t bytes_total = 0;
  int files_done = 0;
};

std::optional<InstallProgress> load_install_progress(const std::string& game_path);
// 和已记下的取较大值：续装开头核对已装文件时进度会从头走一遍，不能把记录往回改。
void save_install_progress(const std::string& game_path, const InstallProgress& progress);
void forget_install_progress(const std::string& game_path);
