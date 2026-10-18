#include "panplay/prefetch.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace panplay {
namespace {

constexpr std::size_t kStepSize = 256u << 10;
constexpr auto kStatsWindow = std::chrono::seconds(10);
// 首字节超过这么久的请求单独记一行。
constexpr long long kSlowOpenMs = 1000;

using Clock = std::chrono::steady_clock;

double ms_between(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

std::string fixed(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", value);
  return text;
}

std::string mb(std::int64_t bytes) { return fixed(static_cast<double>(bytes) / (1 << 20)) + " MB"; }

std::string rate(double bytes, double ms) { return ms > 0 ? fixed(bytes / (1 << 20) / (ms / 1000)) + " MB/s" : "-"; }

std::string percent(double part, double whole) {
  return whole > 0 ? std::to_string(static_cast<int>(part * 100 / whole + 0.5)) + "%" : "-";
}

int next_reader_id() {
  static std::atomic<int> last{0};
  return ++last;
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

}  // namespace

std::unique_ptr<ByteStream> DrivePlugin::open_stream(const std::string& file_id, std::int64_t offset, std::string*,
                                                     AbortSignal*) {
  return std::make_unique<ReadAtStream>(*this, file_id, offset);
}

std::unique_ptr<ByteStream> DrivePlugin::open_range(const std::string& file_id, std::int64_t offset, std::int64_t,
                                                    std::string* error, AbortSignal* abort) {
  return open_stream(file_id, offset, error, abort);
}

namespace {

PrefetchOptions sane(PrefetchOptions options) {
  constexpr std::size_t kMaxBufferBytes = 512u << 20;
  options.block_size = std::clamp<std::size_t>(options.block_size, 64u << 10, 32u << 20);
  options.connections = std::clamp(options.connections, 1, 16);
  const std::size_t max_by_memory = std::max<std::size_t>(kMaxBufferBytes / options.block_size, 1);
  options.max_blocks = std::clamp<std::size_t>(options.max_blocks, static_cast<std::size_t>(options.connections),
                                               std::max(max_by_memory, static_cast<std::size_t>(options.connections)));
  options.max_retries = std::clamp(options.max_retries, 0, 10);
  options.retry_base_ms = std::clamp(options.retry_base_ms, 1, 10000);
  return options;
}

}  // namespace

PrefetchReader::PrefetchReader(DrivePlugin& drive, std::string file_id, std::int64_t size, const CancelToken* cancel,
                               PrefetchOptions options)
    : drive_(drive),
      file_id_(std::move(file_id)),
      size_(size),
      cancel_(cancel),
      options_(sane(options)),
      id_(next_reader_id()),
      detail_left_(options_.connections * 2) {
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：文件 " + file_id_ + "，" + mb(size_) + "，" +
                std::to_string(options_.connections) + " 条连接 × " + mb(static_cast<std::int64_t>(options_.block_size)) +
                " 块，最多 " + std::to_string(options_.max_blocks) + " 块");
  const auto loop = options_.connections <= 1 ? &PrefetchReader::worker_loop : &PrefetchReader::range_worker_loop;
  for (int i = 0; i < options_.connections; ++i) workers_.emplace_back([this, loop] { run_worker(loop); });
}

PrefetchReader::~PrefetchReader() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    if (active_stream_ != nullptr) active_stream_->abort();
    for (ByteStream* stream : range_streams_) stream->abort();
    for (AbortSignal* signal : open_aborts_) signal->abort();
  }
  cv_.notify_all();
  for (auto& worker : workers_) worker.join();
  std::lock_guard<std::mutex> lock(mu_);
  if (!started_ || !log_enabled(LogLevel::debug)) return;
  account_locked();
  const std::int64_t received = received_.load();
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + " 结束：共 " + fixed(total_.elapsed_ms / 1000) + " 秒，收到 " +
                mb(received) + "，读走 " + mb(total_.delivered) + "；" + describe_locked(total_, received));
}

