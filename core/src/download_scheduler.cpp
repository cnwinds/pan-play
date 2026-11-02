#include "download_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace panplay {
namespace {

constexpr int kLevels[] = {1, 2, 3, 4, 6, 8};
// 每秒记一个样本；条数变了以后等 kSettle 秒才开始判断。
constexpr double kSampleSeconds = 1.0;
constexpr double kSettle = 2.0;
// 一段请求刚开始的这么久 TCP 还在提速，不算进每条的速度。
constexpr double kLaneWarmup = 1.0;
// 往上加：每条没被摊薄最快 2 秒就留下；要退回得看满 10 秒。
constexpr double kQuickJudge = 2.0;
constexpr double kFullJudge = 10.0;
// 缓冲按总速度 × 6 秒算，段长按单条速度 × 4 秒算（首字节慢时 × 20 倍首字节）。
constexpr double kWindowSeconds = 6.0;
constexpr double kSegmentSeconds = 4.0;
constexpr std::int64_t kWindowFloor = 32ll << 20;
constexpr std::int64_t kWindowStart = 64ll << 20;
constexpr std::int64_t kMinSegment = 2ll << 20;
constexpr std::int64_t kAlign = 64ll << 10;
// 单连接被缓冲挡住后，空出这么多才接着收，免得一点一点地收。
constexpr std::int64_t kUnblockRoom = 1ll << 20;
// 低于 32 KB/s、或者两秒一个字节都没来，不管和谁比都算卡住。
constexpr double kStuckRate = 32.0 * 1024;
constexpr double kStuckSilence = 2.0;
// 预计比读取方要用的时候晚一秒以上才管；分出去至少省一秒（没有空着的连接、要断开别人时两秒）才分。
constexpr double kLateBy = 1.0;
constexpr double kStealGain = 1.0;
constexpr double kPreemptGain = 2.0;
constexpr double kStealCooldown = 3.0;
constexpr double kRestartCooldown = 10.0;
constexpr std::int64_t kMinSteal = 512ll << 10;
// 往上加被判成分流后，从同一个条数再往上试要等 1、2、5 分钟。
constexpr double kBackoff[] = {60, 120, 300};

std::int64_t align_up(std::int64_t value) { return (value + kAlign - 1) / kAlign * kAlign; }

std::string fixed(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", value);
  return text;
}

std::string mbps(double bytes_per_second) { return fixed(bytes_per_second / (1 << 20)) + " MB/s"; }
std::string mb(std::int64_t bytes) { return fixed(static_cast<double>(bytes) / (1 << 20)) + " MB"; }
std::string percent(double fraction) { return std::to_string(static_cast<int>(fraction * 100 + 0.5)) + "%"; }

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

int level_at_least(double needed, int cap) {
  for (const int level : kLevels) {
    if (level >= needed) return std::min(level, cap);
  }
  return cap;
}

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

}  // namespace

DownloadScheduler::DownloadScheduler(std::vector<std::int64_t> extent_sizes, SchedulerOptions options, LaneMemory memory,
                                     std::int64_t position, double now)
    : options_(options), memory_(memory) {
  std::int64_t at = 0;
  for (const std::int64_t size : extent_sizes) {
    starts_.push_back(at);
    at += std::max<std::int64_t>(0, size);
  }
  if (starts_.empty()) starts_.push_back(0);
  size_ = at;
  options_.max_lanes = std::clamp(options_.max_lanes, 1, 16);
  options_.window_cap = std::max<std::int64_t>(options_.window_cap, 4ll << 20);
  options_.max_retries = std::clamp(options_.max_retries, 0, 10);
  options_.retry_base = std::max(options_.retry_base, 0.001);
  lane_state_.resize(static_cast<std::size_t>(options_.max_lanes));
  position_ = frontier_ = std::clamp<std::int64_t>(position, 0, size_);
  last_ = started_ = changed_at_ = sample_from_ = now;
  if (options_.adaptive) {
    lanes_ = std::clamp(memory_.lanes, 1, lane_cap());
    review_pending_ = lanes_ > 1;
  } else {
    lanes_ = options_.max_lanes;
  }
  const std::int64_t floor = std::min(kWindowFloor, options_.window_cap);
  window_ = memory_.lane_rate > 0 ? static_cast<std::int64_t>(lanes_ * memory_.lane_rate * kWindowSeconds) : kWindowStart;
  window_ = std::max(window_, (lanes_ + 1) * kMinSegment);
  window_ = std::clamp(window_, floor, options_.window_cap);
}

