#pragma once

#include "panplay/sink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace panplay {

// 安装线程的时间花在哪。开关文件包括建目录、新建和关闭文件、记进度、搬文件这些文件操作；
// 列网盘目录、算叠放顺序之类的记作其他。
enum class InstallPhase { other, download, decode, write, files };

// 同一时刻只记一项：进入新的一项时上一项暂停，离开时接着记，各项加起来就是安装用的总时间。
// 构造时绑定到当前线程，别的线程（预读的下载线程等）进出 PhaseScope 不记。
// 打开详细日志时每 10 秒记一行这段时间的占比，见 ADR 0001「诊断日志」。
class InstallClock {
 public:
  InstallClock();
  ~InstallClock();
  InstallClock(const InstallClock&) = delete;
  InstallClock& operator=(const InstallClock&) = delete;

  // 安装结束时的一行总账。一直没开始解压（比如已经装好、读目录就失败）时为空。
  std::string summary(const std::string& name, const std::string& outcome);

  // 当前线程正在计时的安装写出了多少字节、新建了一个文件。没有在计时时什么也不做。
  static void note_written(std::size_t bytes);
  static void note_opened();

 private:
  friend class PhaseScope;
  using Clock = std::chrono::steady_clock;
  static constexpr int kPhases = 5;
  struct Tally {
    double ms[kPhases] = {};
    std::int64_t written = 0;
    int opened = 0;
  };

  InstallPhase switch_to(InstallPhase phase);
  void charge(Clock::time_point now);

  InstallClock* outer_;
  InstallPhase current_ = InstallPhase::other;
  Clock::time_point since_;
  Clock::time_point window_at_;
  Tally total_;
  Tally window_;
  bool extracted_ = false;
  double cpu_at_start_;
  double thread_cpu_at_start_;
  long long faults_at_start_;
};

// 在这个作用域里的时间记到 phase 上，离开时换回原来那项。
class PhaseScope {
 public:
  explicit PhaseScope(InstallPhase phase);
  ~PhaseScope();
  PhaseScope(const PhaseScope&) = delete;
  PhaseScope& operator=(const PhaseScope&) = delete;

 private:
  InstallClock* clock_;
  InstallPhase previous_ = InstallPhase::other;
};

// 包一层 sink：写数据记作写入，其余操作（连同新建、关闭文件）记作开关文件。
std::unique_ptr<InstallSink> timed_sink(InstallSink& inner);

}  // namespace panplay