void PrefetchReader::account_locked() {
  if (!started_) return;
  const auto now = Clock::now();
  const double dt = ms_between(accounted_at_, now);
  accounted_at_ = now;
  const bool multi = options_.connections > 1;
  const std::size_t queued = blocks_.size() + pending_.size();
  const std::size_t held = queued + (multi ? static_cast<std::size_t>(in_flight_) : 0);
  const bool more = error_.empty() && (multi ? assign_position_ : fill_position_) < size_;
  const bool full = more && held >= options_.max_blocks;
  const double busy = in_flight_ * dt;
  count_locked([&](Stats& stats) {
    stats.elapsed_ms += dt;
    stats.busy_ms += busy;
    stats.queued_ms += static_cast<double>(queued) * dt;
    if (full) stats.full_ms += dt;
  });
  if (now - window_at_ < kStatsWindow) return;
  const std::int64_t received = received_.load();
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：近 " + fixed(window_.elapsed_ms / 1000) + " 秒 " +
                describe_locked(window_, received - window_received_));
  window_ = Stats{};
  window_received_ = received;
  window_at_ = now;
}

std::string PrefetchReader::describe_locked(const Stats& stats, std::int64_t received) const {
  const double elapsed = stats.elapsed_ms;
  std::string text = "下载 " + rate(static_cast<double>(received), elapsed);
  if (stats.busy_ms > 0) {
    text += "（平均在下 " + fixed(stats.busy_ms / elapsed) + " 条，每条 " + rate(static_cast<double>(received), stats.busy_ms) +
            "）";
  }
  text += "，读走 " + rate(static_cast<double>(stats.delivered), elapsed) + "，读取方等数据 " +
          percent(stats.wait_ms, elapsed) + "，缓冲满 " + percent(stats.full_ms, elapsed) + "（平均存 " +
          fixed(elapsed > 0 ? stats.queued_ms / elapsed : 0) + " 块）";
  text += "，请求 " + std::to_string(stats.requests) + " 次";
  if (stats.requests > 0) {
    text += "（首字节平均 " + std::to_string(stats.open_ms / stats.requests) + " ms，最长 " +
            std::to_string(stats.open_ms_max) + " ms）";
  }
  text += "，下完 " + std::to_string(stats.blocks) + " 块";
  if (stats.blocks > 0) {
    text += "（平均 " + fixed(static_cast<double>(stats.block_ms) / 1000 / stats.blocks) + " 秒，最长 " +
            fixed(static_cast<double>(stats.block_ms_max) / 1000) + " 秒）";
  }
  if (stats.open_failures > 0) text += "，打开失败 " + std::to_string(stats.open_failures) + " 次";
  if (stats.breaks > 0) text += "，中途断开 " + std::to_string(stats.breaks) + " 次";
  if (stats.resets > 0) text += "，跳转重来 " + std::to_string(stats.resets) + " 次";
  return text;
}

bool PrefetchReader::detail_locked() {
  if (detail_left_ <= 0) return false;
  --detail_left_;
  return true;
}

void PrefetchReader::run_worker(void (PrefetchReader::*loop)(AbortSignal&)) {
  AbortSignal signal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    open_aborts_.push_back(&signal);
  }
  try {
    (this->*loop)(signal);
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mu_);
    error_ = std::string("预读出错: ") + e.what();
    cv_.notify_all();
  }
  std::lock_guard<std::mutex> lock(mu_);
  open_aborts_.erase(std::remove(open_aborts_.begin(), open_aborts_.end(), &signal), open_aborts_.end());
}

bool PrefetchReader::arm_open(AbortSignal& signal, std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  if (stop_ || generation != generation_) return false;
  signal.reset();
  return true;
}