DownloadScheduler::Step DownloadScheduler::next(int lane, double now, Lease* lease, double* wake_at) {
  account(now);
  *wake_at = now + 1;
  if (lane < 0 || lane >= static_cast<int>(lane_state_.size())) return Step::retire;
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (state.leased) return Step::wait;
  if (lane >= lanes_) {
    state.blocked = false;
    return Step::retire;
  }
  if (!error_.empty()) {
    state.blocked = false;
    return Step::wait;
  }
  if (state.retry_at > now) {
    state.blocked = false;
    *wake_at = state.retry_at;
    return Step::wait;
  }
  std::int64_t start = 0;
  std::int64_t end = 0;
  bool open_ended = false;
  if (!holes_.empty()) {
    // 先补空洞（出错放回来的、分出来的后半段）：它们离读取方最近。
    const auto hole = holes_.begin();
    start = hole->first;
    const std::int64_t hole_end = hole->second;
    const std::int64_t limit = std::min(hole_end, extent_end(start));
    const std::int64_t length = segment_length(state);
    end = std::min(limit, start + length);
    if (limit - end < length / 2) end = limit;
    holes_.erase(hole);
    if (end < hole_end) holes_[end] = hole_end;
  } else {
    if (frontier_ >= size_) {
      state.blocked = false;
      return Step::wait;
    }
    start = frontier_;
    const std::int64_t limit = extent_end(start);
    if (lanes_ == 1) {
      end = limit;
      open_ended = true;
    } else {
      const std::int64_t length = segment_length(state);
      end = std::min(limit, start + length);
      // 文件末尾剩不到半段时并进这一段，不单独派一个零头段。
      if (limit - end < length / 2) end = limit;
      // 缓冲空出一整段才派：只空出一点时等着，不领缓冲边上的零头段。
      if (position_ + window_ - start < end - start) {
        state.blocked = true;
        return Step::wait;
      }
    }
    frontier_ = end;
  }
  ranges_[start] = Range{end, start, lane};
  state.leased = true;
  state.lease = ++next_lease_;
  state.key = start;
  state.open_ended = open_ended;
  state.connected = false;
  state.blocked = false;
  state.requested_at = now;
  state.lease_bytes = 0;
  state.trail.clear();
  state.peak = 0;
  ++totals_.requests;
  *lease = Lease{state.lease, start, end, open_ended, extent_of(start)};
  return Step::lease;
}

void DownloadScheduler::connected(int lane, std::uint64_t lease, double now) {
  account(now);
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (!state.leased || state.lease != lease || state.connected) return;
  state.connected = true;
  state.connected_at = state.last_bytes_at = state.unblocked_at = now;
  const double waited = now - state.requested_at;
  first_byte_avg_ = first_byte_avg_ * 0.7 + waited * 0.3;
  ++totals_.connects;
  totals_.first_byte += waited;
  totals_.first_byte_max = std::max(totals_.first_byte_max, waited);
  state.trail.clear();
  state.trail.emplace_back(now, 0);
}

std::int64_t DownloadScheduler::allowance(int lane, std::uint64_t lease, double now) {
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (!state.leased || state.lease != lease) return -1;
  const auto it = ranges_.find(state.key);
  if (it == ranges_.end() || it->second.next >= it->second.end) return -1;
  std::int64_t limit = it->second.end - it->second.next;
  if (state.open_ended) {
    // 单连接的请求没有终点：缓冲满了就不从连接里读，靠 TCP 流量控制让服务器停着。
    const std::int64_t room = position_ + window_ - it->second.next;
    if (room <= 0 || (state.blocked && room < std::min(limit, kUnblockRoom))) {
      if (!state.blocked) {
        account(now);
        state.blocked = true;
      }
      return 0;
    }
    limit = std::min(limit, room);
  }
  if (state.blocked) {
    account(now);
    state.blocked = false;
    state.unblocked_at = now;
    state.trail.clear();
    state.trail.emplace_back(now, state.lease_bytes);
  }
  return limit;
}

std::int64_t DownloadScheduler::received(int lane, std::uint64_t lease, std::int64_t bytes, double now) {
  account(now);
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  const auto it = state.leased && state.lease == lease ? ranges_.find(state.key) : ranges_.end();
  if (it == ranges_.end()) {
    totals_.discarded += bytes;
    return 0;
  }
  Range& range = it->second;
  const std::int64_t accepted = std::clamp<std::int64_t>(range.end - range.next, 0, bytes);
  const std::int64_t behind = std::clamp<std::int64_t>(position_ - range.next, 0, accepted);
  range.next += accepted;
  totals_.received += accepted;
  totals_.skipped += behind;
  totals_.discarded += bytes - accepted;
  interval_received_ += accepted;
  state.lease_bytes += accepted;
  state.sample_bytes += accepted;
  if (accepted > 0) {
    state.last_bytes_at = now;
    state.failures = 0;
  }
  if (state.trail.empty() || now - state.trail.back().first >= 0.1) {
    state.trail.emplace_back(now, state.lease_bytes);
  } else {
    state.trail.back().second = state.lease_bytes;
  }
  while (state.trail.size() > 2 && state.trail[1].first <= now - 3) state.trail.pop_front();
  return accepted;
}

void DownloadScheduler::released(int lane, std::uint64_t lease, const std::string* error, double now) {
  account(now);
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (!state.leased || state.lease != lease) return;
  const auto it = ranges_.find(state.key);
  if (it != ranges_.end() && it->second.lane == lane) {
    Range& range = it->second;
    if (range.next >= range.end) {
      const double spent = now - state.requested_at;
      ++totals_.segments;
      totals_.segment_time += spent;
      totals_.segment_max = std::max(totals_.segment_max, spent);
      const double active = now - state.connected_at;
      if (state.connected && !state.open_ended && active >= 0.5 && state.lease_bytes > 0) {
        const double rate = static_cast<double>(state.lease_bytes) / active;
        state.rate = state.rate > 0 ? (state.rate + rate) / 2 : rate;
        // 段短时整秒的采样凑不齐，下完一段也算一个每条速度的样本。
        if (active >= 1.0) lane_samples_.emplace_back(now, rate);
      }
      range.lane = -1;
      if (range.end <= position_) ranges_.erase(it);
    } else {
      release_range(it, range.next);
    }
  }
  state.leased = false;
  state.key = -1;
  state.connected = false;
  state.blocked = false;
  state.open_ended = false;
  if (error != nullptr) {
    ++totals_.failures;
    ++state.failures;
    if (state.failures > options_.max_retries) {
      error_ = error->empty() ? "下载出错" : *error;
      log(Event::Kind::info, "连接 " + std::to_string(lane) + " 连续出错 " + std::to_string(state.failures) + " 次，不再重试：" + error_);
    } else {
      state.retry_at = now + options_.retry_base * static_cast<double>(1 << std::min(state.failures - 1, 10));
    }
  }
}

