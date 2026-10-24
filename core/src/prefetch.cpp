#include "panplay/prefetch.hpp"
#include "panplay/trace.hpp"

#include "install_clock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
constexpr std::int64_t kMinSegment = 1 << 20;
constexpr std::int64_t kAlign = 64 << 10;
// 低于 32 KB/s 不管和谁比都算卡住。
constexpr double kStuckBytesPerMs = 32.0 * 1024 / 1000;
constexpr int kLevels[] = {1, 2, 3, 4, 6, 8};

using Clock = std::chrono::steady_clock;

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

std::string per_second(double bytes_per_ms) { return fixed(bytes_per_ms * 1000 / (1 << 20)) + " MB/s"; }

std::string percent(double part, double whole) {
  return whole > 0 ? std::to_string(static_cast<int>(part * 100 / whole + 0.5)) + "%" : "-";
}

int next_reader_id() {
  static std::atomic<int> last{0};
  return ++last;
}

int level_above(int current, int cap) {
  for (const int level : kLevels) {
    if (level > current && level <= cap) return level;
  }
  return current < cap ? cap : current;
}

int level_below(int current) {
  int below = 1;
  for (const int level : kLevels) {
    if (level < current) below = level;
  }
  return below;
}

std::int64_t align_up(std::int64_t value) { return (value + kAlign - 1) / kAlign * kAlign; }

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

namespace {

PrefetchOptions sane(PrefetchOptions options) {
  constexpr std::size_t kMaxBufferBytes = 512u << 20;
  options.block_size = std::clamp<std::size_t>(options.block_size, 64u << 10, 32u << 20);
  options.connections = std::clamp(options.connections, 1, 16);
  const std::size_t max_by_memory = std::max<std::size_t>(kMaxBufferBytes / options.block_size, 2);
  options.max_blocks = std::clamp<std::size_t>(options.max_blocks, 2, max_by_memory);
  options.max_retries = std::clamp(options.max_retries, 0, 10);
  options.retry_base_ms = std::clamp(options.retry_base_ms, 1, 10000);
  for (int* value : {&options.settle_ms, &options.measure_ms, &options.review_ms, &options.backoff_ms,
                     &options.segment_ms, &options.stall_ms}) {
    *value = std::max(*value, 1);
  }
  if (!options.tuning) options.tuning = std::make_shared<PrefetchTuning>();
  return options;
}

}  // namespace

PrefetchReader::PrefetchReader(DrivePlugin& drive, std::string file_id, std::int64_t size, const CancelToken* cancel,
                               PrefetchOptions options)
    : drive_(drive),
      file_id_(std::move(file_id)),
      size_(size),
      cancel_(cancel),
      options_(sane(std::move(options))),
      budget_(static_cast<std::int64_t>(options_.block_size * options_.max_blocks)),
      id_(next_reader_id()),
      workers_state_(static_cast<std::size_t>(options_.connections)),
      detail_left_(options_.connections * 2) {
  if (options_.adaptive) {
    std::lock_guard<std::mutex> lock(options_.tuning->mu);
    want_ = std::clamp(options_.tuning->connections, 1, options_.connections);
  } else {
    want_ = options_.connections;
  }
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：文件 " + file_id_ + "，" + mb(size_) + "，" +
                (options_.adaptive ? "最多 " + std::to_string(options_.connections) + " 条连接，从 " +
                                         std::to_string(want_) + " 条开始"
                                   : "固定 " + std::to_string(want_) + " 条连接") +
                "，缓冲 " + mb(static_cast<std::int64_t>(options_.block_size)) + " × " +
                std::to_string(options_.max_blocks) + " 块");
  for (int i = 0; i < options_.connections; ++i) workers_.emplace_back([this, i] { run_worker(i); });
}

PrefetchReader::~PrefetchReader() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    for (auto& worker : workers_state_) {
      if (worker.lease.stream != nullptr) worker.lease.stream->abort();
    }
    for (AbortSignal* signal : open_aborts_) signal->abort();
  }
  cv_.notify_all();
  for (auto& worker : workers_) worker.join();
  std::lock_guard<std::mutex> lock(mu_);
  if (!started_) return;
  account_locked();
  const std::int64_t received = received_.load();
  // 每卷一行，不开详细日志也记：看得出每次请求等首字节多久、多条连接时缓冲够不够用。
  log_info("预读 #" + std::to_string(id_) + " 结束：共 " + fixed(total_.elapsed_ms / 1000) + " 秒，收到 " + mb(received) +
           "，读走 " + mb(total_.delivered) + "，缓冲 " + mb(budget_) + "；" + describe_locked(total_, received));
}

int PrefetchReader::connections() {
  std::lock_guard<std::mutex> lock(mu_);
  return want_;
}

