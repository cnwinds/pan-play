#include "panplay/disk_test.hpp"

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
const char* const kFileName = ".panplay-disk-test.bin";
// 和解压器交给写入接口的块差不多大；写入接口自己攒满 4 MB 再写盘。
constexpr std::size_t kWritePiece = 256 << 10;
constexpr std::size_t kReadChunk = 4 << 20;
constexpr std::size_t kAlign = 4096;

std::string mb_per_second(double rate) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB/s", rate / 1048576);
  return text;
}

}  // namespace

DiskTest::~DiskTest() {
  stop();
  if (thread_.joinable()) thread_.join();
}

std::string DiskTest::start(const Destination& destination, std::int64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return "正在测硬盘速度";
  if (thread_.joinable()) thread_.join();
  running_ = true;
  stop_ = false;
  total_ = bytes;
  status_ = DiskTestStatus{};
  status_.phase = "testing";
  status_.total = total_;
  status_.where = destination.ftp ? "ftp://" + destination.endpoint.host + destination.prefix
                                  : destination.local_root;
  status_.steps.push_back({"写入"});
  if (!destination.ftp) status_.steps.push_back({"读取"});
  thread_ = std::thread([this, destination] { run(destination); });
  return "";
}

void DiskTest::stop() { stop_ = true; }

DiskTestStatus DiskTest::status() {
  std::lock_guard<std::mutex> lock(mu_);
  DiskTestStatus copy = status_;
  if (copy.current >= 0 && copy.bytes > 0) {
    const double seconds = std::chrono::duration<double>(Clock::now() - step_began_).count();
    copy.current_rate = seconds > 0 ? static_cast<double>(copy.bytes) / seconds : 0;
  }
  return copy;
}

bool DiskTest::running() {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

void DiskTest::begin_step(int index) {
  std::lock_guard<std::mutex> lock(mu_);
  status_.current = index;
  status_.bytes = 0;
  step_began_ = Clock::now();
}

void DiskTest::add_bytes(std::size_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  status_.bytes += static_cast<std::int64_t>(bytes);
}

void DiskTest::finish_step() {
  std::lock_guard<std::mutex> lock(mu_);
  const double seconds = std::chrono::duration<double>(Clock::now() - step_began_).count();
  DiskTestStep& step = status_.steps[static_cast<std::size_t>(status_.current)];
  step.rate = seconds > 0 ? static_cast<double>(status_.bytes) / seconds : 0;
  step.done = true;
}

std::string DiskTest::run_write(InstallSink& sink, const std::string& path, const std::string& local) {
  // 4 MB 不重复的数据反复写。
  std::vector<std::uint8_t> block(4 << 20);
  std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
  for (auto& byte : block) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    byte = static_cast<std::uint8_t>(seed);
  }
  std::string error;
  auto writer = sink.open_writer(path, total_, &error);
  if (!writer) return error.empty() ? "无法写入" : error;
  for (std::int64_t done = 0; done < total_;) {
    if (stop_) {
      writer->finish();
      return kStopped;
    }
    const std::size_t at = static_cast<std::size_t>(done % static_cast<std::int64_t>(block.size()));
    const std::size_t size = std::min<std::size_t>(kWritePiece, block.size() - at);
    if (error = writer->write(block.data() + at, size); !error.empty()) return error;
    done += static_cast<std::int64_t>(size);
    add_bytes(size);
  }
  if (error = writer->finish(); !error.empty()) return error;
  if (!local.empty() && !sync_file(std::filesystem::u8path(local))) return "刷盘失败";
  return "";
}

std::string DiskTest::run_read(const std::string& local) {
  RawFileIn in;
  if (!in.open(std::filesystem::u8path(local))) return "无法读取测试文件";
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.read_cached = !in.uncached();
  }
  // 绕过缓存时缓冲地址要按扇区对齐。
  std::vector<std::uint8_t> storage(kReadChunk + kAlign);
  const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
  std::uint8_t* buffer = storage.data() + (kAlign - address % kAlign) % kAlign;
  std::int64_t done = 0;
  while (done < total_) {
    if (stop_) return kStopped;
    const long long got = in.read(buffer, kReadChunk);
    if (got < 0) return "读取出错";
    if (got == 0) break;
    done += got;
    add_bytes(static_cast<std::size_t>(got));
  }
  return done >= total_ ? "" : "测试文件读不全";
}

void DiskTest::run(Destination destination) {
  const std::string path = join_storage_path(destination.prefix, kFileName);
  std::string local;
  if (!destination.ftp) {
    std::filesystem::path out;
    std::string ignored;
    if (storage_path(destination.local_root, path, &out, &ignored)) local = out.u8string();
  }

  std::string error;
  auto sink = open_destination(destination, &error);
  if (sink) {
    begin_step(0);
    error = run_write(*sink, path, local);
    if (error.empty()) finish_step();
    if (error.empty() && !local.empty()) {
      begin_step(1);
      error = run_read(local);
      if (error.empty()) finish_step();
    }
    sink->remove_file(path);
  } else if (error.empty()) {
    error = "无法连接存放位置";
  }

  std::string summary;
  std::string where;
  bool cached = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    where = status_.where;
    cached = status_.read_cached;
    status_.current = -1;
    status_.bytes = 0;
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
  if (!summary.empty()) log_info("硬盘测速 " + where + "：" + summary + (cached ? "（读取没能绕过系统缓存，可能偏高）" : ""));
  if (!error.empty() && error != kStopped) log_warn("硬盘测速没有完成：" + error);
}

}  // namespace panplay