std::int64_t DownloadScheduler::available() const {
  std::int64_t at = position_;
  auto it = ranges_.upper_bound(at);
  if (it != ranges_.begin()) {
    const auto previous = std::prev(it);
    if (previous->second.end > at) it = previous;
  }
  while (it != ranges_.end() && it->first <= at) {
    const Range& range = it->second;
    if (range.next <= at) break;
    at = range.next;
    if (range.next < range.end) break;
    ++it;
  }
  return at;
}

void DownloadScheduler::consumed(std::int64_t to, double now) {
  account(now);
  to = std::min(to, size_);
  if (to <= position_) return;
  totals_.delivered += to - position_;
  interval_consumed_ += to - position_;
  position_ = to;
  frontier_ = std::max(frontier_, position_);
  for (auto it = ranges_.begin(); it != ranges_.end() && it->first < position_;) {
    if (it->second.end <= position_ && it->second.lane < 0) {
      it = ranges_.erase(it);
    } else {
      ++it;
    }
  }
  while (!holes_.empty() && holes_.begin()->first < position_) {
    const std::int64_t end = holes_.begin()->second;
    holes_.erase(holes_.begin());
    if (end > position_) holes_[position_] = end;
  }
}

void DownloadScheduler::set_waiting(bool waiting, double now) {
  if (waiting == waiting_) return;
  account(now);
  waiting_ = waiting;
}

void DownloadScheduler::seek(std::int64_t to, double now) {
  account(now);
  to = std::clamp<std::int64_t>(to, 0, size_);
  if (to == position_) return;
  std::int64_t dropped = 0;
  for (const auto& [start, range] : ranges_) {
    dropped += std::max<std::int64_t>(0, std::min(range.next, to < position_ ? range.next : to) - std::max(start, position_));
  }
  totals_.skipped += dropped;
  if (to < position_ || to >= frontier_) {
    int cut = 0;
    for (std::size_t k = 0; k < lane_state_.size(); ++k) {
      if (!lane_state_[k].leased) continue;
      detach_lane(static_cast<int>(k));
      ++cut;
    }
    log(Event::Kind::debug, "跳转 " + mb(position_) + " -> " + mb(to) + "，丢掉没读的 " + mb(dropped) + "，" + std::to_string(cut) +
                                " 个请求作废，从新位置重新下载");
    ranges_.clear();
    holes_.clear();
    position_ = frontier_ = to;
    ++totals_.resets;
    return;
  }
  // 往前跳、落在已派的范围里：跳过去的段作废，跨过跳转点的段离得近就读过去，远了断开、跳转点以后重新派。
  log(Event::Kind::debug, "往前跳 " + mb(position_) + " -> " + mb(to) + "，跳过 " + mb(dropped));
  position_ = to;
  while (!holes_.empty() && holes_.begin()->first < position_) {
    const std::int64_t end = holes_.begin()->second;
    holes_.erase(holes_.begin());
    if (end > position_) holes_[position_] = end;
  }
  const auto reach = std::max<std::int64_t>(4ll << 20, static_cast<std::int64_t>(lane_estimate()));
  for (auto it = ranges_.begin(); it != ranges_.end() && it->first < to;) {
    Range& range = it->second;
    if (range.end <= to) {
      if (range.lane >= 0 && range.next < range.end) detach_lane(range.lane);
      it = ranges_.erase(it);
      continue;
    }
    if (range.next < to && (range.lane < 0 || to - range.next > reach)) {
      const std::int64_t end = range.end;
      if (range.lane >= 0) detach_lane(range.lane);
      it = ranges_.erase(it);
      unassign(to, end);
      continue;
    }
    ++it;
  }
  frontier_ = std::max(frontier_, position_);
}

// 连接手上的段不要了（数据也不要）：作废这次请求，让外壳断开连接。
void DownloadScheduler::detach_lane(int lane) {
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (!state.leased) return;
  state.leased = false;
  state.key = -1;
  state.connected = state.blocked = state.open_ended = false;
  ++totals_.aborts;
  events_.push_back(Event{Event::Kind::abort, lane, {}});
}

void DownloadScheduler::set_window_cap(std::int64_t cap, double now) {
  account(now);
  options_.window_cap = std::max<std::int64_t>(cap, 4ll << 20);
  window_ = std::min(window_, options_.window_cap);
}

void DownloadScheduler::tick(double now) {
  account(now);
  if (now - sample_from_ >= kSampleSeconds) {
    sample(now);
    update_window(now);
    decide(now);
  }
  check_lanes(now);
}

