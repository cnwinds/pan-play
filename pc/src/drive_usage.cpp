#include "drive_usage.hpp"

#include "review_sync.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

// 别的设备也按这个位置读写，改了要一起改。
const std::string kUsageDir = "/盘游/在用";
constexpr auto kUploadDelay = std::chrono::seconds(3);
constexpr auto kRetryDelay = std::chrono::minutes(1);
// 装的过程中每 5 分钟更新一次心跳；别的设备超过 15 分钟没更新的当它已经停了。
constexpr auto kHeartbeatEvery = std::chrono::minutes(5);
constexpr std::int64_t kHeartbeatStale = 15 * 60;
// 主机上装着哪些会变（装好、删掉），隔一阵子也传一次。
constexpr auto kRefreshEvery = std::chrono::hours(6);
constexpr std::int64_t kMaxFileBytes = 4 << 20;

std::mutex mu;
std::condition_variable wake;
bool started = false;
std::set<std::string> installing;
std::map<std::string, std::int64_t> used;
bool used_loaded = false;
bool upload_wanted = true;
Clock::time_point upload_at = Clock::now();
Clock::time_point uploaded_at;
std::function<std::set<std::string>()> installed_dirs;

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::filesystem::path used_file() { return panplay::app_data_dir() / "drive-usage.json"; }

void load_used_locked() {
  if (used_loaded) return;
  used_loaded = true;
  std::ifstream in(used_file(), std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  const panplay::Json json = panplay::Json::parse(buffer.str(), nullptr);
  if (const panplay::Json* map = json.get("used")) {
    for (const auto& [path, at] : map->object) used[path] = static_cast<std::int64_t>(at.number);
  }
}

std::string used_json_locked() {
  std::string json = "{";
  for (const auto& [path, at] : used) {
    if (json.size() > 1) json += ",";
    json += "\"" + panplay::json_escape(path) + "\":" + std::to_string(at);
  }
  return json + "}";
}

void save_used_locked() {
  std::ofstream out(used_file(), std::ios::binary | std::ios::trunc);
  out << "{\"used\":" << used_json_locked() << "}";
}

std::string own_document() {
  std::set<std::string> dirs = installed_dirs ? installed_dirs() : std::set<std::string>{};
  std::lock_guard<std::mutex> lock(mu);
  load_used_locked();
  const std::int64_t now = now_unix();
  std::string json = "{\"version\":1,\"device\":{\"id\":\"" + panplay::json_escape(sync_device_id()) + "\",\"name\":\"" +
                     panplay::json_escape(sync_device_name()) + "\"},\"updated_at\":" + std::to_string(now) +
                     ",\"installing\":[";
  bool first = true;
  for (const auto& path : installing) {
    json += std::string(first ? "" : ",") + "{\"path\":\"" + panplay::json_escape(path) + "\",\"heartbeat\":" +
            std::to_string(now) + "}";
    first = false;
  }
  json += "],\"installed\":[";
  first = true;
  for (const auto& dir : dirs) {
    json += std::string(first ? "" : ",") + "\"" + panplay::json_escape(dir) + "\"";
    first = false;
  }
  return json + "],\"used\":" + used_json_locked() + "}";
}

void merge(const panplay::Json& doc, DriveUsage& usage) {
  const std::int64_t now = now_unix();
  if (const panplay::Json* list = doc.get("installing")) {
    for (const auto& item : list->array) {
      if (now - static_cast<std::int64_t>(item.num("heartbeat")) <= kHeartbeatStale) usage.installing.insert(item.str("path"));
    }
  }
  if (const panplay::Json* list = doc.get("installed")) {
    for (const auto& item : list->array) {
      if (item.type == panplay::Json::Type::string) usage.installed.insert(item.text);
    }
  }
  if (const panplay::Json* map = doc.get("used")) {
    for (const auto& [path, at] : map->object) {
      auto& slot = usage.used[path];
      slot = std::max(slot, static_cast<std::int64_t>(at.number));
    }
  }
}

void mark_dirty() {
  {
    std::lock_guard<std::mutex> lock(mu);
    upload_wanted = true;
    upload_at = Clock::now() + kUploadDelay;
  }
  wake.notify_all();
}

void sync_loop(const std::function<std::shared_ptr<panplay::DrivePlugin>()>& open_drive) {
  std::this_thread::sleep_for(std::chrono::seconds(10));
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mu);
      auto now = Clock::now();
      const auto next = [&] {
        auto due = uploaded_at + (installing.empty() ? kRefreshEvery : kHeartbeatEvery);
        return upload_wanted ? std::min(upload_at, due) : due;
      };
      while (now < next()) {
        wake.wait_until(lock, next());
        now = Clock::now();
      }
      upload_wanted = false;
      uploaded_at = now;
    }
    const auto drive = open_drive();
    if (!drive || !drive->session_valid()) continue;
    const std::string error = drive->write_small_file(kUsageDir + "/" + sync_device_id() + ".json", own_document());
    if (!error.empty()) {
      PANPLAY_DEBUG("在用记录没能传到网盘，稍后再试：" + error);
      std::lock_guard<std::mutex> lock(mu);
      upload_wanted = true;
      upload_at = Clock::now() + kRetryDelay;
    }
  }
}

}  // namespace

void start_drive_usage_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive,
                            std::function<std::set<std::string>()> installed) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    installed_dirs = std::move(installed);
  }
  std::thread([open_drive = std::move(open_drive)] { sync_loop(open_drive); }).detach();
}

void usage_install_started(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(mu);
    installing.insert(path);
  }
  mark_dirty();
}

void usage_install_stopped(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(mu);
    installing.erase(path);
  }
  mark_dirty();
}

void usage_used(const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(mu);
    load_used_locked();
    used[path] = now_unix();
    save_used_locked();
  }
  mark_dirty();
}

DriveUsage read_drive_usage(panplay::DrivePlugin& drive, std::string* error) {
  DriveUsage usage;
  merge(panplay::Json::parse(own_document(), nullptr), usage);
  const std::string own_file = sync_device_id() + ".json";
  std::string list_error;
  const auto entries = drive.list(kUsageDir, &list_error);
  if (!list_error.empty()) {
    if (list_error.find("errno=-9") == std::string::npos && error) *error = list_error;
    return usage;
  }
  for (const auto& entry : entries) {
    if (entry.is_dir || entry.name == own_file || entry.size <= 0 || entry.size > kMaxFileBytes) continue;
    const panplay::ReadResult read = drive.read_at(entry.file_id, 0, static_cast<std::size_t>(entry.size));
    if (!read.error.empty()) {
      if (error) *error = "读 " + entry.name + " 失败：" + read.error;
      continue;
    }
    merge(panplay::Json::parse(std::string(read.bytes.begin(), read.bytes.end()), nullptr), usage);
  }
  return usage;
}
