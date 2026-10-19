#include "panplay/speed_test.hpp"

#include "panplay/library.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <cstdio>

namespace panplay {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_between(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

std::string mb_per_second(double rate) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB/s", rate / (1 << 20));
  return text;
}

// 游戏库目录和前几个游戏目录里最大的文件，找到 1 GB 以上的就不再往下找。
// 文件太小的话，多条连接很快读到文件尾、要重连，重连的等待会算进测量时间。
// 中途被停下时返回 false、error 留空。
bool pick_file(DrivePlugin& drive, const std::string& library_path, const std::atomic<bool>& stop, DriveEntry* out,
               std::string* error) {
  if (library_path.empty()) {
    *error = "先填好游戏库目录并保存";
    return false;
  }
  const auto children = drive.list(library_path, error);
  if (!error->empty()) return false;
  const auto consider = [&](const std::vector<DriveEntry>& entries) {
    for (const auto& entry : entries) {
      if (!entry.is_dir && entry.size > out->size) *out = entry;
    }
  };
  consider(children);
  int looked = 0;
  for (const auto& child : children) {
    if (stop) return false;
    if (out->size >= (1ll << 30) || looked >= 6) break;
    if (!child.is_dir) continue;
    ++looked;
    std::string list_error;
    consider(drive.list(child.path.empty() ? join_storage_path(library_path, child.name) : child.path, &list_error));
  }
  if (stop) return false;
  if (out->size < (256ll << 20)) {
    *error = "游戏库里没找到够大的文件来测速";
    return false;
  }
  return true;
}

}  // namespace

SpeedTest::~SpeedTest() {
  stop();
  if (thread_.joinable()) thread_.join();
}

std::string SpeedTest::start(std::shared_ptr<DrivePlugin> drive, std::string library_path, SpeedTestOptions options) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return "正在测速";
  if (thread_.joinable()) thread_.join();
  running_ = true;
  stop_ = false;
  last_error_.clear();
  status_ = {};
  status_.phase = "finding";
  status_.levels = options.levels;
  thread_ = std::thread([this, drive, library_path, options] { run(drive, library_path, options); });
  return "";
}

void SpeedTest::stop() { request_stop(); }

// 在锁里置位，等在 cv_ 上的线程才不会错过这次唤醒。
void SpeedTest::request_stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
}

SpeedTestStatus SpeedTest::status() {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

bool SpeedTest::running() {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

std::int64_t SpeedTest::received() const {
  std::int64_t total = 0;
  for (const auto& worker : workers_) total += worker->received;
  return total;
}

int SpeedTest::connected() const {
  return static_cast<int>(
      std::count_if(workers_.begin(), workers_.end(), [](const auto& worker) { return worker->connected.load(); }));
}

void SpeedTest::refresh() {
  const auto now = Clock::now();
  const std::int64_t total = received();
  recent_.emplace_back(now, total);
  while (recent_.size() > 2 && now - recent_[1].first >= std::chrono::seconds(1)) recent_.erase(recent_.begin());
  const double span = seconds_between(recent_.front().first, now);
  std::lock_guard<std::mutex> lock(mu_);
  status_.current = span > 0.1 ? static_cast<double>(total - recent_.front().second) / span : 0;
  status_.bytes = total;
  status_.seconds = seconds_between(began_, now);
  if (status_.measuring.connections > 0) {
    const double measured = seconds_between(measure_from_, now);
    status_.measuring.rate = measured > 0.5 ? static_cast<double>(total - measure_mark_) / measured : status_.current;
  }
}

bool SpeedTest::wait_until(Clock::time_point deadline) {
  for (;;) {
    refresh();
    const auto now = Clock::now();
    if (stop_) return false;
    if (now >= deadline) return true;
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(200)), [&] { return stop_.load(); });
  }
}

// 每条连接从文件里不同的位置开始，没有终点地读到文件末尾，读完从头再来。出错或读不到数据就隔一会儿重连，直到测速结束。
void SpeedTest::read_loop(DrivePlugin& drive, const DriveEntry& file, Worker& worker, int index) {
  std::vector<std::uint8_t> buffer(256 << 10);
  std::int64_t offset = file.size / 8 * (index % 8) / (1 << 20) * (1 << 20);
  int failures = 0;
  while (!stop_) {
    worker.open_abort.reset();
    if (stop_) break;
    if (offset >= file.size) offset = 0;
    std::string error;
    std::unique_ptr<ByteStream> stream = drive.open_stream(file.file_id, offset, &error, &worker.open_abort);
    if (stream) {
      {
        std::lock_guard<std::mutex> lock(worker.stream_mu);
        if (stop_) break;
        worker.stream = stream.get();
      }
      std::int64_t got_here = 0;
      for (;;) {
        const std::size_t got = stream->read(buffer.data(), buffer.size(), &error);
        if (got == 0) break;
        failures = 0;
        worker.connected = true;
        worker.received += static_cast<std::int64_t>(got);
        offset += static_cast<std::int64_t>(got);
        got_here += static_cast<std::int64_t>(got);
      }
      {
        std::lock_guard<std::mutex> lock(worker.stream_mu);
        worker.stream = nullptr;
      }
      if (error.empty() && got_here > 0) continue;
      if (error.empty() && !stop_) error = "网盘没有返回数据";
    }
    if (stop_) break;
    {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = error.empty() ? "连不上网盘的下载节点" : error;
    }
    PANPLAY_DEBUG("测速连接 " + std::to_string(index + 1) + " 出错：" + error);
    failures = std::min(failures + 1, 6);
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(500 * failures), [&] { return stop_.load(); });
  }
}