// 多连接：每条连接领一段 [start, start + block_size)，用一次有界请求下完。
// 下完的段先放在 pending_，按偏移顺序接到 blocks_ 后面，读取方看到的仍是顺序数据。
void PrefetchReader::range_worker_loop(AbortSignal& signal) {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [&] {
      return stop_ || (started_ && error_.empty() && assign_position_ < size_ &&
                       blocks_.size() + pending_.size() + static_cast<std::size_t>(in_flight_) < options_.max_blocks);
    });
    if (stop_) break;
    account_locked();
    const std::uint64_t generation = generation_;
    const std::int64_t start = assign_position_;
    const std::int64_t length = std::min<std::int64_t>(static_cast<std::int64_t>(options_.block_size), size_ - start);
    assign_position_ += length;
    ++in_flight_;
    Block block;
    block.data = take_buffer_locked();
    const bool detailed = detail_locked();
    lock.unlock();

    const std::string tag = "预读 #" + std::to_string(id_) + "：块 @" + mb(start);
    const auto block_from = Clock::now();
    block.start = start;
    block.data.resize(static_cast<std::size_t>(length));
    std::size_t filled = 0;
    int failures = 0;
    std::string failure;
    std::unique_ptr<ByteStream> stream;
    auto drop_stream = [&] {
      if (!stream) return;
      {
        std::lock_guard<std::mutex> guard(mu_);
        range_streams_.erase(std::remove(range_streams_.begin(), range_streams_.end(), stream.get()), range_streams_.end());
      }
      stream.reset();
    };
    struct DropOnExit {
      decltype(drop_stream)& drop;
      ~DropOnExit() { drop(); }
    } drop_on_exit{drop_stream};
    while (filled < block.data.size()) {
      if (stale(generation)) break;
      if (cancel_ && cancel_->cancelled()) {
        failure = "interrupted";
        break;
      }
      if (!stream) {
        const std::int64_t at = start + static_cast<std::int64_t>(filled);
        if (!arm_open(signal, generation)) break;
        std::string open_error;
        const auto open_from = Clock::now();
        auto opened = drive_.open_range(file_id_, at, length - static_cast<std::int64_t>(filled), &open_error, &signal);
        const long long open_ms = ms_since(open_from);
        if (!opened) {
          if (stale(generation)) break;
          {
            std::lock_guard<std::mutex> guard(mu_);
            count_locked([](Stats& stats) { ++stats.open_failures; });
          }
          PANPLAY_DEBUG(tag + " 打开失败，" + std::to_string(open_ms) + " ms，第 " + std::to_string(failures + 1) +
                        " 次：" + open_error);
          if (++failures > options_.max_retries) {
            failure = open_error.empty() ? "无法打开下载" : open_error;
            break;
          }
          if (!wait_retry(failures - 1, generation)) break;
          continue;
        }
        stream = std::move(opened);
        {
          std::lock_guard<std::mutex> guard(mu_);
          range_streams_.push_back(stream.get());
          count_locked([&](Stats& stats) {
            ++stats.requests;
            stats.open_ms += open_ms;
            stats.open_ms_max = std::max(stats.open_ms_max, open_ms);
          });
          if (stop_ || generation != generation_) stream->abort();
        }
        if (detailed || failures > 0 || open_ms >= kSlowOpenMs) {
          PANPLAY_DEBUG(tag + (filled > 0 ? " 从 @" + mb(at) + " 续传" : std::string(" 请求")) + " " +
                        mb(length - static_cast<std::int64_t>(filled)) + "，首字节 " + std::to_string(open_ms) + " ms");
        }
      }
      std::string read_error;
      const std::size_t want = std::min(kStepSize, block.data.size() - filled);
      const std::size_t got = stream->read(block.data.data() + filled, want, &read_error);
      if (got == 0) {
        drop_stream();
        if (stale(generation)) break;
        {
          std::lock_guard<std::mutex> guard(mu_);
          count_locked([](Stats& stats) { ++stats.breaks; });
        }
        PANPLAY_DEBUG(tag + " 下到 " + mb(static_cast<std::int64_t>(filled)) + " 时断开，第 " + std::to_string(failures + 1) +
                      " 次：" + (read_error.empty() ? "连接提前结束" : read_error));
        if (++failures > options_.max_retries) {
          failure = read_error.empty() ? "下载提前结束" : read_error;
          break;
        }
        if (!wait_retry(failures - 1, generation)) break;
        continue;
      }
      filled += got;
      received_ += static_cast<std::int64_t>(got);
      failures = 0;
    }
    drop_stream();
    const long long block_ms = ms_since(block_from);

    lock.lock();
    account_locked();
    --in_flight_;
    if (stop_) break;
    if (generation != generation_) {
      cv_.notify_all();
      continue;
    }
    if (!failure.empty()) {
      error_ = failure;
      cv_.notify_all();
      continue;
    }
    if (filled < block.data.size()) continue;
    count_locked([&](Stats& stats) {
      ++stats.blocks;
      stats.block_ms += block_ms;
      stats.block_ms_max = std::max(stats.block_ms_max, block_ms);
    });
    if (detailed) {
      PANPLAY_DEBUG(tag + " 下完，用 " + fixed(static_cast<double>(block_ms) / 1000) + " 秒（" +
                    rate(static_cast<double>(filled), static_cast<double>(block_ms)) + "）");
    }
    pending_.emplace(start, std::move(block));
    for (auto it = pending_.find(fill_position_); it != pending_.end(); it = pending_.find(fill_position_)) {
      fill_position_ += static_cast<std::int64_t>(it->second.data.size());
      blocks_.push_back(std::move(it->second));
      pending_.erase(it);
    }
    cv_.notify_all();
  }
}

