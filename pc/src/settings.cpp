#include "settings.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/drive.hpp"
#include "panplay/json.hpp"
#include "panplay/storage_path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {

#if defined(PANPLAY_PS5)
constexpr const char* kDefaultTarget = "local";
#else
constexpr const char* kDefaultTarget = "ftp";
#endif
constexpr const char* kConsoleDir = "/mnt/usb0/homebrew";

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::filesystem::path app_dir() { return panplay::app_data_dir(); }

panplay::Json read_json(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return panplay::Json::parse(buffer.str(), nullptr);
}

void read_current(const panplay::Json& json, AppSettings* settings) {
  settings->drive = json.str("drive");
  if (const panplay::Json* drives = json.get("drives")) {
    for (const auto& [id, node] : drives->object) {
      DriveSettings& drive = settings->drives[id];
      if (!node.str("library_path").empty()) drive.library_path = node.str("library_path");
      if (const panplay::Json* values = node.get("values")) {
        for (const auto& [key, value] : values->object) drive.values[key] = value.text;
      }
    }
  }
  settings->target = json.str("target");
  settings->local_dir = json.str("local_dir");
  settings->ftp_host = json.str("ftp_host");
  if (!json.str("ftp_port").empty()) settings->ftp_port = json.str("ftp_port");
  if (!json.str("ftp_dir").empty()) settings->ftp_dir = json.str("ftp_dir");
}

// 版本 1 只有百度一个插件；存放位置是一个地址加主机目录，本机目录也会再套一层主机目录。
void read_legacy(const panplay::Json& json, AppSettings* settings) {
  DriveSettings& baidu = settings->drives["baidu"];
  baidu.values["app_key"] = json.str("app_key");
  baidu.values["secret_key"] = json.str("secret_key");
  if (!json.str("library_path").empty()) baidu.library_path = json.str("library_path");
  const std::string dest = json.str("dest");
  const std::string prefix = json.str("prefix").empty() ? kConsoleDir : json.str("prefix");
  if (dest.rfind("ftp://", 0) == 0) {
    settings->target = "ftp";
    std::string host_port = dest.substr(6);
    host_port = host_port.substr(0, host_port.find('/'));
    const auto colon = host_port.rfind(':');
    settings->ftp_host = host_port.substr(0, colon);
    settings->ftp_port = colon == std::string::npos ? "21" : host_port.substr(colon + 1);
    settings->ftp_dir = prefix;
    return;
  }
#if defined(PANPLAY_PS5)
  const std::string root = dest.empty() ? "/" : dest;
#else
  const std::string root = dest;
#endif
  if (root.empty()) return;
  std::filesystem::path full;
  std::string error;
  settings->target = "local";
  settings->local_dir = panplay::storage_path(std::filesystem::u8path(root), prefix, &full, &error) ? full.u8string() : root;
}

}  // namespace

DriveSettings AppSettings::current_drive() const {
  const auto it = drives.find(drive);
  return it == drives.end() ? DriveSettings{} : it->second;
}

std::string AppSettings::dest() const {
  if (target == "ftp") return ftp_host.empty() ? "" : "ftp://" + ftp_host + ":" + (ftp_port.empty() ? "21" : ftp_port);
  return local_dir;
}

std::string AppSettings::prefix() const {
  if (target == "ftp") return ftp_dir.empty() ? kConsoleDir : ftp_dir;
  return "/";
}

std::vector<std::string> target_modes() {
#if defined(PANPLAY_PS5)
  return {"local"};
#else
  return {"local", "ftp"};
#endif
}

std::filesystem::path settings_file_path() { return app_dir() / "settings.json"; }

std::string drive_session_path(const std::string& drive_id) {
  return (app_dir() / (drive_id + "-session.json")).u8string();
}

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

namespace {

std::mutex choices_mu;

std::filesystem::path choices_path() { return app_dir() / "layer-choices.json"; }

std::map<std::string, std::set<std::string>> read_choices_unlocked() {
  std::map<std::string, std::set<std::string>> choices;
  const panplay::Json json = read_json(choices_path());
  const panplay::Json* games = json.get("skipped");
  if (games == nullptr) return choices;
  for (const auto& [path, ids] : games->object) {
    for (const auto& id : ids.array) {
      if (!id.text.empty()) choices[path].insert(id.text);
    }
  }
  return choices;
}

}  // namespace

