#include "panplay/download.hpp"

#include "panplay/abort.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include "big_memory.hpp"
#include "download_scheduler.hpp"
#include "install_clock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <thread>

namespace panplay {
namespace {

using Clock = std::chrono::steady_clock;

// 收到的数据按逻辑偏移放进 1 MB 一块的格子里；读过的至少留 1 MB，往回跳一小段（比如重读文件头）时不用重连。
constexpr std::int64_t kBlock = 1ll << 20;
constexpr std::int64_t kKeepBehind = 1ll << 20;
constexpr std::size_t kSpareBlocks = 16;
constexpr std::size_t kStep = 256u << 10;
constexpr double kQuietSeconds = 5;
constexpr std::int64_t kQuietBytes = 64ll << 20;
constexpr auto kTick = std::chrono::milliseconds(250);
constexpr double kSummarySeconds = 10;
// 每个会话开头这么多个请求逐个记，看得出连接是不是同时建起来的；首字节超过 1 秒的请求总是记。
constexpr int kDetailRequests = 4;
constexpr long long kSlowOpenMs = 1000;

// 各个会话共用一个时钟起点，记下的退避时间跨会话可比。
double now_seconds() {
  static const Clock::time_point origin = Clock::now();
  return std::chrono::duration<double>(Clock::now() - origin).count();
}

class ReadAtStream final : public ByteStream {
 public:
  ReadAtStream(DrivePlugin& drive, std::string file_id, std::int64_t offset)
      : drive_(drive), file_id_(std::move(file_id)), offset_(offset) {}

  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) override {
    const std::size_t want = std::min<std::size_t>(size, 1u << 20);
    ReadResult got = drive_.read_at(file_id_, offset_, want);
    if (!got.error.empty()) {
      *error = got.error;
      return 0;
    }
    if (got.bytes.size() > want) {
      *error = "网盘返回的数据比请求的多";
      return 0;
    }
    std::memcpy(dst, got.bytes.data(), got.bytes.size());
    offset_ += static_cast<std::int64_t>(got.bytes.size());
    return got.bytes.size();
  }

 private:
  DrivePlugin& drive_;
  std::string file_id_;
  std::int64_t offset_ = 0;
};

std::string fixed(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", value);
  return text;
}

std::string mb(std::int64_t bytes) { return fixed(static_cast<double>(bytes) / (1 << 20)) + " MB"; }
std::string mbps(double bytes, double seconds) {
  return seconds > 0 ? fixed(bytes / (1 << 20) / seconds) + " MB/s" : std::string("-");
}
std::string percent(double part, double whole) {
  return whole > 0 ? std::to_string(static_cast<int>(part * 100 / whole + 0.5)) + "%" : std::string("-");
}

int next_session_id() {
  static std::atomic<int> last{0};
  return ++last;
}

// 一段时间里的账：before 是这段开始时的累计数。
std::string describe(const DownloadScheduler::Totals& now, const DownloadScheduler::Totals& before) {
  const double elapsed = now.elapsed - before.elapsed;
  const auto received = static_cast<double>(now.received - before.received);
  const double busy = now.busy - before.busy;
  std::string text = "收到 " + mb(now.received - before.received) + "（" + mbps(received, elapsed);
  if (busy > 0) text += "，平均在下 " + fixed(busy / elapsed) + " 条，每条 " + mbps(received, busy);
  text += "），读走 " + mb(now.delivered - before.delivered) + "，读取方等数据 " + percent(now.wait - before.wait, elapsed) +
          "，缓冲满 " + percent(now.full - before.full, elapsed);
  const int requests = now.requests - before.requests;
  const int connects = now.connects - before.connects;
  text += "，请求 " + std::to_string(requests) + " 次";
  if (connects > 0) {
    text += "（首字节平均 " + std::to_string(static_cast<int>((now.first_byte - before.first_byte) * 1000 / connects)) +
            " ms，最长 " + std::to_string(static_cast<int>(now.first_byte_max * 1000)) + " ms，占在下时间 " +
            percent(now.first_byte - before.first_byte, now.leased - before.leased) + "）";
  }
  const int segments = now.segments - before.segments;
  if (segments > 0) {
    text += "，下完 " + std::to_string(segments) + " 段（平均 " +
            fixed((now.segment_time - before.segment_time) / segments) + " 秒，最长 " + fixed(now.segment_max) + " 秒）";
  }
  if (now.steals > before.steals) text += "，分走慢连接的后半段 " + std::to_string(now.steals - before.steals) + " 次";
  if (now.aborts > before.aborts) text += "，断开 " + std::to_string(now.aborts - before.aborts) + " 次";
  if (now.failures > before.failures) text += "，出错 " + std::to_string(now.failures - before.failures) + " 次";
  if (now.resets > before.resets) text += "，跳转重来 " + std::to_string(now.resets - before.resets) + " 次";
  if (now.discarded > before.discarded) text += "，丢弃 " + mb(now.discarded - before.discarded);
  if (now.skipped > before.skipped) text += "，跳过 " + mb(now.skipped - before.skipped);
  return text;
}

}  // namespace