void SpeedTest::run(std::shared_ptr<DrivePlugin> drive, std::string library_path, SpeedTestOptions options) {
  DriveEntry file;
  std::string error;
  if (pick_file(*drive, library_path, stop_, &file, &error)) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      status_.phase = "testing";
      status_.file_name = file.name;
      status_.file_size = file.size;
    }
    began_ = Clock::now();
    recent_.clear();
    for (std::size_t level = 0; level < options.levels.size() && !stop_; ++level) {
      const int want = options.levels[level];
      while (static_cast<int>(workers_.size()) < want) {
        workers_.push_back(std::make_unique<Worker>());
        Worker& worker = *workers_.back();
        const int index = static_cast<int>(workers_.size()) - 1;
        worker.thread = std::thread([this, drive, file, &worker, index] { read_loop(*drive, file, worker, index); });
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        status_.connections = want;
      }
      const auto connect_deadline = Clock::now() + std::chrono::milliseconds(options.connect_ms);
      const auto all_connected = [&] {
        return std::all_of(workers_.begin(), workers_.end(), [](const auto& worker) { return worker->connected.load(); });
      };
      while (!all_connected() && wait_until(std::min(connect_deadline, Clock::now() + std::chrono::milliseconds(100))) &&
             Clock::now() < connect_deadline) {
      }
      if (stop_) break;
      if (!workers_.front()->connected) {
        std::lock_guard<std::mutex> lock(mu_);
        error = last_error_.empty() ? "连不上网盘的下载节点" : last_error_;
        break;
      }
      if (level == 0) {
        std::lock_guard<std::mutex> lock(mu_);
        status_.first_byte_ms = seconds_between(began_, Clock::now()) * 1000;
      }
      if (!wait_until(Clock::now() + std::chrono::milliseconds(options.settle_ms))) break;
      measure_from_ = Clock::now();
      measure_mark_ = received();
      const int connected_now = connected();
      {
        std::lock_guard<std::mutex> lock(mu_);
        status_.measuring = {want, connected_now, 0};
      }
      if (!wait_until(measure_from_ + std::chrono::milliseconds(want == 1 ? options.single_ms : options.step_ms))) break;
      const double rate = static_cast<double>(received() - measure_mark_) / seconds_between(measure_from_, Clock::now());
      std::lock_guard<std::mutex> lock(mu_);
      status_.steps.push_back({want, connected_now, rate});
      status_.measuring = {};
    }
  }
  const bool stopped = stop_;
  // 停下时这一档已经量了够久就留下来；速度在收尾之前定格，不把等连接退出的时间算进去。
  SpeedTestStep partial;
  if (stopped && error.empty() && !workers_.empty()) {
    refresh();
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.measuring.connections > 0 && seconds_between(measure_from_, Clock::now()) >= 1.5) {
      partial = status_.measuring;
    }
  }
  request_stop();
  for (auto& worker : workers_) {
    worker->open_abort.abort();
    std::lock_guard<std::mutex> lock(worker->stream_mu);
    if (worker->stream != nullptr) worker->stream->abort();
  }
  for (auto& worker : workers_) {
    if (worker->thread.joinable()) worker->thread.join();
  }
  if (!workers_.empty()) refresh();
  workers_.clear();

  std::lock_guard<std::mutex> lock(mu_);
  if (stopped && error.empty()) {
    if (partial.connections > 0) status_.steps.push_back(partial);
    status_.ended = "stopped";
  }
  status_.measuring = {};
  status_.connections = 0;
  status_.current = 0;
  status_.error = error;
  status_.phase = error.empty() ? "done" : "error";
  std::string summary;
  for (const auto& step : status_.steps) {
    summary += (summary.empty() ? "" : "，") + std::to_string(step.connections) + " 条" +
               (step.connected < step.connections ? "（连上 " + std::to_string(step.connected) + " 条）" : "") + " " +
               mb_per_second(step.rate);
  }
  if (!error.empty()) {
    log_warn("测速没有完成：" + error + (summary.empty() ? "" : "（" + summary + "）"));
  } else if (status_.file_name.empty()) {
    log_info("测速在找文件时停下");
  } else {
    char first_byte[32];
    std::snprintf(first_byte, sizeof(first_byte), "%.2f", status_.first_byte_ms / 1000);
    log_info("测速 " + status_.file_name + "：" + (summary.empty() ? "没量到" : summary) + "，首字节 " + first_byte + " 秒" +
             (status_.ended == "stopped" ? "，中途停下" : ""));
  }
  running_ = false;
}

}  // namespace panplay
