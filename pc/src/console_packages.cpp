#include "console_packages.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"
#include "panplay/trace.hpp"

#if defined(PANPLAY_PS5)
#include "ps5_appinst.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

struct Listed {
  std::string name;
  std::string key;
  std::int64_t size = 0;
};

struct Result {
  std::string state;  // installed / failed
  std::string error;
  std::string content_id;
  std::int64_t at = 0;
};

std::mutex mu;
bool loaded = false;
std::map<std::string, std::vector<Listed>> games;
std::map<std::string, Result> results;

std::filesystem::path store_file() { return panplay::app_data_dir() / "console-packages.json"; }

std::string lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

// 按 md5 认包，同一个包在别的格式、别的游戏目录里也算装过。网盘没给 md5 时用名字和大小。
std::string key_of(const panplay::DriveEntry& entry) {
  return entry.content_hash.empty() ? entry.name + ":" + std::to_string(entry.size) : lower(entry.content_hash);
}

void load_locked() {
  if (loaded) return;
  loaded = true;
  std::ifstream in(store_file(), std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  const panplay::Json json = panplay::Json::parse(buffer.str(), nullptr);
  if (const panplay::Json* list = json.get("games")) {
    for (const auto& [path, items] : list->object) {
      for (const auto& item : items.array) {
        games[path].push_back({item.str("name"), item.str("key"), static_cast<std::int64_t>(item.num("size"))});
      }
    }
  }
  if (const panplay::Json* list = json.get("results")) {
    for (const auto& [key, item] : list->object) {
      results[key] = {item.str("state"), item.str("error"), item.str("content_id"), static_cast<std::int64_t>(item.num("at"))};
    }
  }
}

void save_locked() {
  using panplay::json_escape;
  std::string json = "{\"games\":{";
  bool first = true;
  for (const auto& [path, items] : games) {
    json += std::string(first ? "" : ",") + "\"" + json_escape(path) + "\":[";
    first = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
      json += std::string(i ? "," : "") + "{\"name\":\"" + json_escape(items[i].name) + "\",\"key\":\"" +
              json_escape(items[i].key) + "\",\"size\":" + std::to_string(items[i].size) + "}";
    }
    json += "]";
  }
  json += "},\"results\":{";
  first = true;
  for (const auto& [key, result] : results) {
    json += std::string(first ? "" : ",") + "\"" + json_escape(key) + "\":{\"state\":\"" + json_escape(result.state) +
            "\",\"error\":\"" + json_escape(result.error) + "\",\"content_id\":\"" + json_escape(result.content_id) +
            "\",\"at\":" + std::to_string(result.at) + "}";
    first = false;
  }
  std::ofstream out(store_file(), std::ios::binary | std::ios::trunc);
  out << json << "}}";
}

bool installed_locked(const std::string& key) {
  const auto found = results.find(key);
  return found != results.end() && found->second.state == "installed";
}

void remember(const std::string& key, const std::string& error, const std::string& content_id) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  results[key] = {error.empty() ? "installed" : "failed", error, content_id, static_cast<std::int64_t>(std::time(nullptr))};
  save_locked();
}

// 包配置里 <tag>…</tag> 的值，找不到为空。
std::string tag_value(const std::string& text, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const auto begin = text.find(open);
  if (begin == std::string::npos) return "";
  const auto end = text.find("</" + tag + ">", begin + open.size());
  if (end == std::string::npos || end - begin > 200) return "";
  return text.substr(begin + open.size(), end - begin - open.size());
}

}  // namespace

std::string check_unlock_package(const std::string& file, const std::string& title_id, std::string* content_id) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::u8path(file), ec);
  if (ec) return "读不到下载的包";
  if (static_cast<std::int64_t>(size) > kMaxConsolePackageBytes) return "包太大，不是激活、解锁用的小包";
  std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
  char magic[4] = {};
  if (!in.read(magic, sizeof(magic)) || std::string(magic, 4) != "\x7F" "FIH") return "不是 PS5 的 PKG";
  // 包配置是明文 XML，在包的后半段；一块块往后找，相邻两块留一段重叠，标签不会被切断。
  std::string type;
  std::string id;
  std::string window;
  std::string chunk(1 << 20, '\0');
  in.seekg(0);
  while (in && (type.empty() || id.empty())) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    window = window.substr(window.size() > 512 ? window.size() - 512 : 0) + chunk.substr(0, got);
    if (type.empty()) type = tag_value(window, "content-type");
    if (id.empty()) id = tag_value(window, "content-id");
  }
  if (type != "PS5AC") return type.empty() ? "包里找不到内容类型" : "不是附加内容包（类型 " + type + "）";
  if (id.find("-" + title_id + "_") == std::string::npos) return "包是别的游戏的（" + id + "）";
  *content_id = id;
  return "";
}

void note_console_packages(const std::string& path, const std::vector<panplay::DriveEntry>& packages) {
  std::vector<Listed> listed;
  for (const auto& entry : packages) listed.push_back({entry.name, key_of(entry), entry.size});
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  auto& current = games[path];
  const bool same = current.size() == listed.size() &&
                    std::equal(current.begin(), current.end(), listed.begin(),
                               [](const Listed& a, const Listed& b) { return a.key == b.key && a.name == b.name; });
  if (same && !listed.empty()) return;
  if (listed.empty()) {
    if (games.erase(path) == 0) return;
  } else {
    current = std::move(listed);
  }
  save_locked();
}