std::int64_t PrefetchReader::buffered_end_locked() const {
  if (blocks_.empty()) return position_;
  const Block& last = blocks_.back();
  return last.start + static_cast<std::int64_t>(last.data.size());
}

void PrefetchReader::retire_locked(Block&& block) {
  // 留两块：读取方一次换下一块，后台线程马上就会取走，多留没用，只会多占内存。
  if (history_.data.capacity() >= options_.block_size && spare_.size() < 2) {
    spare_.push_back(std::move(history_.data));
  }
  history_ = std::move(block);
}

std::vector<std::uint8_t> PrefetchReader::take_buffer_locked() {
  if (spare_.empty()) return {};
  std::vector<std::uint8_t> buffer = std::move(spare_.back());
  spare_.pop_back();
  return buffer;
}

bool PrefetchReader::stale(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  return stop_ || generation != generation_;
}

bool PrefetchReader::wait_retry(int attempt, std::uint64_t generation) {
  const auto delay = std::chrono::milliseconds(static_cast<std::int64_t>(options_.retry_base_ms) << attempt);
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait_for(lock, delay, [&] { return stop_ || generation != generation_ || (cancel_ && cancel_->cancelled()); });
  return !stop_ && generation == generation_ && !(cancel_ && cancel_->cancelled());
}

