#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace panplay {

// 记下的连接数结论（ADR 0009）：同一次安装里各层共用，服务端按设备存盘。时间是外壳单调时钟上的秒数。
struct LaneMemory {
  // 往上加时没被摊薄、或拿不准而留下的条数。下次安装开头直接用这么多条。
  int lanes = 1;
  // 定下这个条数时每条连接的速度（字节/秒），0 是不知道。开头复核时拿它判断每条是不是被摊薄了。
  double lane_rate = 0;
  // 往上加被判成分流、退回了：从几条往上试的、当时的总速度、第几次、什么时候能再试。
  int failed_from = 0;
  double failed_total = 0;
  int failures = 0;
  double retry_at = 0;
};

struct SchedulerOptions {
  // adaptive 时是条数上限，否则固定用这么多条。
  int max_lanes = 8;
  bool adaptive = true;
  // 缓冲上限：下载最多领先读取方这么多字节。
  std::int64_t window_cap = 128ll << 20;
  // 连续出错几次算失败，间隔从 retry_base 秒开始翻倍。
  int max_retries = 5;
  double retry_base = 1.0;
};

// 下载会话的决策：每条连接下哪段、什么时候分走慢连接的后半段、开几条、缓冲多大（ADR 0009）。
// 不碰线程、时钟和网络：时间由调用方传入，外壳（DownloadSession）和模拟器用同一套决策。
// 位置都是逻辑偏移：各文件（分卷）的数据首尾相接成一条流，一段不跨文件。
class DownloadScheduler {
 public:
  struct Lease {
    std::uint64_t id = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    // 单连接：请求不设终点，一直发到这段所在文件的末尾；中途收尾时要断开连接。
    bool open_ended = false;
    int extent = 0;
  };
  enum class Step { lease, wait, retire };
  struct Event {
    enum class Kind { abort, info, debug };
    Kind kind = Kind::debug;
    int lane = -1;
    std::string text;
  };
  // 累计的账，外壳按 10 秒和整个会话记日志。
  struct Totals {
    double elapsed = 0;
    double wait = 0;          // 读取方等数据
    double full = 0;          // 有连接被缓冲上限挡着
    double busy = 0;          // 在收数据的连接 × 秒
    double leased = 0;        // 手上有段的连接 × 秒（含等首字节）
    double first_byte = 0;    // 首字节等待合计（秒）
    double first_byte_max = 0;
    double segment_time = 0;
    double segment_max = 0;
    std::int64_t received = 0;
    std::int64_t delivered = 0;
    std::int64_t discarded = 0;  // 收到了却用不上：段被分走或断开时在路上的
    std::int64_t skipped = 0;    // 读取方往前跳过去的
    int requests = 0;
    int connects = 0;
    int segments = 0;
    int failures = 0;
    int steals = 0;
    int aborts = 0;
    int resets = 0;
  };

  DownloadScheduler(std::vector<std::int64_t> extent_sizes, SchedulerOptions options, LaneMemory memory,
                    std::int64_t position, double now);

  // 连接 lane 要下一段。wait 时 *wake_at 是最晚什么时候再问（等重试），其余时候有变化会被叫醒。
  Step next(int lane, double now, Lease* lease, double* wake_at);
  // 收到响应头。
  void connected(int lane, std::uint64_t lease, double now);
  // 这条连接现在可以再收多少字节：-1 是这段结束了（下完、被分走、被断开），0 是缓冲满了先等。
  std::int64_t allowance(int lane, std::uint64_t lease, double now);
  // 收到 bytes 字节，返回接收了多少（段被截短时只收前面的部分）。
  std::int64_t received(int lane, std::uint64_t lease, std::int64_t bytes, double now);
  // 不再下这段：下完、读到出错（error 非空）或连接被断开。没下的部分放回去。
  void released(int lane, std::uint64_t lease, const std::string* error, double now);

  std::int64_t position() const { return position_; }
  std::int64_t size() const { return size_; }
  // 从读取位置起连续收到的数据到哪。
  std::int64_t available() const;
  void consumed(std::int64_t to, double now);
  void set_waiting(bool waiting, double now);
  // 跳到 to：往前跳时已派的段能用就留着，其余作废；往回跳时全部作废、从 to 重新开始。
  void seek(std::int64_t to, double now);
  void set_window_cap(std::int64_t cap, double now);

  // 每 0.25 秒左右调一次：记账、调条数和缓冲、查慢连接。
  void tick(double now);
  std::vector<Event> take_events();