void PrefetchReader::run_worker(int index) {
  AbortSignal signal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    open_aborts_.push_back(&signal);
  }
  try {
    worker_loop(index, signal);
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

// 每条连接一段一段地领：单连接时一个没有终点的请求读到底，多连接时每段一个带终点的请求。
void PrefetchReader::worker_loop(int index, AbortSignal& signal) {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [&] { return stop_ || can_lease_locked(index); });
    if (stop_) break;
    account_locked();
    if (!can_lease_locked(index)) continue;
    make_lease_locked(index);
    ++in_flight_;
    const bool detailed = detail_locked();
    lock.unlock();

    run_lease(index, signal, detailed);

    lock.lock();
    account_locked();
    Worker& worker = workers_state_[static_cast<std::size_t>(index)];
    Lease& lease = worker.lease;
    if (lease.generation == generation_) {
      const auto now = Clock::now();
      if (lease.connected && !lease.held && lease.at > lease.connect_at) {
        const double spent = ms_between(lease.first_byte, now);
        if (spent >= 200) {
          const double speed = static_cast<double>(lease.at - lease.connect_at) / spent;
          worker.speed = worker.speed > 0 ? (worker.speed + speed) / 2 : speed;
          // 缓冲边上领的零头段太短，量不准。
          if (!starving_ && lease.at - lease.connect_at >= kMinSegment / 4) note_lane_rate_locked(speed);
        }
      }
      if (lease.at >= lease.end) {
        const long long spent = static_cast<long long>(ms_between(lease.began, now));
        count_locked([&](Stats& stats) {
          ++stats.segments;
          stats.segment_ms += spent;
          stats.segment_ms_max = std::max(stats.segment_ms_max, spent);
        });
        if (detailed) {
          PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：段 @" + mb(lease.start) + " 下完，" +
                        mb(lease.end - lease.start) + "，用 " + fixed(static_cast<double>(spent) / 1000) + " 秒（" +
                        rate(static_cast<double>(lease.end - lease.start), static_cast<double>(spent)) + "）");
        }
      }
      release_locked(lease);
    }
    lease.active = false;
    lease.chunk.reset();
    lease.stream = nullptr;
    --in_flight_;
    cv_.notify_all();
  }
}

void PrefetchReader::run_lease(int index, AbortSignal& signal, bool detailed) {
  Worker& worker = workers_state_[static_cast<std::size_t>(index)];
  Lease& lease = worker.lease;
  std::uint64_t generation = 0;
  std::string tag;
  {
    std::lock_guard<std::mutex> lock(mu_);
    generation = lease.generation;
    tag = "预读 #" + std::to_string(id_) + "：段 @" + mb(lease.start);
  }
  std::unique_ptr<ByteStream> stream;
  // 这个请求服务器会发到哪：没有终点的请求发到文件末尾，段中途收尾时要断开，不然连接里还有没读的数据。
  std::int64_t stream_end = 0;
  int failures = 0;
  std::string failure;
  auto drop_stream = [&](bool cut) {
    if (!stream) return;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (lease.stream == stream.get()) lease.stream = nullptr;
    }
    if (cut) stream->abort();
    stream.reset();
  };
  // 出异常时也要从 lease.stream 摘掉，否则跳转或析构会去中止已释放的连接。
  struct DropOnExit {
    decltype(drop_stream)& drop;
    ~DropOnExit() { drop(true); }
  } drop_on_exit{drop_stream};

  while (true) {
    std::int64_t at = 0;
    std::int64_t end = 0;
    bool open_ended = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_ || generation != generation_ || lease.restart) return;
      at = lease.at;
      end = lease.end;
      open_ended = lease.open_ended;
    }
    if (at >= end) return;
    if (cancel_ && cancel_->cancelled()) {
      failure = "interrupted";
      break;
    }
    if (!stream) {
      if (!arm_open(signal, generation)) return;
      std::string open_error;
      const bool to_end = open_ended && end >= size_;
      const auto open_from = Clock::now();
      auto opened = to_end ? drive_.open_stream(file_id_, at, &open_error, &signal)
                           : drive_.open_range(file_id_, at, end - at, &open_error, &signal);
      const long long open_ms = ms_since(open_from);
      if (!opened) {
        if (stale(generation)) return;
        {
          std::lock_guard<std::mutex> lock(mu_);
          count_locked([](Stats& stats) { ++stats.open_failures; });
        }
        PANPLAY_DEBUG(tag + " 打开失败，" + std::to_string(open_ms) + " ms，第 " + std::to_string(failures + 1) +
                      " 次：" + open_error);
        if (++failures > options_.max_retries) {
          failure = open_error.empty() ? "无法打开下载" : open_error;
          break;
        }
        if (!wait_retry(failures - 1, generation)) return;
        continue;
      }
      stream = std::move(opened);
      stream_end = to_end ? size_ : end;
      {
        std::lock_guard<std::mutex> lock(mu_);
        lease.stream = stream.get();
        lease.connected = true;
        lease.first_byte = Clock::now();
        lease.connect_at = lease.at;
        lease.check_at = lease.at;
        lease.check_time = lease.first_byte;
        lease.check_rate = -1;
        open_ms_avg_ = open_ms_avg_ * 0.7 + static_cast<double>(open_ms) * 0.3;
        count_locked([&](Stats& stats) {
          ++stats.requests;
          stats.open_ms += open_ms;
          stats.open_ms_max = std::max(stats.open_ms_max, open_ms);
        });
        if (stop_ || generation != generation_ || lease.restart) stream->abort();
      }
      if (detailed || failures > 0 || open_ms >= kSlowOpenMs) {
        PANPLAY_DEBUG(tag + (at > lease.start ? " 从 @" + mb(at) + " 续传" : std::string(" 请求")) + " " +
                      (to_end ? std::string("读到底") : mb(end - at)) + "，首字节 " + std::to_string(open_ms) + " ms");
      }
    }

    std::uint8_t* dst = nullptr;
    std::size_t want = 0;
    ChunkPtr chunk;
    {
      std::unique_lock<std::mutex> lock(mu_);
      bool waited = false;
      while (true) {
        if (stop_ || generation != generation_ || lease.restart) return;
        if (lease.at >= lease.end || (lease.chunk && lease.at < lease.chunk->end())) break;
        const std::int64_t length = std::min<std::int64_t>(static_cast<std::int64_t>(options_.block_size), lease.end - lease.at);
        // 缓冲满了先不读，服务器那边靠 TCP 流量控制停着。
        if (lease.at > position_ && lease.at + length - position_ > budget_) {
          waited = true;
          lease.blocked = true;
          lease.held = true;
          cv_.wait(lock);
          continue;
        }
        lease.chunk = new_chunk_locked(lease.at, length);
        break;
      }
      lease.blocked = false;
      if (waited) {
        // 等缓冲腾地方的时间不算慢。
        lease.check_at = lease.at;
        lease.check_time = Clock::now();
        lease.check_rate = -1;
      }
      if (lease.at >= lease.end) continue;
      chunk = lease.chunk;
      dst = chunk->data.data() + (lease.at - chunk->start);
      want = std::min<std::size_t>(kStepSize, static_cast<std::size_t>(chunk->end() - lease.at));
    }

    std::string read_error;
    const std::size_t got = stream->read(dst, want, &read_error);
    if (got == 0) {
      drop_stream(true);
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ || generation != generation_ || lease.restart) return;
        count_locked([](Stats& stats) { ++stats.breaks; });
      }
      PANPLAY_DEBUG(tag + " 下到 @" + mb(at) + " 时断开，第 " + std::to_string(failures + 1) + " 次：" +
                    (read_error.empty() ? "连接提前结束" : read_error));
      if (++failures > options_.max_retries) {
        failure = read_error.empty() ? "下载提前结束" : read_error;
        break;
      }
      if (!wait_retry(failures - 1, generation)) return;
      continue;
    }
    bool done = false;
    bool cut = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_ || generation != generation_) return;
      chunk->filled += got;
      lease.at += static_cast<std::int64_t>(got);
      worker.received += static_cast<std::int64_t>(got);
      received_ += static_cast<std::int64_t>(got);
      done = lease.at >= lease.end;
      cut = stream_end > lease.end;
      cv_.notify_all();
    }
    failures = 0;
    if (done) {
      drop_stream(cut);
      return;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!stop_ && generation == generation_) error_ = failure;
  cv_.notify_all();
}