std::unique_ptr<ByteStream> DrivePlugin::open_stream(const std::string& file_id, std::int64_t offset, std::string*,
                                                     AbortSignal*) {
  return std::make_unique<ReadAtStream>(*this, file_id, offset);
}

std::unique_ptr<ByteStream> DrivePlugin::open_range(const std::string& file_id, std::int64_t offset, std::int64_t,
                                                    std::string* error, AbortSignal* abort) {
  return open_stream(file_id, offset, error, abort);
}

std::string DrivePlugin::write_small_file(const std::string&, const std::string&) { return "这个网盘不支持写文件"; }

std::string DrivePlugin::remove_path(const std::string&, std::string* moved_to) {
  if (moved_to) moved_to->clear();
  return "这个网盘不支持删除文件";
}

ShareListing DrivePlugin::list_share(const std::string&, const std::string&) {
  ShareListing listing;
  listing.error = "这个网盘不支持分享链接";
  return listing;
}

ShareTransfer DrivePlugin::transfer_share(const std::string&, const std::string&, const std::string&) {
  ShareTransfer transfer;
  transfer.error = "这个网盘不支持转存分享";
  return transfer;
}

std::string DrivePlugin::move_path(const std::string&, const std::string&) { return "这个网盘不支持移动文件"; }

DriveQuota DrivePlugin::quota() {
  DriveQuota quota;
  quota.error = "这个网盘不支持查空间";
  return quota;
}

struct DownloadSession::Impl {
  struct Lane {
    std::thread thread;
    AbortSignal signal;
    ByteStream* stream = nullptr;
  };

  Impl(DrivePlugin& drive_in, std::vector<DownloadExtent> extents_in, const CancelToken* cancel_in, DownloadOptions options_in)
      : drive(drive_in), extents(std::move(extents_in)), cancel(cancel_in), options(std::move(options_in)), id(next_session_id()) {
    if (!options.memory) options.memory = std::make_shared<DownloadMemory>();
    options.max_connections = std::clamp(options.max_connections, 1, 16);
    for (const auto& extent : extents) {
      starts.push_back(size);
      size += std::max<std::int64_t>(0, extent.length);
    }
  }

  std::string tag() const { return "下载 #" + std::to_string(id) + "："; }

  std::int64_t window_cap() const {
    if (options.window_max > 0) return options.window_max;
#if defined(PANPLAY_PS5)
    return dictionary > 0 && dictionary <= (256ull << 20) ? 256ll << 20 : 128ll << 20;
#else
    return 512ll << 20;
#endif
  }

