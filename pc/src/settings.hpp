#pragma once

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
std::string settings_file_path();