std::set<std::string> load_skipped_sets(const std::string& game_path) {
  std::lock_guard<std::mutex> lock(choices_mu);
  const auto choices = read_choices_unlocked();
  const auto it = choices.find(game_path);
  return it == choices.end() ? std::set<std::string>{} : it->second;
}

std::string save_skipped_sets(const std::string& game_path, const std::set<std::string>& skipped) {
  std::lock_guard<std::mutex> lock(choices_mu);
  auto choices = read_choices_unlocked();
  if (skipped.empty()) choices.erase(game_path);
  else choices[game_path] = skipped;
  std::string json = "{\"skipped\":{";
  bool first_game = true;
  for (const auto& [path, ids] : choices) {
    if (!first_game) json += ",";
    first_game = false;
    json += "\"" + panplay::json_escape(path) + "\":[";
    bool first_id = true;
    for (const auto& id : ids) {
      if (!first_id) json += ",";
      first_id = false;
      json += "\"" + panplay::json_escape(id) + "\"";
    }
    json += "]";
  }
  json += "}}";
  std::error_code ec;
  std::filesystem::create_directories(app_dir(), ec);
  std::filesystem::path tmp = choices_path();
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return "无法保存要装的补丁";
  }
  std::filesystem::rename(tmp, choices_path(), ec);
  return ec ? "无法保存要装的补丁" : "";
}

AppSettings load_settings() {
  AppSettings settings;
  const panplay::Json json = read_json(settings_file_path());
  if (json.num("version") >= 2) read_current(json, &settings);
  else read_legacy(json, &settings);

  DriveSettings& baidu = settings.drives["baidu"];
  const panplay::Json old_keys = read_json(std::filesystem::path("session") / "baidu-app.json");
  for (const char* key : {"app_key", "secret_key"}) {
    if (baidu.values[key].empty()) baidu.values[key] = old_keys.str(key);
  }
  const std::string env_key = env_or_empty("PANPLAY_BAIDU_APP_KEY");
  const std::string env_secret = env_or_empty("PANPLAY_BAIDU_SECRET_KEY");
  if (!env_key.empty()) baidu.values["app_key"] = env_key;
  if (!env_secret.empty()) baidu.values["secret_key"] = env_secret;

  if (panplay::find_drive_kind(settings.drive) == nullptr) settings.drive = panplay::drive_kinds().front().id;
  const auto modes = target_modes();
  if (std::find(modes.begin(), modes.end(), settings.target) == modes.end()) settings.target = kDefaultTarget;
#if defined(PANPLAY_PS5)
  if (settings.local_dir.empty()) settings.local_dir = kConsoleDir;
#endif
  return settings;
}

std::string save_settings(const AppSettings& settings) {
  using panplay::json_escape;
  std::string json = "{\"version\":2,\"drive\":\"" + json_escape(settings.drive) + "\",\"drives\":{";
  bool first_drive = true;
  for (const auto& [id, drive] : settings.drives) {
    if (!first_drive) json += ",";
    first_drive = false;
    json += "\"" + json_escape(id) + "\":{\"library_path\":\"" + json_escape(drive.library_path) + "\",\"values\":{";
    bool first_value = true;
    for (const auto& [key, value] : drive.values) {
      if (!first_value) json += ",";
      first_value = false;
      json += "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
    }
    json += "}}";
  }
  json += "},\"target\":\"" + json_escape(settings.target) + "\",\"local_dir\":\"" + json_escape(settings.local_dir) +
          "\",\"ftp_host\":\"" + json_escape(settings.ftp_host) + "\",\"ftp_port\":\"" + json_escape(settings.ftp_port) +
          "\",\"ftp_dir\":\"" + json_escape(settings.ftp_dir) + "\"}";
  const std::filesystem::path path = settings_file_path();
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return "无法保存设置";
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return ec ? "无法保存设置" : "";
}
