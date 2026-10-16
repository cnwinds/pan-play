#include "panplay/trace.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(PANPLAY_PS5)
extern "C" int sceKernelAvailableFlexibleMemorySize(std::size_t* size);
#endif

namespace panplay {
namespace {

std::atomic<bool> enabled{false};
const auto started = std::chrono::steady_clock::now();
std::atomic<int> next_thread{1};

int thread_number() {
  thread_local const int number = next_thread++;
  return number;
}

// 信号处理里也会调用，只能用 write 这类异步信号安全的调用。
void write_raw(const char* data, std::size_t size) {
#if defined(_WIN32)
  _write(2, data, static_cast<unsigned>(size));
#else
  while (size > 0) {
    const ssize_t n = ::write(STDERR_FILENO, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
#endif
}

void on_crash_signal(int sig) {
  char text[96];
  std::size_t length = 0;
  const auto append = [&](const char* part) {
    while (*part && length < sizeof(text)) text[length++] = *part++;
  };
  append("!!! 收到信号 ");
  char digits[12];
  int count = 0;
  for (int value = sig < 0 ? 0 : sig; count == 0 || value > 0; value /= 10) digits[count++] = static_cast<char>('0' + value % 10);
  while (count > 0 && length < sizeof(text)) text[length++] = digits[--count];
  append("，进程结束\n");
  write_raw(text, length);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

void on_terminate() {
  std::string what = "没有正在处理的异常";
  if (const std::exception_ptr error = std::current_exception()) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
      what = "非 std::exception 类型";
    }
  }
  trace("!!! 未捕获的异常，进程结束: " + what);
  std::abort();
}

}  // namespace

void enable_trace() {
  enabled = true;
  thread_number();
}

bool trace_enabled() { return enabled; }

void trace(const std::string& message) {
  if (!enabled) return;
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const int ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  const double uptime = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  char head[96];
  std::snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d.%03d +%.3fs [t%d] ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms, uptime, thread_number());
  const std::string line = head + message + "\n";
  write_raw(line.data(), line.size());
}

void install_crash_trace() {
  for (const int sig : {SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGTERM}) std::signal(sig, on_crash_signal);
#if !defined(_WIN32)
  for (const int sig : {SIGBUS, SIGHUP, SIGSYS}) std::signal(sig, on_crash_signal);
#endif
  std::set_terminate(on_terminate);
  std::atexit([] { trace("进程调用 exit 退出"); });
}

void start_trace_heartbeat() {
  std::thread([] {
    for (int beat = 1;; ++beat) {
      std::this_thread::sleep_for(std::chrono::seconds(beat <= 24 ? 5 : 60));
      trace("心跳 " + memory_summary());
    }
  }).detach();
}

std::string memory_summary() {
  std::string out;
#if !defined(_WIN32)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) out = "峰值内存 " + std::to_string(usage.ru_maxrss / 1024) + " MB";
#endif
#if defined(PANPLAY_PS5)
  std::size_t flexible = 0;
  if (sceKernelAvailableFlexibleMemorySize(&flexible) == 0) {
    out += (out.empty() ? "" : "，") + std::string("剩余可用内存 ") + std::to_string(flexible >> 20) + " MB";
  }
#endif
  return out;
}

}  // namespace panplay