std::int64_t PrefetchReader::lowest_unassigned_locked() const {
  return holes_.empty() ? assign_position_ : std::min(holes_.begin()->first, assign_position_);
}

bool PrefetchReader::can_lease_locked(int index) const {
  if (!started_ || stop_ || !error_.empty() || index >= want_) return false;
  if (workers_state_[static_cast<std::size_t>(index)].lease.active) return false;
  const std::int64_t start = lowest_unassigned_locked();
  return start < size_ && start - position_ < budget_;
}

std::int64_t PrefetchReader::segment_length_locked(int index) const {
  double speed = workers_state_[static_cast<std::size_t>(index)].speed;
  if (speed <= 0) {
    double sum = 0;
    int counted = 0;
    for (const auto& worker : workers_state_) {
      if (worker.speed > 0) {
        sum += worker.speed;
        ++counted;
      }
    }
    if (counted > 0) speed = sum / counted;
  }
  if (speed <= 0 && rate_avg_ > 0) speed = rate_avg_ / want_;
  double span = std::max<double>(options_.segment_ms, 20 * open_ms_avg_);
  // 试着加的连接先领一小段，试失败了损失也小。
  if (tune_ == Tune::up && index >= tune_from_ && tune_base_ > 0) {
    speed = tune_base_ / std::max(1, tune_from_);
    span = options_.measure_ms;
  }
  std::int64_t length = speed > 0 ? static_cast<std::int64_t>(speed * span) : static_cast<std::int64_t>(options_.block_size);
  const std::int64_t cap = std::max<std::int64_t>(kMinSegment, budget_ / std::max(1, want_));
  length = std::clamp(length, kMinSegment, cap);
  return align_up(length);
}

void PrefetchReader::make_lease_locked(int index) {
  Lease& lease = workers_state_[static_cast<std::size_t>(index)].lease;
  lease = Lease{};
  lease.active = true;
  lease.generation = generation_;
  std::int64_t start = assign_position_;
  std::int64_t limit = size_;
  const bool from_hole = !holes_.empty();
  if (from_hole) {
    start = holes_.begin()->first;
    limit = holes_.begin()->second;
    holes_.erase(holes_.begin());
  }
  std::int64_t end = size_;
  if (want_ == 1 && limit >= size_) {
    lease.open_ended = true;
  } else {
    end = std::min(limit, start + segment_length_locked(index));
    end = std::min(limit, std::min(end, std::max(start + kAlign, position_ + budget_)));
  }
  if (from_hole) {
    if (end < limit) holes_[end] = limit;
  } else {
    assign_position_ = end;
  }
  lease.start = start;
  lease.at = start;
  lease.end = end;
  lease.began = Clock::now();
  lease.check_at = start;
  lease.check_time = lease.began;
}

