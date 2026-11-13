#pragma once

#include "download_scheduler.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace panplay {

// 用虚拟时间跑下载决策：造出网络（每条限速、共享瓶颈、首字节、慢连接、卡住、出错）和读取方，
// 每 10 毫秒推进一步，每步检查不变式。自测和 bench-download 都用它，结果可重现。
struct SimScenario {
  std::string name;
  std::vector<std::int64_t> extents;
  double lane_mbps = 10;
  // 所有连接共享的带宽，随时间变化时用 total_at。
  double total_mbps = 100;
  std::function<double(double)> total_at;
  double consume_mbps = 100;
  double first_byte = 0.2;
  double first_byte_jitter = 0.1;
  // 连接断开过（被中止、段被截短没下完、出错）或第一次用时，首字节前还要多等这么久建连接。
  double connect = 0;
  // 每条连接的速度隔 wobble_period 秒左右换一次：wobble_dip 的概率掉到 wobble_low 以下，其余在 0.6～1.4 倍之间。0 是不换。
  double wobble_period = 0;
  double wobble_dip = 0;
  double wobble_low = 0.3;
  // 每个请求有这个概率整段都慢（乘 straggler_factor）、中途卡死、中途出错。
  double straggler_chance = 0;
  double straggler_factor = 0.1;
  double stall_chance = 0;
  double error_chance = 0;
  // 读取方读到 first 时跳到 second。
  std::vector<std::pair<std::int64_t, std::int64_t>> seeks;
  SchedulerOptions options;
  LaneMemory memory;
  double time_limit = 900;
  std::uint64_t seed = 1;
};

struct SimResult {
  std::string error;
  double seconds = 0;
  // 网络和读取方都不浪费时的最短用时。
  double bound = 0;
  DownloadScheduler::Totals totals;
  int final_lanes = 1;
  int most_lanes = 1;
  int lane_changes = 0;
  // 新建的连接（含第一次）。
  int connections = 0;
  LaneMemory memory;
  std::vector<std::string> log;
};

SimResult run_download_sim(const SimScenario& scenario);
std::string describe_sim(const SimScenario& scenario, const SimResult& result);
// 自测和 bench 共用的几个场景。
std::vector<SimScenario> download_sim_scenarios();

}  // namespace panplay
