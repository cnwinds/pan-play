#pragma once

#include "panplay/drive.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panplay {

// 同一次安装里各卷共用的调速结论：定下来的连接数、试探失败了几次、什么时候能再试。见 ADR 0005。
struct PrefetchTuning {
  std::mutex mu;
  int connections = 1;
  int failures = 0;
  std::chrono::steady_clock::time_point next_probe{};
};

struct PrefetchOptions {
  // 收到的数据放进这么大的缓冲块，缓冲最多领先读取方 block_size × max_blocks。
  std::size_t block_size = 8u << 20;
  std::size_t max_blocks = 16;
  // 一次连续出错后最多重连几次，间隔从 retry_base_ms 开始翻倍。
  int max_retries = 5;
  int retry_base_ms = 1000;
  // adaptive 时是连接数上限：先用一条读到底，一条吃不满、多开确实更快才往上加；否则固定用这么多条。
  int connections = 8;
  bool adaptive = true;
  // 调速用的时间（毫秒），自测里调小。
  int settle_ms = 3000;     // 连接数变了以后等这么久才开始判断
  int measure_ms = 3000;    // 一次试探量这么久
  int review_ms = 10000;    // 观察期
  int backoff_ms = 60000;   // 试探失败后第一次退避这么久，第二次翻倍，之后 5 倍
  int segment_ms = 4000;    // 多连接时一段至少下这么久
  int stall_ms = 5000;      // 解压器等着的那段慢成这样持续这么久，就断开重下
  // 默认每份选项一个；复制选项时一起复制，所以同一次安装的各卷共用。
  std::shared_ptr<PrefetchTuning> tuning = std::make_shared<PrefetchTuning>();
};

// 后台线程按顺序读一个网盘文件，读取方从内存取数据。连接数和每段多长按实测调整，见 ADR 0005。
// 只能由一个线程使用 read/seek。
class PrefetchReader {
 public:
  PrefetchReader(DrivePlugin& drive, std::string file_id, std::int64_t size, const CancelToken* cancel,
                 PrefetchOptions options = {});
  ~PrefetchReader();
  PrefetchReader(const PrefetchReader&) = delete;
  PrefetchReader& operator=(const PrefetchReader&) = delete;