  int target_lanes() const { return lanes_; }
  std::int64_t window() const { return window_; }
  std::int64_t window_cap() const { return options_.window_cap; }
  const LaneMemory& memory() const { return memory_; }
  // 每改一次 memory 加一，外壳据此同步到各层共用的结论上。
  int memory_version() const { return memory_version_; }
  const std::string& error() const { return error_; }
  const Totals& totals() const { return totals_; }
  // 正在下载的字节（派出去还没被读走的范围）。
  std::int64_t buffered() const { return frontier_ - position_; }
  // 不变式：已派的段和空洞正好铺满 [读取位置, 派到哪)，互不重叠。出错时返回说明，自测用。
  std::string check() const;

 private:
  struct Range {
    std::int64_t end = 0;
    std::int64_t next = 0;
    int lane = -1;
  };
  struct Lane {
    bool leased = false;
    std::uint64_t lease = 0;
    std::int64_t key = -1;
    bool open_ended = false;
    bool connected = false;
    bool blocked = false;
    double requested_at = 0;
    double connected_at = 0;
    double last_bytes_at = 0;
    double unblocked_at = 0;
    double retry_at = 0;
    int failures = 0;
    double touched_at = -1e9;
    std::int64_t lease_bytes = 0;
    // (时间, 本段收到的字节) 采样，算最近两秒的速度。
    std::deque<std::pair<double, std::int64_t>> trail;
    double peak = 0;
    double rate = 0;
    double active = 0;
    std::int64_t sample_bytes = 0;
  };
  struct Second {
    double at = 0;
    double total = 0;
    double wait = 0;
    double full = 0;
    double consume = 0;
  };
  struct Span {
    double total = 0;
    double wait = 0;
    double full = 0;
    double consume = 0;
    int seconds = 0;
  };
  struct Trial {
    bool active = false;
    bool up = true;
    int from = 1;
    int to = 1;
    double started = 0;
    double base_total = 0;
    double base_lane = 0;
  };

  void account(double now);
  void sample(double now);
  void update_window(double now);
  void decide(double now);
  void judge_trial(double now);
  bool download_bound(const Span& recent) const;
  void start_trial(double now, int to, bool up, const Span& base, double base_lane);
  void set_lanes(int lanes, double now);
  void check_lanes(double now);
  bool steal_from(std::int64_t key, double need_in, double now, double* eta);
  void abort_lane(int lane);
  void detach_lane(int lane);
  void release_range(std::map<std::int64_t, Range>::iterator it, std::int64_t from);
  void unassign(std::int64_t start, std::int64_t end);
  void add_hole(std::int64_t start, std::int64_t end);
  std::int64_t segment_length(const Lane& lane) const;
  std::int64_t extent_end(std::int64_t offset) const;
  int extent_of(std::int64_t offset) const;
  int lane_cap() const;
  double lane_estimate() const;
  double recent_rate(const Lane& lane, double now, double window = 2.0) const;
  Span span(double from, double to) const;
  double lane_median(double from, double to) const;
  bool backoff_blocks(double now, double total) const;
  void remember(int lanes, double lane_rate);
  void log(Event::Kind kind, const std::string& text);
  std::string ranges_text() const;

  std::vector<std::int64_t> starts_;
  std::int64_t size_ = 0;
  SchedulerOptions options_;
  LaneMemory memory_;
  int memory_version_ = 0;

  std::int64_t position_ = 0;
  std::int64_t frontier_ = 0;
  std::map<std::int64_t, Range> ranges_;
  std::map<std::int64_t, std::int64_t> holes_;
  std::vector<Lane> lane_state_;
  std::uint64_t next_lease_ = 0;
  std::string error_;
  std::vector<Event> events_;

  int lanes_ = 1;
  std::int64_t window_ = 64ll << 20;
  bool waiting_ = false;
  double last_ = 0;
  double started_ = 0;
  double changed_at_ = 0;
  double reduced_at_ = -1e9;
  double reduce_span_ = 20;
  bool review_pending_ = false;
  Trial trial_;

  double sample_from_ = 0;
  std::int64_t interval_received_ = 0;
  std::int64_t interval_consumed_ = 0;
  double interval_wait_ = 0;
  double interval_full_ = 0;
  std::deque<Second> seconds_;
  std::deque<std::pair<double, double>> lane_samples_;
  double lane_estimate_ = 0;
  double first_byte_avg_ = 0.25;
  double consume_rate_ = 0;

  Totals totals_;
};

}  // namespace panplay