  void start_locked() {
    if (started) return;
    started = true;
    LaneMemory memory;
    {
      std::lock_guard<std::mutex> lock(options.memory->mu);
      memory.lanes = options.memory->lanes;
      memory.lane_rate = options.memory->lane_rate;
      memory.failed_from = options.memory->failed_from;
      memory.failed_total = options.memory->failed_total;
      memory.failures = options.memory->failures;
      memory.retry_at = options.memory->retry_at;
    }
    SchedulerOptions scheduler_options;
    scheduler_options.max_lanes = options.max_connections;
    scheduler_options.adaptive = options.adaptive;
    scheduler_options.window_cap = window_cap();
    scheduler_options.max_retries = options.max_retries;
    scheduler_options.retry_base = std::max(options.retry_base_ms, 1) / 1000.0;
    std::vector<std::int64_t> lengths;
    for (const auto& extent : extents) lengths.push_back(extent.length);
    scheduler = std::make_unique<DownloadScheduler>(lengths, scheduler_options, memory, position, now_seconds());
    memory_version = scheduler->memory_version();
    kept_from = position;
    summary_at = now_seconds();
    PANPLAY_DEBUG(tag() + std::to_string(extents.size()) + " 个文件共 " + mb(size) + "，从 @" + mb(position) + " 开始，" +
                  (options.adaptive ? "最多 " + std::to_string(options.max_connections) + " 条连接、从 " +
                                          std::to_string(scheduler->target_lanes()) + " 条开始（记下的每条 " +
                                          mbps(memory.lane_rate, 1) + "）"
                                    : "固定 " + std::to_string(scheduler->target_lanes()) + " 条连接") +
                  "，缓冲上限 " + mb(scheduler->window_cap()));
    spawn_locked();
    ticker = std::thread([this] { tick_main(); });
  }

  void spawn_locked() {
    while (static_cast<int>(lanes.size()) < scheduler->target_lanes()) {
      auto lane = std::make_unique<Lane>();
      Lane* raw = lane.get();
      const int index = static_cast<int>(lanes.size());
      lanes.push_back(std::move(lane));
      raw->thread = std::thread([this, index, raw] { lane_main(index, *raw); });
    }
  }

  // 每次调完决策都要处理它要求的事：断开连接、记日志、开新连接、把结论同步给同一次安装的其他会话。
  void after_scheduler_locked() {
    for (auto& event : scheduler->take_events()) {
      if (event.kind == DownloadScheduler::Event::Kind::abort) {
        if (event.lane >= 0 && event.lane < static_cast<int>(lanes.size())) {
          Lane& lane = *lanes[static_cast<std::size_t>(event.lane)];
          lane.signal.abort();
          if (lane.stream != nullptr) lane.stream->abort();
        }
      } else if (event.kind == DownloadScheduler::Event::Kind::info) {
        log_info(tag() + event.text);
      } else {
        PANPLAY_DEBUG(tag() + event.text);
      }
    }
    spawn_locked();
    if (scheduler->memory_version() != memory_version) {
      memory_version = scheduler->memory_version();
      const LaneMemory& memory = scheduler->memory();
      std::lock_guard<std::mutex> lock(options.memory->mu);
      options.memory->lanes = memory.lanes;
      options.memory->lane_rate = memory.lane_rate;
      options.memory->failed_from = memory.failed_from;
      options.memory->failed_total = memory.failed_total;
      options.memory->failures = memory.failures;
      options.memory->retry_at = memory.retry_at;
    }
  }

  void lane_main(int index, Lane& lane) {
    std::vector<std::uint8_t> scratch(kStep);
    std::unique_lock<std::mutex> lock(mu);
    try {
      while (!stop) {
        if (!fatal.empty() || (cancel != nullptr && cancel->cancelled())) {
          lane_cv.wait_for(lock, kTick);
          continue;
        }
        DownloadScheduler::Lease lease;
        double wake_at = 0;
        const double now = now_seconds();
        const auto step = scheduler->next(index, now, &lease, &wake_at);
        after_scheduler_locked();
        if (step != DownloadScheduler::Step::lease) {
          const double delay = std::clamp(wake_at - now, 0.01, 0.25);
          lane_cv.wait_for(lock, std::chrono::duration<double>(delay));
          continue;
        }
        run_lease_locked(index, lane, lease, lock, scratch);
      }
    } catch (const std::bad_alloc&) {
      if (!lock.owns_lock()) lock.lock();
      lane.stream = nullptr;
      // 开头和 is_out_of_memory_error 认的一样：安装按内存不够交给用户重试，不自动再装。
      if (fatal.empty()) fatal = "内存不够：下载缓冲要不到内存。关掉其他程序后重试";
      data_cv.notify_all();
    } catch (const std::exception& e) {
      if (!lock.owns_lock()) lock.lock();
      lane.stream = nullptr;
      if (fatal.empty()) fatal = std::string("下载出错: ") + e.what();
      data_cv.notify_all();
    }
  }