void PrefetchReader::add_hole_locked(std::int64_t start, std::int64_t end) {
  if (start >= end) return;
  if (const auto next = holes_.find(end); next != holes_.end()) {
    end = next->second;
    holes_.erase(next);
  }
  auto prev = holes_.lower_bound(start);
  if (prev != holes_.begin()) {
    --prev;
    if (prev->second == start) {
      start = prev->first;
      holes_.erase(prev);
    }
  }
  holes_[start] = end;
}

// 段没下完就结束（卡住重派、跳过、出错）：没收满的块截到收到的位置，没下的范围放回去重新派。
void PrefetchReader::release_locked(Lease& lease) {
  if (lease.generation != generation_) return;
  if (lease.chunk && lease.chunk->filled < lease.chunk->data.size()) {
    if (lease.chunk->filled == 0) {
      if (const auto it = chunks_.find(lease.chunk->start); it != chunks_.end() && it->second == lease.chunk) chunks_.erase(it);
    } else {
      lease.chunk->data.resize(lease.chunk->filled);
    }
  }
  if (lease.at >= lease.end) return;
  if (lease.end == assign_position_) {
    assign_position_ = std::max(lease.at, std::min(position_, lease.end));
  } else {
    add_hole_locked(std::max(lease.at, position_), lease.end);
  }
}

PrefetchReader::ChunkPtr PrefetchReader::new_chunk_locked(std::int64_t start, std::int64_t length) {
  auto chunk = std::make_shared<Chunk>();
  chunk->start = start;
  if (!spare_.empty()) {
    chunk->data = std::move(spare_.back());
    spare_.pop_back();
  }
  chunk->data.resize(static_cast<std::size_t>(length));
  chunks_[start] = chunk;
  return chunk;
}

PrefetchReader::ChunkPtr PrefetchReader::chunk_at_locked(std::int64_t position) const {
  auto it = chunks_.upper_bound(position);
  if (it == chunks_.begin()) return nullptr;
  --it;
  return position < it->second->end() ? it->second : nullptr;
}

bool PrefetchReader::available_locked(std::int64_t from, std::int64_t to) const {
  std::int64_t at = from;
  while (at < to) {
    const ChunkPtr chunk = chunk_at_locked(at);
    if (!chunk || at >= chunk->filled_end()) return false;
    at = chunk->filled_end();
  }
  return true;
}

std::int64_t PrefetchReader::download_front_locked() const {
  std::int64_t front = position_;
  for (const auto& worker : workers_state_) {
    if (worker.lease.active && worker.lease.generation == generation_) front = std::max(front, worker.lease.at);
  }
  if (!chunks_.empty()) front = std::max(front, chunks_.rbegin()->second->filled_end());
  return front;
}

// 读过的块只留紧挨着的一块；换下来的内存留给下一块用。
void PrefetchReader::prune_locked() {
  std::vector<std::int64_t> behind;
  for (const auto& [start, chunk] : chunks_) {
    if (chunk->end() <= position_) behind.push_back(start);
  }
  if (behind.size() <= 1) return;
  behind.pop_back();
  for (const std::int64_t start : behind) {
    auto it = chunks_.find(start);
    ChunkPtr chunk = std::move(it->second);
    chunks_.erase(it);
    if (chunk.use_count() == 1 && spare_.size() < 2 && chunk->data.capacity() >= options_.block_size) {
      spare_.push_back(std::move(chunk->data));
    }
  }
}

// 往前跳了一小段：跳过的范围不用再下，整段都在跳过范围里的请求断开去领别的。
void PrefetchReader::skip_ahead_locked() {
  prune_locked();
  while (!holes_.empty() && holes_.begin()->first < position_) {
    const auto [start, end] = *holes_.begin();
    holes_.erase(holes_.begin());
    if (end > position_) holes_[position_] = end;
    (void)start;
  }
  assign_position_ = std::max(assign_position_, position_);
  for (auto& worker : workers_state_) {
    Lease& lease = worker.lease;
    if (lease.active && lease.generation == generation_ && !lease.restart && lease.end <= position_) {
      restart_lease_locked(lease);
    }
  }
}

void PrefetchReader::restart_lease_locked(Lease& lease) {
  lease.restart = true;
  if (lease.stream != nullptr) lease.stream->abort();
  cv_.notify_all();
}

void PrefetchReader::add_wait_locked(double ms) {
  count_locked([&](Stats& stats) { stats.wait_ms += ms; });
  review_wait_ms_ += ms;
  measure_wait_ms_ += ms;
  starve_wait_ms_ += ms;
}

void PrefetchReader::note_lane_rate_locked(double rate) {
  lane_rate_ = lane_rate_ > 0 ? (lane_rate_ + rate) / 2 : rate;
}

