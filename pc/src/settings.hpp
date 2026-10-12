#pragma once

#include <filesystem>
#include <map>
#include <string>

struct AppSettings {
  std::string app_key;
  std::string secret_key;
  std::string library_path = "/apps";
  std::string dest;
  std::string prefix = "/mnt/usb0/homebrew";
};

AppSettings load_settings();
std::string save_settings(const AppSettings& settings);
std::filesystem::path settings_file_path();

// 键是 Title ID，目录名里没有 Title ID 时用目录名。
std::map<std::string, std::string> load_name_overrides();
std::string save_name_override(const std::string& key, const std::string& name);