  // 一段一个请求：带终点的段只请求这段；不设终点的段（单连接）请求到这段所在文件的末尾，中途收尾时断开。
  void run_lease_locked(int index, Lane& lane, const DownloadScheduler::Lease& lease, std::unique_lock<std::mutex>& lock,
                        std::vector<std::uint8_t>& scratch) {
    const DownloadExtent& extent = extents[static_cast<std::size_t>(lease.extent)];
    const std::int64_t file_at = extent.offset + (lease.start - starts[static_cast<std::size_t>(lease.extent)]);
    const bool to_file_end = lease.open_ended && extent.offset + extent.length >= extent.file_size;
    const bool detailed = detail_left > 0;
    if (detailed) --detail_left;
    const std::string where = tag() + "连接 " + std::to_string(index) + " 段 @" + mb(lease.start);
    lane.signal.reset();
    lock.unlock();
    std::string error;
    const auto open_from = Clock::now();
    std::unique_ptr<ByteStream> stream =
        to_file_end ? drive.open_stream(extent.file_id, file_at, &error, &lane.signal)
                    : drive.open_range(extent.file_id, file_at, lease.end - lease.start, &error, &lane.signal);
    const long long open_ms = ms_since(open_from);
    lock.lock();
    if (!stream) {
      if (stop) return;
      if (error.empty()) error = "无法打开下载";
      PANPLAY_DEBUG(where + " 打开失败，" + std::to_string(open_ms) + " ms：" + error);
      scheduler->released(index, lease.id, &error, now_seconds());
      after_scheduler_locked();
      return;
    }
    scheduler->connected(index, lease.id, now_seconds());
    lane.stream = stream.get();
    if (lane.signal.aborted()) stream->abort();
    if (detailed || open_ms >= kSlowOpenMs) {
      PANPLAY_DEBUG(where + " 请求 " + (lease.open_ended ? std::string("不设终点") : mb(lease.end - lease.start)) +
                    "，首字节 " + std::to_string(open_ms) + " ms");
    }
    std::int64_t at = lease.start;
    std::string failure;
    while (!stop && !(cancel != nullptr && cancel->cancelled())) {
      const std::int64_t allowed = scheduler->allowance(index, lease.id, now_seconds());
      if (allowed < 0) break;
      if (allowed == 0) {
        lane_cv.wait_for(lock, kTick);
        continue;
      }
      const auto want = static_cast<std::size_t>(std::min<std::int64_t>(allowed, static_cast<std::int64_t>(scratch.size())));
      lock.unlock();
      std::string read_error;
      const std::size_t got = stream->read(scratch.data(), want, &read_error);
      lock.lock();
      if (got == 0) {
        failure = read_error.empty() ? "连接提前结束" : read_error;
        break;
      }
      // 先要到放数据的块：要不到时抛出去，调度还没把这些字节算成已到。
      reserve_locked(at, static_cast<std::int64_t>(got));
      const std::int64_t accepted = scheduler->received(index, lease.id, static_cast<std::int64_t>(got), now_seconds());
      if (accepted > 0) {
        store_locked(at, scratch.data(), accepted);
        if (reader_waiting) data_cv.notify_all();
      }
      at += static_cast<std::int64_t>(got);
      if (accepted < static_cast<std::int64_t>(got)) break;
    }
    lane.stream = nullptr;
    const bool failed = !failure.empty() && !stop;
    if (failed && !lane.signal.aborted()) PANPLAY_DEBUG(where + " 下到 @" + mb(at) + " 时断开：" + failure);
    scheduler->released(index, lease.id, failed ? &failure : nullptr, now_seconds());
    after_scheduler_locked();
    // 服务器还有没发完的数据时这条连接不能复用，断开。
    const bool finished = at >= lease.end;
    lock.unlock();
    if (!finished) stream->abort();
    stream.reset();
    lock.lock();
  }