void PrefetchReader::account_locked() {
  if (!started_) return;
  const auto now = Clock::now();
  const double dt = ms_between(accounted_at_, now);
  accounted_at_ = now;
  std::size_t queued = 0;
  for (const auto& [start, chunk] : chunks_) {
    if (chunk->end() > position_) ++queued;
    (void)start;
  }
  int busy = 0;
  for (const auto& worker : workers_state_) {
    if (worker.lease.active && worker.lease.connected && worker.lease.generation == generation_) ++busy;
  }
  const std::int64_t front = download_front_locked();
  const bool full = read_started_ && error_.empty() && front < size_ &&
                    front - position_ >= budget_ - static_cast<std::int64_t>(options_.block_size);
  count_locked([&](Stats& stats) {
    stats.elapsed_ms += dt;
    stats.busy_ms += busy * dt;
    stats.queued_ms += static_cast<double>(queued) * dt;
    if (full) stats.full_ms += dt;
  });
  if (full) {
    review_full_ms_ += dt;
    measure_full_ms_ += dt;
  }
  if (now - tuned_at_ >= std::chrono::milliseconds(100)) {
    tuned_at_ = now;
    tune_locked(now);
  }
  if (now - window_at_ < kStatsWindow) return;
  const std::int64_t received = received_.load();
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：近 " + fixed(window_.elapsed_ms / 1000) + " 秒 现在 " +
                std::to_string(want_) + " 条，" + describe_locked(window_, received - window_received_));
  window_ = Stats{};
  window_received_ = received;
  window_at_ = now;
}

bool PrefetchReader::all_connected_locked() const {
  for (int i = 0; i < want_; ++i) {
    const Lease& lease = workers_state_[static_cast<std::size_t>(i)].lease;
    if (!lease.active || lease.generation != generation_) {
      if (can_lease_locked(i)) return false;
      continue;
    }
    // 单连接收尾的那条还没换成带范围的请求。
    if (!lease.connected || (lease.open_ended && lease.end < size_)) return false;
  }
  return true;
}

std::string PrefetchReader::speeds_locked(double elapsed_ms) const {
  std::string text;
  for (const auto& worker : workers_state_) {
    const std::int64_t got = worker.received - worker.mark;
    if (got <= 0) continue;
    if (!text.empty()) text += " / ";
    text += fixed(static_cast<double>(got) / (1 << 20) / (elapsed_ms / 1000));
  }
  return text.empty() ? "" : "，每条 " + text + " MB/s";
}

long long PrefetchReader::remember_locked(bool failed, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(options_.tuning->mu);
  long long delay = 0;
  if (failed) {
    const int failures = ++options_.tuning->failures;
    delay = static_cast<long long>(options_.backoff_ms) * (failures == 1 ? 1 : failures == 2 ? 2 : 5);
    options_.tuning->next_probe = now + std::chrono::milliseconds(delay);
  } else {
    options_.tuning->failures = 0;
  }
  options_.tuning->connections = want_;
  return delay;
}

void PrefetchReader::set_want_locked(int connections, Clock::time_point now) {
  if (connections == want_) return;
  const int old = want_;
  want_ = connections;
  changed_at_ = now;
  if (old == 1 && connections > 1) {
    multi_since_ = now;
    // 没有终点的请求改不了范围：在大约 2 秒后能下到的地方收尾，之后的范围派给各条连接。
    for (auto& worker : workers_state_) {
      Lease& lease = worker.lease;
      if (!lease.active || lease.generation != generation_ || !lease.open_ended || lease.end < size_) continue;
      const double speed = rate_avg_ > 0 ? rate_avg_ : worker.speed;
      std::int64_t cut = lease.at + static_cast<std::int64_t>(speed * options_.settle_ms * 2 / 3);
      if (lease.chunk) cut = std::max(cut, lease.chunk->end());
      cut = std::min(size_, align_up(std::max(cut, lease.at)));
      lease.end = cut;
      if (assign_position_ >= size_) assign_position_ = cut;
    }
  }
  // 减掉的连接马上断开，没下的部分交给留下的连接；不等它下完，免得慢的那条堵在解压器前面。
  for (int i = connections; i < static_cast<int>(workers_state_.size()); ++i) {
    Lease& lease = workers_state_[static_cast<std::size_t>(i)].lease;
    if (lease.active && lease.generation == generation_ && !lease.restart) restart_lease_locked(lease);
  }
  cv_.notify_all();
}

void PrefetchReader::start_tune_locked(Tune direction, int level, double base_rate, Clock::time_point now) {
  if (level == want_) return;
  tune_ = direction;
  tune_from_ = want_;
  tune_base_ = base_rate;
  measuring_ = false;
  measure_rounds_ = 0;
  PANPLAY_DEBUG("下载 #" + std::to_string(id_) + "：试 " + std::to_string(level) + " 条（现在 " +
                std::to_string(want_) + " 条，总 " + per_second(base_rate) + "）");
  set_want_locked(level, now);
}

