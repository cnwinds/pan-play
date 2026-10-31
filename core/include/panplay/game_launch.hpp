#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

// 主机上游戏启动的经过和 backport 的 fakelib（见 docs/adr/0008）。这里只放不依赖主机的部分，
// 调系统的部分在 pc/src/ps5_game_watch.cpp。

namespace panplay {

// 游戏目录里要叠到沙盒系统库上的目录名：fakelib2 优先，其次 fakelib（同 ShadowMountPlus），都没有时为空。
std::string pick_fakelib(const std::filesystem::path& game_dir);

// 这个游戏沙盒里的系统库目录 <sandbox_root>/<ID>_NNN/<随机目录>/common/lib。
// 只认有 app0 的沙盒，不止一个时取编号最大的（同 BackPork）。找不到时返回空并写原因。
std::string sandbox_lib_dir(const std::filesystem::path& sandbox_root, const std::string& title_id, std::string* error);

// 一次启动的经过。时间都是 Unix 毫秒。
struct LaunchReport {
  std::string title_id;
  std::int64_t at = 0;            // 看到进程执行的时间
  std::int64_t pid = 0;           // 游戏进程
  std::string fakelib;            // 叠上去的目录名（fakelib、fakelib2），没有 backport 库时为空
  std::string fakelib_by;         // 谁挂的：panplay、ShadowMountPlus、BackPork
  std::string fakelib_error;      // 盘游挂的时候出的错
  std::int64_t mount_ms = -1;     // 从看到进程执行到叠好用了多久
  std::int64_t exited_at = 0;     // 进程退出的时间，还在运行时为 0
};

// 盘游注册的游戏每次启动的经过，只留每个游戏最近一次，存在内存里。线程安全。
class LaunchTracker {
 public:
  // 同一游戏退出后这么久之内又执行，算同一次启动（游戏自己换执行文件）。
  static constexpr std::int64_t kRestartMs = 5000;

  void exec(const std::string& title_id, std::int64_t pid, std::int64_t now_ms);
  void fakelib(std::int64_t pid, const std::string& dir, const std::string& by, const std::string& error,
               std::int64_t mount_ms);
  void exited(std::int64_t pid, std::int64_t now_ms);
  std::optional<LaunchReport> last(const std::string& title_id) const;

 private:
  LaunchReport* by_pid(std::int64_t pid);

  mutable std::mutex mu_;
  std::map<std::string, LaunchReport> reports_;
};

LaunchTracker& game_launches();

std::string launch_report_json(const LaunchReport& report);

int run_game_launch_self_test();

}  // namespace panplay