  std::uint8_t* block_locked(std::int64_t index) {
    auto it = blocks.find(index);
    if (it != blocks.end()) return it->second;
    std::uint8_t* block = nullptr;
    if (!spare.empty()) {
      block = spare.back();
      spare.pop_back();
    } else {
      block = static_cast<std::uint8_t*>(big_alloc(static_cast<std::size_t>(kBlock)));
      if (block == nullptr) throw std::bad_alloc();
    }
    blocks[index] = block;
    return block;
  }

  void free_block_locked(std::uint8_t* block) {
    if (spare.size() < kSpareBlocks) {
      spare.push_back(block);
    } else {
      big_free(block);
    }
  }

  void reserve_locked(std::int64_t at, std::int64_t bytes) {
    const std::int64_t from = std::max(at, scheduler->position());
    for (std::int64_t index = from / kBlock; index * kBlock < at + bytes; ++index) block_locked(index);
  }

  // 读过的数据读取方用不上，不存。
  void store_locked(std::int64_t at, const std::uint8_t* data, std::int64_t bytes) {
    const std::int64_t floor = scheduler->position();
    if (at < floor) {
      const std::int64_t skip = std::min(bytes, floor - at);
      at += skip;
      data += skip;
      bytes -= skip;
    }
    while (bytes > 0) {
      const std::int64_t index = at / kBlock;
      const std::int64_t offset = at - index * kBlock;
      const std::int64_t part = std::min(bytes, kBlock - offset);
      std::memcpy(block_locked(index) + offset, data, static_cast<std::size_t>(part));
      at += part;
      data += part;
      bytes -= part;
    }
  }

  bool copy_out_locked(std::int64_t at, std::uint8_t* dst, std::int64_t bytes) {
    while (bytes > 0) {
      const std::int64_t index = at / kBlock;
      const std::int64_t offset = at - index * kBlock;
      const std::int64_t part = std::min(bytes, kBlock - offset);
      const auto it = blocks.find(index);
      if (it == blocks.end()) return false;
      std::memcpy(dst, it->second + offset, static_cast<std::size_t>(part));
      at += part;
      dst += part;
      bytes -= part;
    }
    return true;
  }

  // 读取方身后只留 1 MB；在正下载的范围再往前的块是跳转前留下的，也还掉。
  void prune_locked() {
    const std::int64_t keep_from = std::max(kept_from, scheduler->position() - kKeepBehind);
    const std::int64_t keep_to =
        scheduler->position() + std::max(scheduler->window_cap(), scheduler->buffered()) + kBlock;
    for (auto it = blocks.begin(); it != blocks.end();) {
      const std::int64_t start = it->first * kBlock;
      if (start + kBlock <= keep_from || start >= keep_to) {
        free_block_locked(it->second);
        it = blocks.erase(it);
      } else {
        ++it;
      }
    }
    kept_from = std::max(kept_from, keep_from / kBlock * kBlock);
  }

