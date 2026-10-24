#include "detail_cache.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace {

std::mutex mu;
bool loaded = false;
std::string cache_key;
std::map<std::string, GameDetail> details;

std::filesystem::path cache_path() { return panplay::app_data_dir() / "game-details.json"; }

void load_locked() {
  if (loaded) return;
  loaded = true;
  std::ifstream in(cache_path(), std::ios::binary);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string error;
  const panplay::Json json = panplay::Json::parse(buffer.str(), &error);
  const panplay::Json* games = json.get("games");
  if (!error.empty() || games == nullptr) return;
  cache_key = json.str("key");
  for (const auto& [path, node] : games->object) {
    GameDetail detail;
    detail.state = node.str("state");
    detail.archive_bytes = static_cast<std::int64_t>(node.num("archive_bytes"));
    detail.formats = node.str("formats");
    detail.layers = node.str("layers");
    detail.fingerprint = node.str("fingerprint");
    detail.variants = node.str("variants");
    detail.installed_variant = node.str("installed_variant");
    if (const panplay::Json* installed = node.get("installed")) {
      for (const auto& id : installed->array) detail.installed.push_back(id.text);
    }
    if (!detail.state.empty()) details[path] = std::move(detail);
  }
}

void save_locked() {
  using panplay::json_escape;
  std::string json = "{\"key\":\"" + json_escape(cache_key) + "\",\"games\":{";
  bool first = true;
  for (const auto& [path, detail] : details) {
    if (!first) json += ",";
    first = false;
    json += "\"" + json_escape(path) + "\":{\"state\":\"" + json_escape(detail.state) +
            "\",\"archive_bytes\":" + std::to_string(detail.archive_bytes) + ",\"formats\":\"" +
            json_escape(detail.formats) + "\",\"layers\":\"" + json_escape(detail.layers) + "\",\"fingerprint\":\"" +
            json_escape(detail.fingerprint) + "\",\"variants\":\"" + json_escape(detail.variants) +
            "\",\"installed_variant\":\"" + json_escape(detail.installed_variant) + "\",\"installed\":[";
    for (std::size_t i = 0; i < detail.installed.size(); ++i) {
      if (i > 0) json += ",";
      json += "\"" + json_escape(detail.installed[i]) + "\"";
    }
    json += "]}";
  }
  json += "}}";
  std::error_code ec;
  std::filesystem::create_directories(cache_path().parent_path(), ec);
  std::filesystem::path tmp = cache_path();
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return;
  }
  std::filesystem::rename(tmp, cache_path(), ec);
}

}  // namespace

std::optional<GameDetail> cached_game_detail(const std::string& key, const std::string& path) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (key != cache_key) return std::nullopt;
  const auto it = details.find(path);
  if (it == details.end()) return std::nullopt;
  return it->second;
}

std::string cached_game_fingerprint(const std::string& key, const std::string& path) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (key != cache_key) return "";
  const auto it = details.find(path);
  return it == details.end() ? "" : it->second.fingerprint;
}

bool store_game_detail(const std::string& key, const std::string& path, const GameDetail& detail) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (key != cache_key) {
    details.clear();
    cache_key = key;
  }
  const auto it = details.find(path);
  if (it != details.end() && it->second == detail) return false;
  details[path] = detail;
  save_locked();
  return true;
}

void forget_game_detail(const std::string& path) {
  const std::string game_dir = panplay::game_dir_of(path);
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  bool erased = false;
  for (auto it = details.begin(); it != details.end();) {
    if (panplay::game_dir_of(it->first) != game_dir) {
      ++it;
      continue;
    }
    it = details.erase(it);
    erased = true;
  }
  if (erased) save_locked();
}
