#pragma once

#include "panplay/drive.hpp"

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

 private:
  struct Block {
    std::int64_t start = 0;
    std::vector<std::uint8_t> data;
    std::size_t used = 0;
  };

  void worker_loop(AbortSignal& signal);
  void range_worker_loop(AbortSignal& signal);
  void run_worker(void (PrefetchReader::*loop)(AbortSignal&));
  // 在 generation 没变时清掉 signal 上次的中止状态；变了返回 false。
  bool arm_open(AbortSignal& signal, std::uint64_t generation);
  bool wait_retry(int attempt, std::uint64_t generation);
  bool stale(std::uint64_t generation);
  std::int64_t buffered_end_locked() const;

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
  std::vector<std::thread> workers_;
};

}  // namespace panplay