  void tick_main() {
    std::unique_lock<std::mutex> lock(mu);
    while (!stop) {
      tick_cv.wait_for(lock, kTick);
      if (stop) break;
      const double now = now_seconds();
      scheduler->tick(now);
      after_scheduler_locked();
      lane_cv.notify_all();
      if (now - summary_at >= kSummarySeconds) {
        summary_at = now;
        if (log_enabled(LogLevel::debug)) {
          const auto& totals = scheduler->totals();
          PANPLAY_DEBUG(tag() + "近 10 秒" + describe(totals, summary_from) + "，" +
                        std::to_string(scheduler->target_lanes()) + " 条连接，缓冲 " + mb(scheduler->buffered()) + " / " +
                        mb(scheduler->window()));
          summary_from = totals;
        }
      }
    }
  }

  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) {
    if (size == 0) return 0;
    PhaseScope timing(InstallPhase::download);
    std::unique_lock<std::mutex> lock(mu);
    if (position >= this->size) return 0;
    start_locked();
    const std::int64_t consumed = scheduler->position();
    if (position < consumed) {
      const auto n = std::min<std::int64_t>(static_cast<std::int64_t>(size), consumed - position);
      if (!copy_out_locked(position, dst, n)) {
        *error = "下载缓冲里少了读过的数据";
        return 0;
      }
      position += n;
      InstallClock::note_read(static_cast<std::size_t>(n));
      return static_cast<std::size_t>(n);
    }
    bool waited = false;
    const auto stop_waiting = [&] {
      if (!waited) return;
      reader_waiting = false;
      scheduler->set_waiting(false, now_seconds());
    };
    while (true) {
      const std::int64_t ready = scheduler->available();
      if (ready > position) {
        const auto n = std::min<std::int64_t>(static_cast<std::int64_t>(size), ready - position);
        if (!copy_out_locked(position, dst, n)) {
          stop_waiting();
          *error = "下载缓冲里少了收到的数据";
          return 0;
        }
        const std::int64_t before = position;
        position += n;
        stop_waiting();
        scheduler->consumed(position, now_seconds());
        if (position / kBlock != before / kBlock) {
          prune_locked();
          lane_cv.notify_all();
        }
        after_scheduler_locked();
        InstallClock::note_read(static_cast<std::size_t>(n));
        return static_cast<std::size_t>(n);
      }
      if (!fatal.empty() || !scheduler->error().empty()) {
        stop_waiting();
        *error = fatal.empty() ? scheduler->error() : fatal;
        return 0;
      }
      if (cancel != nullptr && cancel->cancelled()) {
        stop_waiting();
        *error = "interrupted";
        return 0;
      }
      if (!waited) {
        waited = true;
        reader_waiting = true;
        scheduler->set_waiting(true, now_seconds());
      }
      data_cv.wait_for(lock, std::chrono::milliseconds(100));
    }
  }

  std::string seek(std::int64_t to) {
    if (to < 0 || to > size) return "读取位置越界";
    std::lock_guard<std::mutex> lock(mu);
    if (!started) {
      position = to;
      return "";
    }
    if (to >= kept_from && to <= scheduler->position()) {
      position = to;
      return "";
    }
    // 往前跳过的这段已经下好时，跳转起点之前留着的数据仍然连续，往回跳一小段还能用。
    const bool contiguous = to > scheduler->position() && to <= scheduler->available();
    scheduler->seek(to, now_seconds());
    position = to;
    if (!contiguous) kept_from = to;
    prune_locked();
    after_scheduler_locked();
    lane_cv.notify_all();
    return "";
  }

  void set_dictionary(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mu);
    if (bytes <= dictionary) return;
    dictionary = bytes;
    if (!started) return;
    const std::int64_t cap = window_cap();
    if (cap == scheduler->window_cap()) return;
    PANPLAY_DEBUG(tag() + "字典 " + mb(static_cast<std::int64_t>(bytes)) + "，缓冲上限改为 " + mb(cap));
    scheduler->set_window_cap(cap, now_seconds());
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mu);
      stop = true;
      for (auto& lane : lanes) {
        lane->signal.abort();
        if (lane->stream != nullptr) lane->stream->abort();
      }
    }
    lane_cv.notify_all();
    data_cv.notify_all();
    tick_cv.notify_all();
    for (auto& lane : lanes) {
      if (lane->thread.joinable()) lane->thread.join();
    }
    if (ticker.joinable()) ticker.join();
    std::lock_guard<std::mutex> lock(mu);
    if (started) {
      scheduler->tick(now_seconds());
      after_scheduler_locked();
      const auto& totals = scheduler->totals();
      // 下了一阵的会话不开详细日志也记一行：看得出用了几条连接、首字节占多少、读取方等了多久。
      // 读目录、读小文件这种一眨眼的会话只在详细日志里记。
      const std::string line = tag() + "结束，共 " + fixed(totals.elapsed) + " 秒，" +
                               describe(totals, DownloadScheduler::Totals{}) + "，最后 " +
                               std::to_string(scheduler->target_lanes()) + " 条连接，缓冲上限 " + mb(scheduler->window_cap());
      if (totals.elapsed >= kQuietSeconds || totals.received >= kQuietBytes) {
        log_info(line);
      } else {
        PANPLAY_DEBUG(line);
      }
    }
    for (auto& [index, block] : blocks) big_free(block);
    blocks.clear();
    for (std::uint8_t* block : spare) big_free(block);
    spare.clear();
  }

  DrivePlugin& drive;
  std::vector<DownloadExtent> extents;
  std::vector<std::int64_t> starts;
  std::int64_t size = 0;
  const CancelToken* cancel;
  DownloadOptions options;
  const int id;
  std::uint64_t dictionary = 0;

  std::mutex mu;
  std::condition_variable data_cv;
  std::condition_variable lane_cv;
  std::condition_variable tick_cv;
  std::unique_ptr<DownloadScheduler> scheduler;
  bool started = false;
  bool stop = false;
  std::string fatal;
  // 读取方的位置。往回跳到还留着的数据里时比 scheduler 的位置小，读到那里之前不经过调度。
  std::int64_t position = 0;
  std::int64_t kept_from = 0;
  bool reader_waiting = false;
  std::map<std::int64_t, std::uint8_t*> blocks;
  std::vector<std::uint8_t*> spare;
  std::vector<std::unique_ptr<Lane>> lanes;
  std::thread ticker;
  int memory_version = 0;
  int detail_left = kDetailRequests;
  double summary_at = 0;
  DownloadScheduler::Totals summary_from;
};