  // 返回读到的字节数，0 表示文件结束。取消时 error 为 "interrupted"。
  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error);
  std::string seek(std::int64_t position);
  std::int64_t position() const { return position_; }
  std::int64_t size() const { return size_; }
  // 日志里的预读编号（预读 #N），调用方记下它对应哪个分卷。
  int id() const { return id_; }
  // 现在用几条连接。
  int connections();

 private:
  using Clock = std::chrono::steady_clock;

  struct Chunk {
    std::int64_t start = 0;
    std::vector<std::uint8_t> data;
    std::size_t filled = 0;
    std::int64_t end() const { return start + static_cast<std::int64_t>(data.size()); }
    std::int64_t filled_end() const { return start + static_cast<std::int64_t>(filled); }
  };
  using ChunkPtr = std::shared_ptr<Chunk>;

  // 一条连接手上的一段 [start, end)，下到 at。open_ended 的请求没有终点，服务器会一直发到文件末尾，
  // 所以中途收尾（end 改小）时要断开连接。
  struct Lease {
    bool active = false;
    std::uint64_t generation = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t at = 0;
    bool open_ended = false;
    // 卡住或拖后腿：断开，没下的部分放回去重新派。
    bool restart = false;
    bool connected = false;
    // 缓冲满了在等读取方腾地方：这时不收数据不是网慢，不量速度。held 是这段里等过。
    bool blocked = false;
    bool held = false;
    Clock::time_point began;
    Clock::time_point first_byte;
    std::int64_t connect_at = 0;
    ByteStream* stream = nullptr;
    ChunkPtr chunk;
    // 卡住检查：每 stall_ms 量一次这段时间的速度（字节/毫秒），-1 是还没量过。peak_rate 是这个请求量到过的最快。
    std::int64_t check_at = 0;
    Clock::time_point check_time;
    double check_rate = -1;
    double peak_rate = 0;
  };
  struct Worker {
    Lease lease;
    // 这条连接最近的速度（字节/毫秒），每段下完更新。
    double speed = 0;
    // 这条连接一共收到的字节，试探时按它算每条的速度。
    std::int64_t received = 0;
    std::int64_t mark = 0;
  };

  // 下载诊断：打开详细日志时每 10 秒记一行近况，结束时记一行总账。各项含义见 ADR 0001「诊断日志」和 ADR 0005。
  struct Stats {
    std::int64_t delivered = 0;
    int requests = 0;
    int open_failures = 0;
    int breaks = 0;
    int resets = 0;
    int restarts = 0;
    long long open_ms = 0;
    long long open_ms_max = 0;
    int segments = 0;
    long long segment_ms = 0;
    long long segment_ms_max = 0;
    double elapsed_ms = 0;
    double wait_ms = 0;
    double busy_ms = 0;
    double full_ms = 0;
    double queued_ms = 0;
  };

  enum class Tune { steady, up, down };

  void run_worker(int index);
  void worker_loop(int index, AbortSignal& signal);
  void run_lease(int index, AbortSignal& signal, bool detailed);
  // 在 generation 没变时清掉 signal 上次的中止状态；变了返回 false。
  bool arm_open(AbortSignal& signal, std::uint64_t generation);
  bool wait_retry(int attempt, std::uint64_t generation);
  bool stale(std::uint64_t generation);

  // 以下都要持有 mu_。
  std::int64_t lowest_unassigned_locked() const;
  bool can_lease_locked(int index) const;
  void make_lease_locked(int index);
  std::int64_t segment_length_locked(int index) const;
  void release_locked(Lease& lease);
  void add_hole_locked(std::int64_t start, std::int64_t end);
  ChunkPtr new_chunk_locked(std::int64_t start, std::int64_t length);
  ChunkPtr chunk_at_locked(std::int64_t position) const;
  bool available_locked(std::int64_t from, std::int64_t to) const;
  std::int64_t download_front_locked() const;
  void prune_locked();
  void skip_ahead_locked();
  void restart_lease_locked(Lease& lease);
  void add_wait_locked(double ms);
  // 在改动连接、缓冲之前调用：把上次到现在这段按当时的状态记账，满 10 秒写一行，顺便调速。
  void account_locked();
  void tune_locked(Clock::time_point now);
  void check_stalls_locked(Clock::time_point now);
  void note_lane_rate_locked(double rate);
  void start_tune_locked(Tune direction, int level, double base_rate, Clock::time_point now);
  void set_want_locked(int connections, Clock::time_point now);
  // 把定下来的连接数记到各卷共用的 tuning 上；failed 时记一次失败，返回要退避多少毫秒。
  long long remember_locked(bool failed, Clock::time_point now);
  bool all_connected_locked() const;
  std::string speeds_locked(double elapsed_ms) const;
  template <class Apply>
  void count_locked(Apply apply) {
    apply(total_);
    apply(window_);
  }
  std::string describe_locked(const Stats& stats, std::int64_t received) const;
  // 这次请求要不要逐条记：每次从头开始下的前几个请求都记，看得出连接是不是同时建起来的。
  bool detail_locked();

  DrivePlugin& drive_;
  const std::string file_id_;
  const std::int64_t size_;
  const CancelToken* cancel_;
  const PrefetchOptions options_;
  const std::int64_t budget_;
  const int id_;

  std::int64_t position_ = 0;

  std::mutex mu_;
  std::condition_variable cv_;
  // 收到的数据按起点排好；读取方读过的只留最近一块，往回跳一小段（比如重读文件头）时不用重连。
  std::map<std::int64_t, ChunkPtr> chunks_;
  // 每块都重新分配几 MB、用完就释放，装一个游戏要来回几千次，堆会越来越碎；换下来的内存在这里留着复用。
  std::vector<std::vector<std::uint8_t>> spare_;
  // 还没派出去的范围：assign_position_ 之后的全部，加上中途收回来的空洞。
  std::int64_t assign_position_ = 0;
  std::map<std::int64_t, std::int64_t> holes_;
  std::vector<Worker> workers_state_;
  std::uint64_t generation_ = 0;
  bool started_ = false;
  bool stop_ = false;
  std::string error_;
  int in_flight_ = 0;
  // 每个后台线程一个，用来中止还在建立中的连接。
  std::vector<AbortSignal*> open_aborts_;
  double open_ms_avg_ = 200;
  Clock::time_point waiting_since_{};
  bool waiting_ = false;
  // 读取方最近一个检查期（stall_ms）里有一半以上时间在等数据。慢慢来数据的那段隔一会儿交一点，
  // 读取方不会连续等很久，只看连续等了多久认不出它。
  bool starving_ = false;
  Clock::time_point starve_from_{};
  double starve_wait_ms_ = 0;
  // 读取方不缺数据时量到的单条连接速度（字节/毫秒）。卡住时总速度跟着掉、其他连接被缓冲堵着，
  // 拿它们比认不出卡住的那段，要和卡住之前的正常速度比。
  double lane_rate_ = 0;

  // 调速状态。
  int want_ = 1;
  Tune tune_ = Tune::steady;
  int tune_from_ = 1;
  double tune_base_ = 0;
  bool measuring_ = false;
  int measure_rounds_ = 0;
  Clock::time_point changed_at_;
  Clock::time_point measure_from_;
  std::int64_t measure_received_ = 0;
  double measure_wait_ms_ = 0;
  double measure_full_ms_ = 0;
  Clock::time_point review_from_;
  std::int64_t review_received_ = 0;
  double review_wait_ms_ = 0;
  double review_full_ms_ = 0;
  double rate_avg_ = 0;
  Clock::time_point multi_since_;
  Clock::time_point tuned_at_;

  // 后台线程读网络时不拿锁，收到的字节数单独计。
  std::atomic<std::int64_t> received_{0};
  std::int64_t window_received_ = 0;
  Stats total_;
  Stats window_;
  Clock::time_point accounted_at_;
  Clock::time_point window_at_;
  int detail_left_ = 0;

  std::vector<std::thread> workers_;
};

}  // namespace panplay
