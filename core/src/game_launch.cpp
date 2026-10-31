#include "panplay/game_launch.hpp"

#include "panplay/json.hpp"

#include <cctype>
#include <cstdlib>

namespace panplay {

namespace fs = std::filesystem;

std::string pick_fakelib(const fs::path& game_dir) {
  std::error_code ec;
  for (const char* name : {"fakelib2", "fakelib"}) {
    if (fs::is_directory(game_dir / name, ec)) return name;
  }
  return "";
}

std::string sandbox_lib_dir(const fs::path& sandbox_root, const std::string& title_id, std::string* error) {
  const std::string prefix = title_id + "_";
  long best_number = -1;
  fs::path best;
  std::error_code ec;
  for (fs::directory_iterator it(sandbox_root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size()) continue;
    const std::string suffix = name.substr(prefix.size());
    bool digits = suffix.size() <= 6;
    for (const char ch : suffix) digits = digits && std::isdigit(static_cast<unsigned char>(ch));
    std::error_code entry_ec;
    if (!digits || !fs::is_directory(it->path() / "app0", entry_ec)) continue;
    const long number = std::strtol(suffix.c_str(), nullptr, 10);
    if (number > best_number) {
      best_number = number;
      best = it->path();
    }
  }
  if (ec) {
    *error = "列不出 " + sandbox_root.u8string() + "：" + ec.message();
    return "";
  }
  if (best.empty()) {
    *error = "找不到 " + title_id + " 的沙盒";
    return "";
  }
  for (fs::directory_iterator it(best, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    std::error_code entry_ec;
    if (name == "app0" || !fs::is_directory(it->path() / "common" / "lib", entry_ec)) continue;
    return (it->path() / "common" / "lib").u8string();
  }
  *error = best.filename().u8string() + " 里找不到系统库目录 common/lib";
  return "";
}

void LaunchTracker::exec(const std::string& title_id, std::int64_t pid, std::int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = reports_.find(title_id);
  if (it != reports_.end()) {
    LaunchReport& report = it->second;
    if (report.pid == pid && report.exited_at == 0) return;
    const bool restart = report.exited_at != 0 && now_ms - report.exited_at <= kRestartMs;
    // 换执行文件时新进程有可能比旧进程的退出先到。
    const bool replacing = report.exited_at == 0;
    if (restart || replacing) {
      report.pid = pid;
      report.exited_at = 0;
      // 新进程的沙盒要重新叠，结果由监视线程再报。
      report.fakelib.clear();
      report.fakelib_by.clear();
      report.fakelib_error.clear();
      report.mount_ms = -1;
      return;
    }
  }
  LaunchReport report;
  report.title_id = title_id;
  report.at = now_ms;
  report.pid = pid;
  reports_[title_id] = report;
}

LaunchReport* LaunchTracker::by_pid(std::int64_t pid) {
  for (auto& [title_id, report] : reports_) {
    if (report.pid == pid) return &report;
  }
  return nullptr;
}

void LaunchTracker::fakelib(std::int64_t pid, const std::string& dir, const std::string& by, const std::string& error,
                            std::int64_t mount_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  LaunchReport* report = by_pid(pid);
  if (report == nullptr) return;
  report->fakelib = dir;
  report->fakelib_by = by;
  report->fakelib_error = error;
  report->mount_ms = error.empty() ? mount_ms : -1;
}

void LaunchTracker::exited(std::int64_t pid, std::int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  if (LaunchReport* report = by_pid(pid); report != nullptr && report->exited_at == 0) report->exited_at = now_ms;
}

std::optional<LaunchReport> LaunchTracker::last(const std::string& title_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = reports_.find(title_id);
  if (it == reports_.end()) return std::nullopt;
  return it->second;
}

LaunchTracker& game_launches() {
  static LaunchTracker tracker;
  return tracker;
}

std::string launch_report_json(const LaunchReport& report) {
  return "{\"at\":" + std::to_string(report.at) + ",\"pid\":" + std::to_string(report.pid) +
         ",\"fakelib\":\"" + json_escape(report.fakelib) + "\",\"fakelib_by\":\"" + json_escape(report.fakelib_by) +
         "\",\"fakelib_error\":\"" + json_escape(report.fakelib_error) + "\",\"mount_ms\":" +
         std::to_string(report.mount_ms) + ",\"exited_at\":" + std::to_string(report.exited_at) + "}";
}

}  // namespace panplay
