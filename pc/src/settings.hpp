#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// 每个网盘插件各存一份，切换插件不会丢。
struct DriveSettings {
  std::string library_path = "/apps";
  std::map<std::string, std::string> values;
};

struct AppSettings {
  std::string drive;
  std::map<std::string, DriveSettings> drives;
  // "local" 写本机文件夹，"ftp" 发到主机的 FTP。
  std::string target;
  std::string local_dir;
  std::string ftp_host;
  std::string ftp_port = "2121";
  std::string ftp_dir = "/mnt/usb0/homebrew";

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
