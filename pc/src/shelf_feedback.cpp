#include "shelf_feedback.hpp"

#include "review_sync.hpp"
#include "shelf_library.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// 盘架按这个位置读，改了要两边一起改。
const std::string kFeedbackDir = "/盘游/反馈";
constexpr auto kUploadDelay = std::chrono::seconds(3);
constexpr auto kRetryDelay = std::chrono::minutes(1);
constexpr auto kCheckEvery = std::chrono::minutes(30);
constexpr std::int64_t kKeepSeconds = 30 * 86400;

struct Item {
  std::string id;
  std::string title_id;
  std::string format;
  std::string surl;
  int plan_rev = 0;
  std::string kind;
  std::string detail;
  std::int64_t at = 0;
};

std::mutex mu;
std::condition_variable wake;
bool started = false;
bool loaded = false;
std::vector<Item> items;
bool upload_wanted = false;
Clock::time_point upload_at;

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::filesystem::path store_file() { return panplay::app_data_dir() / "shelf-feedback.json"; }

std::string new_id() {
  std::random_device device;
  std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
  static const char* hex = "0123456789abcdef";
  std::string id;
  for (int i = 0; i < 16; ++i) id += hex[engine() % 16];
  return id;
}

std::string items_json_locked() {
  std::string json = "[";
  for (const auto& item : items) {
    if (json.size() > 1) json += ",";
    json += "{\"id\":\"" + panplay::json_escape(item.id) + "\",\"title_id\":\"" + panplay::json_escape(item.title_id) +
            "\",\"format\":\"" + panplay::json_escape(item.format) + "\",\"surl\":\"" + panplay::json_escape(item.surl) +
            "\",\"plan_rev\":" + std::to_string(item.plan_rev) + ",\"kind\":\"" + panplay::json_escape(item.kind) +
            "\",\"detail\":\"" + panplay::json_escape(item.detail) + "\",\"at\":" + std::to_string(item.at) + "}";
  }
  return json + "]";
}

void load_locked() {
  if (loaded) return;
  loaded = true;
  std::ifstream in(store_file(), std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  const panplay::Json json = panplay::Json::parse(buffer.str(), nullptr);
  if (const panplay::Json* list = json.get("items")) {
    for (const auto& entry : list->array) {
      Item item{entry.str("id"), entry.str("title_id"), entry.str("format"), entry.str("surl"),
                static_cast<int>(entry.num("plan_rev")), entry.str("kind"), entry.str("detail"),
                static_cast<std::int64_t>(entry.num("at"))};
      if (!item.id.empty() && !item.surl.empty()) items.push_back(std::move(item));
    }
  }
}

void save_locked() {
  std::ofstream out(store_file(), std::ios::binary | std::ios::trunc);
  out << "{\"items\":" << items_json_locked() << "}";
}

// 盘架已经重新分析过的（资源库里这条链接的编排版本更高了）和太旧的去掉。返回有没有去掉。
bool prune_locked() {
  const auto library = shelf_library();
  const std::int64_t now = now_unix();
  const auto before = items.size();
  items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& item) {
                if (now - item.at > kKeepSeconds) return true;
                if (!library) return false;
                const ShelfGame* game = library->find(item.title_id);
                if (game == nullptr) return false;
                for (const auto& variant : game->variants) {
                  if (variant.surl == item.surl && variant.plan && variant.plan->rev > item.plan_rev) return true;
                }
                return false;
              }),
              items.end());
  return items.size() != before;
}

void sync_loop(const std::function<std::shared_ptr<panplay::DrivePlugin>()>& open_drive) {
  auto checked_at = Clock::now();
  while (true) {
    bool upload = false;
    {
      std::unique_lock<std::mutex> lock(mu);
      auto next = [&] { return upload_wanted ? std::min(upload_at, checked_at + kCheckEvery) : checked_at + kCheckEvery; };
      while (Clock::now() < next()) wake.wait_until(lock, next());
      load_locked();
      if (Clock::now() >= checked_at + kCheckEvery) {
        checked_at = Clock::now();
        if (prune_locked()) {
          save_locked();
          upload_wanted = true;
          upload_at = Clock::now();
        }
      }
      upload = upload_wanted && Clock::now() >= upload_at;
      if (upload) upload_wanted = false;
    }
    if (!upload) continue;
    const auto drive = open_drive();
    std::string document;
    {
      std::lock_guard<std::mutex> lock(mu);
      document = "{\"version\":1,\"device\":{\"id\":\"" + panplay::json_escape(sync_device_id()) + "\",\"name\":\"" +
                 panplay::json_escape(sync_device_name()) + "\"},\"updated_at\":" + std::to_string(now_unix()) +
                 ",\"items\":" + items_json_locked() + "}";
    }
    const std::string error = drive && drive->session_valid()
                                  ? drive->write_small_file(kFeedbackDir + "/" + sync_device_id() + ".json", document)
                                  : "百度未登录";
    if (error.empty()) {
      PANPLAY_DEBUG("给盘架的反馈已传到网盘");
    } else {
      panplay::log_warn("给盘架的反馈没能传到网盘，稍后再试：" + error);
      std::lock_guard<std::mutex> lock(mu);
      upload_wanted = true;
      upload_at = Clock::now() + kRetryDelay;
    }
  }
}

}  // namespace

void start_shelf_feedback_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    load_locked();
    // 启动时传一次：上次没传上去的接着传。
    upload_wanted = !items.empty();
    upload_at = Clock::now() + std::chrono::seconds(10);
  }
  std::thread([open_drive = std::move(open_drive)] { sync_loop(open_drive); }).detach();
}

bool report_to_shelf(const std::string& path, const std::string& kind, const std::string& detail) {
  const auto library = shelf_library();
  const std::string game_dir = panplay::game_dir_of(path);
  const std::string title_id = panplay::find_title_id(game_dir.substr(game_dir.find_last_of('/') + 1));
  const ShelfVariant* variant = library ? library->variant(title_id, panplay::variant_format(path)) : nullptr;
  if (variant == nullptr) return false;
  const int rev = variant->plan ? variant->plan->rev : 0;
  {
    std::lock_guard<std::mutex> lock(mu);
    load_locked();
    auto same = std::find_if(items.begin(), items.end(), [&](const Item& item) {
      return item.surl == variant->surl && item.kind == kind && item.plan_rev == rev;
    });
    if (same == items.end()) {
      items.push_back({new_id(), title_id, variant->format, variant->surl, rev, kind, detail.substr(0, 500), now_unix()});
    } else {
      same->detail = detail.substr(0, 500);
      same->at = now_unix();
    }
    save_locked();
    upload_wanted = true;
    upload_at = Clock::now() + kUploadDelay;
  }
  wake.notify_all();
  panplay::log_info("通知盘架重新分析 " + title_id + " " + variant->format + "（" + kind + "）：" + detail);
  return true;
}

bool shelf_feedback_pending(const std::string& path) {
  const auto library = shelf_library();
  const std::string game_dir = panplay::game_dir_of(path);
  const std::string title_id = panplay::find_title_id(game_dir.substr(game_dir.find_last_of('/') + 1));
  const ShelfVariant* variant = library ? library->variant(title_id, panplay::variant_format(path)) : nullptr;
  if (variant == nullptr) return false;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const int rev = variant->plan ? variant->plan->rev : 0;
  return std::any_of(items.begin(), items.end(), [&](const Item& item) { return item.surl == variant->surl && item.plan_rev >= rev; });
}
