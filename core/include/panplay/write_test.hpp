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

// 写盘测速：在存放位置用几种写法各写一个测试文件，写完刷到盘上再删掉，看哪种写法快、盘本身能写多快。
// 第一种走安装用的写入接口（FTP 存放位置只测这一种），其余直接调系统的 write，每次写的块大小不同。
struct WriteTestStep {
  std::string label;
  double rate = 0;  // 字节/秒，含最后刷盘的时间
  bool done = false;
};

struct WriteTestStatus {
  // idle、testing、done、error
  std::string phase = "idle";
  std::string where;
  std::vector<WriteTestStep> steps;
  int current = -1;          // 正在测第几种
  std::int64_t written = 0;  // 这一种已经写了多少
  std::int64_t total = 0;    // 每种写多少
  double current_rate = 0;   // 这一种到现在的平均速度（还没刷盘）
  // stopped：用户中途停下。
  std::string ended;
  std::string error;
};

class WriteTest {
 public:
  ~WriteTest();
  // 测试文件写在 destination.prefix 下。已经在测时返回原因。
  std::string start(const Destination& destination, std::int64_t bytes_per_step = 512LL << 20);
  void stop();
  WriteTestStatus status();
  bool running();

 private:
  void run(Destination destination);
  // 写满 total_ 字节并刷到盘上，返回错误，被停下时返回 kStopped。
  std::string run_sink(InstallSink& sink, const std::string& path, const std::string& local,
                       const std::vector<std::uint8_t>& block);
  std::string run_direct(const std::string& local, std::size_t chunk, const std::vector<std::uint8_t>& block);
  void begin_step(int index);
  void add_written(std::size_t bytes);
  void finish_step();

  std::mutex mu_;
  WriteTestStatus status_;
  std::int64_t total_ = 0;

  bool running_ = false;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::chrono::steady_clock::time_point step_began_;
};

}  // namespace panplay
