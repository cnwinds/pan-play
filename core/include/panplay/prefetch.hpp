#pragma once

#include "panplay/drive.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panplay {

// 默认值来自 PPSA05106 实测：8 条连接装 1.5 GB 用 26 秒，1 条 55 秒，16 条反而变慢。见 ADR 0001。
struct PrefetchOptions {
  std::size_t block_size = 8u << 20;
  std::size_t max_blocks = 16;
  // 一次连续出错后最多重连几次，间隔从 retry_base_ms 开始翻倍。
  int max_retries = 5;
  int retry_base_ms = 1000;
  // 大于 1 时多条连接并行下不同的块。max_blocks 同时限制排队和下载中的块数，要不小于 connections。
  int connections = 8;
};

// 后台线程按块顺序读一个网盘文件，读取方从内存取数据。
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

 private:
  struct Block {
    std::int64_t start = 0;
    std::vector<std::uint8_t> data;
    std::size_t used = 0;
  };

  // 下载诊断：打开详细日志时每 10 秒记一行近况，结束时记一行总账。各项含义见 ADR 0001「诊断日志」。
  struct Stats {
    std::int64_t delivered = 0;
    int requests = 0;
    int open_failures = 0;
    int breaks = 0;
    int resets = 0;
    long long open_ms = 0;
    long long open_ms_max = 0;
    int blocks = 0;
    long long block_ms = 0;
    long long block_ms_max = 0;
    double elapsed_ms = 0;
    double wait_ms = 0;
    double busy_ms = 0;
    double full_ms = 0;
    double queued_ms = 0;
  };

  void worker_loop(AbortSignal& signal);
  void range_worker_loop(AbortSignal& signal);
  void run_worker(void (PrefetchReader::*loop)(AbortSignal&));
  // 在 generation 没变时清掉 signal 上次的中止状态；变了返回 false。
  bool arm_open(AbortSignal& signal, std::uint64_t generation);
  bool wait_retry(int attempt, std::uint64_t generation);
  bool stale(std::uint64_t generation);
  std::int64_t buffered_end_locked() const;
  // 以下两个要持有 mu_。读完的块换下 history_，旧 history_ 的内存留给下一块用。
  void retire_locked(Block&& block);
  std::vector<std::uint8_t> take_buffer_locked();
  // 以下要持有 mu_。account_locked 在改动连接数、缓冲之前调用，把上次到现在这段按当时的状态记账，满 10 秒写一行。
  void account_locked();
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

  std::int64_t position_ = 0;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Block> blocks_;
  // 刚读完的块留着，往回跳一小段（比如重读文件头）时不用重新连接。
  Block history_;
  // 每块都重新分配几 MB、用完就释放，装一个游戏要来回几千次，堆会越来越碎；换下来的内存在这里留着复用。
  std::vector<std::vector<std::uint8_t>> spare_;
  std::int64_t fill_position_ = 0;
  std::uint64_t generation_ = 0;
  bool started_ = false;
  bool stop_ = false;
  std::string error_;
  ByteStream* active_stream_ = nullptr;
  // 多连接时用：下一块分给谁、下完但还没轮到的块、正在下的块数和连接。
  std::int64_t assign_position_ = 0;
  std::map<std::int64_t, Block> pending_;
  int in_flight_ = 0;
  std::vector<ByteStream*> range_streams_;
  // 每个后台线程一个，用来中止还在建立中的连接。
  std::vector<AbortSignal*> open_aborts_;

  const int id_;
  // 后台线程读网络时不拿锁，收到的字节数单独计。
  std::atomic<std::int64_t> received_{0};
  std::int64_t window_received_ = 0;
  Stats total_;
  Stats window_;
  std::chrono::steady_clock::time_point accounted_at_;
  std::chrono::steady_clock::time_point window_at_;
  int detail_left_ = 0;

  std::vector<std::thread> workers_;
};

}  // namespace panplay