DownloadSession::DownloadSession(DrivePlugin& drive, std::vector<DownloadExtent> extents, const CancelToken* cancel,
                                 DownloadOptions options)
    : impl_(std::make_unique<Impl>(drive, std::move(extents), cancel, std::move(options))) {}

DownloadSession::~DownloadSession() { impl_->shutdown(); }

std::size_t DownloadSession::read(std::uint8_t* dst, std::size_t size, std::string* error) {
  return impl_->read(dst, size, error);
}

std::string DownloadSession::seek(std::int64_t position) { return impl_->seek(position); }

std::int64_t DownloadSession::position() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->position;
}

std::int64_t DownloadSession::size() const { return impl_->size; }

void DownloadSession::set_dictionary(std::uint64_t bytes) { impl_->set_dictionary(bytes); }

int DownloadSession::id() const { return impl_->id; }

int DownloadSession::connections() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->started ? impl_->scheduler->target_lanes() : 0;
}

std::string download_memory_json(DownloadMemory& memory) {
  std::lock_guard<std::mutex> lock(memory.mu);
  return "{\"lanes\":" + std::to_string(memory.lanes) + ",\"lane_mbps\":" + fixed(memory.lane_rate / (1 << 20)) + "}";
}

void load_download_memory(const std::string& json, DownloadMemory* memory) {
  const Json parsed = Json::parse(json, nullptr);
  if (parsed.type != Json::Type::object) return;
  std::lock_guard<std::mutex> lock(memory->mu);
  if (parsed.get("lanes") != nullptr) memory->lanes = std::clamp(static_cast<int>(parsed.num("lanes")), 1, 8);
  if (parsed.get("lane_mbps") != nullptr) memory->lane_rate = std::max(0.0, parsed.num("lane_mbps")) * (1 << 20);
}

}  // namespace panplay
