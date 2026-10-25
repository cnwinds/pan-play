#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace panplay {

// 日志分四级，每行带时间、启动后秒数、线程号和级别，不经缓冲直接写出，进程被杀前写下的都留得住。
//   错误、警告：出了问题，任何时候都记。
//   信息：启动、服务就绪、安装和删除的结果这类大事，正式使用时默认记到这一级。
//   调试：逐个请求、网络连接、心跳这类排查用的细节，设置里打开「详细日志」时才记。
// 没打开日志文件时写到 stderr，默认只记警告以上。
enum class LogLevel { error = 0, warn = 1, info = 2, debug = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
bool log_enabled(LogLevel level);
void log_line(LogLevel level, const std::string& message);

inline void log_error(const std::string& message) { log_line(LogLevel::error, message); }
inline void log_warn(const std::string& message) { log_line(LogLevel::warn, message); }
inline void log_info(const std::string& message) { log_line(LogLevel::info, message); }

// 调试日志用这个宏：没打开详细日志时连消息都不拼。
#define PANPLAY_DEBUG(message)                                                   \
  do {                                                                           \
    if (::panplay::log_enabled(::panplay::LogLevel::debug)) {                    \
      ::panplay::log_line(::panplay::LogLevel::debug, (message));                \
    }                                                                            \
  } while (0)

// 往这个文件追加日志，超过 2 MB 时改名成 <名字>.old.log 再开新文件，只留一份旧的。
// 运行中文件被删掉时，最多过 2 秒、下一行日志写之前会重新建出来。
// capture_stdio 时把标准输出和标准错误也接到这个文件：PS5 上加载器把它们接在管道上，没人读时写满会卡住线程。
bool open_log_file(const std::filesystem::path& path, bool capture_stdio);

// 环境变量 PANPLAY_LOG（error、warn、info、debug）指定的级别，没设或认不出时返回 fallback。
LogLevel log_level_from_env(LogLevel fallback);

// 收到崩溃信号、有未捕获的异常、调用 exit 时各记一行。被 SIGKILL 杀掉时记不下，只能看最后一行停在哪。
void install_crash_trace();

// 给当前线程装信号备用栈：线程栈溢出时，崩溃信号的处理也有地方跑。每个线程要各装一次。
void install_signal_stack();

// 后台定时记一行心跳和内存占用（调试级）：开头两分钟每 5 秒一次，之后每分钟一次。
void start_trace_heartbeat();

// 峰值内存，PS5 上再加剩余可用内存；取不到时为空。
std::string memory_summary();

inline long long ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace panplay