std::vector<DownloadScheduler::Event> DownloadScheduler::take_events() {
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

void DownloadScheduler::account(double now) {
  const double dt = now - last_;
  if (dt <= 0) return;
  last_ = now;
  totals_.elapsed += dt;
  if (waiting_) {
    totals_.wait += dt;
    interval_wait_ += dt;
  }
  bool full = false;
  for (auto& lane : lane_state_) {
    if (lane.blocked) full = true;
    if (!lane.leased) continue;
    totals_.leased += dt;
    if (lane.connected && !lane.blocked) {
      totals_.busy += dt;
      lane.active += dt;
    }
  }
  if (full) {
    totals_.full += dt;
    interval_full_ += dt;
  }
}

void DownloadScheduler::sample(double now) {
  const double span_seconds = now - sample_from_;
  Second second;
  second.at = now;
  second.total = static_cast<double>(interval_received_) / span_seconds;
  second.wait = interval_wait_ / span_seconds;
  second.full = interval_full_ / span_seconds;
  second.consume = static_cast<double>(interval_consumed_) / span_seconds;
  seconds_.push_back(second);
  while (seconds_.size() > 200) seconds_.pop_front();
  const double reading = span_seconds - interval_wait_;
  if (reading >= 0.3 * span_seconds && interval_consumed_ > 0) {
    const double rate = static_cast<double>(interval_consumed_) / reading;
    consume_rate_ = consume_rate_ > 0 ? consume_rate_ * 0.7 + rate * 0.3 : rate;
  }
  for (auto& lane : lane_state_) {
    if (lane.leased && lane.connected && !lane.blocked && lane.active >= 0.8 * span_seconds &&
        now - std::max(lane.connected_at, lane.unblocked_at) >= kLaneWarmup + span_seconds) {
      lane_samples_.emplace_back(now, static_cast<double>(lane.sample_bytes) / lane.active);
    }
    lane.active = 0;
    lane.sample_bytes = 0;
  }
  while (!lane_samples_.empty() && lane_samples_.front().first < now - 60) lane_samples_.pop_front();
  if (const double estimate = lane_median(now - 30, now); estimate > 0) lane_estimate_ = estimate;
  interval_received_ = interval_consumed_ = 0;
  interval_wait_ = interval_full_ = 0;
  sample_from_ = now;
}

// 缓冲按总速度 × 6 秒：慢时少占内存，快时让每条连接都放得下一整段。
void DownloadScheduler::update_window(double now) {
  const Span recent = span(now - 5, now);
  const double speed = std::max(recent.total, lanes_ * lane_estimate());
  if (speed <= 0) return;
  std::int64_t target = static_cast<std::int64_t>(speed * kWindowSeconds);
  target = std::max(target, (lanes_ + 1) * kMinSegment);
  window_ = std::clamp(target, std::min(kWindowFloor, options_.window_cap), options_.window_cap);
}

void DownloadScheduler::decide(double now) {
  if (!options_.adaptive || !error_.empty()) return;
  if (trial_.active) {
    judge_trial(now);
    return;
  }
  if (now - changed_at_ < kSettle) return;
  // 开头用的是记下的条数：第一个观察期（10 秒）每条明显比记下的慢，就试着减，总速度没掉才留下。观察期里不做别的调整。
  if (review_pending_) {
    if (now - started_ < kSettle + kFullJudge) return;
    review_pending_ = false;
    const Span level = span(now - kFullJudge, now);
    const double lane = lane_median(now - kFullJudge, now);
    if (lanes_ > 1 && level.wait >= 0.2 && memory_.lane_rate > 0 && lane > 0 && lane < 0.6 * memory_.lane_rate) {
      const int lower = std::min(level_at_least(std::ceil(level.total / memory_.lane_rate), lanes_), level_below(lanes_));
      log(Event::Kind::info, "开头复核：每条只有 " + mbps(lane) + "（记下的是 " + mbps(memory_.lane_rate) + "），试着减到 " +
                                 std::to_string(lower) + " 条");
      start_trial(now, lower, false, level, lane);
      return;
    }
  }
  const int cap = lane_cap();
  // 读取方在等、缓冲没满、也没在攒（下多少读走多少）：下载是瓶颈，加一档。之前确认过的条数直接回去。
  const Span recent = span(now - 3, now);
  if (recent.seconds >= 3 && download_bound(recent) && lanes_ < cap) {
    const bool back = lanes_ < memory_.lanes;
    const int target = back ? std::min(memory_.lanes, cap) : level_above(lanes_, cap);
    const double level_from = std::max(changed_at_ + kSettle, now - kFullJudge);
    const Span level = span(level_from, now);
    if (target > lanes_ && level.seconds >= 2 && (back || !backoff_blocks(now, level.total))) {
      if (back && now - reduced_at_ < 60) reduce_span_ = std::min(160.0, reduce_span_ * 2);
      start_trial(now, target, true, level, lane_median(level_from, now));
      return;
    }
  }
  // 读取方几乎不等、缓冲常满：慢在解压或写盘，减到够用的条数（解压读走的速度加五成余量）。
  if (lanes_ > 1 && now - changed_at_ >= reduce_span_) {
    const Span bound = span(now - reduce_span_, now);
    const double lane = lane_estimate() > 0 ? lane_estimate() : bound.total / lanes_;
    if (bound.seconds >= reduce_span_ - 1 && bound.wait < 0.05 && bound.full >= 0.5 && lane > 0) {
      const int needed = level_at_least(std::ceil(bound.consume * 1.5 / lane), lanes_);
      if (needed < lanes_) {
        log(Event::Kind::info, "解压跟不上（近 " + std::to_string(static_cast<int>(reduce_span_)) + " 秒读取方等数据 " +
                                   percent(bound.wait) + "、缓冲满 " + percent(bound.full) + "，读走 " + mbps(bound.consume) +
                                   "，每条约 " + mbps(lane) + "），减到 " + std::to_string(needed) + " 条");
        reduced_at_ = now;
        set_lanes(needed, now);
        return;
      }
    }
  }
}

// 读取方等数据超过两成、缓冲没满、下来的数据也没在缓冲里攒着：快不了是因为下载。
// 下得比读得快还在等，是读取方追上了最前面那段，靠分走慢连接的后半段解决，加连接没用。
bool DownloadScheduler::download_bound(const Span& recent) const {
  return recent.wait >= 0.2 && recent.full < 0.5 && recent.total <= recent.consume * 1.2 + 256 * 1024;
}

void DownloadScheduler::start_trial(double now, int to, bool up, const Span& base, double base_lane) {
  trial_.active = true;
  trial_.up = up;
  trial_.from = lanes_;
  trial_.to = to;
  trial_.started = now;
  trial_.base_total = base.total;
  trial_.base_lane = base_lane > 0 ? base_lane : base.total / lanes_;
  log(Event::Kind::debug, std::string(up ? "试 " : "减到 ") + std::to_string(to) + " 条（现在 " + std::to_string(lanes_) +
                              " 条，总 " + mbps(base.total) + "，每条 " + mbps(trial_.base_lane) + "）");
  set_lanes(to, now);
}

// 加连接的判定只比每条的速度：叠加时每条不变，分流时每条摊薄到 旧条数/新条数。两个时刻的总速度噪声太大，只用来兜底。
void DownloadScheduler::judge_trial(double now) {
  const double elapsed = now - trial_.started;
  if (elapsed < kSettle) return;
  const double from = trial_.started + kSettle;
  const Span got = span(from, now);
  const double lane = lane_median(from, now);
  const std::string numbers = "总 " + mbps(trial_.base_total) + " → " + mbps(got.total) + "，每条 " +
                              mbps(trial_.base_lane) + " → " + (lane > 0 ? mbps(lane) : std::string("-"));
  const std::string head = std::string(trial_.up ? "试 " : "减到 ") + std::to_string(trial_.to) + " 条：";
  if (!trial_.up) {
    if (elapsed < kSettle + kFullJudge) return;
    trial_.active = false;
    if (got.total >= 0.9 * trial_.base_total || got.wait < 0.1) {
      memory_.lanes = lanes_;
      if (lane > 0) memory_.lane_rate = lane;
      ++memory_version_;
      log(Event::Kind::info, head + numbers + "，总速度没掉，保留");
    } else {
      log(Event::Kind::info, head + numbers + "，总速度掉了，回到 " + std::to_string(trial_.from) + " 条");
      set_lanes(trial_.from, now);
    }
    return;
  }
  if (got.seconds >= 2 && got.wait < 0.1 && got.full > 0.3) {
    log(Event::Kind::info, head + numbers + "，缓冲常满、下载已经够读，保留");
    trial_.active = false;
    return;
  }
  // 完全分流时每条降到 旧条数/新条数；降幅不到四成算没被摊薄，超过六成算摊薄了，中间拿不准。
  const double shared = static_cast<double>(trial_.from) / trial_.to;
  if (elapsed >= kSettle + kQuickJudge && lane > 0 && lane >= trial_.base_lane * (shared + (1 - shared) * 0.6)) {
    memory_.failures = 0;
    memory_.failed_from = 0;
    remember(lanes_, lane);
    log(Event::Kind::info, head + numbers + "，每条没被摊薄，保留");
    trial_.active = false;
    const Span recent = span(now - 2, now);
    const int cap = lane_cap();
    if (recent.seconds >= 2 && download_bound(recent) && lanes_ < cap && !backoff_blocks(now, got.total)) {
      // 每条一点没少、读取方大半时间在等：离瓶颈还远，跳两档。
      int target = level_above(lanes_, cap);
      if (recent.wait >= 0.5 && lane >= 0.9 * trial_.base_lane) target = level_above(target, cap);
      start_trial(now, target, true, got, lane);
    }
    return;
  }
  if (elapsed < kSettle + kFullJudge) return;
  trial_.active = false;
  const bool diluted = lane > 0 && lane <= trial_.base_lane * (shared + (1 - shared) * 0.4);
  const bool flat = got.total <= trial_.base_total * 1.1;
  if (diluted && flat) {
    ++memory_.failures;
    memory_.failed_from = trial_.from;
    memory_.failed_total = trial_.base_total;
    const double delay = kBackoff[std::min(memory_.failures, 3) - 1];
    memory_.retry_at = now + delay;
    if (memory_.lanes > trial_.from) memory_.lanes = trial_.from;
    ++memory_version_;
    log(Event::Kind::info, head + numbers + "，只是分流，退回 " + std::to_string(trial_.from) + " 条，" +
                               std::to_string(static_cast<int>(delay)) + " 秒后再试");
    set_lanes(trial_.from, now);
    return;
  }
  remember(lanes_, lane);
  log(Event::Kind::info, head + numbers + (diluted ? "，每条摊薄了但总速度涨了，保留" : "，拿不准，保留"));
}

void DownloadScheduler::set_lanes(int lanes, double now) {
  lanes = std::clamp(lanes, 1, options_.max_lanes);
  if (lanes == lanes_) return;
  const int old = lanes_;
  lanes_ = lanes;
  changed_at_ = now;
  if (old == 1 && lanes > 1) {
    // 单连接那个没有终点的请求改不了范围：在大约 2 秒后能下到的地方收尾，后面的派给各条连接。
    for (auto& lane : lane_state_) {
      if (!lane.leased || !lane.open_ended) continue;
      const auto it = ranges_.find(lane.key);
      if (it == ranges_.end()) continue;
      double rate = recent_rate(lane, now);
      if (rate <= 0) rate = lane_estimate();
      if (rate <= 0) rate = 4.0 * (1 << 20);
      const std::int64_t cut = align_up(std::max(it->second.next, position_) + static_cast<std::int64_t>(rate * 2));
      if (cut < it->second.end) {
        const std::int64_t end = it->second.end;
        it->second.end = cut;
        unassign(cut, end);
      }
      lane.open_ended = false;
    }
  }
  if (lanes_ > old && lane_estimate() > 0) {
    const auto wanted = static_cast<std::int64_t>(lanes_ * lane_estimate() * kWindowSeconds);
    window_ = std::max(window_, std::min(options_.window_cap, wanted));
  }
}

// 只看读取方马上要用的两段：预计比要用的时候晚、这条连接又明显慢（或卡住），就把它的后半段分给别的连接。
void DownloadScheduler::check_lanes(double now) {
  for (auto& lane : lane_state_) {
    if (!lane.leased) continue;
    const double rate = recent_rate(lane, now);
    if (rate > lane.peak) lane.peak = rate;
  }
  if (!error_.empty() || position_ >= size_) return;
  const std::int64_t head = available();
  if (head >= size_) return;
  bool unassigned = head >= frontier_;
  if (!unassigned) {
    auto hole = holes_.upper_bound(head);
    if (hole != holes_.begin()) unassigned = std::prev(hole)->second > head;
  }
  if (unassigned) {
    // 读取方要的那段没人在下（出错放回来的），空着的连接马上会来接；没有空着的，就断开最靠后的那段先来接。
    for (int k = 0; k < lanes_; ++k) {
      const Lane& lane = lane_state_[static_cast<std::size_t>(k)];
      if (!lane.leased && lane.retry_at <= now) return;
    }
    if (!waiting_ && (consume_rate_ <= 0 || static_cast<double>(head - position_) / consume_rate_ > 2)) return;
    int last = -1;
    std::int64_t last_start = head;
    for (const auto& [start, range] : ranges_) {
      if (range.lane >= 0 && start > last_start && range.next < range.end) {
        last = range.lane;
        last_start = start;
      }
    }
    if (last < 0 || now - lane_state_[static_cast<std::size_t>(last)].touched_at < kStealCooldown) return;
    log(Event::Kind::debug, "读取方要的 @" + mb(head) + " 没有连接在下，断开最靠后的段 @" + mb(last_start) + " 先来接");
    lane_state_[static_cast<std::size_t>(last)].touched_at = now;
    abort_lane(last);
    return;
  }
  auto it = ranges_.upper_bound(head);
  if (it == ranges_.begin()) return;
  --it;
  if (it->second.end <= head) return;
  // 从读取方马上要的那段往后排：每段要用的时间是前面各段都下完、读取方也读到这里的时候。
  std::vector<std::int64_t> keys;
  for (; it != ranges_.end(); ++it) {
    if (it->second.lane >= 0 && it->second.next < it->second.end) keys.push_back(it->first);
  }
  double ready = 0;
  for (const std::int64_t key : keys) {
    const auto found = ranges_.find(key);
    if (found == ranges_.end()) continue;
    const double reach = consume_rate_ > 0 ? static_cast<double>(found->second.next - position_) / consume_rate_ : 0;
    double eta = 0;
    if (steal_from(key, std::max(ready, reach), now, &eta)) return;
    ready = std::max(ready, eta);
  }
}

bool DownloadScheduler::steal_from(std::int64_t key, double need_in, double now, double* eta) {
  *eta = 0;
  const auto it = ranges_.find(key);
  if (it == ranges_.end() || it->second.lane < 0 || it->second.next >= it->second.end) return false;
  Range& range = it->second;
  const int k = range.lane;
  Lane& lane = lane_state_[static_cast<std::size_t>(k)];
  const std::int64_t remaining = range.end - range.next;
  // 和正在下的其他连接比；没有可比的，就和这条连接自己之前的速度、这次量到的每条速度比。
  std::vector<double> others;
  for (std::size_t i = 0; i < lane_state_.size(); ++i) {
    if (static_cast<int>(i) == k || !lane_state_[i].leased) continue;
    const double rate = recent_rate(lane_state_[i], now);
    if (rate > 0) others.push_back(rate);
  }
  const bool compared = !others.empty();
  double normal = median(others);
  if (normal <= 0) normal = std::max(lane.peak, lane_estimate());
  double rate = recent_rate(lane, now);
  bool stuck = false;
  if (lane.connected) {
    // 别的连接都在正常收、只有它一秒没动静，也按卡住算。
    const double silence = now - std::max({lane.last_bytes_at, lane.connected_at, lane.unblocked_at});
    stuck = !lane.blocked && silence >= (compared ? kStuckSilence / 2 : kStuckSilence);
    if (rate >= 0 && rate < kStuckRate) stuck = true;
  } else {
    stuck = now - lane.requested_at >= std::max(3.0, 5 * first_byte_avg_);
  }
  // 两秒的速度还量不出来时先看一秒的：差到四分之一以下就不用再等一秒。
  double ratio = compared ? 0.5 : 0.25;
  if (rate < 0 && compared) {
    rate = recent_rate(lane, now, 1.0);
    ratio = 0.25;
  }
  const bool slow = !stuck && rate >= 0 && normal > 0 && rate < ratio * normal;
  if (stuck) {
    *eta = 1e9;
  } else if (rate > 0) {
    *eta = static_cast<double>(remaining) / rate;
  } else if (normal > 0) {
    *eta = std::max(0.0, first_byte_avg_ - (now - lane.requested_at)) + static_cast<double>(remaining) / normal;
  }
  if ((!stuck && !slow) || lane.blocked) return false;
  if (!stuck && now - lane.touched_at < (compared ? kStealCooldown : kRestartCooldown)) return false;
  if (*eta - need_in < kLateBy) return false;
  int idle = -1;
  for (int i = 0; i < lanes_; ++i) {
    const Lane& other = lane_state_[static_cast<std::size_t>(i)];
    if (i != k && !other.leased && other.retry_at <= now) {
      idle = i;
      break;
    }
  }
  int victim = -1;
  std::int64_t victim_start = range.end;
  for (const auto& [start, other] : ranges_) {
    if (other.lane >= 0 && other.lane != k && start >= victim_start && other.next < other.end) {
      victim = other.lane;
      victim_start = start;
    }
  }
  const std::string where = "段 @" + mb(key) + " 下到 @" + mb(range.next);
  const std::string speed = stuck ? std::string("卡住了") : "只有 " + mbps(std::max(rate, 0.0)) + "（正常 " + mbps(normal) + "）";
  if (stuck || (idle < 0 && victim < 0)) {
    // 卡住了，或者只有这一条又明显变慢：断开换一条连接，没下的部分放回去（空着的连接或它自己马上来接）。
    if (!stuck && compared) return false;
    log(Event::Kind::debug, where + " 时" + speed + "，读取方 " + fixed(std::max(0.0, need_in)) + " 秒后要用，断开重下剩下的 " +
                                mb(remaining));
    lane.touched_at = now;
    ++totals_.steals;
    abort_lane(k);
    return true;
  }
  // 切点让两边同时下完：慢的留前面一截，快的从切点接手（要先等一个首字节）。
  const double fast = normal;
  const double v = std::max(rate, 1.0);
  const double g = first_byte_avg_;
  std::int64_t cut = range.next + static_cast<std::int64_t>(v * (static_cast<double>(remaining) + g * fast) / (v + fast));
  cut = std::clamp(align_up(cut), range.next, range.end);
  if (range.end - cut < kMinSteal) return false;
  const double after = std::max(static_cast<double>(cut - range.next) / v, g + static_cast<double>(range.end - cut) / fast);
  const double gain = *eta - after;
  if (gain < (idle >= 0 ? kStealGain : kPreemptGain)) return false;
  log(Event::Kind::debug, where + " 时" + speed + "，读取方 " + fixed(std::max(0.0, need_in)) + " 秒后要用，后 " +
                              mb(range.end - cut) + " 分给" + (idle >= 0 ? std::string("空着的连接") : "断开最靠后的段 @" + mb(victim_start) + " 的连接") +
                              "，预计早 " + fixed(gain) + " 秒");
  const std::int64_t end = range.end;
  range.end = cut;
  unassign(cut, end);
  lane.touched_at = now;
  ++totals_.steals;
  if (idle < 0) {
    lane_state_[static_cast<std::size_t>(victim)].touched_at = now;
    abort_lane(victim);
  }
  return true;
}

void DownloadScheduler::abort_lane(int lane) {
  Lane& state = lane_state_[static_cast<std::size_t>(lane)];
  if (!state.leased) return;
  const auto it = ranges_.find(state.key);
  if (it != ranges_.end() && it->second.lane == lane) release_range(it, it->second.next);
  detach_lane(lane);
}

// 段在 from 处结束：没下的 [from, end) 放回去，段本身只留收到的部分。
void DownloadScheduler::release_range(std::map<std::int64_t, Range>::iterator it, std::int64_t from) {
  Range& range = it->second;
  const std::int64_t end = range.end;
  from = std::clamp(from, it->first, end);
  range.end = from;
  range.next = std::min(range.next, from);
  range.lane = -1;
  unassign(std::max(from, position_), end);
  if (range.end <= std::max(it->first, position_)) ranges_.erase(it);
}

void DownloadScheduler::unassign(std::int64_t start, std::int64_t end) {
  if (start >= end) return;
  if (end == frontier_) {
    frontier_ = start;
    while (!holes_.empty() && holes_.rbegin()->second == frontier_) {
      frontier_ = holes_.rbegin()->first;
      holes_.erase(std::prev(holes_.end()));
    }
    frontier_ = std::max(frontier_, position_);
    return;
  }
  add_hole(start, end);
}

void DownloadScheduler::add_hole(std::int64_t start, std::int64_t end) {
  if (start >= end) return;
  if (const auto next = holes_.find(end); next != holes_.end()) {
    end = next->second;
    holes_.erase(next);
  }
  auto previous = holes_.lower_bound(start);
  if (previous != holes_.begin()) {
    --previous;
    if (previous->second == start) {
      start = previous->first;
      holes_.erase(previous);
    }
  }
  holes_[start] = end;
}

std::int64_t DownloadScheduler::segment_length(const Lane& lane) const {
  double rate = lane_estimate();
  if (rate <= 0) rate = lane.rate;
  if (rate <= 0) rate = 4.0 * (1 << 20);
  const double span_seconds = std::max(kSegmentSeconds, 20 * first_byte_avg_);
  const std::int64_t room = window_ / (lanes_ + 1);
  const auto wanted = static_cast<std::int64_t>(rate * span_seconds);
  return align_up(std::clamp(wanted, kMinSegment, std::max(kMinSegment, room)));
}

int DownloadScheduler::extent_of(std::int64_t offset) const {
  const auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
  return std::max(0, static_cast<int>(it - starts_.begin()) - 1);
}

std::int64_t DownloadScheduler::extent_end(std::int64_t offset) const {
  const auto index = static_cast<std::size_t>(extent_of(offset));
  return index + 1 < starts_.size() ? starts_[index + 1] : size_;
}

// 缓冲上限放得下几条连接各领一整段（至少 1.5 秒的量）：放不下时再加连接只会把段切碎。
int DownloadScheduler::lane_cap() const {
  if (!options_.adaptive) return options_.max_lanes;
  const auto piece = std::max<std::int64_t>(4ll << 20, static_cast<std::int64_t>(lane_estimate() * 1.5));
  const int by_window = static_cast<int>(options_.window_cap / piece) - 1;
  return std::clamp(by_window, 1, options_.max_lanes);
}

double DownloadScheduler::lane_estimate() const { return lane_estimate_ > 0 ? lane_estimate_ : memory_.lane_rate; }

// 最近 window 秒的速度；连上或从缓冲满恢复不到 window 秒、或者正被缓冲挡着时返回 -1。
double DownloadScheduler::recent_rate(const Lane& lane, double now, double window) const {
  if (!lane.leased || !lane.connected || lane.blocked) return -1;
  if (now - std::max(lane.connected_at, lane.unblocked_at) < window || lane.trail.empty()) return -1;
  const double from = now - window;
  std::int64_t before = lane.trail.front().second;
  for (auto it = lane.trail.rbegin(); it != lane.trail.rend(); ++it) {
    if (it->first <= from) {
      before = it->second;
      break;
    }
  }
  return static_cast<double>(lane.lease_bytes - before) / window;
}

DownloadScheduler::Span DownloadScheduler::span(double from, double to) const {
  Span out;
  for (const Second& second : seconds_) {
    if (second.at <= from || second.at > to) continue;
    out.total += second.total;
    out.wait += second.wait;
    out.full += second.full;
    out.consume += second.consume;
    ++out.seconds;
  }
  if (out.seconds > 0) {
    out.total /= out.seconds;
    out.wait /= out.seconds;
    out.full /= out.seconds;
    out.consume /= out.seconds;
  }
  return out;
}

double DownloadScheduler::lane_median(double from, double to) const {
  std::vector<double> values;
  for (const auto& [at, rate] : lane_samples_) {
    if (at > from && at <= to) values.push_back(rate);
  }
  return values.size() >= 3 ? median(std::move(values)) : 0;
}

// 退避只管从退回时那个条数往上试；条数变了、或者总速度比那时掉了三成、涨了两成，说明情况变了，不用等。
bool DownloadScheduler::backoff_blocks(double now, double total) const {
  return memory_.failed_from == lanes_ && now < memory_.retry_at && total >= 0.7 * memory_.failed_total &&
         total <= 1.2 * memory_.failed_total;
}

void DownloadScheduler::remember(int lanes, double lane_rate) {
  memory_.lanes = std::max(memory_.lanes, lanes);
  if (lane_rate > 0) memory_.lane_rate = lane_rate;
  ++memory_version_;
}

void DownloadScheduler::log(Event::Kind kind, const std::string& text) { events_.push_back(Event{kind, -1, text}); }

std::string DownloadScheduler::ranges_text() const {
  std::string text;
  for (const auto& [start, range] : ranges_) {
    text += " [" + std::to_string(start) + "," + std::to_string(range.next) + "," + std::to_string(range.end) + " #" +
            std::to_string(range.lane) + "]";
  }
  for (const auto& [start, end] : holes_) text += " (" + std::to_string(start) + "," + std::to_string(end) + ")";
  return text + " P=" + std::to_string(position_) + " F=" + std::to_string(frontier_);
}

std::string DownloadScheduler::check() const {
  if (position_ > frontier_ || frontier_ > size_) return "读取位置、派到哪不对：" + ranges_text();
  std::vector<std::pair<std::int64_t, std::int64_t>> pieces;
  for (const auto& [start, range] : ranges_) {
    if (range.next < start || range.next > range.end) return "段收到的位置越界：" + ranges_text();
    if (range.lane >= 0) {
      const Lane& lane = lane_state_[static_cast<std::size_t>(range.lane)];
      if (!lane.leased || lane.key != start) return "段和连接对不上：" + ranges_text();
    }
    if (range.end <= position_) {
      if (range.lane < 0 || range.next != range.end) return "读过的段没清掉：" + ranges_text();
      continue;
    }
    pieces.emplace_back(std::max(start, position_), range.end);
  }
  for (const auto& [start, end] : holes_) {
    if (start < position_ || end <= start) return "空洞越界：" + ranges_text();
    pieces.emplace_back(start, end);
  }
  std::sort(pieces.begin(), pieces.end());
  std::int64_t at = position_;
  for (const auto& [start, end] : pieces) {
    if (start != at) return "段和空洞没有正好铺满：" + ranges_text();
    at = end;
  }
  if (at != frontier_) return "段和空洞没有铺到派到的位置：" + ranges_text();
  for (std::size_t k = 0; k < lane_state_.size(); ++k) {
    const Lane& lane = lane_state_[k];
    if (!lane.leased) continue;
    const auto it = ranges_.find(lane.key);
    if (it != ranges_.end() && it->second.lane != static_cast<int>(k)) return "连接手上的段被别人拿着：" + ranges_text();
  }
  return "";
}

}  // namespace panplay
