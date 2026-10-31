#include "panplay/game_launch.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace panplay {
namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
  std::cerr << "游戏启动自检失败: " << message << "\n";
  return 1;
}

int check_pick_fakelib(const fs::path& root) {
  const fs::path game = root / "game";
  fs::create_directories(game);
  if (!pick_fakelib(game).empty()) return fail("没有 fakelib 时应为空");
  fs::create_directories(game / "fakelib");
  if (pick_fakelib(game) != "fakelib") return fail("应找到 fakelib");
  fs::create_directories(game / "fakelib2");
  if (pick_fakelib(game) != "fakelib2") return fail("fakelib2 应优先");
  return 0;
}

int check_sandbox(const fs::path& root) {
  const fs::path sandbox = root / "sandbox";
  fs::create_directories(sandbox);
  std::string error;
  if (!sandbox_lib_dir(sandbox, "PPSA01234", &error).empty() || error.empty()) return fail("没有沙盒时应报错");

  // 别的游戏、没有 app0 的旧沙盒、名字不对的都不算。
  fs::create_directories(sandbox / "PPSA09999_000" / "app0");
  fs::create_directories(sandbox / "PPSA09999_000" / "aBcD" / "common" / "lib");
  fs::create_directories(sandbox / "PPSA01234_005" / "xYz" / "common" / "lib");
  fs::create_directories(sandbox / "PPSA01234_x" / "app0");
  error.clear();
  if (!sandbox_lib_dir(sandbox, "PPSA01234", &error).empty()) return fail("没有 app0 的沙盒不该用");

  fs::create_directories(sandbox / "PPSA01234_000" / "app0");
  error.clear();
  if (!sandbox_lib_dir(sandbox, "PPSA01234", &error).empty() || error.find("common/lib") == std::string::npos) {
    return fail("沙盒里没有 common/lib 时应说明: " + error);
  }
  fs::create_directories(sandbox / "PPSA01234_000" / "Rnd01" / "common" / "lib");
  const fs::path first = sandbox / "PPSA01234_000" / "Rnd01" / "common" / "lib";
  if (sandbox_lib_dir(sandbox, "PPSA01234", &error) != first.u8string()) return fail("没找到沙盒的系统库目录");

  fs::create_directories(sandbox / "PPSA01234_001" / "app0");
  fs::create_directories(sandbox / "PPSA01234_001" / "Rnd02" / "common" / "lib");
  const fs::path second = sandbox / "PPSA01234_001" / "Rnd02" / "common" / "lib";
  if (sandbox_lib_dir(sandbox, "PPSA01234", &error) != second.u8string()) return fail("几个沙盒时应取编号最大的");
  return 0;
}

int check_tracker() {
  LaunchTracker tracker;
  if (tracker.last("PPSA01234")) return fail("还没启动过时不该有记录");

  // 盘游发起：先记请求，进程起来后挂上 fakelib，几秒后退出。
  tracker.requested("PPSA01234", 1000);
  tracker.exec("PPSA01234", 77, 3000);
  tracker.fakelib(77, "fakelib", "panplay", "", 40);
  auto report = tracker.last("PPSA01234");
  if (!report || !report->requested || report->pid != 77 || report->at != 1000) return fail("进程执行应算进盘游发起的那次");
  if (report->fakelib != "fakelib" || report->mount_ms != 40 || !report->fakelib_error.empty()) return fail("fakelib 结果没记上");
  tracker.exited(77, 7000);
  report = tracker.last("PPSA01234");
  if (!report || report->exited_at != 7000) return fail("退出时间没记上");

  // 退出后马上换新进程：同一次启动，fakelib 结果等新进程重新报。
  tracker.exec("PPSA01234", 78, 9000);
  report = tracker.last("PPSA01234");
  if (!report || report->pid != 78 || report->exited_at != 0 || !report->requested || !report->fakelib.empty()) {
    return fail("退出后马上又执行应算同一次启动");
  }
  tracker.exited(77, 9100);
  report = tracker.last("PPSA01234");
  if (!report || report->exited_at != 0) return fail("旧进程迟到的退出不该算到新进程上");
  tracker.exec("PPSA01234", 78, 9200);
  if (tracker.last("PPSA01234")->at != 1000) return fail("同一进程重复执行不该重开一次");
  tracker.exited(78, 60000);

  // 隔了很久在桌面上启动：新的一次，不是盘游发起的。
  tracker.exec("PPSA01234", 90, 200000);
  report = tracker.last("PPSA01234");
  if (!report || report->requested || report->at != 200000 || report->exited_at != 0) return fail("隔很久再启动应是新的一次");
  tracker.fakelib(90, "fakelib2", "panplay", "mount: Operation not permitted", 12);
  report = tracker.last("PPSA01234");
  if (!report || report->fakelib_error.empty() || report->mount_ms != -1) return fail("挂载失败时不该有用时");

  // 启动接口失败：没有进程，之后进程执行另起一次。
  tracker.requested("PPSA05678", 300000);
  tracker.request_failed("PPSA05678", "启动失败，系统返回 0x80940005");
  report = tracker.last("PPSA05678");
  if (!report || report->launch_error.empty() || report->pid != 0) return fail("启动接口的错误没记上");
  tracker.exec("PPSA05678", 99, 301000);
  report = tracker.last("PPSA05678");
  if (!report || report->requested || !report->launch_error.empty()) return fail("启动失败后看到的进程应另起一次");

  const std::string json = launch_report_json(*report);
  if (json.find("\"pid\":99") == std::string::npos || json.find("\"requested\":false") == std::string::npos) {
    return fail("JSON 不对: " + json);
  }
  return 0;
}

}  // namespace

int run_game_launch_self_test() {
  const fs::path root = fs::temp_directory_path() / ("panplay-game-launch-" + std::to_string(std::rand()));
  std::error_code ec;
  fs::remove_all(root, ec);
  int result = check_pick_fakelib(root);
  if (!result) result = check_sandbox(root);
  if (!result) result = check_tracker();
  fs::remove_all(root, ec);
  return result;
}

}  // namespace panplay