void PrefetchReader::tune_locked(Clock::time_point now) {
  if (stop_) return;
  check_stalls_locked(now);
  if (!options_.adaptive || !error_.empty()) return;
  const std::string head = "下载 #" + std::to_string(id_) + "：";
  if (tune_ == Tune::steady) {
    const double elapsed = ms_between(review_from_, now);
    if (elapsed < options_.review_ms) return;
    const std::int64_t received = received_.load();
    const double speed = static_cast<double>(received - review_received_) / elapsed;
    const double wait = review_wait_ms_ / elapsed;
    const double full = review_full_ms_ / elapsed;
    const double consumed = static_cast<double>(total_.delivered - review_delivered_) / elapsed;
    review_from_ = now;
    review_received_ = received;
    review_delivered_ = total_.delivered;
    review_wait_ms_ = 0;
    review_full_ms_ = 0;
    rate_avg_ = speed;
    if (ms_between(changed_at_, now) < options_.settle_ms) return;
    if (wait > 0.2 && want_ < options_.connections) {
      bool allowed = false;
      {
        std::lock_guard<std::mutex> lock(options_.tuning->mu);
        allowed = now >= options_.tuning->next_probe;
      }
      if (allowed) {
        start_tune_locked(Tune::up, level_above(want_, options_.connections), speed, now);
        return;
      }
    }
    if (want_ > 1 && wait < 0.05 && full > 0.5) {
      // 只减到够解压器读的条数（留三成余量）：一档档往下减会减过头，解压器反过来等下载。
      const double lane = lane_rate_ > 0 ? lane_rate_ : speed / want_;
      int lower = want_;
      if (lane > 0) {
        const int needed = std::max(1, static_cast<int>(std::ceil(consumed * 1.3 / lane)));
        lower = 1;
        for (const int level : kLevels) {
          if (level >= needed) {
            lower = level;
            break;
          }
        }
        lower = std::min(lower, want_);
      }
      if (lower < want_) {
        log_info(head + "解压跟不上（缓冲满 " + percent(full, 1) + "，解压器读走 " + per_second(consumed) +
                 "，每条约 " + per_second(lane) + "），减到 " + std::to_string(lower) + " 条");
        set_want_locked(lower, now);
        std::lock_guard<std::mutex> lock(options_.tuning->mu);
        options_.tuning->connections = want_;
        return;
      }
    }
    if (want_ > 1 && ms_between(multi_since_, now) >= 12.0 * options_.review_ms) {
      start_tune_locked(Tune::down, level_below(want_), speed, now);
    }
    return;
  }

  if (!measuring_) {
    if (all_connected_locked() || ms_between(changed_at_, now) >= 2.0 * options_.settle_ms) {
      measuring_ = true;
      measure_from_ = now;
      measure_received_ = received_.load();
      measure_wait_ms_ = 0;
      measure_full_ms_ = 0;
      for (auto& worker : workers_state_) worker.mark = worker.received;
    }
    return;
  }
  const double elapsed = ms_between(measure_from_, now);
  if (elapsed < options_.measure_ms) return;
  const std::int64_t received = received_.load();
  const double speed = static_cast<double>(received - measure_received_) / elapsed;
  const double wait = measure_wait_ms_ / elapsed;
  const double full = measure_full_ms_ / elapsed;
  const double ratio = tune_base_ > 0 ? speed / tune_base_ : 10;
  const std::string tried = std::to_string(want_);
  const std::string before = std::to_string(tune_from_);
  const std::string numbers = "总 " + per_second(tune_base_) + " → " + per_second(speed) + speeds_locked(elapsed);
  const auto settle = [&] {
    tune_ = Tune::steady;
    measuring_ = false;
    review_from_ = now;
    review_received_ = received;
    review_delivered_ = total_.delivered;
    review_wait_ms_ = 0;
    review_full_ms_ = 0;
    rate_avg_ = speed;
  };
  if (tune_ == Tune::up) {
    // 多连接时最远的那段可能已经顶到缓冲上限，解压器却还在等最前面那段，这时瓶颈仍在下载。
    if (full > 0.3 && wait < 0.2) {
      set_want_locked(tune_from_, now);
      const long long delay = remember_locked(true, now);
      log_info(head + "试 " + tried + " 条：" + numbers + "，缓冲常满、瓶颈不在下载，退回 " + before + " 条，" +
               std::to_string(delay / 1000) + " 秒后再试");
      settle();
      return;
    }
    if (ratio >= 1.3) {
      remember_locked(false, now);
      log_info(head + "试 " + tried + " 条：" + numbers + "，确实更快，保留");
      settle();
      if (wait > 0.2 && want_ < options_.connections) start_tune_locked(Tune::up, level_above(want_, options_.connections), speed, now);
      return;
    }
    // 旧连接没变慢、只是新连接特别慢：碰上了卡住的请求，不是分流。给新连接换一条再量一次。
    if (measure_rounds_ == 0) {
      double old_sum = 0;
      double new_min = -1;
      for (int i = 0; i < want_; ++i) {
        const Worker& worker = workers_state_[static_cast<std::size_t>(i)];
        const double got = static_cast<double>(worker.received - worker.mark) / elapsed;
        if (i < tune_from_) {
          old_sum += got;
        } else {
          new_min = new_min < 0 ? got : std::min(new_min, got);
        }
      }
      if (old_sum >= 0.85 * tune_base_ && new_min >= 0 && new_min < 0.25 * old_sum / std::max(1, tune_from_)) {
        PANPLAY_DEBUG(head + "试 " + tried + " 条：" + numbers + "，旧连接没变慢，新连接只有 " + per_second(new_min) +
                      "，换一条再量");
        for (int i = tune_from_; i < want_; ++i) {
          Lease& lease = workers_state_[static_cast<std::size_t>(i)].lease;
          if (lease.active && lease.generation == generation_ && !lease.restart) restart_lease_locked(lease);
        }
        ++measure_rounds_;
        measuring_ = false;
        changed_at_ = now;
        return;
      }
    }
    if (ratio >= 1.15 && measure_rounds_ == 0) {
      ++measure_rounds_;
      measure_from_ = now;
      measure_received_ = received;
      measure_wait_ms_ = 0;
      measure_full_ms_ = 0;
      for (auto& worker : workers_state_) worker.mark = worker.received;
      return;
    }
    set_want_locked(tune_from_, now);
    const long long delay = remember_locked(true, now);
    log_info(head + "试 " + tried + " 条：" + numbers + "，只是分流，退回 " + before + " 条，" +
             std::to_string(delay / 1000) + " 秒后再试");
    settle();
    return;
  }
  multi_since_ = now;
  if (ratio >= 0.9) {
    remember_locked(false, now);
    log_info(head + "减到 " + tried + " 条：" + numbers + "，没变慢，保留");
  } else {
    set_want_locked(tune_from_, now);
    log_info(head + "减到 " + tried + " 条：" + numbers + "，变慢了，回到 " + before + " 条");
  }
  settle();
}

