#include "panplay/write_test.hpp"

#include "panplay/library.hpp"
#include "panplay/raw_file.hpp"
#include "panplay/storage_path.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace panplay {

namespace {

using Clock = std::chrono::steady_clock;

const char* const kStopped = "stopped";
const char* const kFileName = ".panplay-write-test.bin";
constexpr std::size_t kSinkPiece = 256 << 10;

struct DirectWay {
  const char* label;
  std::size_t chunk;
};
constexpr DirectWay kDirectWays[] = {
    {"直接写，每次 64 KB", 64 << 10},
    {"直接写，每次 1 MB", 1 << 20},
    {"直接写，每次 4 MB", 4 << 20},
};

std::string mb_per_second(double rate) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB/s", rate / 1048576);
  return text;
}

}  // namespace

WriteTest::~WriteTest() {
  stop();
  if (thread_.joinable()) thread_.join();
}

std::string WriteTest::start(const Destination& destination, std::int64_t bytes_per_step) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return "正在测写盘速度";
  if (thread_.joinable()) thread_.join();
  running_ = true;
  stop_ = false;
  total_ = bytes_per_step;
  status_ = WriteTestStatus{};
  status_.phase = "testing";
  status_.total = total_;
  status_.where = destination.ftp ? "ftp://" + destination.endpoint.host + destination.prefix
                                  : destination.local_root;
  status_.steps.push_back({"盘游现在的写法"});
  if (!destination.ftp) {
    for (const auto& way : kDirectWays) status_.steps.push_back({way.label});
  }
  thread_ = std::thread([this, destination] { run(destination); });
  return "";
}

void WriteTest::stop() { stop_ = true; }

WriteTestStatus WriteTest::status() {
  std::lock_guard<std::mutex> lock(mu_);
  WriteTestStatus copy = status_;
  if (copy.current >= 0 && copy.written > 0) {
    const double seconds = std::chrono::duration<double>(Clock::now() - step_began_).count();
    copy.current_rate = seconds > 0 ? static_cast<double>(copy.written) / seconds : 0;
  }
  return copy;
}

bool WriteTest::running() {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

void WriteTest::begin_step(int index) {
  std::lock_guard<std::mutex> lock(mu_);
  status_.current = index;
  status_.written = 0;
  step_began_ = Clock::now();
}

void WriteTest::add_written(std::size_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  status_.written += static_cast<std::int64_t>(bytes);
}

void WriteTest::finish_step() {
  std::lock_guard<std::mutex> lock(mu_);
  const double seconds = std::chrono::duration<double>(Clock::now() - step_began_).count();
  WriteTestStep& step = status_.steps[static_cast<std::size_t>(status_.current)];
  step.rate = seconds > 0 ? static_cast<double>(status_.written) / seconds : 0;
  step.done = true;
}

std::string WriteTest::run_sink(InstallSink& sink, const std::string& path, const std::string& local,
                                const std::vector<std::uint8_t>& block) {
  std::string error;
  auto writer = sink.open_writer(path, total_, &error);
  if (!writer) return error.empty() ? "无法写入" : error;
  for (std::int64_t done = 0; done < total_;) {
    if (stop_) {
      writer->finish();
      return kStopped;
    }
    const std::size_t at = static_cast<std::size_t>(done % static_cast<std::int64_t>(block.size()));
    const std::size_t size = std::min<std::size_t>(kSinkPiece, block.size() - at);
    if (error = writer->write(block.data() + at, size); !error.empty()) return error;
    done += static_cast<std::int64_t>(size);
    add_written(size);
  }
  if (error = writer->finish(); !error.empty()) return error;
  if (!local.empty() && !sync_file(std::filesystem::u8path(local))) return "刷盘失败";
  return "";
}

std::string WriteTest::run_direct(const std::string& local, std::size_t chunk, const std::vector<std::uint8_t>& block) {
  RawFileOut out(std::filesystem::u8path(local));
  if (!out.ok()) return "无法写入";
  std::string error;
  for (std::int64_t done = 0; done < total_ && error.empty();) {
    if (stop_) {
      error = kStopped;
      break;
    }
    const std::size_t at = static_cast<std::size_t>(done % static_cast<std::int64_t>(block.size()));
    const std::size_t size = std::min<std::size_t>(chunk, block.size() - at);
    if (!out.write_all(block.data() + at, size)) {
      error = "写入中断";
      break;
    }
    done += static_cast<std::int64_t>(size);
    add_written(size);
  }
  if (error.empty() && !out.sync()) error = "刷盘失败";
  out.close();
  return error;
}

void WriteTest::run(Destination destination) {
  // 4 MB 不重复的数据，每种写法都按顺序反复写它。
  std::vector<std::uint8_t> block(4 << 20);
  std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
  for (auto& byte : block) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    byte = static_cast<std::uint8_t>(seed);
  }
  const std::string path = join_storage_path(destination.prefix, kFileName);
  std::string local;
  if (!destination.ftp) {
    std::filesystem::path out;
    std::string ignored;
    if (storage_path(destination.local_root, path, &out, &ignored)) local = out.u8string();
  }

  std::string error;
  auto sink = open_destination(destination, &error);
  std::size_t steps = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    steps = status_.steps.size();
  }
  for (std::size_t i = 0; i < steps && error.empty() && sink; ++i) {
    begin_step(static_cast<int>(i));
    error = i == 0 ? run_sink(*sink, path, local, block) : run_direct(local, kDirectWays[i - 1].chunk, block);
    if (error.empty()) finish_step();
    sink->remove_file(path);
  }
  if (!sink && error.empty()) error = "无法连接存放位置";

  std::string summary;
  std::string where;
  {
    std::lock_guard<std::mutex> lock(mu_);
    where = status_.where;
    status_.current = -1;
    status_.written = 0;
    for (const auto& step : status_.steps) {
      if (step.done) summary += (summary.empty() ? "" : "，") + step.label + " " + mb_per_second(step.rate);
    }
    if (error == kStopped) {
      status_.phase = "done";
      status_.ended = "stopped";
    } else if (!error.empty()) {
      status_.phase = "error";
      status_.error = error;
    } else {
      status_.phase = "done";
    }
    running_ = false;
  }
  if (!summary.empty()) log_info("写盘测速 " + where + "：" + summary);
  if (!error.empty() && error != kStopped) log_warn("写盘测速没有完成：" + error);
}

}  // namespace panplay
