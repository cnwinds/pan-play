#include "settings.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::filesystem::path app_dir() { return panplay::app_data_dir(); }

void read_keys(const std::filesystem::path& path, AppSettings* settings) {
  std::ifstream in(path);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const panplay::Json json = panplay::Json::parse(buffer.str(), nullptr);
  if (settings->app_key.empty()) settings->app_key = json.str("app_key");
  if (settings->secret_key.empty()) settings->secret_key = json.str("secret_key");
  if (json.str("library_path").size() > 0 && settings->library_path == "/apps") {
    settings->library_path = json.str("library_path");
  }
  if (settings->dest.empty()) settings->dest = json.str("dest");
  if (!json.str("prefix").empty()) settings->prefix = json.str("prefix");
}

}  // namespace

std::filesystem::path settings_file_path() { return app_dir() / "settings.json"; }

namespace {

std::mutex overrides_mu;

std::map<std::string, std::string> read_overrides_unlocked() {
  std::map<std::string, std::string> names;
  std::ifstream in(app_dir() / "name-overrides.json", std::ios::binary);
  if (!in) return names;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const panplay::Json json = panplay::Json::parse(buffer.str(), nullptr);
  const panplay::Json* items = json.get("names");
  if (items == nullptr) return names;
  for (const auto& [key, value] : items->object) {
    if (!value.text.empty()) names[key] = value.text;
  }
  return names;
}

}  // namespace

std::map<std::string, std::string> load_name_overrides() {
  std::lock_guard<std::mutex> lock(overrides_mu);
  return read_overrides_unlocked();
}

std::string save_name_override(const std::string& key, const std::string& name) {
  std::lock_guard<std::mutex> lock(overrides_mu);
  auto names = read_overrides_unlocked();
  if (name.empty()) names.erase(key);
  else names[key] = name;
  std::string json = "{\"names\":{";
  bool first = true;
  for (const auto& [k, v] : names) {
    if (!first) json += ",";
    first = false;
    json += "\"" + panplay::json_escape(k) + "\":\"" + panplay::json_escape(v) + "\"";
  }
  json += "}}";
  const std::filesystem::path path = app_dir() / "name-overrides.json";
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return "无法保存名称";
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return ec ? "无法保存名称" : "";
}

AppSettings load_settings() {
  AppSettings settings;
  read_keys(app_dir() / "settings.json", &settings);
  read_keys(std::filesystem::path("session") / "baidu-app.json", &settings);
  const std::string env_key = env_or_empty("PANPLAY_BAIDU_APP_KEY");
  const std::string env_secret = env_or_empty("PANPLAY_BAIDU_SECRET_KEY");
  if (!env_key.empty()) settings.app_key = env_key;
  if (!env_secret.empty()) settings.secret_key = env_secret;
  if (settings.prefix.empty()) settings.prefix = "/mnt/usb0/homebrew";
  if (settings.library_path.empty()) settings.library_path = "/apps";
  return settings;
}

std::string save_settings(const AppSettings& settings) {
  std::ofstream out(settings_file_path(), std::ios::trunc);
  if (!out) return "无法保存设置";
  out << "{\"app_key\":\"" << panplay::json_escape(settings.app_key) << "\",\"secret_key\":\""
      << panplay::json_escape(settings.secret_key) << "\",\"library_path\":\"" << panplay::json_escape(settings.library_path)
      << "\",\"dest\":\"" << panplay::json_escape(settings.dest) << "\",\"prefix\":\""
      << panplay::json_escape(settings.prefix) << "\"}";
  return out ? "" : "无法保存设置";
}
