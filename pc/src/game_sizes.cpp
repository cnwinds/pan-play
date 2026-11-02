#include "game_sizes.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

// 网盘上的压缩包很少变，变了打开详情时也会更新；后台一天重量一次兜底。
constexpr std::int64_t kStaleSeconds = 24 * 60 * 60;
// 每量完一款游戏歇一下，别把百度的列目录接口打到限流。
constexpr auto kGap = std::chrono::milliseconds(300);
constexpr auto kNotifyEvery = std::chrono::seconds(3);
constexpr auto kRetryDelay = std::chrono::minutes(5);
constexpr int kMaxFailures = 3;

struct Entry {
  GameSize size;
  std::int64_t at = 0;
};

std::mutex mu;
std::condition_variable wake;
bool loaded = false;
bool dirty = false;
std::string cache_key;
std::map<std::string, Entry> entries;

bool started = false;
std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive;
std::function<void()> on_change;
// 列表最近一次要量的游戏目录；generation 每要求一次加一，后台发现变了就按新的来。
std::string wanted_key;
std::vector<std::string> wanted_dirs;
long long wanted_generation = 0;

std::filesystem::path cache_path() { return panplay::app_data_dir() / "game-sizes.json"; }

std::int64_t now_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

bool same_size(const GameSize& left, const GameSize& right) {
  if (left.bytes != right.bytes || left.variants.size() != right.variants.size()) return false;
  for (std::size_t i = 0; i < left.variants.size(); ++i) {
    if (left.variants[i].format != right.variants[i].format || left.variants[i].path != right.variants[i].path) return false;
  }
  return true;
}

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
    Entry entry;
    entry.size.bytes = static_cast<std::int64_t>(node.num("bytes"));
    entry.at = static_cast<std::int64_t>(node.num("at"));
    if (const panplay::Json* variants = node.get("variants")) {
      for (const auto& variant : variants->array) entry.size.variants.push_back({variant.str("format"), variant.str("path")});
    }
    entries[path] = std::move(entry);
  }
}

void save_locked() {
  using panplay::json_escape;
  dirty = false;
  std::string json = "{\"key\":\"" + json_escape(cache_key) + "\",\"games\":{";
  bool first = true;
  for (const auto& [path, entry] : entries) {
    if (!first) json += ",";
    first = false;
    json += "\"" + json_escape(path) + "\":{\"bytes\":" + std::to_string(entry.size.bytes) +
            ",\"at\":" + std::to_string(entry.at) + ",\"variants\":[";
    for (std::size_t i = 0; i < entry.size.variants.size(); ++i) {
      if (i > 0) json += ",";
      json += "{\"format\":\"" + json_escape(entry.size.variants[i].format) + "\",\"path\":\"" +
              json_escape(entry.size.variants[i].path) + "\"}";
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

// 换了网盘或游戏库目录：原来量的都不算数。
void use_key_locked(const std::string& key) {
  load_locked();
  if (cache_key == key) return;
  entries.clear();
  cache_key = key;
  dirty = true;
}

void flush() {
  std::lock_guard<std::mutex> lock(mu);
  if (dirty) save_locked();
}

bool stale_locked(const std::string& key, const std::string& dir) {
  if (cache_key != key) return true;
  const auto it = entries.find(dir);
  return it == entries.end() || now_seconds() - it->second.at >= kStaleSeconds;
}

// 量到的一款游戏。分了格式时游戏目录记各个变体，每个变体目录各记一项。
void record_locked(const std::string& dir, const std::vector<panplay::VariantSize>& sizes) {
  const std::int64_t at = now_seconds();
  Entry game;
  game.at = at;
  const bool split = !(sizes.size() == 1 && sizes.front().path == dir);
  for (const auto& size : sizes) {
    if (size.path == dir) game.size.bytes = size.bytes;
    if (!split) continue;
    game.size.variants.push_back({size.format, size.path});
    if (size.path != dir) entries[size.path] = Entry{GameSize{size.bytes, {}}, at};
  }
  entries[dir] = std::move(game);
  dirty = true;
}

// 返回 false 表示连着出错、这一轮没量完。
bool scan_round(const std::string& key, const std::vector<std::string>& dirs, long long generation) {
  const auto drive = open_drive();
  if (!drive || !drive->session_valid()) return true;
  int failures = 0;
  std::size_t measured = 0;
  bool news = false;
  auto notified = Clock::now();
  for (const auto& dir : dirs) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (wanted_generation != generation) break;
      if (!stale_locked(key, dir)) continue;
    }
    std::string error;
    const auto sizes = panplay::measure_game(*drive, dir, &error);
    if (!error.empty()) {
      PANPLAY_DEBUG("量 " + dir + " 的容量失败：" + error);
      if (++failures < kMaxFailures) continue;
      panplay::log_warn("后台量游戏容量连着出错，" + std::to_string(kRetryDelay.count()) + " 分钟后再试：" + error);
      flush();
      if (news) on_change();
      return false;
    }
    failures = 0;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (cache_key != key) break;
      record_locked(dir, sizes);
    }
    ++measured;
    news = true;
    if (Clock::now() - notified >= kNotifyEvery) {
      flush();
      on_change();
      notified = Clock::now();
      news = false;
    }
    std::this_thread::sleep_for(kGap);
  }
  flush();
  if (news) on_change();
  if (measured > 0) panplay::log_info("后台量好了 " + std::to_string(measured) + " 款游戏的容量");
  return true;
}

void scan_loop() {
  long long seen = 0;
  bool retry = false;
  while (true) {
    std::string key;
    std::vector<std::string> dirs;
    {
      std::unique_lock<std::mutex> lock(mu);
      // 上一轮连着出错时，过几分钟整轮再来（量过的会跳过）；其间列表又要求了就按新的来。
      if (retry) wake.wait_for(lock, kRetryDelay, [&] { return wanted_generation != seen; });
      else wake.wait(lock, [&] { return wanted_generation != seen; });
      seen = wanted_generation;
      key = wanted_key;
      dirs = wanted_dirs;
    }
    retry = !scan_round(key, dirs, seen);
  }
}

}  // namespace

void start_size_scan(std::function<std::shared_ptr<panplay::DrivePlugin>()> drive, std::function<void()> changed) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    open_drive = std::move(drive);
    on_change = std::move(changed);
  }
  std::thread(scan_loop).detach();
}

void want_game_sizes(const std::string& key, const std::vector<std::string>& game_dirs) {
  {
    std::lock_guard<std::mutex> lock(mu);
    use_key_locked(key);
    const std::set<std::string> listed(game_dirs.begin(), game_dirs.end());
    for (auto it = entries.begin(); it != entries.end();) {
      if (listed.count(panplay::game_dir_of(it->first)) != 0) {
        ++it;
        continue;
      }
      it = entries.erase(it);
      dirty = true;
    }
    if (dirty) save_locked();
    wanted_key = key;
    wanted_dirs = game_dirs;
    ++wanted_generation;
  }
  wake.notify_all();
}

std::optional<GameSize> cached_game_size(const std::string& key, const std::string& path) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (cache_key != key) return std::nullopt;
  const auto it = entries.find(path);
  if (it == entries.end()) return std::nullopt;
  return it->second.size;
}

bool store_game_size(const std::string& key, const std::string& path, const GameSize& size) {
  std::lock_guard<std::mutex> lock(mu);
  use_key_locked(key);
  Entry& entry = entries[path];
  const bool changed = entry.at == 0 || !same_size(entry.size, size);
  entry.size = size;
  entry.at = now_seconds();
  if (changed || dirty) save_locked();
  return changed;
}
