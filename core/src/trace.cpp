#include "panplay/trace.hpp"

#include "big_memory.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>
#include <io.h>
#include <psapi.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(PANPLAY_PS5)
extern "C" int sceKernelAvailableFlexibleMemorySize(std::size_t* size);

struct SceLibcMallocManagedSize {
  std::uint16_t size;
  std::uint16_t version;
  std::uint32_t reserved1;
  std::size_t maxSystemSize;
  std::size_t currentSystemSize;
  std::size_t maxInuseSize;
  std::size_t currentInuseSize;
};
extern "C" int malloc_stats_fast(SceLibcMallocManagedSize* stats);
#endif

namespace panplay {
namespace {

constexpr std::uint64_t kMaxLogBytes = 2u << 20;

std::atomic<int> current_level{static_cast<int>(LogLevel::warn)};
const auto started = std::chrono::steady_clock::now();
std::atomic<int> next_thread{1};

// 日志文件的描述符，没打开时为 -1，写到 stderr。信号处理里只读它，换文件时在锁里改。
std::atomic<int> log_fd{-1};
std::mutex file_mu;
std::filesystem::path file_path;
std::uint64_t file_bytes = 0;
bool capture_stdio_to_file = false;

int thread_number() {
  thread_local const int number = next_thread++;
  return number;
}

// 信号处理里也会调用，只能用 write 这类异步信号安全的调用。
void write_raw(const char* data, std::size_t size) {
  const int fd = log_fd.load();
#if defined(_WIN32)
  _write(fd >= 0 ? fd : 2, data, static_cast<unsigned>(size));
#else
  while (size > 0) {
    const ssize_t n = ::write(fd >= 0 ? fd : STDERR_FILENO, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
#endif
}

void close_fd(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  ::close(fd);
#endif
}

std::filesystem::path old_log_path() {
  std::filesystem::path name = file_path.stem();
  name += ".old";
  name += file_path.extension();
  return file_path.parent_path() / name;
}

// 调用方持有 file_mu。
bool reopen_locked() {
#if defined(_WIN32)
  const int fd = _wopen(file_path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  const int fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
  if (fd < 0) return false;
  std::error_code ec;
  const auto size = std::filesystem::file_size(file_path, ec);
  file_bytes = ec ? 0 : size;
#if !defined(_WIN32)
  if (capture_stdio_to_file) {
    std::fflush(nullptr);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
  }
#endif
  const int previous = log_fd.exchange(fd);
  if (previous >= 0) close_fd(previous);
  return true;
}

// 调用方持有 file_mu。Windows 上开着的文件改不了名，先关掉再改。
void rotate_locked() {
  const int previous = log_fd.exchange(-1);
  if (previous >= 0) close_fd(previous);
  std::error_code ec;
  std::filesystem::rename(file_path, old_log_path(), ec);
  reopen_locked();
}

const char* level_tag(LogLevel level) {
  switch (level) {
    case LogLevel::error: return "E";
    case LogLevel::warn: return "W";
    case LogLevel::info: return "I";
    case LogLevel::debug: return "D";
  }
  return "?";
}

void write_crash_line(int sig, const void* address) {
  char text[128];
  std::size_t length = 0;
  const auto append = [&](const char* part) {
    while (*part && length < sizeof(text)) text[length++] = *part++;
  };
  append("!!! 收到信号 ");
  char digits[20];
  int count = 0;
  for (int value = sig < 0 ? 0 : sig; count == 0 || value > 0; value /= 10) digits[count++] = static_cast<char>('0' + value % 10);
  while (count > 0 && length < sizeof(text)) text[length++] = digits[--count];
  if (address != nullptr) {
    append("，地址 0x");
    auto value = reinterpret_cast<std::uintptr_t>(address);
    for (count = 0; count == 0 || value > 0; value >>= 4) digits[count++] = "0123456789abcdef"[value & 0xF];
    while (count > 0 && length < sizeof(text)) text[length++] = digits[--count];
  }
  append("，进程结束\n");
  write_raw(text, length);
}

#if defined(_WIN32)
void on_crash_signal(int sig) {
  write_crash_line(sig, nullptr);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
#else
// 装在备用栈上（SA_ONSTACK），线程栈溢出时也能跑；SA_RESETHAND 让第二次收到同一信号时按默认处理结束进程。
void on_crash_signal(int sig, siginfo_t* info, void*) {
  write_crash_line(sig, info != nullptr ? info->si_addr : nullptr);
  std::raise(sig);
}
#endif

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
  log_error("!!! 未捕获的异常，进程结束: " + what);
  std::abort();
}

}  // namespace

void set_log_level(LogLevel level) { current_level = static_cast<int>(level); }

LogLevel log_level() { return static_cast<LogLevel>(current_level.load()); }

bool log_enabled(LogLevel level) { return static_cast<int>(level) <= current_level.load(); }

LogLevel log_level_from_env(LogLevel fallback) {
  const char* value = std::getenv("PANPLAY_LOG");
  if (value == nullptr) return fallback;
  if (std::strcmp(value, "error") == 0) return LogLevel::error;
  if (std::strcmp(value, "warn") == 0) return LogLevel::warn;
  if (std::strcmp(value, "info") == 0) return LogLevel::info;
  if (std::strcmp(value, "debug") == 0) return LogLevel::debug;
  return fallback;
}

bool open_log_file(const std::filesystem::path& path, bool capture_stdio) {
  thread_number();
  std::lock_guard<std::mutex> lock(file_mu);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  file_path = path;
  capture_stdio_to_file = capture_stdio;
  // 追加写：再次启动不会冲掉上次崩溃前的记录。
  if (std::filesystem::file_size(path, ec) > kMaxLogBytes && !ec) std::filesystem::rename(path, old_log_path(), ec);
  return reopen_locked();
}

void log_line(LogLevel level, const std::string& message) {
  if (!log_enabled(level)) return;
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
  std::snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d.%03d +%.3fs [t%d] %s ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms, uptime, thread_number(),
                level_tag(level));
  const std::string line = head + message + "\n";
  std::lock_guard<std::mutex> lock(file_mu);
  write_raw(line.data(), line.size());
  if (log_fd.load() < 0) return;
  // 写文件时，警告和错误在控制台上也打一份，开着窗口的人能直接看到。
  if (level <= LogLevel::warn && !capture_stdio_to_file) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
  file_bytes += line.size();
  if (file_bytes > kMaxLogBytes) rotate_locked();
}

void install_crash_trace() {
#if defined(_WIN32)
  for (const int sig : {SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGTERM}) std::signal(sig, on_crash_signal);
#else
  install_signal_stack();
  struct sigaction action {};
  action.sa_sigaction = on_crash_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGHUP, SIGSYS}) sigaction(sig, &action, nullptr);
#endif
  std::set_terminate(on_terminate);
  std::atexit([] { log_info("进程调用 exit 退出"); });
}

void install_signal_stack() {
#if !defined(_WIN32)
  alignas(16) thread_local char stack[64 * 1024];
  stack_t alternate{};
  alternate.ss_sp = stack;
  alternate.ss_size = sizeof(stack);
  sigaltstack(&alternate, nullptr);
#endif
}

void start_trace_heartbeat() {
  std::thread([] {
    for (int beat = 1;; ++beat) {
      std::this_thread::sleep_for(std::chrono::seconds(beat <= 24 ? 5 : 60));
      PANPLAY_DEBUG("心跳 " + memory_summary());
    }
  }).detach();
}

std::string memory_summary() {
  std::string out;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    out = "峰值内存 " + std::to_string(counters.PeakWorkingSetSize >> 20) + " MB";
  }
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) out = "峰值内存 " + std::to_string(usage.ru_maxrss / 1024) + " MB";
#endif
  if (const std::uint64_t peak = big_memory_peak(); peak > 0) {
    out += (out.empty() ? "" : "，") + std::string("解压字典占用 ") + std::to_string(big_memory_in_use() >> 20) + " MB（最多 " +
           std::to_string(peak >> 20) + " MB）";
  }
#if defined(PANPLAY_PS5)
  SceLibcMallocManagedSize heap{};
  heap.size = sizeof(heap);
  heap.version = 1;
  if (malloc_stats_fast(&heap) == 0) {
    out += (out.empty() ? "" : "，") + std::string("堆在用 ") + std::to_string(heap.currentInuseSize >> 20) + " MB（最多 " +
           std::to_string(heap.maxInuseSize >> 20) + " MB），堆向系统要了 " + std::to_string(heap.currentSystemSize >> 20) +
           " MB";
  }
  std::size_t flexible = 0;
  if (sceKernelAvailableFlexibleMemorySize(&flexible) == 0) {
    out += (out.empty() ? "" : "，") + std::string("剩余可用内存 ") + std::to_string(flexible >> 20) + " MB";
  }
#endif
  return out;
}

}  // namespace panplay