// 解压器在等的那段如果卡住或明显拖后腿，断开它，没下的部分放回去马上重新派。
void PrefetchReader::check_stalls_locked(Clock::time_point now) {
  if (const double span = ms_between(starve_from_, now); span >= options_.stall_ms) {
    starving_ = starve_wait_ms_ >= span / 2;
    starve_from_ = now;
    starve_wait_ms_ = 0;
  }
  for (auto& worker : workers_state_) {
    Lease& lease = worker.lease;
    if (!lease.active || lease.generation != generation_ || !lease.connected || lease.restart) continue;
    if (lease.blocked) {
      lease.check_at = lease.at;
      lease.check_time = now;
      lease.check_rate = -1;
      continue;
    }
    const double span = ms_between(lease.check_time, now);
    if (span >= options_.stall_ms) {
      lease.check_rate = static_cast<double>(lease.at - lease.check_at) / span;
      lease.check_at = lease.at;
      lease.check_time = now;
      lease.peak_rate = std::max(lease.peak_rate, lease.check_rate);
      if (!starving_) note_lane_rate_locked(lease.check_rate);
    }
  }
  if (!waiting_) return;
  if (!starving_ && ms_between(waiting_since_, now) < std::min(1000.0, options_.stall_ms / 2.0)) return;
  std::int64_t need = position_;
  if (const ChunkPtr chunk = chunk_at_locked(position_)) need = chunk->filled_end();
  Lease* slow = nullptr;
  std::vector<double> others;
  for (auto& worker : workers_state_) {
    Lease& lease = worker.lease;
    if (!lease.active || lease.generation != generation_ || !lease.connected || lease.restart) continue;
    if (need >= lease.start && need < lease.end) {
      slow = &lease;
    } else if (lease.check_rate >= 0) {
      others.push_back(lease.check_rate);
    }
  }
  if (slow == nullptr || slow->check_rate < 0) return;
  // 先和正在下的其他连接比；没有（单连接，或者其他连接都被缓冲堵着）就和卡住之前的单条速度、
  // 这个请求自己之前的速度比，都没有才退到总速度。
  double normal = rate_avg_;
  double limit = 0.10;
  if (!others.empty()) {
    std::nth_element(others.begin(), others.begin() + static_cast<std::ptrdiff_t>(others.size() / 2), others.end());
    normal = others[others.size() / 2];
    limit = 0.25;
  } else if (lane_rate_ > 0 || slow->peak_rate > 0) {
    normal = std::max(lane_rate_, slow->peak_rate);
    limit = 0.25;
  }
  if (slow->check_rate >= kStuckBytesPerMs && (normal <= 0 || slow->check_rate >= limit * normal)) return;
  PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：段 @" + mb(slow->start) + " 下到 @" + mb(slow->at) + " 时只有 " +
                per_second(slow->check_rate) + (normal > 0 ? "（正常 " + per_second(normal) + "）" : std::string()) +
                "，解压器在等，断开重下");
  count_locked([](Stats& stats) { ++stats.restarts; });
  slow->check_time = now;
  slow->check_rate = -1;
  restart_lease_locked(*slow);
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
  text += "，下完 " + std::to_string(stats.segments) + " 段";
  if (stats.segments > 0) {
    text += "（平均 " + fixed(static_cast<double>(stats.segment_ms) / 1000 / stats.segments) + " 秒，最长 " +
            fixed(static_cast<double>(stats.segment_ms_max) / 1000) + " 秒）";
  }
  if (stats.open_failures > 0) text += "，打开失败 " + std::to_string(stats.open_failures) + " 次";
  if (stats.breaks > 0) text += "，中途断开 " + std::to_string(stats.breaks) + " 次";
  if (stats.restarts > 0) text += "，卡住重派 " + std::to_string(stats.restarts) + " 次";
  if (stats.resets > 0) text += "，跳转重来 " + std::to_string(stats.resets) + " 次";
  return text;
}

