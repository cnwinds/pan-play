#include "install_clock.hpp"

#include "panplay/trace.hpp"

#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace panplay {
namespace {

thread_local InstallClock* bound = nullptr;

constexpr const char* kPhaseNames[] = {"其他", "等下载", "解压", "写入", "开关文件"};
constexpr InstallPhase kReportOrder[] = {InstallPhase::download, InstallPhase::decode, InstallPhase::write,
                                         InstallPhase::files, InstallPhase::other};

std::string seconds_text(double ms) {
  const double seconds = ms / 1000;
  const long long whole = static_cast<long long>(seconds);
  char text[48];
  if (seconds < 60) {
    std::snprintf(text, sizeof(text), "%.1f 秒", seconds);
  } else if (seconds < 3600) {
    std::snprintf(text, sizeof(text), "%lld 分 %lld 秒", whole / 60, whole % 60);
  } else {
    std::snprintf(text, sizeof(text), "%lld 小时 %lld 分", whole / 3600, whole % 3600 / 60);
  }
  return text;
}

std::string bytes_text(std::int64_t bytes) {
  char text[32];
  if (bytes >= (1ll << 30)) {
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ll << 30));
  } else if (bytes >= (1 << 20)) {
    std::snprintf(text, sizeof(text), "%lld MB", static_cast<long long>(bytes >> 20));
  } else {
    std::snprintf(text, sizeof(text), "%lld KB", static_cast<long long>((bytes + 1023) >> 10));
  }
  return text;
}

// 整个进程（连同下载线程、解压线程）用掉的 CPU 时间。
double process_cpu_seconds() {
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
  const auto seconds = [](const timeval& time) { return static_cast<double>(time.tv_sec) + time.tv_usec / 1e6; };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}

std::string rate_text(std::int64_t bytes, double ms) {
  const double rate = static_cast<double>(bytes) / (1 << 20) / (ms / 1000);
  char text[32];
  std::snprintf(text, sizeof(text), rate < 10 ? "%.1f MB/s" : "%.0f MB/s", rate);
  return text;
}

}  // namespace

InstallClock::InstallClock()
    : outer_(bound), since_(Clock::now()), window_at_(since_), cpu_at_start_(process_cpu_seconds()) {
  bound = this;
}

InstallClock::~InstallClock() { bound = outer_; }

void InstallClock::note_written(std::size_t bytes) {
  if (bound == nullptr) return;
  bound->total_.written += static_cast<std::int64_t>(bytes);
  bound->window_.written += static_cast<std::int64_t>(bytes);
}

void InstallClock::note_opened() {
  if (bound == nullptr) return;
  ++bound->total_.opened;
  ++bound->window_.opened;
}

void InstallClock::charge(Clock::time_point now) {
  const double ms = std::chrono::duration<double, std::milli>(now - since_).count();
  total_.ms[static_cast<int>(current_)] += ms;
  window_.ms[static_cast<int>(current_)] += ms;
  since_ = now;
}

namespace {

// 各项的占比，durations 时再带上各项用了多久。写入带上写了多少、写入时多快，开关文件带上新建了几个文件。
template <class Tally>
std::string describe(const Tally& tally, double all_ms, bool durations) {
  std::string text;
  for (const InstallPhase phase : kReportOrder) {
    const int index = static_cast<int>(phase);
    const double ms = tally.ms[index];
    const long percent = all_ms > 0 ? std::lround(ms * 100 / all_ms) : 0;
    if (phase == InstallPhase::other && percent < 1) continue;
    std::string extra = durations ? seconds_text(ms) : "";
    const auto add = [&](const std::string& part) { extra += (extra.empty() ? "" : "，") + part; };
    if (phase == InstallPhase::write && tally.written > 0) {
      add("写了 " + bytes_text(tally.written));
      if (ms >= 50) add("写入时 " + rate_text(tally.written, ms));
    }
    if (phase == InstallPhase::files && tally.opened > 0) {
      add("新建 " + std::to_string(tally.opened) + " 个文件");
      if (durations) add("平均每个 " + std::to_string(std::lround(ms / tally.opened)) + " ms");
    }
    text += (text.empty() ? "" : "，") + std::string(kPhaseNames[index]) + " " + std::to_string(percent) + "%" +
            (extra.empty() ? "" : "（" + extra + "）");
  }
  return text;
}

template <class Tally>
double sum_ms(const Tally& tally) {
  double all = 0;
  for (const double ms : tally.ms) all += ms;
  return all;
}

}  // namespace

