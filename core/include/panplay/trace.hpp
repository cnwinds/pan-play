#pragma once

#include <chrono>
#include <string>

namespace panplay {

// 运行轨迹：每行带时间、启动后秒数和线程号，不经缓冲直接写到 stderr，进程被杀前写下的都留得住。
// 默认关闭；PS5 上启动时打开（stderr 接到 panplay.log），其他平台设环境变量 PANPLAY_TRACE 打开。
void enable_trace();
bool trace_enabled();
void trace(const std::string& message);

// 收到崩溃信号、有未捕获的异常、调用 exit 时各记一行。被 SIGKILL 杀掉时记不下，只能看最后一行停在哪。
void install_crash_trace();

// 后台定时记一行心跳和内存占用：开头两分钟每 5 秒一次，之后每分钟一次。
void start_trace_heartbeat();

// 峰值内存，PS5 上再加剩余可用内存；取不到时为空。
std::string memory_summary();

inline long long ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace panplay