bool PrefetchReader::detail_locked() {
  if (detail_left_ <= 0) return false;
  --detail_left_;
  return true;
}

void PrefetchReader::start() {
  std::lock_guard<std::mutex> lock(mu_);
  start_locked();
}

void PrefetchReader::start_locked() {
  if (started_) return;
  started_ = true;
  const auto now = Clock::now();
  accounted_at_ = window_at_ = review_from_ = changed_at_ = tuned_at_ = multi_since_ = starve_from_ = now;
  window_received_ = review_received_ = received_.load();
  cv_.notify_all();
}

bool PrefetchReader::fully_assigned() {
  std::lock_guard<std::mutex> lock(mu_);
  return started_ && error_.empty() && lowest_unassigned_locked() >= size_;
}

std::size_t PrefetchReader::read(std::uint8_t* dst, std::size_t size, std::string* error) {
  if (size == 0 || position_ >= size_) return 0;
  PhaseScope timing(InstallPhase::download);
  std::unique_lock<std::mutex> lock(mu_);
  start_locked();
  if (!read_started_) {
    // 提前下载那段时间读取方还没来：先按"没满"记完账，再开始算缓冲满，也不拿来判断该加还是该减连接。
    account_locked();
    read_started_ = true;
    review_from_ = changed_at_ = Clock::now();
    review_received_ = received_.load();
    review_delivered_ = total_.delivered;
    review_wait_ms_ = review_full_ms_ = 0;
  }
  bool waiting = false;
  Clock::time_point waited_from;
  const auto note_wait = [&] {
    if (!waiting) return;
    const auto now = Clock::now();
    add_wait_locked(ms_between(waited_from, now));
    waited_from = now;
  };
  while (true) {
    const ChunkPtr chunk = chunk_at_locked(position_);
    if (chunk && position_ < chunk->filled_end()) {
      note_wait();
      waiting_ = false;
      const std::size_t n = std::min<std::size_t>(size, static_cast<std::size_t>(chunk->filled_end() - position_));
      std::memcpy(dst, chunk->data.data() + (position_ - chunk->start), n);
      position_ += static_cast<std::int64_t>(n);
      count_locked([&](Stats& stats) { stats.delivered += static_cast<std::int64_t>(n); });
      if (position_ >= chunk->end()) {
        account_locked();
        prune_locked();
        cv_.notify_all();
      }
      return n;
    }
    if (!error_.empty()) {
      note_wait();
      waiting_ = false;
      *error = error_;
      return 0;
    }
    if (cancel_ && cancel_->cancelled()) {
      note_wait();
      waiting_ = false;
      *error = "interrupted";
      return 0;
    }
    if (!waiting) {
      waiting = true;
      waited_from = Clock::now();
      if (!waiting_) {
        waiting_ = true;
        waiting_since_ = waited_from;
      }
    }
    cv_.wait_for(lock, std::chrono::milliseconds(100));
    note_wait();
    account_locked();
  }
}

std::string PrefetchReader::seek(std::int64_t position) {
  if (position < 0 || position > size_) return "读取位置越界";
  if (position == position_) return "";
  std::lock_guard<std::mutex> lock(mu_);
  account_locked();
  waiting_ = false;
  // 往回跳到还留着的数据里（比如重读刚读完的文件头）：不重连。
  if (position < position_ && available_locked(position, position_)) {
    position_ = position;
    return "";
  }
  // 往前跳不远：不重连，跳过的数据丢掉；重新连接要一秒左右，还得重新提速。
  const std::int64_t keep_reach = static_cast<std::int64_t>(options_.block_size) * 2;
  if (position > position_ && error_.empty() && position <= download_front_locked() + keep_reach) {
    position_ = position;
    skip_ahead_locked();
    cv_.notify_all();
    return "";
  }
  if (log_enabled(LogLevel::debug)) {
    std::int64_t dropped = 0;
    for (const auto& [start, chunk] : chunks_) {
      if (chunk->filled_end() > position_) dropped += chunk->filled_end() - std::max(chunk->start, position_);
      (void)start;
    }
    PANPLAY_DEBUG("预读 #" + std::to_string(id_) + "：跳转 " + mb(position_) + " -> " + mb(position) +
                  "，丢掉缓冲里没读的 " + mb(dropped) + "，" + std::to_string(in_flight_) + " 个请求作废，重新开始下载");
  }
  count_locked([](Stats& stats) { ++stats.resets; });
  detail_left_ = options_.connections;
  const auto now = Clock::now();
  if (tune_ != Tune::steady) {
    want_ = tune_from_;
    tune_ = Tune::steady;
    measuring_ = false;
  }
  changed_at_ = now;
  starving_ = false;
  starve_from_ = now;
  starve_wait_ms_ = 0;
  ++generation_;
  chunks_.clear();
  holes_.clear();
  error_.clear();
  position_ = position;
  assign_position_ = position;
  for (auto& worker : workers_state_) {
    if (worker.lease.stream != nullptr) worker.lease.stream->abort();
  }
  for (AbortSignal* signal : open_aborts_) signal->abort();
  cv_.notify_all();
  return "";
}

}  // namespace panplay