InstallPhase InstallClock::switch_to(InstallPhase phase) {
  const auto now = Clock::now();
  charge(now);
  const InstallPhase previous = current_;
  current_ = phase;
  if (phase == InstallPhase::decode) extracted_ = true;
  if (now - window_at_ >= std::chrono::seconds(10)) {
    if (extracted_ && log_enabled(LogLevel::debug)) {
      const double all = sum_ms(window_);
      log_line(LogLevel::debug, "安装耗时（最近 " + seconds_text(all) + "）：" + describe(window_, all, false));
    }
    window_ = {};
    window_at_ = now;
  }
  return previous;
}

std::string InstallClock::summary(const std::string& name, const std::string& outcome) {
  charge(Clock::now());
  if (!extracted_) return "";
  const double all = sum_ms(total_);
  InstallPhase most = InstallPhase::download;
  for (const InstallPhase phase : kReportOrder) {
    if (total_.ms[static_cast<int>(phase)] > total_.ms[static_cast<int>(most)]) most = phase;
  }
  std::string cpu;
  if (const double used = process_cpu_seconds() - cpu_at_start_; cpu_at_start_ >= 0 && all > 0 && used >= 0) {
    char text[64];
    std::snprintf(text, sizeof(text), "；整个盘游平均占 %.2f 个 CPU 核", used * 1000 / all);
    cpu = text;
  }
  return "安装耗时 " + name + "（" + outcome + "）：共 " + seconds_text(all) + "，" + describe(total_, all, true) +
         "；时间大多花在" + kPhaseNames[static_cast<int>(most)] + cpu;
}

PhaseScope::PhaseScope(InstallPhase phase) : clock_(bound) {
  if (clock_ != nullptr) previous_ = clock_->switch_to(phase);
}

PhaseScope::~PhaseScope() {
  if (clock_ != nullptr) clock_->switch_to(previous_);
}

namespace {

class TimedWriter final : public FileWriter {
 public:
  explicit TimedWriter(std::unique_ptr<FileWriter> inner) : inner_(std::move(inner)) {}
  ~TimedWriter() override {
    PhaseScope scope(InstallPhase::files);
    inner_.reset();
  }

  std::string write(const std::uint8_t* data, std::size_t size) override {
    PhaseScope scope(InstallPhase::write);
    std::string error = inner_->write(data, size);
    if (error.empty()) InstallClock::note_written(size);
    return error;
  }

  std::string finish() override {
    PhaseScope scope(InstallPhase::files);
    return inner_->finish();
  }

 private:
  std::unique_ptr<FileWriter> inner_;
};

class TimedSink final : public InstallSink {
 public:
  explicit TimedSink(InstallSink& inner) : inner_(inner) {}

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) override {
    PhaseScope scope(InstallPhase::files);
    auto writer = inner_.open_writer(path, size, error);
    if (!writer) return writer;
    InstallClock::note_opened();
    return std::make_unique<TimedWriter>(std::move(writer));
  }
  std::string ensure_dir(const std::string& path) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.ensure_dir(path);
  }
  std::int64_t file_size(const std::string& path, std::string* error) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.file_size(path, error);
  }
  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.store(path, data, size);
  }
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.append(path, data, size);
  }
  std::string rename_path(const std::string& from, const std::string& to) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.rename_path(from, to);
  }
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.read_file(path, out, max_bytes);
  }
  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.list_children(path, error);
  }
  std::string remove_file(const std::string& path) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.remove_file(path);
  }
  std::string remove_dir(const std::string& path) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.remove_dir(path);
  }
  std::string remove_tree(const std::string& path) override {
    PhaseScope scope(InstallPhase::files);
    return inner_.remove_tree(path);
  }

 private:
  InstallSink& inner_;
};

}  // namespace

std::unique_ptr<InstallSink> timed_sink(InstallSink& inner) { return std::make_unique<TimedSink>(inner); }

}  // namespace panplay
