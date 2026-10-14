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
constexpr const char* kConsoleDir = "/data/homebrew";

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
      const panplay::DriveKind* kind = panplay::find_drive_kind(id);
      const panplay::Json* values = node.get("values");
      if (kind == nullptr || values == nullptr) continue;
      // 插件不再声明的参数（比如百度以前的 AppKey、SecretKey）不读进来，下次保存时就从文件里去掉了。
      for (const auto& field : kind->fields) {
        if (values->get(field.key) != nullptr) drive.values[field.key] = values->str(field.key);
      }
    }
  }
  settings->target = json.str("target");
  settings->local_dir = json.str("local_dir");
  settings->ftp_host = json.str("ftp_host");
  if (!json.str("ftp_port").empty()) settings->ftp_port = json.str("ftp_port");
  if (!json.str("ftp_dir").empty()) settings->ftp_dir = json.str("ftp_dir");
  settings->lan_access = json.boolean_or("lan_access", true);
  settings->require_code = json.boolean_or("require_code", false);
}

// 版本 1 只有百度一个插件；存放位置是一个地址加主机目录，本机目录也会再套一层主机目录。
void read_legacy(const panplay::Json& json, AppSettings* settings) {
  DriveSettings& baidu = settings->drives["baidu"];
  if (!json.str("library_path").empty()) baidu.library_path = json.str("library_path");
  const std::string dest = json.str("dest");
  // 旧版配置不写 prefix 时用的是当时的默认目录。
  const std::string prefix = json.str("prefix").empty() ? "/mnt/usb0/homebrew" : json.str("prefix");
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

namespace {

std::mutex progress_mu;

std::filesystem::path progress_path() { return app_dir() / "install-progress.json"; }

std::map<std::string, InstallProgress> read_progress_unlocked() {
  std::map<std::string, InstallProgress> records;
  const panplay::Json json = read_json(progress_path());
  const panplay::Json* games = json.get("games");
  if (games == nullptr) return records;
  for (const auto& [path, item] : games->object) {
    InstallProgress progress;
    progress.bytes_done = static_cast<std::int64_t>(item.num("bytes_done"));
    progress.bytes_total = static_cast<std::int64_t>(item.num("bytes_total"));
    progress.files_done = static_cast<int>(item.num("files_done"));
    records[path] = progress;
  }
  return records;
}

void write_progress_unlocked(const std::map<std::string, InstallProgress>& records) {
  std::string json = "{\"games\":{";
  bool first = true;
  for (const auto& [path, progress] : records) {
    if (!first) json += ",";
    first = false;
    json += "\"" + panplay::json_escape(path) + "\":{\"bytes_done\":" + std::to_string(progress.bytes_done) +
            ",\"bytes_total\":" + std::to_string(progress.bytes_total) +
            ",\"files_done\":" + std::to_string(progress.files_done) + "}";
  }
  json += "}}";
  std::error_code ec;
  std::filesystem::create_directories(app_dir(), ec);
  std::filesystem::path tmp = progress_path();
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return;
  }
  std::filesystem::rename(tmp, progress_path(), ec);
}

}  // namespace

std::optional<InstallProgress> load_install_progress(const std::string& game_path) {
  std::lock_guard<std::mutex> lock(progress_mu);
  const auto records = read_progress_unlocked();
  const auto it = records.find(game_path);
  if (it == records.end()) return std::nullopt;
  return it->second;
}

void save_install_progress(const std::string& game_path, const InstallProgress& progress) {
  if (progress.bytes_done <= 0 && progress.files_done <= 0) return;
  std::lock_guard<std::mutex> lock(progress_mu);
  auto records = read_progress_unlocked();
  InstallProgress& record = records[game_path];
  const InstallProgress before = record;
  record.bytes_done = std::max(record.bytes_done, progress.bytes_done);
  record.bytes_total = std::max(record.bytes_total, progress.bytes_total);
  record.files_done = std::max(record.files_done, progress.files_done);
  if (record.bytes_done == before.bytes_done && record.bytes_total == before.bytes_total &&
      record.files_done == before.files_done) {
    return;
  }
  write_progress_unlocked(records);
}

void forget_install_progress(const std::string& game_path) {
  std::lock_guard<std::mutex> lock(progress_mu);
  auto records = read_progress_unlocked();
  if (records.erase(game_path) == 0) return;
  write_progress_unlocked(records);
}

AppSettings load_settings() {
  AppSettings settings;
  const panplay::Json json = read_json(settings_file_path());
  if (json.num("version") >= 2) read_current(json, &settings);
  else read_legacy(json, &settings);

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
          "\",\"ftp_dir\":\"" + json_escape(settings.ftp_dir) + "\",\"lan_access\":" +
          (settings.lan_access ? "true" : "false") + ",\"require_code\":" + (settings.require_code ? "true" : "false") +
          "}";
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
