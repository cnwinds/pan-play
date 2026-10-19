#pragma once

#include "panplay/drive.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panplay {

// 下载测速：从网盘读数据、读到就丢，不解压也不写盘。先用一条连接，再逐档加到几条同时读，
// 看单连接和总带宽各能跑多快。百度给单条连接的速度会浮动，某一档没涨不代表再加也不涨，所以每档都量。
struct SpeedTestOptions {
  std::vector<int> levels = {1, 2, 4, 8};
  int settle_ms = 1000;   // 每档连上以后先等这么久（避开 TCP 起步）再开始量
  int single_ms = 8000;   // 单连接量这么久
  int step_ms = 4000;     // 多连接每档量这么久
  int connect_ms = 15000; // 这一档的连接这么久还没收到数据就不等了
};

struct SpeedTestStep {
  int connections = 0;
  int connected = 0;  // 开始量时真正收到过数据的条数，可能少于 connections
  double rate = 0;    // 字节/秒
};

struct SpeedTestStatus {
  // idle、finding（找测速用的文件）、testing、done、error
  std::string phase = "idle";
  std::string file_name;
  std::int64_t file_size = 0;
  std::vector<int> levels;
  std::vector<SpeedTestStep> steps;
  // 正在量的这一档：连接数和量到现在的平均速度；不在量时 connections 为 0。
  SpeedTestStep measuring;
  int connections = 0;
  double current = 0;  // 最近 1 秒的总速度
  std::int64_t bytes = 0;
  double seconds = 0;
  double first_byte_ms = 0;
  // stopped：用户中途停下。
  std::string ended;
  std::string error;
};

class SpeedTest {
 public:
  ~SpeedTest();
  // 在游戏库目录里找一个够大的文件开始测速。已经在测时返回原因。
  std::string start(std::shared_ptr<DrivePlugin> drive, std::string library_path, SpeedTestOptions options = {});
  void stop();
  SpeedTestStatus status();
  bool running();

 private:
  struct Worker {
    std::thread thread;
    std::atomic<std::int64_t> received{0};
    std::atomic<bool> connected{false};
    AbortSignal open_abort;
    std::mutex stream_mu;
    ByteStream* stream = nullptr;
  };

  void run(std::shared_ptr<DrivePlugin> drive, std::string library_path, SpeedTestOptions options);
  void read_loop(DrivePlugin& drive, const DriveEntry& file, Worker& worker, int index);
  // 睡到 deadline 或者被停下，期间每 200 毫秒刷新一次近况。返回 false 表示被停下。
  bool wait_until(std::chrono::steady_clock::time_point deadline);
  void refresh();
  void request_stop();
  std::int64_t received() const;
  int connected() const;

  std::mutex mu_;
  std::condition_variable cv_;
  SpeedTestStatus status_;
  std::string last_error_;
  bool running_ = false;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::chrono::steady_clock::time_point began_;
  std::vector<std::pair<std::chrono::steady_clock::time_point, std::int64_t>> recent_;
  std::chrono::steady_clock::time_point measure_from_;
  std::int64_t measure_mark_ = 0;
};

}  // namespace panplay
