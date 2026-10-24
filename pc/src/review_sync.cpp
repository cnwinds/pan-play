#include "review_sync.hpp"

#include "reviews.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// 盘架也按这个位置读，改了要两边一起改。
const std::string kReviewDir = "/盘游/评价";
constexpr auto kUploadDelay = std::chrono::seconds(3);
constexpr auto kRetryDelay = std::chrono::minutes(1);
constexpr auto kRefreshEvery = std::chrono::minutes(10);
constexpr auto kStaleAfter = std::chrono::minutes(2);
constexpr std::int64_t kMaxFileBytes = 4 << 20;

std::mutex mu;
std::condition_variable wake;
bool started = false;
bool upload_wanted = true;  // 启动时也看一眼：上次没传完的接着传
Clock::time_point upload_at = Clock::now();
bool refresh_wanted = true;
Clock::time_point refreshed_at;
// 网盘上每个评价文件上次读到的 fs_id；覆盖写一次 fs_id 就变，没变的不重读。
std::map<std::string, std::string> seen_files;

std::string device_id() {
  const auto path = panplay::app_data_dir() / "device-id";
  {
    std::ifstream in(path, std::ios::binary);
    std::string id;
    if (in && std::getline(in, id) && id.size() >= 8) return id;
  }
  std::random_device device;
  std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
  static const char* hex = "0123456789abcdef";
  std::string id;
  for (int i = 0; i < 16; ++i) id += hex[engine() % 16];
  std::ofstream(path, std::ios::binary | std::ios::trunc) << id;
  return id;
}

std::string device_name() {
#if defined(PANPLAY_PS5)
  return "PS5";
#else
  for (const char* key : {"COMPUTERNAME", "HOSTNAME"}) {
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') return std::string("电脑 ") + value;
  }
  return "电脑";
#endif
}

// 返回出错原因；网盘上还没有这个目录不算错。
std::string pull(panplay::DrivePlugin& drive, const std::string& own_file) {
  std::string error;
  const auto entries = drive.list(kReviewDir, &error);
  if (!error.empty()) {
    if (error.find("errno=-9") == std::string::npos) return error;
    keep_devices({});
    return "";
  }
  std::vector<std::string> ids;
  for (const auto& entry : entries) {
    if (entry.is_dir || entry.name == own_file || entry.name.size() < 6 ||
        entry.name.compare(entry.name.size() - 5, 5, ".json") != 0) {
      continue;
    }
    const std::string id = entry.name.substr(0, entry.name.size() - 5);
    ids.push_back(id);
    {
      std::lock_guard<std::mutex> lock(mu);
      if (seen_files[entry.name] == entry.file_id) continue;
    }
    if (entry.size <= 0 || entry.size > kMaxFileBytes) continue;
    const panplay::ReadResult read = drive.read_at(entry.file_id, 0, static_cast<std::size_t>(entry.size));
    if (!read.error.empty()) {
      panplay::log_warn("读别的设备的评价 " + entry.name + " 失败：" + read.error);
      continue;
    }
    std::string name;
    std::string parse_error;
    const auto list = parse_reviews(std::string(read.bytes.begin(), read.bytes.end()), nullptr, &name, &parse_error);
    if (!parse_error.empty()) {
      panplay::log_warn("别的设备的评价 " + entry.name + " 读不懂：" + parse_error);
      continue;
    }
    set_device_reviews(id, name.empty() ? "另一台设备" : name, list);
    std::lock_guard<std::mutex> lock(mu);
    seen_files[entry.name] = entry.file_id;
    PANPLAY_DEBUG("读到 " + (name.empty() ? id : name) + " 的评价 " + std::to_string(list.size()) + " 条");
  }
  keep_devices(ids);
  return "";
}

void sync_loop(const std::function<std::shared_ptr<panplay::DrivePlugin>()>& open_drive) {
  const std::string id = device_id();
  const std::string name = device_name();
  const std::string own_file = id + ".json";
  // 断网（比如主机进了待机）时每 10 分钟都会失败，只在第一次记警告，恢复时再记一行。
  bool pull_failing = false;
  std::this_thread::sleep_for(std::chrono::seconds(5));
  while (true) {
    bool do_upload = false;
    bool do_refresh = false;
    {
      std::unique_lock<std::mutex> lock(mu);
      // 每次被叫醒都重算该等到什么时候：刚评价完要 3 秒后上传，不能还按原来的 10 分钟等。
      auto now = Clock::now();
      while (!(upload_wanted && now >= upload_at) && !refresh_wanted && now < refreshed_at + kRefreshEvery) {
        const auto next_refresh = refreshed_at + kRefreshEvery;
        wake.wait_until(lock, upload_wanted ? std::min(upload_at, next_refresh) : next_refresh);
        now = Clock::now();
      }
      do_upload = upload_wanted && now >= upload_at;
      do_refresh = refresh_wanted || now >= refreshed_at + kRefreshEvery;
      if (do_upload) upload_wanted = false;
      if (do_refresh) {
        refresh_wanted = false;
        refreshed_at = now;
      }
    }
    if (!do_upload && !do_refresh) continue;
    const auto drive = open_drive();
    if (!drive || !drive->session_valid()) {
      // 还没登录：过会儿再看，登录之后就能传上去。
      std::lock_guard<std::mutex> lock(mu);
      if (do_upload) {
        upload_wanted = true;
        upload_at = Clock::now() + kRetryDelay;
      }
      continue;
    }
    if (do_upload) {
      if (const auto upload = pending_review_upload(id, name)) {
        const std::string error = drive->write_small_file(kReviewDir + "/" + own_file, upload->document);
        if (error.empty()) {
          mark_reviews_uploaded(upload->revision);
          panplay::log_info("评价已同步到网盘 " + kReviewDir);
        } else {
          panplay::log_warn("评价没能同步到网盘，稍后再试：" + error);
          std::lock_guard<std::mutex> lock(mu);
          upload_wanted = true;
          upload_at = Clock::now() + kRetryDelay;
        }
      }
    }
    if (do_refresh) {
      if (const std::string error = pull(*drive, own_file); !error.empty()) {
        if (pull_failing) {
          PANPLAY_DEBUG("还是没能读到别的设备的评价：" + error);
        } else {
          panplay::log_warn("没能读到别的设备的评价：" + error + "（恢复之前不再重复记）");
        }
        pull_failing = true;
      } else if (pull_failing) {
        pull_failing = false;
        panplay::log_info("又能读到别的设备的评价了");
      }
    }
  }
}

}  // namespace

void start_review_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    refreshed_at = Clock::now();
  }
  std::thread([open_drive = std::move(open_drive)] { sync_loop(open_drive); }).detach();
}

void review_changed() {
  {
    std::lock_guard<std::mutex> lock(mu);
    upload_wanted = true;
    upload_at = Clock::now() + kUploadDelay;
  }
  wake.notify_all();
}

void refresh_reviews_if_stale() {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (refresh_wanted || Clock::now() - refreshed_at < kStaleAfter) return;
    refresh_wanted = true;
  }
  wake.notify_all();
}