void PrefetchReader::worker_loop(AbortSignal& signal) {
  std::unique_ptr<ByteStream> stream;
  std::int64_t stream_position = -1;
  std::uint64_t stream_generation = 0;
  int failures = 0;

  auto drop_stream = [&] {
    {
      std::lock_guard<std::mutex> lock(mu_);
      active_stream_ = nullptr;
    }
    stream.reset();
    stream_position = -1;
  };
  // 出异常时也要从 active_stream_ 摘掉，否则跳转或析构会去中止已释放的连接。要在 lock 之前声明。
  struct DropOnExit {
    decltype(drop_stream)& drop;
    ~DropOnExit() { drop(); }
  } drop_on_exit{drop_stream};

  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [&] {
      return stop_ || (started_ && error_.empty() && fill_position_ < size_ && blocks_.size() < options_.max_blocks);
    });
    if (stop_) break;
    account_locked();
    const std::uint64_t generation = generation_;
    const std::int64_t start = fill_position_;
    ++in_flight_;
    Block block;
    block.data = take_buffer_locked();
    lock.unlock();

    const std::string tag = "预读 #" + std::to_string(id_) + "：";
    const auto block_from = Clock::now();
    block.start = start;
    block.data.resize(static_cast<std::size_t>(std::min<std::int64_t>(options_.block_size, size_ - start)));
    std::size_t filled = 0;
    std::string failure;
    while (filled < block.data.size()) {
      if (stale(generation)) break;
      if (cancel_ && cancel_->cancelled()) {
        failure = "interrupted";
        break;
      }
      const std::int64_t at = start + static_cast<std::int64_t>(filled);
      if (!stream || stream_generation != generation || stream_position != at) {
        drop_stream();
        if (!arm_open(signal, generation)) break;
        std::string open_error;
        const auto open_from = Clock::now();
        auto opened = drive_.open_stream(file_id_, at, &open_error, &signal);
        const long long open_ms = ms_since(open_from);
        if (!opened) {
          if (stale(generation)) break;
          {
            std::lock_guard<std::mutex> guard(mu_);
            count_locked([](Stats& stats) { ++stats.open_failures; });
          }
          PANPLAY_DEBUG(tag + "打开下载 @" + mb(at) + " 失败，" + std::to_string(open_ms) + " ms，第 " +
                        std::to_string(failures + 1) + " 次：" + open_error);
          if (++failures > options_.max_retries) {
            failure = open_error.empty() ? "无法打开下载" : open_error;
            break;
          }
          if (!wait_retry(failures - 1, generation)) break;
          continue;
        }
        PANPLAY_DEBUG(tag + "打开下载 @" + mb(at) + "，首字节 " + std::to_string(open_ms) + " ms");
        stream = std::move(opened);
        stream_position = at;
        stream_generation = generation;
        std::lock_guard<std::mutex> guard(mu_);
        active_stream_ = stream.get();
        count_locked([&](Stats& stats) {
          ++stats.requests;
          stats.open_ms += open_ms;
          stats.open_ms_max = std::max(stats.open_ms_max, open_ms);
        });
        if (stop_ || generation != generation_) stream->abort();
      }
      std::string read_error;
      const std::size_t want = std::min(kStepSize, block.data.size() - filled);
      const std::size_t got = stream->read(block.data.data() + filled, want, &read_error);
      if (got == 0) {
        drop_stream();
        if (stale(generation)) break;
        {
          std::lock_guard<std::mutex> guard(mu_);
          count_locked([](Stats& stats) { ++stats.breaks; });
        }
        PANPLAY_DEBUG(tag + "下到 @" + mb(at) + " 时断开，第 " + std::to_string(failures + 1) + " 次：" +
                      (read_error.empty() ? "连接提前结束" : read_error));
        if (++failures > options_.max_retries) {
          failure = read_error.empty() ? "下载提前结束" : read_error;
          break;
        }
        if (!wait_retry(failures - 1, generation)) break;
        continue;
      }
      filled += got;
      received_ += static_cast<std::int64_t>(got);
      stream_position += static_cast<std::int64_t>(got);
      failures = 0;
    }
    const long long block_ms = ms_since(block_from);

    lock.lock();
    account_locked();
    --in_flight_;
    if (stop_) break;
    if (generation != generation_) continue;
    if (!failure.empty()) {
      error_ = failure;
      cv_.notify_all();
      continue;
    }
    if (filled < block.data.size()) continue;
    count_locked([&](Stats& stats) {
      ++stats.blocks;
      stats.block_ms += block_ms;
      stats.block_ms_max = std::max(stats.block_ms_max, block_ms);
    });
    fill_position_ = start + static_cast<std::int64_t>(filled);
    blocks_.push_back(std::move(block));
    cv_.notify_all();
  }
  lock.unlock();
  drop_stream();
}

std::size_t PrefetchReader::read(std::uint8_t* dst, std::size_t size, std::string* error) {
  if (size == 0 || position_ >= size_) return 0;
  std::unique_lock<std::mutex> lock(mu_);
  if (!started_) {
    started_ = true;
    accounted_at_ = window_at_ = Clock::now();
    window_received_ = received_.load();
    cv_.notify_all();
  }
  bool waiting = false;
  Clock::time_point waited_from;
  const auto note_wait = [&] {
    if (!waiting) return;
    const auto now = Clock::now();
    const double ms = ms_between(waited_from, now);
    waited_from = now;
    count_locked([&](Stats& stats) { stats.wait_ms += ms; });
  };
  while (true) {
    if (!blocks_.empty()) {
      Block& front = blocks_.front();
      const std::int64_t front_end = front.start + static_cast<std::int64_t>(front.data.size());
      if (front_end <= position_) {
        account_locked();
        retire_locked(std::move(front));
        blocks_.pop_front();
        cv_.notify_all();
        continue;
      }
      if (front.start + static_cast<std::int64_t>(front.used) < position_) {
        front.used = static_cast<std::size_t>(position_ - front.start);
      }
      break;
    }
    if (!error_.empty()) {
      note_wait();
      *error = error_;
      return 0;
    }
    if (cancel_ && cancel_->cancelled()) {
      note_wait();
      *error = "interrupted";
      return 0;
    }
    if (!waiting) {
      waiting = true;
      waited_from = Clock::now();
    }
    cv_.wait_for(lock, std::chrono::milliseconds(100));
    note_wait();
    account_locked();
  }
  Block& front = blocks_.front();
  const std::size_t n = std::min(size, front.data.size() - front.used);
  std::memcpy(dst, front.data.data() + front.used, n);
  front.used += n;
  position_ += static_cast<std::int64_t>(n);
  count_locked([&](Stats& stats) { stats.delivered += static_cast<std::int64_t>(n); });
  if (front.used == front.data.size()) {
    account_locked();
    retire_locked(std::move(front));
    blocks_.pop_front();
    cv_.notify_all();
  }
  return n;
}