void forget_console_packages(const std::string& path) {
  const std::string game_dir = panplay::game_dir_of(path);
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  bool changed = false;
  for (const auto& [listed_path, items] : games) {
    if (panplay::game_dir_of(listed_path) != game_dir) continue;
    for (const auto& item : items) changed = results.erase(item.key) > 0 || changed;
  }
  if (changed) save_locked();
}

int pending_console_packages(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const auto found = games.find(path);
  if (found == games.end()) return 0;
  return static_cast<int>(std::count_if(found->second.begin(), found->second.end(),
                                        [](const Listed& item) { return !installed_locked(item.key); }));
}

std::string console_packages_json(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const auto found = games.find(path);
  if (found == games.end()) return "[]";
  std::string json = "[";
  for (const auto& item : found->second) {
    const auto result = results.find(item.key);
#if defined(PANPLAY_PS5)
    const std::string state = result == results.end() ? "pending" : result->second.state;
#else
    const std::string state = result == results.end() ? "manual" : result->second.state;
#endif
    json += std::string(json.size() > 1 ? "," : "") + "{\"name\":\"" + panplay::json_escape(item.name) +
            "\",\"size\":" + std::to_string(item.size) + ",\"state\":\"" + state + "\",\"error\":\"" +
            panplay::json_escape(result == results.end() ? "" : result->second.error) + "\"}";
  }
  return json + "]";
}

std::string install_console_packages(panplay::DrivePlugin& drive, const std::string& path, const std::string& title_id,
                                     const std::vector<panplay::DriveEntry>& packages,
                                     const std::function<void(const std::string&)>& progress) {
  note_console_packages(path, packages);
#if !defined(PANPLAY_PS5)
  (void)drive;
  (void)title_id;
  (void)progress;
  return packages.empty() ? "" : "电脑上的盘游装不了主机安装包，要在主机上的盘游里装";
#else
  std::string failures;
  const auto fail = [&](const panplay::DriveEntry& entry, const std::string& key, const std::string& error) {
    panplay::log_warn("主机上安装 " + entry.name + " 失败：" + error);
    remember(key, error, "");
    failures += (failures.empty() ? "" : "；") + entry.name + "：" + error;
  };
  for (std::size_t i = 0; i < packages.size(); ++i) {
    const panplay::DriveEntry& entry = packages[i];
    const std::string key = key_of(entry);
    {
      std::lock_guard<std::mutex> lock(mu);
      load_locked();
      if (installed_locked(key)) continue;
    }
    if (entry.size <= 0 || entry.size > kMaxConsolePackageBytes) {
      fail(entry, key, "包太大，不是激活、解锁用的小包");
      continue;
    }
    const std::string count = packages.size() > 1 ? "（" + std::to_string(i + 1) + "/" + std::to_string(packages.size()) + "）" : "";
    progress("正在下载要在主机上安装的 " + entry.name + count + "…");
    const std::filesystem::path dir = panplay::app_data_dir() / "packages";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (std::to_string(std::hash<std::string>{}(key)) + ".pkg");
    std::string error;
    {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      constexpr std::size_t kChunk = 4u << 20;
      for (std::int64_t offset = 0; offset < entry.size && error.empty();) {
        const auto length = static_cast<std::size_t>(std::min<std::int64_t>(kChunk, entry.size - offset));
        const panplay::ReadResult read = drive.read_at(entry.file_id, offset, length);
        if (!read.error.empty() || read.bytes.empty()) {
          error = read.error.empty() ? "从网盘下载失败" : "从网盘下载失败：" + read.error;
          break;
        }
        out.write(reinterpret_cast<const char*>(read.bytes.data()), static_cast<std::streamsize>(read.bytes.size()));
        offset += static_cast<std::int64_t>(read.bytes.size());
      }
      if (error.empty() && !out) error = "写不进数据目录";
    }
    std::string content_id;
    if (error.empty()) error = check_unlock_package(file.u8string(), title_id, &content_id);
    if (error.empty()) error = load_app_inst_util();
    if (error.empty()) {
      progress("正在主机上安装 " + entry.name + count + "…");
      std::string installing_id;
      error = with_shellcore_auth([&] { return install_package(file.u8string(), entry.name, &installing_id); });
      if (!installing_id.empty()) content_id = installing_id;
    }
    // 装是在后台进行的，查到 playable 才算装好。附加内容包很小，一般几秒钟。
    if (error.empty()) {
      std::string status;
      int status_failures = 0;
      for (int round = 0; round < 240; ++round) {
        int code = 0;
        const std::string query = with_shellcore_auth([&] { return package_install_status(content_id, &status, &code); });
        if (!query.empty()) {
          if (++status_failures >= 5) {
            panplay::log_warn("查不到 " + entry.name + " 的安装进度（" + query + "），当作已经装上");
            status = "playable";
            break;
          }
        } else if (status == "playable") {
          break;
        } else if (status == "error" || status == "none") {
          error = code != 0 ? installer_error_text(code) : "主机没有装上";
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      if (error.empty() && status != "playable") error = "两分钟还没装好（状态 " + status + "）";
    }
    std::filesystem::remove(file, ec);
    if (!error.empty()) {
      fail(entry, key, error);
      continue;
    }
    panplay::log_info("主机上装好了 " + entry.name + "（" + content_id + "）");
    remember(key, "", content_id);
  }
  return failures;
#endif
}
