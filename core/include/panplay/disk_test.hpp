#pragma once

#include "panplay/install.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panplay {

// 硬盘读写测速：在存放位置用安装时的写法写一个测试文件、刷到盘上，再绕过系统缓存读一遍，测完删掉。
// 看得出内置 SSD 或外接硬盘能写多快（装游戏时写盘的上限）、读多快。FTP 存放位置只测写。
struct DiskTestStep {
  std::string label;
  double rate = 0;  // 字节/秒，写入含最后刷盘的时间
  bool done = false;
};

struct DiskTestStatus {
  // idle、testing、done、error
  std::string phase = "idle";
  std::string where;
  std::vector<DiskTestStep> steps;
  int current = -1;        // 正在测第几项
  std::int64_t bytes = 0;  // 这一项已经写了或读了多少
  std::int64_t total = 0;  // 每项多少
  double current_rate = 0;
  // 系统不支持绕过缓存，读速度可能是内存速度。
  bool read_cached = false;
  // stopped：用户中途停下或开始安装。
  std::string ended;
  std::string error;
};

class DiskTest {
 public:
  ~DiskTest();
  // 测试文件写在 destination.prefix 下。已经在测时返回原因。
  std::string start(const Destination& destination, std::int64_t bytes = 1LL << 30);
  void stop();
  DiskTestStatus status();
  bool running();

 private:
  void run(Destination destination);
  // 返回错误，被停下时返回 kStopped。
  std::string run_write(InstallSink& sink, const std::string& path, const std::string& local);
  std::string run_read(const std::string& local);
  void begin_step(int index);
  void add_bytes(std::size_t bytes);
  void finish_step();

  std::mutex mu_;
  DiskTestStatus status_;
  std::int64_t total_ = 0;

  bool running_ = false;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::chrono::steady_clock::time_point step_began_;
};

}  // namespace panplay