std::string PrefetchReader::seek(std::int64_t position) {
  if (position < 0 || position > size_) return "读取位置越界";
  if (position == position_) return "";
  std::lock_guard<std::mutex> lock(mu_);
  account_locked();
  // 往前跳不远时保留连接，read 会丢掉跳过的数据；重新连接要一秒左右，还得重新提速。
  const std::int64_t keep_reach = static_cast<std::int64_t>(options_.block_size) * 2;
  const std::int64_t requested_end = options_.connections > 1 ? std::max(buffered_end_locked(), assign_position_)
                                                               : std::max(buffered_end_locked(), fill_position_);
  if (position > position_ && error_.empty() && position <= requested_end + keep_reach) {
    while (!blocks_.empty()) {
      Block& front = blocks_.front();
      const std::int64_t front_end = front.start + static_cast<std::int64_t>(front.data.size());
      if (position < front_end) {
        front.used = static_cast<std::size_t>(position - front.start);
        break;
      }
      retire_locked(std::move(front));
      blocks_.pop_front();
    }
    position_ = position;
    cv_.notify_all();
    return "";
  }
  if (position < position_ && error_.empty()) {
    if (!blocks_.empty() && position >= blocks_.front().start && position_ <= blocks_.front().start + static_cast<std::int64_t>(blocks_.front().data.size())) {
      blocks_.front().used = static_cast<std::size_t>(position - blocks_.front().start);
      position_ = position;
      return "";
    }
    const std::int64_t history_end = history_.start + static_cast<std::int64_t>(history_.data.size());
    if (!history_.data.empty() && position >= history_.start && position_ <= history_end + (blocks_.empty() ? 0 : static_cast<std::int64_t>(blocks_.front().used))) {
      if (!blocks_.empty()) blocks_.front().used = 0;
      history_.used = static_cast<std::size_t>(position - history_.start);
      blocks_.push_front(std::move(history_));
      history_ = Block{};
      position_ = position;
      return "";
    }
  }
  if (log_enabled(LogLevel::debug)) {
    std::int64_t dropped = 0;
    for (const Block& block : blocks_) dropped += static_cast<std::int64_t>(block.data.size() - block.used);
    for (const auto& entry : pending_) dropped += static_cast<std::int64_t>(entry.second.data.size());
    PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：跳转 " + mb(position_) + " -> " + mb(position) +
                  "，丢掉缓冲里没读的 " + mb(dropped) + "，" + std::to_string(in_flight_) + " 块正在下的作废，重新开始下载");
  }
  count_locked([](Stats& stats) { ++stats.resets; });
  detail_left_ = options_.connections;
  ++generation_;
  blocks_.clear();
  pending_.clear();
  history_ = Block{};
  error_.clear();
  fill_position_ = position;
  assign_position_ = position;
  position_ = position;
  if (active_stream_ != nullptr) active_stream_->abort();
  for (ByteStream* stream : range_streams_) stream->abort();
  for (AbortSignal* signal : open_aborts_) signal->abort();
  cv_.notify_all();
  return "";
}

}  // namespace panplay
