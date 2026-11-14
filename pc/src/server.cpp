#include "server.hpp"

#include "detail_cache.hpp"
#include "game_sizes.hpp"
#include "review_sync.hpp"
#include "reviews.hpp"
#include "drive_usage.hpp"
#include "settings.hpp"
#include "shelf_feedback.hpp"
#include "shelf_library.hpp"
#include "shelf_plan.hpp"
#include "console_packages.hpp"
#include "shelf_transfer.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/catalog.hpp"
#include "panplay/catalog_sync.hpp"
#include "panplay/drive.hpp"
#include "panplay/game_launch.hpp"
#include "panplay/http.hpp"
#include "panplay/install.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"
#include "panplay/socket_util.hpp"
#include "panplay/speed_test.hpp"
#include "panplay/disk_test.hpp"
#include "panplay/store_meta.hpp"
#include "panplay/trace.hpp"
#include "panplay/version.hpp"

#if defined(PANPLAY_PS5)
#include "ps5_appinst.hpp"
#include "ps5_notify.hpp"
#include "ps5_titles.hpp"
#endif

#include "embedded_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

std::filesystem::path login_challenge_file() { return panplay::app_data_dir() / "login-challenge.json"; }

// 进程内只留一个网盘插件实例，下载地址缓存和令牌刷新都在它里面；换插件或改了参数才换新实例。
std::shared_ptr<panplay::DrivePlugin> open_drive() {
  static std::mutex mu;
  static std::shared_ptr<panplay::DrivePlugin> drive;
  static std::string key;
  const AppSettings settings = load_settings();
  const DriveSettings current = settings.current_drive();
  std::string wanted = settings.drive;
  for (const auto& [name, value] : current.values) wanted += "\n" + name + "=" + value;
  std::lock_guard<std::mutex> lock(mu);
  if (!drive || key != wanted) {
    drive = panplay::make_drive(settings.drive, current.values, drive_session_path(settings.drive));
    key = wanted;
  }
  return drive;
}

// 设置页要的全部内容：可选的插件和它们各自的参数，以及保存方式。
std::string settings_json(const AppSettings& settings) {
  using panplay::json_escape;
  std::string json = "{\"drive\":\"" + json_escape(settings.drive) + "\",\"drives\":[";
  bool first_kind = true;
  for (const auto& kind : panplay::drive_kinds()) {
    if (!first_kind) json += ",";
    first_kind = false;
    const auto saved = settings.drives.find(kind.id);
    const DriveSettings drive = saved == settings.drives.end() ? DriveSettings{} : saved->second;
    json += "{\"id\":\"" + json_escape(kind.id) + "\",\"name\":\"" + json_escape(kind.name) + "\",\"intro\":\"" +
            json_escape(kind.intro) + "\",\"library_path\":\"" + json_escape(drive.library_path) + "\",\"fields\":[";
    for (std::size_t i = 0; i < kind.fields.size(); ++i) {
      const auto& field = kind.fields[i];
      const auto value = drive.values.find(field.key);
      if (i > 0) json += ",";
      json += "{\"key\":\"" + json_escape(field.key) + "\",\"label\":\"" + json_escape(field.label) +
              "\",\"secret\":" + (field.secret ? "true" : "false") + ",\"hint\":\"" + json_escape(field.hint) +
              "\",\"value\":\"" + json_escape(value == drive.values.end() ? "" : value->second) + "\"}";
    }
    json += "]}";
  }
  json += "],\"target\":\"" + json_escape(settings.target) + "\",\"targets\":[";
  const auto modes = target_modes();
  for (std::size_t i = 0; i < modes.size(); ++i) json += (i > 0 ? ",\"" : "\"") + modes[i] + "\"";
  json += "],\"local_dir\":\"" + json_escape(settings.local_dir) + "\",\"ftp_host\":\"" + json_escape(settings.ftp_host) +
          "\",\"ftp_port\":\"" + json_escape(settings.ftp_port) + "\",\"ftp_dir\":\"" + json_escape(settings.ftp_dir) +
          "\",\"max_dictionary_mb\":" + std::to_string(settings.max_dictionary_mb) +
          ",\"default_dictionary_mb\":" + std::to_string(kDefaultMaxDictionaryMb) +
          ",\"verbose_log\":" + (settings.verbose_log ? "true" : "false") +
          ",\"drive_cap_gb\":" + std::to_string(settings.drive_cap_gb) +
          ",\"drive_reserve_gb\":" + std::to_string(settings.drive_reserve_gb) + ",\"log_path\":\"" +
          json_escape((panplay::app_data_dir() / "panplay.log").u8string()) + "\",\"version\":\"" +
          json_escape(panplay::app_version()) + "\"}";
  return json;
}

std::string trimmed(std::string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

// 只改当前插件那一份参数，其他插件的保持不动。返回不合格的原因。
std::string apply_settings(const panplay::Json& json, AppSettings* settings) {
  const panplay::DriveKind* kind = panplay::find_drive_kind(json.str("drive"));
  if (kind == nullptr) return "不认识这个网盘插件";
  const auto modes = target_modes();
  const std::string mode = json.str("target");
  if (std::find(modes.begin(), modes.end(), mode) == modes.end()) return "这台设备不支持这种保存方式";

  DriveSettings& drive = settings->drives[kind->id];
  const std::string library_path = trimmed(json.str("library_path"));
  if (!library_path.empty() && library_path.front() != '/') return "游戏库目录要以 / 开头";
  if (!library_path.empty()) drive.library_path = library_path;
  if (const panplay::Json* values = json.get("values")) {
    for (const auto& field : kind->fields) {
      if (values->get(field.key) != nullptr) drive.values[field.key] = trimmed(values->str(field.key));
    }
  }

  std::string host = trimmed(json.str("ftp_host"));
  if (host.rfind("ftp://", 0) == 0) host = host.substr(6);
  while (!host.empty() && host.back() == '/') host.pop_back();
  const std::string port = trimmed(json.str("ftp_port"));
  const std::string ftp_dir = trimmed(json.str("ftp_dir"));
  if (mode == "ftp" && !host.empty()) {
    const bool digits = !port.empty() && port.size() <= 5 &&
                        std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); });
    if (!digits || std::stoi(port) < 1 || std::stoi(port) > 65535) return "端口要是 1 到 65535 之间的数字";
    if (host.find_first_of(":/ ") != std::string::npos) return "主机地址只填 IP 或主机名，端口单独填";
  }
  if (!ftp_dir.empty() && ftp_dir.front() != '/') return "主机目录要以 / 开头";
  const bool has_dictionary = json.get("max_dictionary_mb") != nullptr;
  const double dictionary = json.num("max_dictionary_mb");
  if (has_dictionary && (dictionary < 0 || dictionary > 65536 || dictionary != static_cast<double>(static_cast<std::uint64_t>(dictionary)))) {
    return "字典上限要是 0 到 65536 之间的整数（MB）";
  }
  const auto whole_gb = [&](const char* key, double low, std::uint64_t* out) -> bool {
    if (json.get(key) == nullptr) return true;
    const double value = json.num(key);
    if (value < low || value > 1048576 || value != static_cast<double>(static_cast<std::uint64_t>(value))) return false;
    *out = static_cast<std::uint64_t>(value);
    return true;
  };
  std::uint64_t cap = settings->drive_cap_gb;
  std::uint64_t reserve = settings->drive_reserve_gb;
  if (!whole_gb("drive_cap_gb", 1, &cap)) return "游戏区上限要是大于 0 的整数（GB）";
  if (!whole_gb("drive_reserve_gb", 0, &reserve)) return "网盘保底要是 0 或以上的整数（GB）";

  settings->drive = kind->id;
  settings->target = mode;
  settings->local_dir = trimmed(json.str("local_dir"));
  settings->ftp_host = host;
  if (!port.empty()) settings->ftp_port = port;
  if (!ftp_dir.empty()) settings->ftp_dir = ftp_dir;
  if (has_dictionary) settings->max_dictionary_mb = static_cast<std::uint64_t>(dictionary);
  settings->drive_cap_gb = cap;
  settings->drive_reserve_gb = reserve;
  if (json.get("verbose_log") != nullptr) settings->verbose_log = json.boolean_or("verbose_log", false);
  return "";
}

std::filesystem::path ui_root() {
  const std::filesystem::path found = panplay::executable_dir();
  const std::filesystem::path exe = found.empty() ? std::filesystem::path(".") : found;
  const std::filesystem::path candidates[] = {exe / "ui", exe / ".." / "ui", exe / ".." / ".." / "ui", std::filesystem::path("ui")};
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate / "index.html")) return candidate;
  }
  return "ui";
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// 磁盘上有 ui/ 目录时优先用它，改界面不用重新编译；没有时用打包进程序的那份。
std::string ui_file(const std::string& name) {
  std::string bytes = read_file(ui_root() / name);
  if (bytes.empty()) {
    for (std::size_t i = 0; i < panplay_embedded::file_count; ++i) {
      const auto& file = panplay_embedded::files[i];
      if (name == file.name) return std::string(reinterpret_cast<const char*>(file.data), file.size);
    }
  }
  return bytes;
}

// 局域网访问：总是监听所有网卡，不允许局域网访问时只接本机的连接，所以设置页改了立刻生效。
// 开了口令后，别的设备要先输口令换一个会话 Cookie；口令存在数据目录。本机打开不用口令。
struct LanAccess {
  std::atomic<bool> enabled{true};
  std::atomic<bool> require_code{false};
  bool required = false;
  int port = 0;
  std::string code;
  std::mutex mu;
  std::set<std::string> sessions;
};

LanAccess lan;

std::function<void()> on_quit;

std::string random_digits(std::size_t count) {
  std::random_device device;
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out.push_back(static_cast<char>('0' + device() % 10));
  return out;
}

std::string random_token() {
  static const char* hex = "0123456789abcdef";
  std::random_device device;
  std::string out;
  for (int i = 0; i < 32; ++i) out.push_back(hex[device() % 16]);
  return out;
}

std::string access_code_path() { return (panplay::app_data_dir() / "access-code.txt").u8string(); }

std::string load_or_create_access_code() {
  const std::string path = access_code_path();
  std::string code = read_file(std::filesystem::u8path(path));
  while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) code.pop_back();
  if (code.size() >= 4) return code;
  code = random_digits(6);
  std::ofstream out(std::filesystem::u8path(path), std::ios::trunc);
  out << code << "\n";
  return code;
}

std::string save_access_code(const std::string& code) {
  std::error_code ec;
  std::filesystem::create_directories(panplay::app_data_dir(), ec);
  std::ofstream out(std::filesystem::u8path(access_code_path()), std::ios::trunc);
  out << code << "\n";
  return out ? "" : "无法保存访问口令";
}

bool same_text(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

struct InstallTask {
  std::string path;
  panplay::InstallResult result;
  std::shared_ptr<panplay::CancelToken> cancel;
  // 平均速度从第一次真正解压算起：续装开头核对已装文件时进度跳得飞快，不算进去。
  // rate_base 是开始计时时的进度，没开始时为 -1；rate_last 是最近一次解压进度的时间。
  std::int64_t rate_base = -1;
  std::chrono::steady_clock::time_point rate_since;
  std::chrono::steady_clock::time_point rate_last;
  // 剩余时间按最近的速度估：每秒取一次样，做时间常数 60 秒的指数加权平均。recent_bytes 为 -1 时没在取样
  // （还没开始解压、在核对已装文件、在等自动续装），下次正在安装时重新起头，中间停着的时间不算。
  double recent_rate = 0;
  double recent_seconds = 0;
  std::int64_t recent_bytes = -1;
  std::chrono::steady_clock::time_point recent_at;
  // 全部暂停时停下的：装完这一轮回到队首，不进已完成。
  bool pausing = false;
};

// 安装队列：一次只装一个，装完自动接着装下一个。正在装的和排队的会存到磁盘，程序重开后接着装。
// finished 是这次运行里装完、失败或取消的，最近的在前，用户可以清掉。
struct InstallQueue {
  std::mutex mu;
  std::unique_ptr<InstallTask> current;
  std::deque<InstallTask> waiting;
  std::deque<InstallTask> finished;
  // 正在删除文件（没装完的或整个游戏）的游戏，删完之前不能再加入队列。
  std::set<std::string> discarding;
  // 全部暂停：排队的不开始，用户点继续之前一直停着；存到磁盘，重开程序后还是暂停的。
  bool paused = false;
  // 不是用户点的暂停（存放位置写满了）时写明原因，安装条上显示；继续或队列清空时清掉。
  std::string pause_reason;
  // 正在退出：不再开始新的安装。
  bool quitting = false;
  bool worker_running = false;
  std::thread worker;
};

InstallQueue installs;
constexpr std::size_t kFinishedKept = 50;

// 游戏库里能看到的情况（装好、没装完、删掉、评价、名字）每变一次加一。队列接口和游戏库接口都带上，
// 页面发现和自己取游戏库时的不一样就重取，别的设备上做的改动、装完一个游戏，不用刷新页面就能看到。
std::atomic<long long> library_version{1};

void library_changed() { library_version.fetch_add(1); }

// 正在装的是 "installing"，排队的是 "queued"，不在队列里没有记录。
std::map<std::string, std::string> queue_states() {
  std::lock_guard<std::mutex> lock(installs.mu);
  std::map<std::string, std::string> states;
  // 装的是某种格式（变体目录）时，游戏库列表里按游戏目录也显示出来。
  for (auto task = installs.waiting.rbegin(); task != installs.waiting.rend(); ++task) {
    states[panplay::game_dir_of(task->path)] = "queued";
    states[task->path] = "queued";
  }
  if (installs.current) {
    states[panplay::game_dir_of(installs.current->path)] = "installing";
    states[installs.current->path] = "installing";
  }
  return states;
}

// 本机安装队列里的（正在装的和排队的），腾网盘空间时不动它们。
std::set<std::string> queued_paths() {
  std::lock_guard<std::mutex> lock(installs.mu);
  std::set<std::string> paths;
  if (installs.current) paths.insert(installs.current->path);
  for (const auto& task : installs.waiting) paths.insert(task.path);
  return paths;
}

void send_all(SOCKET socket, const std::string& data) { panplay::send_all(socket, data.data(), data.size()); }

void reply(SOCKET socket, int status, const std::string& type, const std::string& body, const std::string& extra = "") {
  const std::string text = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                           "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
                           extra + "Connection: close\r\n\r\n" + body;
  send_all(socket, text);
}

void reply_json(SOCKET socket, int status, const std::string& body) { reply(socket, status, "application/json; charset=utf-8", body); }

std::string json_error(const std::string& message) { return "{\"error\":\"" + panplay::json_escape(message) + "\"}"; }

// 压缩包本身装不了时返回 "empty" 或 "unsupported"，能装返回空。
std::string archive_problem(const panplay::GameFolder& game) {
  if (game.sets.empty()) return "empty";
  for (const auto& set : game.sets) {
    if (set.format == "zip" || set.format == "rar" || set.format == "7z") return "";
  }
  return "unsupported";
}

// 和游戏库列表的判断一致：暂存区里有这款游戏的目录就算没装完（续装日志可能已经没了，留下的文件要能删掉；
// 已装好的游戏更新到一半也是这样），其次存放目录里有这款游戏的目录就算装好了。用 FTP 拷进去或别的工具装的游戏
// 没有盘游的完成标记，也算装好。
std::string game_state(panplay::InstallSink& sink, const std::string& prefix, const panplay::GameFolder& game) {
  const std::string folder = game.title_id.empty() ? game.name : game.title_id;
  std::string error;
  const auto staging = sink.list_children(panplay::join_storage_path(prefix, ".panplay-staging"), &error);
  if (std::find(staging.begin(), staging.end(), folder) != staging.end()) return "continue";
  error.clear();
  const auto installed = sink.list_children(prefix, &error);
  if (std::find(installed.begin(), installed.end(), folder) != installed.end()) return "installed";
  return "install";
}

std::string formats_of(const panplay::GameFolder& game) {
  std::string formats;
  for (const auto& set : game.sets) {
    if (formats.find(set.format) == std::string::npos) {
      if (!formats.empty()) formats += "+";
      formats += set.format;
    }
  }
  return formats.empty() ? "-" : formats;
}

struct ShelfPlacement {
  std::unordered_set<std::string> installed_dirs;
  std::unordered_set<std::string> staging_dirs;
  std::map<std::string, std::string> queued;
};

ShelfPlacement read_shelf_placement(panplay::InstallSink& sink, const std::string& prefix) {
  ShelfPlacement placement;
  std::string error;
  for (const auto& name : sink.list_children(prefix, &error)) {
    if (name != ".panplay-staging") placement.installed_dirs.insert(name);
  }
  error.clear();
  for (const auto& name : sink.list_children(panplay::join_storage_path(prefix, ".panplay-staging"), &error)) {
    placement.staging_dirs.insert(name);
  }
  return placement;
}

std::string shelf_state(const ShelfPlacement& placement, const std::string& name, const std::string& title_id,
                        const std::string& path) {
  const std::string folder = title_id.empty() ? name : title_id;
  const auto queued = placement.queued.find(path);
  if (queued != placement.queued.end()) return queued->second;
  if (placement.staging_dirs.count(folder) != 0) return "continue";
  if (placement.installed_dirs.count(folder) != 0) return "installed";
  return "install";
}

// 不析构：退出时还在测的话，析构要等后台线程收尾。
panplay::SpeedTest& speed_test = *new panplay::SpeedTest;

// 开始测速时拿来和网速比的那次安装：正在装的，没有就取最近装过、算得出速度的。
// 记在服务端，刷新页面或换个页面看结论还是同一次。
struct SpeedReference {
  std::string path;
  long long speed = 0;
  bool running = false;
};
std::mutex speed_reference_mu;
SpeedReference speed_reference;

std::string speed_test_json() {
  const panplay::SpeedTestStatus s = speed_test.status();
  const auto rate = [](double value) { return std::to_string(static_cast<long long>(value)); };
  const auto step = [&](const panplay::SpeedTestStep& item) {
    return "{\"connections\":" + std::to_string(item.connections) + ",\"connected\":" + std::to_string(item.connected) +
           ",\"rate\":" + rate(item.rate) + "}";
  };
  std::string install = "null";
  {
    std::lock_guard<std::mutex> lock(speed_reference_mu);
    if (!speed_reference.path.empty()) {
      install = "{\"path\":\"" + panplay::json_escape(speed_reference.path) + "\",\"speed\":" +
                std::to_string(speed_reference.speed) + ",\"running\":" + (speed_reference.running ? "true" : "false") + "}";
    }
  }
  std::string levels;
  for (const int level : s.levels) levels += (levels.empty() ? "" : ",") + std::to_string(level);
  std::string steps;
  for (const auto& item : s.steps) steps += (steps.empty() ? "" : ",") + step(item);
  return "{\"phase\":\"" + s.phase + "\",\"file\":\"" + panplay::json_escape(s.file_name) +
         "\",\"file_size\":" + std::to_string(s.file_size) + ",\"levels\":[" + levels + "],\"steps\":[" + steps +
         "],\"measuring\":" + (s.measuring.connections > 0 ? step(s.measuring) : "null") +
         ",\"connections\":" + std::to_string(s.connections) + ",\"current\":" + rate(s.current) +
         ",\"bytes\":" + std::to_string(s.bytes) + ",\"seconds\":" + std::to_string(static_cast<long long>(s.seconds)) +
         ",\"first_byte_ms\":" + std::to_string(static_cast<long long>(s.first_byte_ms)) + ",\"ended\":\"" + s.ended +
         "\",\"error\":\"" + panplay::json_escape(s.error) + "\",\"install\":" + install + "}";
}

panplay::DiskTest& disk_test = *new panplay::DiskTest;

std::string disk_test_json() {
  const panplay::DiskTestStatus s = disk_test.status();
  const auto rate = [](double value) { return std::to_string(static_cast<long long>(value)); };
  std::string steps;
  for (const auto& step : s.steps) {
    steps += (steps.empty() ? "" : ",") + std::string("{\"label\":\"") + panplay::json_escape(step.label) +
             "\",\"rate\":" + rate(step.rate) + ",\"done\":" + (step.done ? "true" : "false") + "}";
  }
  return "{\"phase\":\"" + s.phase + "\",\"where\":\"" + panplay::json_escape(s.where) + "\",\"steps\":[" + steps +
         "],\"current\":" + std::to_string(s.current) + ",\"bytes\":" + std::to_string(s.bytes) +
         ",\"total\":" + std::to_string(s.total) + ",\"current_rate\":" + rate(s.current_rate) + ",\"ended\":\"" +
         s.ended + "\",\"error\":\"" + panplay::json_escape(s.error) + "\",\"read_cached\":" +
         (s.read_cached ? "true" : "false") + "}";
}

std::string catalog_status_json() {
  const panplay::GameCatalog& catalog = panplay::GameCatalog::instance();
  const panplay::CatalogMeta meta = catalog.meta();
  const panplay::CatalogSyncInfo sync = panplay::CatalogSync::instance().info();
  const char* phase = sync.phase == panplay::CatalogSyncPhase::updating ? "updating" : "idle";
  return std::string("{\"loaded\":") + (catalog.loaded() ? "true" : "false") +
         ",\"builtin\":" + (meta.builtin ? "true" : "false") + ",\"count\":" +
         std::to_string(catalog.size()) + ",\"stale\":" + (catalog.is_stale(sync.stale_days) ? "true" : "false") +
         ",\"version\":" + std::to_string(meta.version) + ",\"path\":\"" + panplay::json_escape(meta.path) +
         "\",\"source\":\"" + panplay::json_escape(meta.source) + "\",\"generated_at\":\"" +
         panplay::json_escape(meta.generated_at) + "\",\"generated_from\":\"" +
         panplay::json_escape(meta.generated_from) + "\",\"file_mtime\":" + std::to_string(meta.file_mtime_unix) +
         ",\"sync\":{\"phase\":\"" + std::string(phase) + "\",\"last_error\":\"" +
         panplay::json_escape(sync.last_error) + "\",\"last_attempt\":" + std::to_string(sync.last_attempt_unix) +
         ",\"last_success\":" + std::to_string(sync.last_success_unix) + ",\"auto\":true,\"stale_days\":" +
         std::to_string(sync.stale_days) + ",\"check_interval_hours\":" + std::to_string(sync.check_interval_hours) +
         "}}";
}

panplay::StoreMetaCache* store_meta = nullptr;

using NameOverrides = std::map<std::string, std::string>;

std::string name_key(const std::string& name, const std::string& title_id) { return title_id.empty() ? name : title_id; }

// 展示名：手动改名 > 商店名称 > PlayStation-Titles 英文名 > 目录名（由页面兜底）。
// search_text 不为空时写入展示名和英文名，供游戏库搜索用。
std::string json_string_array(const std::vector<std::string>& items) {
  std::string out = "[";
  for (const auto& item : items) {
    if (out.size() > 1) out += ",";
    out += "\"" + panplay::json_escape(item) + "\"";
  }
  return out + "]";
}

std::string store_meta_fields(const panplay::StoreMeta& meta, bool detail) {
  const bool rated = meta.found && meta.rating_count > 0;
  char rating[32];
  std::snprintf(rating, sizeof(rating), "%.2f", meta.rating);
  std::string json = std::string(",\"rating\":") + (rated ? rating : "null") +
                     ",\"rating_count\":" + std::to_string(rated ? meta.rating_count : 0) +
                     ",\"has_cn_sub\":" + (meta.found && meta.has_cn_sub() ? "true" : "false") +
                     ",\"has_cn_voice\":" + (meta.found && meta.has_cn_voice() ? "true" : "false") +
                     ",\"players\":" + (meta.found && meta.players > 0 ? std::to_string(meta.players) : "null");
  if (!detail) return json;
  std::string dist = "null";
  if (rated && meta.rating_dist.size() == 5) {
    dist = "[";
    for (const int pct : meta.rating_dist) dist += (dist.size() > 1 ? "," : "") + std::to_string(pct);
    dist += "]";
  }
  json += ",\"publisher\":\"" + panplay::json_escape(meta.publisher) + "\",\"genres\":\"" +
          panplay::json_escape(meta.genres) + "\",\"release_date\":\"" + panplay::json_escape(meta.release_date) +
          "\",\"description\":\"" + panplay::json_escape(meta.description) + "\",\"rating_dist\":" + dist +
          ",\"screen_langs\":" + json_string_array(meta.screen_langs) + ",\"spoken_langs\":" +
          json_string_array(meta.spoken_langs) + ",\"store_platforms\":" + json_string_array(meta.platforms) +
          ",\"online_play\":\"" + panplay::json_escape(meta.online_play) + "\",\"ps_plus_required\":" +
          (meta.ps_plus_required ? "true" : "false") + ",\"in_game_purchases\":" +
          (meta.in_game_purchases ? "true" : "false") + ",\"classification\":\"" +
          panplay::json_escape(meta.classification) + "\",\"edition\":\"" + panplay::json_escape(meta.edition) + "\"";
  return json;
}

// 查商店用哪个 conceptId：这款的商店页是空的、盘架借了同名游戏的那页时用那页的；资料库没收录时用盘架认出来的。
std::int64_t store_concept_of(const panplay::CatalogMatch& match, const ShelfGame* shelf) {
  if (shelf != nullptr && shelf->store_concept_id > 0) return shelf->store_concept_id;
  if (match.found && match.entry.concept_id > 0) return match.entry.concept_id;
  return shelf != nullptr ? shelf->concept_id : 0;
}

// 帖子封面地址的指纹（FNV-1a），换了图地址就变，浏览器不会用缓存的旧图。
std::string site_cover_key(const std::string& url) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char c : url) hash = (hash ^ c) * 1099511628211ull;
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}

// 盘架资源库里有这款游戏时，商店和资料库都没有的名字、封面、介绍用盘架的（介绍取自资源站帖子）。
std::string game_meta_fields(const std::string& name, const std::string& title_id, const NameOverrides& overrides,
                             bool detail, std::string* search_text = nullptr, panplay::StoreMeta* meta_out = nullptr) {
  panplay::CatalogMatch match;
  if (!title_id.empty()) match = panplay::GameCatalog::instance().lookup(title_id, name);
  const auto library = shelf_library();
  const ShelfGame* shelf = library ? library->find(title_id) : nullptr;
  const std::string shelf_name = shelf != nullptr ? shelf->name : "";
  const std::int64_t concept_id = store_concept_of(match, shelf);

  panplay::StoreMeta meta;
  std::string meta_state = "none";
  if (concept_id > 0 && store_meta != nullptr) meta_state = panplay::store_meta_state_name(store_meta->lookup(concept_id, &meta));
  if (meta_out != nullptr) *meta_out = meta;
  const bool site_info = !meta.found && shelf != nullptr && !shelf->info.empty();
  if (site_info) {
    meta.description = shelf->info.description;
    meta.publisher = shelf->info.publisher;
    meta.release_date = shelf->info.release_date;
  }

  std::string display_name;
  std::string name_source;
  const auto custom = overrides.find(name_key(name, title_id));
  if (custom != overrides.end()) {
    display_name = custom->second;
    name_source = "user";
  } else if (meta.found) {
    display_name = meta.name;
    name_source = "store";
  } else if (match.found) {
    display_name = match.entry.name;
    name_source = "catalog";
  } else if (!shelf_name.empty()) {
    display_name = shelf_name;
    name_source = "shelf";
  }

  if (search_text != nullptr) {
    *search_text = display_name + "\n" + (match.found ? match.entry.name : "") + "\n" + shelf_name;
  }

  std::string cover_url;
  // 封面从 AVIF、WebP 换成 JPEG 时抓取时间不变，加上 jpg 让浏览器别用缓存的旧图。
  if (meta.has_cover) cover_url = "/api/cover/" + std::to_string(concept_id) + "?v=" + std::to_string(meta.fetched_at) + "jpg";
  else if (match.found && !match.entry.cover_url.empty()) cover_url = match.entry.cover_url;
  // 帖子封面在外面的图床上，PS5 的浏览器打不开，由盘游下载后再给页面。
  else if (site_info && !shelf->info.cover.empty()) cover_url = "/api/site-cover/" + title_id + "?v=" + site_cover_key(shelf->info.cover);

  std::string json = ",\"display_name\":\"" + panplay::json_escape(display_name) + "\",\"name_source\":\"" + name_source +
                     "\",\"description_source\":\"" + (meta.description.empty() ? "" : site_info ? "site" : "store") +
                     "\",\"store_locale\":\"" + panplay::json_escape(meta.found ? meta.locale : "") +
                     "\",\"catalog_name\":\"" + panplay::json_escape(match.found ? match.entry.name : "") +
                     "\",\"cover_url\":\"" + panplay::json_escape(cover_url) + "\",\"meta_state\":\"" + meta_state +
                     "\",\"concept_id\":" + std::to_string(concept_id) + ",\"region\":\"" +
                     panplay::json_escape(match.entry.region) + "\",\"publisher_id\":\"" +
                     panplay::json_escape(match.entry.publisher_id) + "\",\"platform\":\"" +
                     panplay::json_escape(match.entry.platform) + "\",\"catalog_matched\":" +
                     (match.found ? "true" : "false") + ",\"catalog_match_by\":\"" + panplay::json_escape(match.match_by) + "\"";
  return json + store_meta_fields(meta, detail);
}

std::string meta_json(const std::string& name, const NameOverrides& overrides, bool detail) {
  const std::string title_id = panplay::find_title_id(name);
  return "{\"name\":\"" + panplay::json_escape(name) + "\",\"title_id\":\"" + panplay::json_escape(title_id) + "\"" +
         game_meta_fields(name, title_id, overrides, detail) + "}";
}

std::string folder_json(const std::string& name, const std::string& path, const std::string& title_id,
                        std::int64_t archive_bytes, const std::string& state, const std::string& meta_fields) {
  return "{\"name\":\"" + panplay::json_escape(name) + "\",\"path\":\"" + panplay::json_escape(path) + "\",\"title_id\":\"" +
         panplay::json_escape(title_id) + "\",\"archive_bytes\":" + std::to_string(archive_bytes) +
         ",\"formats\":\"\",\"state\":\"" + state + "\"" + meta_fields + "}";
}

// 游戏目录名。path 是按格式分的变体目录（…/PPSA01234/@exfat）时取上一层。
std::string folder_name_of(const std::string& path) {
  const std::string game_dir = panplay::game_dir_of(path);
  const auto slash = game_dir.find_last_of('/');
  return slash == std::string::npos ? game_dir : game_dir.substr(slash + 1);
}

// 评价、盘架里格式的叫法：变体目录 @other 在盘架里叫 unknown。
std::string review_format(const std::string& path) {
  const std::string format = panplay::variant_format(path);
  return format == "other" ? "unknown" : format;
}

std::string image_type(const std::string& bytes) {
  if (bytes.size() >= 4 && bytes.compare(1, 3, "PNG") == 0) return "image/png";
  if (bytes.size() >= 12 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
  if (bytes.size() >= 12 && bytes.compare(4, 4, "ftyp") == 0) return "image/avif";
  return "image/jpeg";
}

std::mutex site_cover_mu;
// 帖子封面下载失败的时刻，过一阵才再试，免得一页几十个格子每次都去等慢图床。
std::map<std::string, std::chrono::steady_clock::time_point> site_cover_failed;

// 盘架资源库里这款游戏的帖子封面：按地址缓存在数据目录的 site-covers/，没有时下载。取不到时为空。
std::string site_cover_bytes(const std::string& title_id) {
  const auto library = shelf_library();
  const ShelfGame* shelf = library ? library->find(title_id) : nullptr;
  if (shelf == nullptr || shelf->info.cover.empty()) return "";
  const std::string url = shelf->info.cover;
  const std::string key = site_cover_key(url);
  const auto dir = panplay::app_data_dir() / "site-covers";
  const auto path = dir / (title_id + "-" + key + ".img");
  if (std::string bytes = read_file(path); !bytes.empty()) return bytes;
  {
    std::lock_guard<std::mutex> lock(site_cover_mu);
    const auto failed = site_cover_failed.find(key);
    if (failed != site_cover_failed.end() && std::chrono::steady_clock::now() - failed->second < std::chrono::minutes(10)) {
      return "";
    }
  }
  panplay::HttpRequest request;
  request.url = url;
  request.timeout_ms = 30000;
  const panplay::HttpResponse response = panplay::http_send(request);
  const std::string& body = response.body;
  const bool image = body.size() > 4 && ((static_cast<unsigned char>(body[0]) == 0xFF && static_cast<unsigned char>(body[1]) == 0xD8) ||
                                         body.compare(1, 3, "PNG") == 0 || body.compare(0, 4, "GIF8") == 0 ||
                                         (body.size() >= 12 && (body.compare(8, 4, "WEBP") == 0 || body.compare(4, 4, "ftyp") == 0)));
  if (!response.error.empty() || response.status != 200 || !image) {
    panplay::log_warn("帖子封面 " + title_id + " 取不到：" +
                      (!response.error.empty() ? response.error : "HTTP " + std::to_string(response.status)) + "，" + url);
    std::lock_guard<std::mutex> lock(site_cover_mu);
    site_cover_failed[key] = std::chrono::steady_clock::now();
    return "";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::filesystem::remove(tmp, ec);
  return body;
}

// 游戏库目录和主机上的安装情况。读一次要列网盘目录、连主机，约两秒，所以缓存起来：
// 翻页、搜索、切换筛选都用缓存，只有页面要求刷新（首次打开、装完一个游戏）或设置变了才重读。
struct ShelfListing {
  struct Folder {
    std::string name;
    std::string path;
    std::string title_id;
  };
  std::string key;
  std::vector<Folder> folders;
  // 盘游装好、网盘里已经没有对应游戏目录的游戏。path 是按存放目录名拼出的游戏库路径，网盘里并不存在。
  std::vector<Folder> orphans;
  bool dest_ready = false;
  bool sink_ready = false;
  ShelfPlacement placement;
};

std::mutex shelf_mu;
std::shared_ptr<const ShelfListing> shelf_cache;

std::string shelf_key(const AppSettings& settings) {
  return settings.drive + "\n" + settings.library_path() + "\n" + settings.dest() + "\n" + settings.prefix();
}

// 网盘上的容量和存放位置无关。
std::string sizes_key(const AppSettings& settings) { return settings.drive + "\n" + settings.library_path(); }

// 游戏库列表读到的主机安装情况，还没读过或设置变了时为空。
std::shared_ptr<const ShelfListing> cached_shelf(const AppSettings& settings) {
  std::lock_guard<std::mutex> lock(shelf_mu);
  return shelf_cache && shelf_cache->key == shelf_key(settings) ? shelf_cache : nullptr;
}

// 主机上装着的、网盘游戏库目录里也有的游戏目录，按列表上次读到的安装情况。写进在用记录，腾网盘空间时先退它们。
std::set<std::string> installed_game_dirs() {
  std::set<std::string> dirs;
  const auto listing = cached_shelf(load_settings());
  if (!listing || !listing->sink_ready) return dirs;
  for (const auto& folder : listing->folders) {
    if (listing->placement.installed_dirs.count(folder.title_id.empty() ? folder.name : folder.title_id) != 0) {
      dirs.insert(folder.path);
    }
  }
  return dirs;
}

bool same_listing(const ShelfListing& left, const ShelfListing& right) {
  const auto same_folders = [](const std::vector<ShelfListing::Folder>& a, const std::vector<ShelfListing::Folder>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const ShelfListing::Folder& x, const ShelfListing::Folder& y) { return x.path == y.path; });
  };
  return left.key == right.key && left.dest_ready == right.dest_ready && left.sink_ready == right.sink_ready &&
         same_folders(left.folders, right.folders) && same_folders(left.orphans, right.orphans) &&
         left.placement.installed_dirs == right.placement.installed_dirs &&
         left.placement.staging_dirs == right.placement.staging_dirs;
}

void read_listing_placement(ShelfListing& listing, panplay::InstallSink& sink, const AppSettings& settings) {
  listing.sink_ready = true;
  listing.placement = read_shelf_placement(sink, settings.prefix());
  std::unordered_set<std::string> claimed;
  for (const auto& folder : listing.folders) claimed.insert(folder.title_id.empty() ? folder.name : folder.title_id);
  // 存放位置常和别的自制程序共用（/data/homebrew），只认带盘游完成标记的文件夹。
  for (const auto& dir : listing.placement.installed_dirs) {
    if (claimed.count(dir) != 0 || dir.rfind(".panplay", 0) == 0) continue;
    std::string size_error;
    const std::string marker = panplay::join_storage_path(panplay::join_storage_path(settings.prefix(), dir), ".panplay-done.json");
    if (sink.file_size(marker, &size_error) < 0) continue;
    listing.orphans.push_back({dir, panplay::join_storage_path(settings.library_path(), dir), panplay::find_title_id(dir)});
  }
  std::sort(listing.orphans.begin(), listing.orphans.end(),
            [](const ShelfListing::Folder& left, const ShelfListing::Folder& right) { return left.name < right.name; });
}

std::shared_ptr<const ShelfListing> shelf_listing(panplay::DrivePlugin& drive, bool fresh, std::string* error) {
  const AppSettings settings = load_settings();
  const std::string key = shelf_key(settings);
  std::lock_guard<std::mutex> lock(shelf_mu);
  if (!fresh && shelf_cache && shelf_cache->key == key) return shelf_cache;
  const std::string library_path = settings.library_path();
  const auto children = drive.list(library_path, error);
  if (!error->empty()) return nullptr;
  auto listing = std::make_shared<ShelfListing>();
  listing->key = key;
  for (const auto& child : children) {
    if (!child.is_dir) continue;
    const std::string path = child.path.empty() ? panplay::join_storage_path(library_path, child.name) : child.path;
    listing->folders.push_back({child.name, path, panplay::find_title_id(child.name)});
  }
  panplay::Destination destination;
  std::string dest_error;
  const std::string dest = settings.dest();
  listing->dest_ready = !dest.empty() && panplay::parse_destination(dest, &destination, &dest_error);
  if (listing->dest_ready) {
    if (auto sink = panplay::open_destination(destination, &dest_error)) read_listing_placement(*listing, *sink, settings);
  }
  if (shelf_cache && !same_listing(*shelf_cache, *listing)) library_changed();
  shelf_cache = listing;
  std::vector<std::string> game_dirs;
  for (const auto& folder : listing->folders) game_dirs.push_back(folder.path);
  want_game_sizes(sizes_key(settings), game_dirs);
  return listing;
}

// 装完、停下、删掉游戏后只重读主机上的安装情况，换掉列表缓存里的这部分；网盘目录没变，不重列。
void refresh_shelf_placement() {
  const AppSettings settings = load_settings();
  const auto old = cached_shelf(settings);
  if (!old || !old->dest_ready) return;
  panplay::Destination destination;
  std::string error;
  if (!panplay::parse_destination(settings.dest(), &destination, &error)) return;
  auto sink = panplay::open_destination(destination, &error);
  std::shared_ptr<ShelfListing> listing;
  if (sink) {
    listing = std::make_shared<ShelfListing>(*old);
    listing->orphans.clear();
    read_listing_placement(*listing, *sink, settings);
  }
  std::lock_guard<std::mutex> lock(shelf_mu);
  // 读的时候列表已经整个重读过了，那份更新，不用换；连不上存放位置时丢掉缓存，下次整个重读。
  if (shelf_cache == old) shelf_cache = listing;
}

bool is_orphan(const ShelfListing& listing, const std::string& path) {
  return std::any_of(listing.orphans.begin(), listing.orphans.end(),
                     [&](const ShelfListing::Folder& folder) { return folder.path == path; });
}

// 详情页实时读到的安装情况和游戏库列表缓存的不一样时，改掉列表缓存里的这一项，回到列表就是新的。
void sync_shelf_state(const std::string& key, const std::string& name, const std::string& title_id,
                      const std::string& state) {
  const bool installed = state == "installed" || state == "update";
  const bool staging = state == "continue";
  if (!installed && !staging && state != "install") return;
  const std::string folder = title_id.empty() ? name : title_id;
  std::lock_guard<std::mutex> lock(shelf_mu);
  if (!shelf_cache || shelf_cache->key != key || !shelf_cache->sink_ready) return;
  const ShelfPlacement& old = shelf_cache->placement;
  const bool was_staging = old.staging_dirs.count(folder) != 0;
  const bool was_installed = old.installed_dirs.count(folder) != 0;
  if (was_staging == staging && (staging || was_installed == installed)) return;
  auto listing = std::make_shared<ShelfListing>(*shelf_cache);
  if (staging) listing->placement.staging_dirs.insert(folder);
  else listing->placement.staging_dirs.erase(folder);
  if (installed) listing->placement.installed_dirs.insert(folder);
  else if (!staging) listing->placement.installed_dirs.erase(folder);
  shelf_cache = listing;
  library_changed();
}

std::string url_decode(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out += ' ';
    } else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string query_param(const std::string& query, const std::string& key) {
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto end = std::min(query.find('&', start), query.size());
    const std::string pair = query.substr(start, end - start);
    const auto eq = pair.find('=');
    if (pair.substr(0, eq) == key) return eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
    start = end + 1;
  }
  return "";
}

std::string ascii_lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

// 游戏库格子上显示哪个版本的评价：游戏目录按格式分了的，取装着的那种格式，没装就取第一个评过的。
std::string listed_fingerprint(const std::string& key, const std::string& path) {
  const std::string own = cached_game_fingerprint(key, path);
  if (!own.empty()) return own;
  const auto detail = cached_game_detail(key, path);
  if (!detail || detail->variants.empty()) return "";
  std::string first;
  for (const auto& variant : panplay::Json::parse(detail->variants, nullptr).array) {
    const std::string fingerprint = cached_game_fingerprint(key, variant.str("path"));
    if (fingerprint.empty() || !find_review(fingerprint)) continue;
    if (variant.str("format") == detail->installed_variant) return fingerprint;
    if (first.empty()) first = fingerprint;
  }
  return first;
}

// 游戏库格子上显示的容量，还没量过时为 0。分了格式的游戏显示主机上装着的那种（打开过详情才知道），
// 否则显示详情页默认打开的第一种。
std::int64_t listed_bytes(const std::string& sizes, const std::string& key, const std::string& path) {
  const auto size = cached_game_size(sizes, path);
  if (!size || size->variants.empty()) return size ? size->bytes : 0;
  // 量容量时记下的顺序可能是旧的，按现在的优先顺序挑。
  std::string pick = std::min_element(size->variants.begin(), size->variants.end(), [](const auto& left, const auto& right) {
                       return panplay::variant_rank(left.format) < panplay::variant_rank(right.format);
                     })->path;
  if (const auto detail = cached_game_detail(key, path); detail && !detail->installed_variant.empty()) {
    for (const auto& variant : size->variants) {
      if (variant.format == detail->installed_variant) pick = variant.path;
    }
  }
  if (pick == path) return size->bytes;
  const auto chosen = cached_game_size(sizes, pick);
  return chosen ? chosen->bytes : 0;
}

// 一页游戏。filter 是 all/installed/continue（装到一半）/todo（没装过），q 匹配目录名、Title ID、展示名和英文名。
// cn=1 只留商店标了中文字幕或语音的，multi=1 只留支持两人以上的；sort 是 rating/release/name，不传按目录顺序。
// total 是符合筛选的总数，all/installed 是整个游戏库的数量。
std::string games_json(panplay::DrivePlugin& drive, const std::string& query) {
  const bool fresh = query_param(query, "fresh") == "1";
  const std::string filter = query_param(query, "filter");
  const std::string needle = ascii_lower(query_param(query, "q"));
  const std::size_t offset = static_cast<std::size_t>(std::max(0L, std::strtol(query_param(query, "offset").c_str(), nullptr, 10)));
  long limit = std::strtol(query_param(query, "limit").c_str(), nullptr, 10);
  if (limit <= 0 || limit > 500) limit = 500;

  const std::string sort = query_param(query, "sort");
  const bool chinese_only = query_param(query, "cn") == "1";
  const bool multi_only = query_param(query, "multi") == "1";

  // 在读列表之前取：读的过程中又有变化时，页面拿到的是旧号，会再取一次。
  const long long version = library_version.load();
  std::string error;
  const auto listing = shelf_listing(drive, fresh, &error);
  if (!listing) return json_error(error);
  ShelfPlacement placement = listing->placement;
  placement.queued = queue_states();
  const NameOverrides overrides = load_name_overrides();
  const std::string sizes = sizes_key(load_settings());
  struct Row {
    std::string json;
    std::string name;
    panplay::StoreMeta meta;
  };
  std::vector<Row> rows;
  std::size_t installed = 0;
  std::size_t continuing = 0;
  // drive：网盘游戏库目录里的；console：装在主机上、网盘里已经没有的；shelf：盘架资源库里有、网盘里还没有的，
  // 路径按转存后会放的位置拼，装的时候先转存（ADR 0011）。
  enum class Where { drive, console, shelf };
  const auto library = shelf_library();
  std::vector<ShelfListing::Folder> remote_folders;
  std::map<std::string, const ShelfGame*> shelf_games;
  if (library) {
    std::unordered_set<std::string> on_drive;
    for (const auto& folder : listing->folders) on_drive.insert(folder.title_id);
    const std::string library_path = load_settings().library_path();
    for (const auto& game : library->games) {
      if (on_drive.count(game.title_id) != 0) continue;
      const auto orphan = std::find_if(listing->orphans.begin(), listing->orphans.end(),
                                       [&](const ShelfListing::Folder& folder) { return folder.title_id == game.title_id; });
      if (orphan != listing->orphans.end()) {
        shelf_games[orphan->path] = &game;
        continue;
      }
      remote_folders.push_back({game.title_id, panplay::join_storage_path(library_path, game.title_id), game.title_id});
      shelf_games[remote_folders.back().path] = &game;
    }
  }
  std::vector<std::pair<const ShelfListing::Folder*, Where>> entries;
  for (const auto& folder : listing->folders) entries.emplace_back(&folder, Where::drive);
  for (const auto& folder : listing->orphans) entries.emplace_back(&folder, Where::console);
  for (const auto& folder : remote_folders) entries.emplace_back(&folder, Where::shelf);
  for (const auto& [entry, where] : entries) {
    const ShelfListing::Folder& folder = *entry;
    const bool on_drive = where == Where::drive;
    const auto found = shelf_games.find(folder.path);
    const ShelfGame* shelf = found == shelf_games.end() ? nullptr : found->second;
    std::string state = "need-settings";
    if (where == Where::console) state = "installed";
    else if (listing->sink_ready) state = shelf_state(placement, folder.name, folder.title_id, folder.path);
    else if (listing->dest_ready) state = "install";
    // 有没有新补丁要读网盘目录和完成标记，列表不读，用详情页上次读到的。
    if (state == "installed") {
      const auto detail = cached_game_detail(listing->key, folder.path);
      if (detail && detail->state == "update") state = "update";
    }
    const bool on_console = state == "installed" || state == "update";
    if (on_console) ++installed;
    if (state == "continue") ++continuing;
    if (filter == "installed" && !on_console) continue;
    if (filter == "todo" && (on_console || state == "continue")) continue;
    if (filter == "continue" && state != "continue") continue;
    std::string search_text;
    panplay::StoreMeta store;
    const std::string meta = game_meta_fields(folder.name, folder.title_id, overrides, false, &search_text, &store);
    if (!needle.empty() &&
        ascii_lower(folder.name + "\n" + folder.title_id + "\n" + search_text).find(needle) == std::string::npos) {
      continue;
    }
    if (chinese_only && !(store.found && (store.has_cn_sub() || store.has_cn_voice()))) continue;
    if (multi_only && !(store.found && store.players >= 2)) continue;
    const std::string shown = search_text.substr(0, search_text.find('\n'));
    // 打开过详情、读过压缩包的才知道指纹；没读过的不查，列表不为这个多发请求。
    std::string review_field = where == Where::console ? ",\"on_drive\":false" : "";
    if (shelf) review_field += ",\"remote\":true";
    if (const auto review = find_review(listed_fingerprint(listing->key, folder.path))) {
      review_field += std::string(",\"review_state\":\"") + (review->playable ? "good" : "bad") + "\"";
    } else if (shelf) {
      // 盘架把分享和评价对上了的，网盘里还没有也能标出来。
      for (const auto& variant : shelf->variants) {
        if (!variant.reviewed) continue;
        review_field += std::string(",\"review_state\":\"") + (variant.playable ? "good" : "bad") + "\"";
        break;
      }
    }
    std::int64_t bytes = on_drive ? listed_bytes(sizes, listing->key, folder.path) : 0;
    if (!on_drive && shelf) {
      bytes = std::min_element(shelf->variants.begin(), shelf->variants.end(), [](const auto& left, const auto& right) {
                return panplay::variant_rank(shelf_variant_dir(left.format).substr(1)) <
                       panplay::variant_rank(shelf_variant_dir(right.format).substr(1));
              })->size;
    }
    rows.push_back({folder_json(folder.name, folder.path, folder.title_id, bytes, state, meta + review_field),
                    shown.empty() ? folder.name : shown, std::move(store)});
  }

  // 没评分、没发售日的排在最后；同分时保持游戏库目录的顺序。
  if (sort == "rating") {
    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
      const bool l = left.meta.rating_count > 0;
      const bool r = right.meta.rating_count > 0;
      if (l != r) return l;
      if (left.meta.rating != right.meta.rating) return left.meta.rating > right.meta.rating;
      return left.meta.rating_count > right.meta.rating_count;
    });
  } else if (sort == "release") {
    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
      if (left.meta.release_date.empty() != right.meta.release_date.empty()) return right.meta.release_date.empty();
      return left.meta.release_date > right.meta.release_date;
    });
  } else if (sort == "name") {
    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
      return panplay::natural_compare(ascii_lower(left.name), ascii_lower(right.name)) < 0;
    });
  }

  std::string items;
  for (std::size_t index = offset; index < rows.size() && index < offset + static_cast<std::size_t>(limit); ++index) {
    if (!items.empty()) items += ",";
    items += rows[index].json;
  }
  const std::size_t matched = rows.size();
  return "{\"games\":[" + items + "],\"offset\":" + std::to_string(offset) + ",\"total\":" + std::to_string(matched) +
         ",\"all\":" + std::to_string(entries.size()) + ",\"installed\":" + std::to_string(installed) +
         ",\"continue\":" + std::to_string(continuing) + ",\"need_settings\":" + (listing->dest_ready ? "false" : "true") + ",\"version\":" + std::to_string(version) + "}";
}

// 能装的每组压缩包一项，供安装前挑选补丁。主游戏是直接放在游戏目录里最大的那组（都在子目录里时取最大的），
// 必装；子目录里的算补丁，游戏目录里其他的（多是 backport）算附加。按主游戏、补丁（自然排序）、附加的顺序列出。
std::string layers_json(const panplay::GameFolder& game, const std::set<std::string>& skipped,
                        const std::vector<std::string>& installed, std::string* main_fingerprint = nullptr) {
  struct Layer {
    const panplay::ArchiveSet* set;
    std::string id;
    std::string folder;
    std::int64_t bytes = 0;
    std::string role;
  };
  std::vector<Layer> layers;
  for (const auto& set : game.sets) {
    if (set.format != "zip" && set.format != "rar" && set.format != "7z") continue;
    Layer layer{&set, panplay::archive_set_id(set), panplay::archive_folder(game.path, set.parts.front().path)};
    for (const auto& part : set.parts) layer.bytes += part.size;
    layers.push_back(std::move(layer));
  }
  Layer* main = nullptr;
  for (const bool root_only : {true, false}) {
    for (auto& layer : layers) {
      if (root_only && !layer.folder.empty()) continue;
      if (main == nullptr || layer.bytes > main->bytes) main = &layer;
    }
    if (main != nullptr) break;
  }
  for (auto& layer : layers) layer.role = &layer == main ? "main" : layer.folder.empty() ? "extra" : "patch";
  const auto rank = [](const Layer& layer) { return layer.role == "main" ? 0 : layer.role == "patch" ? 1 : 2; };
  std::stable_sort(layers.begin(), layers.end(), [&](const Layer& left, const Layer& right) {
    if (rank(left) != rank(right)) return rank(left) < rank(right);
    if (const int order = panplay::natural_compare(left.folder, right.folder); order != 0) return order < 0;
    return panplay::natural_compare(left.set->parts.front().name, right.set->parts.front().name) < 0;
  });
  if (main_fingerprint != nullptr) *main_fingerprint = main != nullptr ? panplay::content_fingerprint(*main->set) : "";
  std::string json = "[";
  for (const auto& layer : layers) {
    if (json.size() > 1) json += ",";
    const bool is_installed = std::find(installed.begin(), installed.end(), layer.id) != installed.end();
    json += "{\"id\":\"" + panplay::json_escape(layer.id) + "\",\"fingerprint\":\"" +
            panplay::content_fingerprint(*layer.set) + "\",\"name\":\"" +
            panplay::json_escape(layer.set->parts.front().name) + "\",\"folder\":\"" + panplay::json_escape(layer.folder) +
            "\",\"parts\":" + std::to_string(layer.set->parts.size()) + ",\"bytes\":" + std::to_string(layer.bytes) +
            ",\"role\":\"" + layer.role + "\",\"skipped\":" +
            (layer.role != "main" && skipped.count(layer.id) != 0 ? "true" : "false") +
            ",\"installed\":" + (is_installed ? "true" : "false");
    // 主游戏各分卷的 md5 随评价同步出去，盘架拿分享里的文件按它认出同一份资源。
    if (layer.role == "main") {
      json += ",\"volumes\":[";
      for (std::size_t i = 0; i < layer.set->parts.size(); ++i) {
        const auto& part = layer.set->parts[i];
        if (i > 0) json += ",";
        json += "{\"md5\":\"" + panplay::json_escape(part.content_hash) + "\",\"size\":" + std::to_string(part.size) +
                ",\"name\":\"" + panplay::json_escape(part.name) + "\"}";
      }
      json += "]";
    }
    json += "}";
  }
  return json + "]";
}

// 实时读网盘上的压缩包和主机上的安装情况。主机连不上时安装情况用 fallback（上次读到的）。
std::string shelf_variant_json(const ShelfVariant& variant) {
  return ",\"remote\":true,\"size\":" + std::to_string(variant.size) + ",\"alive\":" +
         (variant.alive < 0 ? "null" : variant.alive ? "true" : "false") + ",\"version\":\"" +
         panplay::json_escape(variant.version) + "\",\"firmware\":\"" + panplay::json_escape(variant.firmware) +
         "\",\"review_state\":\"" + (variant.reviewed ? (variant.playable ? "good" : "bad") : "") +
         "\",\"review_note\":\"" + panplay::json_escape(variant.review_note) + "\"";
}

// 资源库里有、网盘里还没有的那种格式：主机上的情况照常读，压缩包要等转存之后才知道。
void read_remote_detail(const AppSettings& settings, const std::string& path, const ShelfVariant* variant,
                        const std::optional<GameDetail>& fallback, GameDetail& detail) {
  detail.archive_bytes = variant ? variant->size : 0;
  detail.formats = "-";
  detail.layers = "[]";
  detail.state = "need-settings";
  panplay::Destination destination;
  std::string dest_error;
  if (settings.dest().empty() || !panplay::parse_destination(settings.dest(), &destination, &dest_error)) return;
  panplay::GameFolder game;
  game.name = folder_name_of(path);
  game.path = path;
  game.title_id = panplay::find_title_id(game.name);
  if (auto sink = panplay::open_destination(destination, &dest_error)) {
    detail.state = game_state(*sink, settings.prefix(), game);
    if (detail.state == "installed") {
      const auto installed_variant = panplay::installed_variant(*sink, settings.prefix(), path);
      detail.installed_variant = installed_variant.value_or("");
      if (installed_variant && *installed_variant != panplay::variant_format(path) && path != panplay::game_dir_of(path)) {
        detail.state = "other";
      }
    }
  } else if (fallback && fallback->state != "need-settings") {
    detail.state = fallback->state;
    detail.installed_variant = fallback->installed_variant;
  } else if (const auto listing = cached_shelf(settings); listing && listing->sink_ready) {
    detail.state = shelf_state(listing->placement, game.name, game.title_id, path);
  } else {
    detail.state = "install";
  }
}

// 暂存区里还留着东西，可装好的游戏和网盘里要装的一致：是以前中断的安装（比如另一种格式）留下的，再装也只会把它清掉。
// 这款游戏的哪种格式都不在队列里、也没在删除或注册时先清掉，不然会一直显示没装完。清掉了返回 true。
bool clear_stale_staging(panplay::InstallSink& sink, const AppSettings& settings, const std::string& path,
                         const panplay::GameFolder& game, const std::vector<panplay::PlanCopy>& copies) {
  if (!panplay::install_current(sink, settings.prefix(), path, game.sets, load_skipped_sets(path), copies)) return false;
  {
    std::lock_guard<std::mutex> lock(installs.mu);
    const auto same_game = [&](const std::string& other) { return panplay::game_dir_of(other) == panplay::game_dir_of(path); };
    const bool queued = (installs.current && same_game(installs.current->path)) ||
                        std::any_of(installs.waiting.begin(), installs.waiting.end(),
                                    [&](const InstallTask& task) { return same_game(task.path); });
    if (queued || !installs.discarding.insert(path).second) return false;
  }
  const std::string error = panplay::discard_install(sink, settings.prefix(), path);
  {
    std::lock_guard<std::mutex> lock(installs.mu);
    installs.discarding.erase(path);
  }
  if (!error.empty()) {
    panplay::log_warn("清理 " + path + " 装好后留在暂存区的文件失败：" + error);
    return false;
  }
  panplay::log_info("已清理 " + path + " 装好后留在暂存区的文件");
  return true;
}

// 编排里只适用于某些固件的层（backport）按这台主机的固件去掉，返回记日志用的一句话，没去掉时为空。
// 电脑上的盘游不知道主机的固件，全留着。
std::string fit_plan_to_console(ShelfPlan& plan) {
#if defined(PANPLAY_PS5)
  const auto dropped = drop_layers_for_firmware(plan, firmware_major());
  if (dropped.empty()) return "";
  std::string names;
  for (const auto& name : dropped) names += (names.empty() ? "" : "、") + name;
  return "主机固件 " + firmware_text() + " 用不上，不装：" + names;
#else
  (void)plan;
  return "";
#endif
}

std::mutex share_files_mu;
// 没有编排的分享里有哪些文件，按 surl 记 30 分钟，免得每次打开详情都去列分享。
std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<panplay::ShareFile>>> share_files_cache;

// 网盘里还没有的格式，转存前就列出要装的压缩包组，有补丁时点安装先让用户挑：有编排按编排（去掉不适用这台主机固件的层），
// 没有编排看分享里的文件。archive_set_id 只看文件名和大小，挑的结果转存后照样用。列不出来时为空数组，照旧转存后全装。
std::string remote_layers_json(panplay::DrivePlugin& drive, const std::string& path, const ShelfVariant& variant) {
  panplay::GameFolder game;
  game.path = path;
  if (variant.plan) {
    std::string error;
    auto full = load_shelf_plan(drive, panplay::find_title_id(folder_name_of(path)), variant, &error);
    if (!full) return "[]";
    fit_plan_to_console(*full);
    game.sets = plan_archive_sets(path, *full);
  } else {
    std::vector<panplay::ShareFile> files;
    {
      std::lock_guard<std::mutex> lock(share_files_mu);
      const auto cached = share_files_cache.find(variant.surl);
      if (cached != share_files_cache.end() && std::chrono::steady_clock::now() - cached->second.first < std::chrono::minutes(30)) {
        files = cached->second.second;
      }
    }
    if (files.empty()) {
      const panplay::ShareListing share = drive.list_share(variant.surl, variant.pwd);
      if (!share.error.empty() || share.files.empty()) {
        panplay::log_warn("转存前列不出分享里的压缩包（" + path + "）：" + (share.error.empty() ? "分享是空的" : share.error));
        return "[]";
      }
      files = share.files;
      std::lock_guard<std::mutex> lock(share_files_mu);
      share_files_cache[variant.surl] = {std::chrono::steady_clock::now(), files};
    }
    game.sets = share_archive_sets(path, files);
  }
  return layers_json(game, load_skipped_sets(path), {});
}

GameDetail read_game_detail(panplay::DrivePlugin& drive, const AppSettings& settings, const std::string& path,
                            const std::optional<GameDetail>& fallback, std::string* error) {
  GameDetail detail;
  const std::string game_dir = panplay::game_dir_of(path);
  const auto library = shelf_library();
  const ShelfGame* shelf = library ? library->find(panplay::find_title_id(folder_name_of(path))) : nullptr;
  const auto variants = panplay::list_variants(drive, game_dir, error);
  // 网盘里还没有这款游戏，资源库里有：照样列出它的各种格式，装的时候先转存。
  const bool missing = shelf && error->find("errno=-9") != std::string::npos;
  if (missing) error->clear();
  if (!error->empty()) return detail;
  // 网盘里是没分格式的旧目录时不加资源库的格式，照旧装网盘里那份。
  struct Listed {
    std::string format;
    std::string path;
    const ShelfVariant* shelf = nullptr;
  };
  std::vector<Listed> listed;
  for (const auto& variant : variants) listed.push_back({variant.format, variant.path, nullptr});
  if (shelf && (missing || !variants.empty())) {
    for (const auto& item : shelf->variants) {
      const std::string dir = shelf_variant_dir(item.format);
      const std::string variant_path = panplay::join_storage_path(game_dir, dir);
      const bool present = std::any_of(variants.begin(), variants.end(),
                                       [&](const panplay::GameVariant& variant) { return variant.path == variant_path; });
      if (!present) listed.push_back({dir.substr(1), variant_path, &item});
    }
  }
  std::stable_sort(listed.begin(), listed.end(), [](const Listed& left, const Listed& right) {
    return panplay::variant_rank(left.format) < panplay::variant_rank(right.format);
  });
  const ShelfVariant* remote_here = nullptr;
  if (!listed.empty()) {
    detail.variants = "[";
    for (const auto& variant : listed) {
      if (detail.variants.size() > 1) detail.variants += ",";
      detail.variants += "{\"format\":\"" + panplay::json_escape(variant.format) + "\",\"path\":\"" +
                         panplay::json_escape(variant.path) + "\"" +
                         (variant.shelf ? shelf_variant_json(*variant.shelf) : "") + "}";
      if (variant.shelf && variant.path == path) remote_here = variant.shelf;
    }
    detail.variants += "]";
  }
  if (remote_here || missing) {
    read_remote_detail(settings, path, remote_here, fallback, detail);
    if (remote_here) detail.layers = remote_layers_json(drive, path, *remote_here);
    return detail;
  }
  const auto files = panplay::collect_archives(drive, path, error);
  if (!error->empty()) return detail;
  panplay::GameFolder game;
  game.name = folder_name_of(path);
  game.path = path;
  game.title_id = panplay::find_title_id(game.name);
  game.sets = panplay::group_archives(files);
  // 有编排、对得上时，列出的压缩包组、有没有更新都按编排（ADR 0012）。
  std::vector<panplay::PlanCopy> plan_copies;
  if (const ShelfVariant* planned = library ? library->variant(game.title_id, panplay::variant_format(path)) : nullptr;
      planned && planned->plan) {
    std::string plan_error;
    auto full = load_shelf_plan(drive, game.title_id, *planned, &plan_error);
    if (full) fit_plan_to_console(*full);
    ResolvedPlan resolved = full ? resolve_plan(drive, path, *full, &plan_error) : ResolvedPlan{};
    if (full && plan_error.empty() && resolved.missing.empty()) {
      game.sets = std::move(resolved.plan.layers);
      plan_copies = std::move(resolved.plan.copies);
      note_console_packages(path, resolved.plan.packages);
    }
  }
  for (const auto& set : game.sets) {
    for (const auto& part : set.parts) detail.archive_bytes += part.size;
  }
  detail.formats = formats_of(game);
  detail.state = "need-settings";
  panplay::Destination destination;
  std::string dest_error;
  const std::string dest = settings.dest();
  if (!dest.empty() && panplay::parse_destination(dest, &destination, &dest_error)) {
    detail.state = archive_problem(game);
    // 按格式分了的游戏目录本身没有压缩包；详情页要知道主机上装的是哪种格式，默认打开那一种。
    if (detail.state == "empty" && !variants.empty()) {
      if (auto sink = panplay::open_destination(destination, &dest_error)) {
        detail.installed_variant = panplay::installed_variant(*sink, settings.prefix(), path).value_or("");
      }
    }
  }
  if (detail.state.empty()) {
    if (auto sink = panplay::open_destination(destination, &dest_error)) {
      detail.state = game_state(*sink, settings.prefix(), game);
      if (detail.state == "continue" && clear_stale_staging(*sink, settings, path, game, plan_copies)) {
        detail.state = game_state(*sink, settings.prefix(), game);
      }
      // 装好之后网盘里又放了补丁（或改了要装哪些）：读一下这款游戏的完成标记，和现在要装的压缩包对一下。
      if (detail.state == "installed") {
        const auto installed_variant = panplay::installed_variant(*sink, settings.prefix(), path);
        detail.installed_variant = installed_variant.value_or("");
        // 主机上装的是这款游戏的另一种格式。
        if (!variants.empty() && installed_variant && *installed_variant != panplay::variant_format(path)) {
          detail.state = "other";
        } else {
          if (panplay::install_outdated(*sink, settings.prefix(), path, game.sets, load_skipped_sets(path), plan_copies)) {
            detail.state = "update";
          }
#if defined(PANPLAY_PS5)
          // 编排里要在主机上安装的包还没装好：再装一次会补装（游戏文件没变的不重装）。
          if (detail.state == "installed" && pending_console_packages(path) > 0) detail.state = "update";
#endif
          detail.installed = panplay::installed_sets(*sink, settings.prefix(), path);
        }
      }
    } else if (fallback && fallback->state != "need-settings") {
      detail.state = fallback->state;
      detail.installed = fallback->installed;
      detail.installed_variant = fallback->installed_variant;
    } else if (const auto listing = cached_shelf(settings); listing && listing->sink_ready) {
      detail.state = shelf_state(listing->placement, game.name, game.title_id, path);
    } else {
      detail.state = "install";
    }
  }
  detail.layers = layers_json(game, load_skipped_sets(path), detail.installed, &detail.fingerprint);
  return detail;
}

// 游戏装在哪个文件夹，删除前给用户确认。存放位置没填好时为空。
std::string install_location(const AppSettings& settings, const std::string& path) {
  const std::string folder = panplay::install_folder_name(path);
  if (folder.empty()) return "";
  if (settings.target == "ftp") {
    return settings.ftp_host.empty() ? "" : settings.ftp_host + ":" + panplay::join_storage_path(settings.prefix(), folder);
  }
  if (settings.local_dir.empty()) return "";
  return (std::filesystem::u8path(settings.local_dir) / std::filesystem::u8path(folder)).u8string();
}

// 游戏界面上显示的名字：用户改过的、商店的中文名、资料库的英文名，都没有就用网盘目录名。
std::string game_display_name(const std::string& path) {
  const std::string name = folder_name_of(path);
  const std::string title_id = panplay::find_title_id(name);
  const NameOverrides overrides = load_name_overrides();
  if (const auto custom = overrides.find(name_key(name, title_id)); custom != overrides.end()) return custom->second;
  panplay::CatalogMatch match;
  if (!title_id.empty()) match = panplay::GameCatalog::instance().lookup(title_id, name);
  const auto library = shelf_library();
  const ShelfGame* shelf = library ? library->find(title_id) : nullptr;
  const std::int64_t concept_id = store_concept_of(match, shelf);
  panplay::StoreMeta meta;
  if (concept_id > 0 && store_meta != nullptr) store_meta->lookup(concept_id, &meta);
  if (meta.found && !meta.name.empty()) return meta.name;
  if (match.found && !match.entry.name.empty()) return match.entry.name;
  if (shelf != nullptr && !shelf->name.empty()) return shelf->name;
  return name;
}

// 评价时装着哪几组：装好的游戏按完成标记，没装过的只记主游戏。
std::vector<ReviewLayer> review_layers(const GameDetail& detail) {
  std::vector<ReviewLayer> out;
  const panplay::Json layers = panplay::Json::parse(detail.layers, nullptr);
  for (const auto& layer : layers.array) {
    const bool installed = layer.boolean_or("installed", false);
    if (installed || (detail.installed.empty() && layer.str("role") == "main")) {
      out.push_back({layer.str("fingerprint"), layer.str("name")});
    }
  }
  return out;
}

// 主游戏那组的各分卷。旧版存的详情缓存里没有，为空。
std::vector<ReviewPart> review_parts(const GameDetail& detail) {
  std::vector<ReviewPart> out;
  const panplay::Json layers = panplay::Json::parse(detail.layers, nullptr);
  for (const auto& layer : layers.array) {
    if (layer.str("role") != "main") continue;
    if (const panplay::Json* parts = layer.get("volumes")) {
      for (const auto& part : parts->array) {
        out.push_back({part.str("md5"), static_cast<std::int64_t>(part.num("size")), part.str("name")});
      }
    }
  }
  return out;
}

// 装的时候发现主游戏那组是 PKG 安装包：盘游装不了，自动把这个版本记成有问题，游戏库里一眼看得出来。
// 已经评过的不覆盖。
void review_install_package(const std::string& path, const std::string& set_id) {
  const AppSettings settings = load_settings();
  std::optional<GameDetail> detail = cached_game_detail(shelf_key(settings), path);
  if (!detail || detail->fingerprint.empty()) {
    std::string error;
    detail = read_game_detail(*open_drive(), settings, path, std::nullopt, &error);
    if (!error.empty()) {
      panplay::log_warn("没能自动标记 " + path + "：" + error);
      return;
    }
  }
  const panplay::Json layers = panplay::Json::parse(detail->layers, nullptr);
  const bool main = std::any_of(layers.array.begin(), layers.array.end(), [&](const panplay::Json& layer) {
    return layer.str("id") == set_id && layer.str("role") == "main";
  });
  if (!main || detail->fingerprint.empty() || find_review(detail->fingerprint)) return;
  GameReview review;
  review.fingerprint = detail->fingerprint;
  review.title_id = panplay::find_title_id(folder_name_of(path));
  review.name = game_display_name(path);
  review.format = review_format(path);
  review.playable = false;
  review.note = "是 PKG 安装包，盘游装不了（安装时自动标记）";
#if defined(PANPLAY_PS5)
  review.firmware = firmware_text();
#endif
  review.at = static_cast<std::int64_t>(std::time(nullptr));
  review.layers = review_layers(*detail);
  review.parts = review_parts(*detail);
  if (const std::string error = save_review(review); !error.empty()) {
    panplay::log_warn("没能自动标记 " + review.name + "：" + error);
    return;
  }
  review_changed();
  panplay::log_info("自动评价 " + review.name + "：有问题，" + review.note);
}

// 照编排装的游戏要在主机上安装的包，安装时对到网盘里的文件，装好、注册后取走去装。按网盘变体目录记。
std::mutex planned_packages_mu;
std::map<std::string, std::vector<panplay::DriveEntry>> planned_packages;

#if defined(PANPLAY_PS5)
// 主机上装到本机文件夹的游戏才能注册到主机桌面。
std::string console_game_dir(const AppSettings& settings, const std::string& path) {
  return settings.target == "local" ? install_location(settings, path) : "";
}

// 最近一次自动注册失败的原因，按网盘游戏目录记，详情页显示。
std::mutex console_errors_mu;
std::map<std::string, std::string> console_errors;

void remember_console_error(const std::string& path, const std::string& error) {
  std::lock_guard<std::mutex> lock(console_errors_mu);
  if (error.empty()) console_errors.erase(path);
  else console_errors[path] = error;
}

std::string console_json(const std::string& path, const std::string& game_dir) {
  const ConsoleTitle title = inspect_console_title(game_dir);
  std::string error;
  {
    std::lock_guard<std::mutex> lock(console_errors_mu);
    const auto found = console_errors.find(path);
    if (found != console_errors.end()) error = found->second;
  }
  using panplay::json_escape;
  const auto launch = title.title_id.empty() ? std::nullopt : panplay::game_launches().last(title.title_id);
  return "{\"title_id\":\"" + json_escape(title.title_id) + "\",\"registered\":" + (title.registered ? "true" : "false") +
         ",\"mounted\":" + (title.mounted ? "true" : "false") + ",\"files_ok\":" + (title.files_ok ? "true" : "false") +
         ",\"problem\":\"" + json_escape(title.problem) + "\",\"error\":\"" + json_escape(error) + "\",\"fakelib\":\"" +
         json_escape(title.fakelib) + "\",\"fakelib_by\":\"" + json_escape(title.fakelib_by) + "\",\"launch\":" +
         (launch ? panplay::launch_report_json(*launch) : "null") + "}";
}

// 重新注册和删除共用一张占用表：删到一半时不能又挂上、注册回去；查队列和占用在同一把锁里，中间插不进安装。
// 占上了返回空，调用方用完要 release_console_path；占不上返回原因，还在安装队列里时返回 queued_reason。
std::string claim_console_path(const std::string& path, const std::string& queued_reason) {
  std::lock_guard<std::mutex> lock(installs.mu);
  const bool queued = (installs.current && installs.current->path == path) ||
                      std::any_of(installs.waiting.begin(), installs.waiting.end(),
                                  [&](const InstallTask& task) { return task.path == path; });
  if (queued) return queued_reason;
  if (!installs.discarding.insert(path).second) return "正在处理这个游戏，稍等再试";
  return "";
}

void release_console_path(const std::string& path) {
  std::lock_guard<std::mutex> lock(installs.mu);
  installs.discarding.erase(path);
}

struct ConsolePathClaim {
  const std::string& path;
  ~ConsolePathClaim() { release_console_path(path); }
};

// 装完后注册到主机桌面。这次写了文件（新装、装了更新）就重新注册，主机上显示的版本才会跟着变；
// 本来就装好的，已经注册好、挂载也在时不重做。返回现在是不是已经注册在主机上。
bool register_after_install(const std::string& path, bool files_changed) {
  const std::string game_dir = console_game_dir(load_settings(), path);
  if (game_dir.empty()) return false;
  const ConsoleTitle title = inspect_console_title(game_dir);
  if (title.title_id.empty() || !title.problem.empty()) {
    if (!title.problem.empty()) panplay::log_info("不注册到主机：" + title.problem);
    remember_console_error(path, "");
    return false;
  }
  if (!files_changed && title.registered && title.mounted && title.files_ok) return true;
  {
    std::lock_guard<std::mutex> lock(installs.mu);
    if (installs.current && installs.current->path == path) installs.current->result.message = "正在注册到主机桌面…";
  }
  const std::string error = register_console_title(game_dir);
  remember_console_error(path, error);
  return error.empty();
}

// 装好、注册到主机后，把激活完整版、解锁 DLC 的授权包装上：编排里列出的（网盘里单独放着的），
// 和压缩包里带着、解压到游戏目录里的。
void install_console_side_packages(const std::string& path) {
  std::vector<panplay::DriveEntry> packages;
  {
    std::lock_guard<std::mutex> lock(planned_packages_mu);
    const auto found = planned_packages.find(path);
    if (found != planned_packages.end()) {
      packages = std::move(found->second);
      planned_packages.erase(found);
    }
  }
  const auto progress = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(installs.mu);
    if (installs.current && installs.current->path == path) installs.current->result.message = message;
  };
  const std::string title_id = panplay::find_title_id(folder_name_of(path));
  std::string error;
  if (!packages.empty() && pending_console_packages(path) > 0) {
    auto drive = open_drive();
    error = install_console_packages(*drive, path, title_id, packages, progress);
  }
  const std::string game_dir = console_game_dir(load_settings(), path);
  if (!game_dir.empty()) {
    const std::string in_game = install_game_dir_packages(path, game_dir, title_id, progress);
    if (!in_game.empty()) error += (error.empty() ? "" : "；") + in_game;
  }
  if (!error.empty()) notify("激活包没装上，详情页里有原因：" + error);
}

// 新装或装了更新时在主机上弹「已安装完成」，带游戏图标。
void announce_installed(const std::string& path, bool registered) {
  const std::string game_dir = console_game_dir(load_settings(), path);
  if (game_dir.empty()) return;
  const ConsoleTitle title = inspect_console_title(game_dir);
  const std::string icon = registered && !title.title_id.empty() ? "/user/appmeta/" + title.title_id + "/icon0.png"
                                                                  : game_dir + "/sce_sys/icon0.png";
  notify_game_installed(title.title_id, registered, game_display_name(path), icon);
}
#endif

// 这个变体（或整款游戏）网盘里还没有，只在资源库里：装的时候先转存。
bool not_on_drive(const GameDetail& detail, const std::string& path) {
  const auto variants = panplay::Json::parse(detail.variants, nullptr).array;
  if (variants.empty()) return false;
  if (path == panplay::game_dir_of(path)) {
    return std::all_of(variants.begin(), variants.end(),
                       [](const panplay::Json& variant) { return variant.boolean_or("remote", false); });
  }
  return std::any_of(variants.begin(), variants.end(), [&](const panplay::Json& variant) {
    return variant.str("path") == path && variant.boolean_or("remote", false);
  });
}

// 详情页要的游戏信息。cached 时只给上次读到的（source 为 cache，没有时为 none），立刻返回；
// 否则实时读网盘和主机（source 为 live），读到的存起来，游戏库列表缓存里这款游戏的状态也跟着改。
std::string one_game_json(const std::string& path, bool cached) {
  const AppSettings settings = load_settings();
  const std::string key = shelf_key(settings);
  const std::string name = folder_name_of(path);
  const std::string title_id = panplay::find_title_id(name);
  std::optional<GameDetail> detail = cached_game_detail(key, path);
  std::string source = detail ? "cache" : "none";
  std::string state = detail ? detail->state : "";
  std::shared_ptr<const ShelfListing> listing = cached_shelf(settings);
  if (!listing && !cached) {
    std::string listing_error;
    listing = shelf_listing(*open_drive(), false, &listing_error);
  }
  const bool orphan = listing && is_orphan(*listing, path);
  if (orphan) {
    detail = GameDetail{};
    detail->state = "installed";
    source = "live";
    state = detail->state;
  } else if (cached && detail) {
    // 是否装在主机上以列表读到的为准（和格子上显示的一致），详情缓存只补上列表不知道的“有更新”。
    const bool placement = state == "install" || state == "continue" || state == "installed" || state == "update" ||
                           state == "other";
    if (placement && listing && listing->sink_ready) {
      const std::string listed = shelf_state(listing->placement, name, title_id, path);
      state = listed == "installed" && (state == "update" || state == "other") ? state : listed;
    }
  } else if (!cached) {
    std::string error;
    const GameDetail live = read_game_detail(*open_drive(), settings, path, detail, &error);
    if (!error.empty()) return json_error(error);
    store_game_detail(key, path, live);
    if (fill_review_parts(live.fingerprint, review_parts(live))) review_changed();
    // 网盘里还没有的不记容量，后台量容量时它不在网盘上，会被当成删掉的记录清掉。
    if (!not_on_drive(live, path)) {
      GameSize size{live.archive_bytes, {}};
      if (panplay::game_dir_of(path) == path) {
        for (const auto& variant : panplay::Json::parse(live.variants, nullptr).array) {
          if (!variant.boolean_or("remote", false)) size.variants.push_back({variant.str("format"), variant.str("path")});
        }
      }
      if (store_game_size(sizes_key(settings), path, size)) library_changed();
    }
    sync_shelf_state(key, name, title_id, live.state);
    // 暂存区已经没了（在别处删掉或装完），下次就是从头装，旧记录取较大值会把进度显示高。
    if (live.state == "install" || live.state == "installed" || live.state == "update") forget_install_progress(path);
    detail = live;
    source = "live";
    state = live.state;
  }
  std::string json = "{\"name\":\"" + panplay::json_escape(name) + "\",\"path\":\"" + panplay::json_escape(path) +
                     "\",\"game_dir\":\"" + panplay::json_escape(panplay::game_dir_of(path)) + "\",\"variant\":\"" +
                     panplay::json_escape(panplay::variant_format(path)) + "\",\"title_id\":\"" +
                     panplay::json_escape(title_id) + "\",\"source\":\"" + source + "\"";
  if (orphan) json += ",\"on_drive\":false";
  // 读到详情就带上：详情页切换格式时用 Object.assign 合并，不带的话上一种格式的 true 会留下来。
  // 还没读到时不带，免得把游戏库格子上的标记冲掉。
  if (detail) json += std::string(",\"remote\":") + (not_on_drive(*detail, path) ? "true" : "false");
  // 盘架编排的版本，和这种格式有没有已经通知盘架、还没重新分析的问题（ADR 0012）。
  if (const auto library = shelf_library()) {
    if (const ShelfVariant* variant = library->variant(title_id, panplay::variant_format(path))) {
      json += std::string(",\"shelf_plan\":") + (variant->plan ? std::to_string(variant->plan->rev) : "0") +
              ",\"shelf_feedback\":" + (shelf_feedback_pending(path) ? "true" : "false") +
              ",\"console_packages\":" + console_packages_json(path);
    }
  }
  if (detail) {
    json += ",\"variants\":" + (detail->variants.empty() ? std::string("[]") : detail->variants) +
            ",\"installed_variant\":\"" + panplay::json_escape(detail->installed_variant) + "\"";
    std::string resume = "null";
    if (state == "continue") {
      if (const auto progress = load_install_progress(path)) {
        resume = "{\"bytes_done\":" + std::to_string(progress->bytes_done) +
                 ",\"bytes_total\":" + std::to_string(progress->bytes_total) +
                 ",\"files_done\":" + std::to_string(progress->files_done) + "}";
      }
    }
    const auto queued = queue_states();
    if (queued.count(path) != 0) state = queued.at(path);
    json += ",\"archive_bytes\":" + std::to_string(detail->archive_bytes) + ",\"formats\":\"" +
            panplay::json_escape(detail->formats) + "\",\"state\":\"" + state + "\",\"layers\":" +
            (detail->layers.empty() ? "[]" : detail->layers) + ",\"resume\":" + resume + ",\"install_dir\":\"" +
            panplay::json_escape(install_location(settings, path)) + "\"";
#if defined(PANPLAY_PS5)
    const std::string game_dir = console_game_dir(settings, path);
    if (!game_dir.empty() && (state == "installed" || state == "update")) {
      json += ",\"console\":" + console_json(path, game_dir);
    }
#endif
    const auto review = find_review(detail->fingerprint);
    json += ",\"fingerprint\":\"" + panplay::json_escape(detail->fingerprint) + "\",\"review\":" +
            (review ? review_json(*review) : "null") + ",\"review_state\":\"" +
            (review ? (review->playable ? "good" : "bad") : "") + "\"";
    const auto others = reviews_of_title(title_id, detail->fingerprint);
    json += ",\"other_review\":" + (others.empty() ? std::string("null") : review_json(others.front()));
  }
  return json + game_meta_fields(name, title_id, load_name_overrides(), true) + "}";
}

constexpr double kRecentRateSeconds = 60;
// 取样不满这么多秒不给剩余时间：刚开始几十秒的速度常常比后面快，估出来的时间太乐观。
constexpr double kRecentRateWarmup = 20;

void sample_recent_rate(InstallTask& task, std::int64_t done, std::chrono::steady_clock::time_point now) {
  // 续装退回到上一个装完的文件时进度会变小，也重新起头。
  if (task.recent_bytes < 0 || done < task.recent_bytes) {
    task.recent_bytes = done;
    task.recent_at = now;
    return;
  }
  const double seconds = std::chrono::duration<double>(now - task.recent_at).count();
  if (seconds < 1) return;
  const double rate = static_cast<double>(done - task.recent_bytes) / seconds;
  const double weight = 1 - std::exp(-seconds / kRecentRateSeconds);
  task.recent_rate = task.recent_seconds > 0 ? task.recent_rate + weight * (rate - task.recent_rate) : rate;
  task.recent_seconds += seconds;
  task.recent_bytes = done;
  task.recent_at = now;
}

InstallProgress progress_of(const panplay::InstallResult& result) {
  InstallProgress progress;
  progress.bytes_done = result.bytes_done;
  progress.bytes_total = result.bytes_total;
  progress.files_done = result.files_done;
  return progress;
}

void publish_progress(const panplay::InstallResult& snapshot) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(installs.mu);
    InstallTask* task = installs.current.get();
    if (task == nullptr || task->cancel->cancelled()) return;
    task->result.message = snapshot.message;
    task->result.files_done = snapshot.files_done;
    task->result.files_total = snapshot.files_total;
    task->result.bytes_done = snapshot.bytes_done;
    task->result.bytes_total = snapshot.bytes_total;
    task->result.files_checked = snapshot.files_checked;
    task->result.files_to_check = snapshot.files_to_check;
    task->result.dictionary = snapshot.dictionary;
    task->result.slow = snapshot.slow;
    if (snapshot.message == "正在安装" && snapshot.bytes_total > 0) {
      task->rate_last = std::chrono::steady_clock::now();
      if (task->rate_base < 0) {
        task->rate_base = snapshot.bytes_done;
        task->rate_since = task->rate_last;
      }
      sample_recent_rate(*task, snapshot.bytes_done, task->rate_last);
    } else {
      task->recent_bytes = -1;
    }
    path = task->path;
  }
  // 程序被直接关掉或断电时，详情页还能看到大致装到哪里。
  static auto last_saved = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last_saved < std::chrono::seconds(10)) return;
  last_saved = now;
  save_install_progress(path, progress_of(snapshot));
}

std::filesystem::path queue_file() { return panplay::app_data_dir() / "install-queue.json"; }

void save_queue_locked() {
  // 队列空了暂停就没意义了，留着会让之后加入的游戏莫名不开始。
  if (!installs.current && installs.waiting.empty()) installs.paused = false;
  if (!installs.paused) installs.pause_reason.clear();
  std::string json = std::string("{\"paused\":") + (installs.paused ? "true" : "false") + ",\"pause_reason\":\"" +
                     panplay::json_escape(installs.pause_reason) + "\",\"paths\":[";
  bool first = true;
  const auto add = [&](const InstallTask& task) {
    if (!first) json += ",";
    first = false;
    json += "\"" + panplay::json_escape(task.path) + "\"";
  };
  if (installs.current) add(*installs.current);
  for (const auto& task : installs.waiting) add(task);
  json += "]}";
  std::error_code ec;
  std::filesystem::create_directories(queue_file().parent_path(), ec);
  std::ofstream out(queue_file(), std::ios::binary | std::ios::trunc);
  out << json;
}

std::unique_ptr<panplay::InstallSink> open_sink(const AppSettings& settings, std::string* error) {
  panplay::Destination destination;
  if (settings.dest().empty() || !panplay::parse_destination(settings.dest(), &destination, error)) {
    if (error->empty()) *error = "先在设置里填写存放位置";
    return nullptr;
  }
  auto sink = panplay::open_destination(destination, error);
  if (!sink && error->empty()) *error = "无法连接存放位置";
  return sink;
}

// 盘架给这种格式排了编排、网盘里的文件都对得上时照编排装（ADR 0012）。对不上时告诉盘架重新分析，这次照旧按规则装。
std::optional<panplay::InstallPlan> install_plan_for(panplay::DrivePlugin& drive, const std::string& path) {
  const auto library = shelf_library();
  const ShelfVariant* variant =
      library ? library->variant(panplay::find_title_id(folder_name_of(path)), panplay::variant_format(path)) : nullptr;
  if (variant == nullptr || !variant->plan) return std::nullopt;
  std::string error;
  auto full = load_shelf_plan(drive, panplay::find_title_id(folder_name_of(path)), *variant, &error);
  if (!full) {
    panplay::log_warn("没取到盘架的编排（第 " + std::to_string(variant->plan->rev) + " 版），按规则装：" + error);
    return std::nullopt;
  }
  if (const std::string skipped = fit_plan_to_console(*full); !skipped.empty()) panplay::log_info(skipped);
  ResolvedPlan resolved = resolve_plan(drive, path, *full, &error);
  if (!error.empty()) {
    panplay::log_warn("没能核对盘架的编排，按规则装：" + error);
    return std::nullopt;
  }
  if (!resolved.missing.empty()) {
    std::string missing;
    for (std::size_t i = 0; i < resolved.missing.size() && i < 5; ++i) missing += (i ? "、" : "") + resolved.missing[i];
    if (resolved.missing.size() > 5) missing += " 等 " + std::to_string(resolved.missing.size()) + " 个";
    report_to_shelf(path, "mismatch", "网盘里找不到 " + missing);
    panplay::log_warn("网盘里的文件和盘架的编排（第 " + std::to_string(variant->plan->rev) + " 版）对不上，按规则装：找不到 " + missing);
    return std::nullopt;
  }
  panplay::log_info("照盘架的编排（第 " + std::to_string(variant->plan->rev) + " 版）装：" +
                    std::to_string(resolved.plan.layers.size()) + " 层压缩包，放 " +
                    std::to_string(resolved.plan.copies.size()) + " 个文件");
  return std::move(resolved.plan);
}

panplay::InstallResult run_install(const std::string& path, panplay::CancelToken* cancel) {
  panplay::InstallResult result;
  try {
    panplay::InstallResult starting;
    starting.message = "正在读取压缩包…";
    publish_progress(starting);
    auto drive = open_drive();
    const AppSettings settings = load_settings();
    std::string error;
    auto sink = open_sink(settings, &error);
    if (!sink) {
      result.status = "error";
      result.message = error;
      // 填好了却连不上（FTP 那头的主机在休眠、网络还没恢复），等一会儿再连；没填好的等也没用。
      result.auto_resume = !settings.dest().empty();
      return result;
    }
    // 网盘里还没有、盘架资源库里有的：先转存进来，网盘装不下时才先腾空间；按上限退游戏在转存后的后台做（ADR 0011）。
    const PrepareResult prepared =
        prepare_on_drive(*drive, settings, sizes_key(settings), path, queued_paths(), *cancel, [](const std::string& message) {
          panplay::InstallResult step;
          step.message = message;
          publish_progress(step);
        });
    if (prepared.transferred) {
      std::string listing_error;
      shelf_listing(*drive, true, &listing_error);
      tidy_drive_later(drive, settings, sizes_key(settings), panplay::game_dir_of(path), queued_paths, [] {
        std::string error;
        shelf_listing(*open_drive(), true, &error);
      });
    }
    if (cancel->cancelled()) {
      result.status = "interrupted";
      result.message = "已中断，再次执行安装会继续";
      return result;
    }
    if (!prepared.error.empty()) {
      result.status = "error";
      result.message = prepared.error;
      result.retryable = prepared.retryable;
      result.auto_resume = prepared.auto_resume;
      return result;
    }
    {
      panplay::InstallRequest request;
      request.source_dir = path;
      request.dest_prefix = settings.prefix();
      request.on_progress = publish_progress;
      request.cancel = cancel;
      request.skip_sets = load_skipped_sets(path);
      request.max_dictionary = settings.max_dictionary_mb << 20;
      request.download.memory = download_memory();
      request.plan = install_plan_for(*drive, path);
      if (request.plan) {
        note_console_packages(path, request.plan->packages);
        std::lock_guard<std::mutex> lock(planned_packages_mu);
        planned_packages[path] = request.plan->packages;
      }
      result = panplay::install_game(*drive, *sink, request);
      save_download_memory();
      // 照编排装，失败原因在压缩包或编排上（不是网络、内存、空间这些）时告诉盘架重新分析。
      if (request.plan && result.status == "error" && !result.auto_resume && !panplay::is_disk_full_error(result.message) &&
          result.message.find("字典") == std::string::npos && result.message.find("内存不够") == std::string::npos &&
          result.message.find("已经装了") == std::string::npos) {
        report_to_shelf(path, "install_failed", result.message);
      }
    }
  } catch (const std::exception& ex) {
    result.status = "error";
    result.message = std::string("安装失败: ") + ex.what();
  } catch (...) {
    result.status = "error";
    result.message = "安装失败";
  }
  if (result.status.empty()) {
    result.status = "error";
    result.message = result.message.empty() ? "安装失败" : result.message;
  }
  return result;
}

// 意外停下（网络断开、主机休眠唤醒）的安装，等一会儿自动接着装，已装好的文件不重装。
// 等的间隔逐次拉长；这一轮比上一轮装得多，就说明网络回来过，重新计次，长时间安装中偶尔断一下不会把次数用光。
panplay::InstallResult install_with_auto_resume(const std::string& path, panplay::CancelToken& cancel) {
  static constexpr int kDelays[] = {10, 30, 60, 120, 300};
  constexpr int kRounds = sizeof(kDelays) / sizeof(kDelays[0]);
  int round = 0;
  std::int64_t last_done = -1;
  for (;;) {
    panplay::InstallResult result = run_install(path, &cancel);
    if (result.status != "error" || !result.auto_resume || cancel.cancelled()) return result;
    if (last_done >= 0 && result.bytes_done > last_done) round = 0;
    last_done = result.bytes_done;
    if (round >= kRounds) return result;
    const int delay = kDelays[round++];
    panplay::log_warn("安装中断：" + result.message + "，" + std::to_string(delay) + " 秒后自动继续（第 " +
                      std::to_string(round) + " / " + std::to_string(kRounds) + " 次）");
    save_install_progress(path, progress_of(result));
    for (int left = delay; left > 0; --left) {
      {
        std::lock_guard<std::mutex> lock(installs.mu);
        if (installs.current && installs.current->path == path) {
          installs.current->result.message = "安装中断，" + std::to_string(left) + " 秒后自动继续";
        }
      }
      if (cancel.cancelled()) break;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (cancel.cancelled()) {
      result.status = "interrupted";
      result.message = "已中断，再次执行安装会继续";
      result.auto_resume = false;
      return result;
    }
    panplay::log_info("自动继续安装 " + path);
  }
}

void queue_worker() {
  for (;;) {
    std::string path;
    std::shared_ptr<panplay::CancelToken> cancel;
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      if (installs.waiting.empty() || installs.paused || installs.quitting) {
        installs.worker_running = false;
        return;
      }
      installs.current = std::make_unique<InstallTask>(std::move(installs.waiting.front()));
      installs.waiting.pop_front();
      installs.current->cancel = std::make_shared<panplay::CancelToken>();
      installs.current->result = {};
      installs.current->rate_base = -1;
      installs.current->recent_rate = 0;
      installs.current->recent_seconds = 0;
      installs.current->recent_bytes = -1;
      installs.current->pausing = false;
      installs.current->result.message = "正在读取压缩包…";
      path = installs.current->path;
      cancel = installs.current->cancel;
    }
    // 测速和安装抢网络、抢硬盘，两边都不准：开始装就停掉测速。
    if (speed_test.running() || disk_test.running()) {
      speed_test.stop();
      disk_test.stop();
      panplay::log_info("开始安装，停掉正在进行的测速");
    }
    panplay::log_info("开始安装 " + path);
    usage_install_started(path);
    usage_used(path);
    panplay::InstallResult result = install_with_auto_resume(path, *cancel);
    usage_install_stopped(path);
    if (result.status == "interrupted" && cancel->cancelled()) {
      result.message = "已暂停，已装好 " + std::to_string(result.files_done) + " 个文件，点继续接着装";
    }
    panplay::log_line(result.status == "error" ? panplay::LogLevel::warn : panplay::LogLevel::info,
                      "安装结束 " + path + "：" + result.status + " " + result.message + "，" + panplay::memory_summary());
    if (result.status == "installed" || result.status == "already") {
      forget_install_progress(path);
#if defined(PANPLAY_PS5)
      const bool registered = register_after_install(path, result.status == "installed");
      if (registered) install_console_side_packages(path);
      if (result.status == "installed") announce_installed(path, registered);
#endif
    } else {
      save_install_progress(path, progress_of(result));
      if (!result.install_package_set.empty()) review_install_package(path, result.install_package_set);
    }
    forget_game_detail(path);
    refresh_shelf_placement();
    // 存放位置写满了：后面排队的装下去只会接着失败，整个队列停下，这个游戏回到队首，腾出空间后点继续接着装。
    const bool disk_full = result.status == "error" && panplay::is_disk_full_error(result.message);
#if defined(PANPLAY_PS5)
    if (disk_full) notify("存放位置空间不够，安装队列已全部暂停");
#endif
    std::lock_guard<std::mutex> lock(installs.mu);
    InstallTask done = std::move(*installs.current);
    installs.current.reset();
    done.result = result;
    done.cancel.reset();
    if (disk_full) {
      installs.paused = true;
      installs.pause_reason = result.message;
      installs.waiting.push_front(std::move(done));
    } else if (done.pausing && result.status == "interrupted") {
      done.result.message = "已暂停";
      installs.waiting.push_front(std::move(done));
    } else {
      installs.finished.push_front(std::move(done));
      if (installs.finished.size() > kFinishedKept) installs.finished.pop_back();
    }
    library_changed();
    save_queue_locked();
  }
}

// 退出前先让手上的事停在稳妥的地方：正在装的游戏停在能续装的地方、回到队首；正在删的删完；
// PS5 上不在注册应用、挂载目录这些系统调用中途结束进程。等太久就不等了，照样退出。
void stop_for_quit() {
  using Clock = std::chrono::steady_clock;
  constexpr auto kWait = std::chrono::seconds(20);
  const auto started = Clock::now();
  std::string busy;
  {
    std::lock_guard<std::mutex> lock(installs.mu);
    installs.quitting = true;
    if (installs.current && installs.current->cancel) {
      busy = "，先停下正在装的 " + installs.current->path;
      installs.current->pausing = true;
      installs.current->cancel->cancel();
    }
    if (!installs.discarding.empty()) busy += "，等 " + std::to_string(installs.discarding.size()) + " 个游戏删完";
  }
  panplay::log_info("收到退出请求" + busy);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      if (!installs.worker_running && installs.discarding.empty()) break;
    }
    if (Clock::now() - started >= kWait) {
      panplay::log_warn("退出：等了 20 秒安装或删除还没停下，直接退出");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#if defined(PANPLAY_PS5)
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kWait - (Clock::now() - started));
  if (!hold_privileged_ops(std::max(left, std::chrono::milliseconds(2000)))) {
    panplay::log_warn("退出：注册应用或挂载目录一直没做完，直接退出");
  }
#endif
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
  panplay::log_info("退出：用了 " + std::to_string(ms) + " ms 停好，进程结束");
}

void start_worker_locked() {
  if (installs.worker_running || installs.paused || installs.quitting || installs.waiting.empty()) return;
  if (installs.worker.joinable()) installs.worker.join();
  installs.worker_running = true;
  installs.worker = std::thread(queue_worker);
}

void load_queue() {
  std::string error;
  const panplay::Json json = panplay::Json::parse(read_file(queue_file()), &error);
  const panplay::Json* paths = json.get("paths");
  if (!error.empty() || paths == nullptr) return;
  std::lock_guard<std::mutex> lock(installs.mu);
  for (const auto& item : paths->array) {
    if (item.type != panplay::Json::Type::string || item.text.empty()) continue;
    InstallTask task;
    task.path = item.text;
    installs.waiting.push_back(std::move(task));
  }
  installs.paused = json.boolean_or("paused", false);
  installs.pause_reason = installs.paused ? json.str("pause_reason") : "";
  if (!installs.waiting.empty()) {
    panplay::log_info("上次没装完的队列有 " + std::to_string(installs.waiting.size()) + " 个，" +
                      (installs.paused ? "队列是暂停的，等点继续" : "接着装"));
  }
  start_worker_locked();
}

// 平均速度（字节/秒），seconds 带回解压了几秒。正在解压时按现在算，停下后按最后一次解压进度算。
double average_speed(const InstallTask& task, bool running, double* seconds) {
  *seconds = 0;
  if (task.rate_base < 0) return 0;
  const panplay::InstallResult& r = task.result;
  const bool live = running && r.message == "正在安装";
  const auto until = live ? std::chrono::steady_clock::now() : task.rate_last;
  *seconds = std::chrono::duration<double>(until - task.rate_since).count();
  const std::int64_t moved = r.bytes_done - task.rate_base;
  return *seconds >= 3 && moved > 0 ? static_cast<double>(moved) / *seconds : 0;
}

// 平均速度、最近的速度、预计还要几秒、解压了几秒。剩余时间按最近的速度算，还在估算时为 -1。
std::string rate_fields(const InstallTask& task, bool running) {
  if (task.rate_base < 0) return ",\"speed\":0,\"recent_speed\":0,\"eta\":-1,\"elapsed\":0";
  const panplay::InstallResult& r = task.result;
  double seconds = 0;
  const double speed = average_speed(task, running, &seconds);
  const bool estimated = task.recent_seconds >= kRecentRateWarmup && task.recent_rate > 0;
  const double recent = estimated ? task.recent_rate : 0;
  const std::int64_t left = std::max<std::int64_t>(0, r.bytes_total - r.bytes_done);
  const long long eta = running && recent > 0 ? static_cast<long long>(static_cast<double>(left) / recent) : -1;
  return ",\"speed\":" + std::to_string(static_cast<long long>(speed)) +
         ",\"recent_speed\":" + std::to_string(static_cast<long long>(recent)) + ",\"eta\":" + std::to_string(eta) +
         ",\"elapsed\":" + std::to_string(static_cast<long long>(seconds));
}

std::string task_json(const InstallTask& task, const char* state, const NameOverrides& overrides) {
  const std::string name = folder_name_of(task.path);
  const std::string title_id = panplay::find_title_id(name);
  const panplay::InstallResult& r = task.result;
  return std::string("{\"path\":\"") + panplay::json_escape(task.path) + "\",\"name\":\"" + panplay::json_escape(name) +
         "\",\"title_id\":\"" + panplay::json_escape(title_id) + "\",\"game_dir\":\"" +
         panplay::json_escape(panplay::game_dir_of(task.path)) + "\",\"variant\":\"" +
         panplay::json_escape(panplay::variant_format(task.path)) + "\",\"state\":\"" + state + "\",\"status\":\"" +
         panplay::json_escape(r.status) + "\",\"message\":\"" + panplay::json_escape(r.message) +
         "\",\"files_done\":" + std::to_string(r.files_done) + ",\"files_total\":" + std::to_string(r.files_total) +
         ",\"bytes_done\":" + std::to_string(r.bytes_done) + ",\"bytes_total\":" + std::to_string(r.bytes_total) +
         ",\"files_checked\":" + std::to_string(r.files_checked) + ",\"files_to_check\":" + std::to_string(r.files_to_check) +
         ",\"dictionary\":" + std::to_string(r.dictionary) + ",\"slow\":\"" + panplay::json_escape(r.slow) + "\"" +
         ",\"retryable\":" + (r.retryable ? "true" : "false") + rate_fields(task, std::strcmp(state, "running") == 0) +
         game_meta_fields(name, title_id, overrides, false) + "}";
}

// 队列顺序：正在装的，排队的，最近完成的。
// 本机时区比 UTC 快多少分钟。PS5 自带浏览器拿不到时区、按 UTC 算，界面上的钟点改按运行盘游这台机器的时区显示。
int local_utc_offset_minutes() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  std::tm utc{};
#if defined(_WIN32)
  localtime_s(&local, &now);
  gmtime_s(&utc, &now);
#else
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
#endif
  utc.tm_isdst = local.tm_isdst;
  return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60);
}

std::string queue_json() {
  const NameOverrides overrides = load_name_overrides();
  std::lock_guard<std::mutex> lock(installs.mu);
  std::string items;
  const auto add = [&](const InstallTask& task, const char* state) {
    if (!items.empty()) items += ",";
    items += task_json(task, state, overrides);
  };
  if (installs.current) add(*installs.current, "running");
  for (const auto& task : installs.waiting) add(task, "queued");
  for (const auto& task : installs.finished) add(task, "done");
  return "{\"tasks\":[" + items + "],\"paused\":" + (installs.paused ? "true" : "false") + ",\"pause_reason\":\"" +
         panplay::json_escape(installs.pause_reason) + "\",\"utc_offset_min\":" + std::to_string(local_utc_offset_minutes()) +
         ",\"version\":" + std::to_string(library_version.load()) + "}";
}

SpeedReference find_speed_reference() {
  SpeedReference found;
  std::lock_guard<std::mutex> lock(installs.mu);
  double seconds = 0;
  const auto pick = [&](const InstallTask& task, bool running) {
    const double speed = average_speed(task, running, &seconds);
    if (!found.path.empty() || !(speed > 0)) return;
    found = {task.path, static_cast<long long>(speed), running};
  };
  if (installs.current) pick(*installs.current, true);
  for (const auto& task : installs.finished) pick(task, false);
  return found;
}

template <typename Tasks>
bool erase_path(Tasks& tasks, const std::string& path) {
  const auto it = std::find_if(tasks.begin(), tasks.end(), [&](const InstallTask& task) { return task.path == path; });
  if (it == tasks.end()) return false;
  tasks.erase(it);
  return true;
}

std::string request_target(const std::string& header) {
  const auto line_end = header.find("\r\n");
  const std::string line = header.substr(0, line_end);
  const auto first = line.find(' ');
  const auto second = line.find(' ', first == std::string::npos ? 0 : first + 1);
  if (first == std::string::npos || second == std::string::npos) return "/";
  return line.substr(first + 1, second - first - 1);
}

std::string request_method(const std::string& header) {
  const auto space = header.find(' ');
  if (space == std::string::npos) return "";
  return header.substr(0, space);
}

std::string header_value(const std::string& header, const std::string& name) {
  std::size_t start = header.find("\r\n");
  while (start != std::string::npos) {
    start += 2;
    const std::size_t end = header.find("\r\n", start);
    const std::string line = header.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const std::size_t colon = line.find(':');
    if (colon == name.size()) {
      bool same = true;
      for (std::size_t i = 0; i < colon; ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
          same = false;
          break;
        }
      }
      if (same) {
        std::size_t value = colon + 1;
        while (value < line.size() && line[value] == ' ') ++value;
        return line.substr(value);
      }
    }
    start = end;
  }
  return "";
}

// Host 头里的主机名，去掉端口，小写；IPv6 保留方括号。
std::string host_name(std::string host) {
  if (host.rfind("http://", 0) == 0) host = host.substr(7);
  if (!host.empty() && host.front() == '[') {
    const std::size_t end = host.find(']');
    return end == std::string::npos ? "" : ascii_lower(host.substr(0, end + 1));
  }
  const std::size_t colon = host.rfind(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  return ascii_lower(host);
}

bool is_loopback_host(const std::string& host) {
  const std::string name = host_name(host);
  return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

// 只认按 IP、localhost、局域网机器名（不带点）或 .local 名打开的页面。
// DNS 重绑定得用攻击者自己的域名，在这里就被挡掉，所以不开口令也不怕外网网页借浏览器来操作。
bool is_home_host(const std::string& host) {
  const std::string name = host_name(host);
  if (name.empty()) return false;
  if (name.front() == '[') return true;
  in_addr ipv4{};
  if (inet_pton(AF_INET, name.c_str(), &ipv4) == 1) return true;
  if (name.find('.') == std::string::npos) return true;
  return name.size() > 6 && name.compare(name.size() - 6, 6, ".local") == 0;
}

// POST 要求 JSON 让浏览器跨站请求必须预检，而这里不响应预检；带了 Origin 时还要和 Host 对得上。
bool trusted_request(const std::string& header, const std::string& method) {
  const std::string host = header_value(header, "Host");
  if (!is_home_host(host)) return false;
  if (method != "POST") return true;
  const std::string origin = header_value(header, "Origin");
  if (!origin.empty() && ascii_lower(origin) != "http://" + ascii_lower(host)) return false;
  return header_value(header, "Content-Type").rfind("application/json", 0) == 0;
}

std::string cookie_value(const std::string& header, const std::string& name) {
  const std::string cookies = header_value(header, "Cookie");
  std::size_t start = 0;
  while (start < cookies.size()) {
    std::size_t end = cookies.find(';', start);
    if (end == std::string::npos) end = cookies.size();
    std::string pair = cookies.substr(start, end - start);
    const std::size_t first = pair.find_first_not_of(' ');
    pair = first == std::string::npos ? "" : pair.substr(first);
    if (pair.rfind(name + "=", 0) == 0) return pair.substr(name.size() + 1);
    start = end + 1;
  }
  return "";
}

// 本机打开（连接来自本机且按 localhost 打开）不用口令。
bool local_request(bool local_peer, const std::string& header) {
  return local_peer && is_loopback_host(header_value(header, "Host"));
}

bool unlocked(bool local_peer, const std::string& header) {
  if (!lan.require_code || local_request(local_peer, header)) return true;
  const std::string token = cookie_value(header, "panplay_session");
  std::lock_guard<std::mutex> lock(lan.mu);
  return !token.empty() && lan.sessions.count(token) != 0;
}

std::string current_code() {
  std::lock_guard<std::mutex> lock(lan.mu);
  return lan.code;
}

std::string new_session_cookie() {
  const std::string token = random_token();
  {
    std::lock_guard<std::mutex> lock(lan.mu);
    lan.sessions.insert(token);
  }
  return "Set-Cookie: panplay_session=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000\r\n";
}

// 口令核对一次只做一个，错了停一秒，挡住并发猜口令。
void handle_unlock(SOCKET client, const std::string& body) {
  if (!lan.require_code) {
    reply_json(client, 200, "{}");
    return;
  }
  const panplay::Json json = panplay::Json::parse(body, nullptr);
  static std::mutex attempts;
  {
    std::lock_guard<std::mutex> one_at_a_time(attempts);
    if (!same_text(json.str("code"), current_code())) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      reply_json(client, 403, json_error("口令不对"));
      return;
    }
  }
  reply(client, 200, "application/json; charset=utf-8", "{}", new_session_cookie());
}

std::string lan_address();

std::string lan_url() {
  if (!lan.enabled) return "";
  const std::string address = lan_address();
  return address.empty() ? "" : "http://" + address + ":" + std::to_string(lan.port);
}

std::string access_json(bool local_peer, const std::string& header) {
  return std::string("{\"lan\":") + (lan.enabled ? "true" : "false") +
         ",\"lan_required\":" + (lan.required ? "true" : "false") +
         ",\"require_code\":" + (lan.require_code ? "true" : "false") +
         ",\"code\":\"" + panplay::json_escape(current_code()) + "\"" +
         ",\"local\":" + (local_request(local_peer, header) ? "true" : "false") +
         ",\"url\":\"" + panplay::json_escape(lan_url()) + "\"}";
}

// 改局域网访问 / 口令。口令换了或刚开口令时，旧会话全部作废，发起修改的这台设备直接拿新会话。
void handle_access(SOCKET client, bool local_peer, const std::string& header, const std::string& body) {
  std::string error;
  const panplay::Json json = panplay::Json::parse(body, &error);
  if (!error.empty() || json.type != panplay::Json::Type::object) {
    reply_json(client, 400, json_error("请求格式不对"));
    return;
  }
  static std::mutex one_change;
  std::lock_guard<std::mutex> one_at_a_time(one_change);
  const bool want_lan = lan.required || json.boolean_or("lan", lan.enabled);
  const bool want_code = json.boolean_or("require_code", lan.require_code);
  std::string code = json.get("code") ? trimmed(json.str("code")) : current_code();
  if (code.size() < 4 || code.size() > 32 ||
      std::any_of(code.begin(), code.end(), [](unsigned char c) { return c <= ' '; })) {
    reply_json(client, 400, json_error("口令要 4 到 32 个字符，不能有空格"));
    return;
  }
  if (!want_lan && !local_request(local_peer, header)) {
    reply_json(client, 400, json_error("关闭局域网访问后这台设备就打不开盘游了，请在运行盘游的电脑上关"));
    return;
  }
  const bool code_changed = code != current_code();
  if (code_changed) {
    error = save_access_code(code);
    if (!error.empty()) {
      reply_json(client, 500, json_error(error));
      return;
    }
  }
  AppSettings settings = load_settings();
  settings.lan_access = want_lan;
  settings.require_code = want_code;
  error = save_settings(settings);
  if (!error.empty()) {
    reply_json(client, 500, json_error(error));
    return;
  }
  const bool reset_sessions = code_changed || (want_code && !lan.require_code);
  {
    std::lock_guard<std::mutex> lock(lan.mu);
    lan.code = code;
    if (reset_sessions) lan.sessions.clear();
  }
  lan.enabled = want_lan;
  lan.require_code = want_code;
  reply(client, 200, "application/json; charset=utf-8", access_json(local_peer, header),
        reset_sessions ? new_session_cookie() : "");
}

void handle_client(SOCKET client, bool local_peer) {
  std::string data;
  char buffer[4096];
  while (data.find("\r\n\r\n") == std::string::npos && data.size() < 65536) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) return;
    data.append(buffer, buffer + n);
  }
  const auto split = data.find("\r\n\r\n");
  if (split == std::string::npos) return;
  const std::string header = data.substr(0, split);
  std::string body = data.substr(split + 4);
  std::size_t length = 0;
  const auto content = header.find("Content-Length:");
  if (content != std::string::npos) {
    length = static_cast<std::size_t>(std::strtoul(header.c_str() + content + 15, nullptr, 10));
  }
  while (body.size() < length && body.size() < 1024 * 1024) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    body.append(buffer, buffer + n);
  }
  body.resize(std::min(body.size(), length));

  const std::string method = request_method(header);
  const std::string target = request_target(header);
  // 界面每秒轮询队列、测速近况，每几秒查资料库状态，封面一页几十张，这些不记。
  if (target != "/api/queue" && target != "/api/catalog" && target != "/api/speedtest" &&
      target.rfind("/api/cover/", 0) != 0 && target.rfind("/api/site-cover/", 0) != 0) {
    PANPLAY_DEBUG("请求 " + method + " " + target + (local_peer ? "（本机）" : "（局域网）"));
  }
  if (!local_peer && !lan.enabled) {
    reply_json(client, 403, json_error("局域网访问已关闭，请在运行盘游的电脑上打开设置里的局域网访问"));
    return;
  }
  if (!trusted_request(header, method)) {
    reply_json(client, 403, json_error("请用 IP 地址或本机名打开盘游"));
    return;
  }

  if (method == "GET" && (target == "/" || target == "/index.html")) {
    reply(client, 200, "text/html; charset=utf-8", ui_file("index.html"));
    return;
  }
  if (method == "GET" && target == "/app.css") {
    reply(client, 200, "text/css; charset=utf-8", ui_file("app.css"));
    return;
  }
  if (method == "GET" && target == "/app.js") {
    reply(client, 200, "text/javascript; charset=utf-8", ui_file("app.js"));
    return;
  }
  if (method == "POST" && target == "/api/unlock") {
    handle_unlock(client, body);
    return;
  }
  if (!unlocked(local_peer, header)) {
    reply_json(client, 401, "{\"error\":\"需要访问口令\",\"locked\":true}");
    return;
  }
  if (method == "GET" && target == "/api/access") {
    reply_json(client, 200, access_json(local_peer, header));
    return;
  }
  if (method == "POST" && target == "/api/access") {
    handle_access(client, local_peer, header, body);
    return;
  }
  if (method == "GET" && target.rfind("/api/cover/", 0) == 0) {
    const std::string id = target.substr(11, target.find('?') == std::string::npos ? std::string::npos : target.find('?') - 11);
    const bool digits = !id.empty() && id.size() < 19 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); });
    const std::string bytes = digits && store_meta != nullptr ? read_file(store_meta->cover_file(std::stoll(id))) : "";
    if (bytes.empty()) {
      reply_json(client, 404, json_error("没有这张封面"));
      return;
    }
    reply(client, 200, image_type(bytes), bytes, "Cache-Control: max-age=86400\r\n");
    return;
  }
  if (method == "GET" && target.rfind("/api/site-cover/", 0) == 0) {
    const std::string id = target.substr(16, target.find('?') == std::string::npos ? std::string::npos : target.find('?') - 16);
    const bool valid = !id.empty() && id.size() <= 16 &&
                       std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
    const std::string bytes = valid ? site_cover_bytes(id) : "";
    if (bytes.empty()) {
      reply_json(client, 404, json_error("没有这张封面"));
      return;
    }
    reply(client, 200, image_type(bytes), bytes, "Cache-Control: max-age=86400\r\n");
    return;
  }
  if (method == "POST" && (target == "/api/meta" || target == "/api/name" || target == "/api/meta/refresh")) {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty()) {
      reply_json(client, 400, json_error(parse_error));
      return;
    }
    if (target == "/api/meta") {
      const NameOverrides overrides = load_name_overrides();
      std::string out = "{\"items\":[";
      const panplay::Json* names = json.get("names");
      const bool detail = json.boolean_or("detail", false);
      if (names != nullptr) {
        for (std::size_t i = 0; i < names->array.size() && i < 500; ++i) {
          if (i > 0) out += ",";
          out += meta_json(names->array[i].text, overrides, detail);
        }
      }
      reply_json(client, 200, out + "]}");
      return;
    }
    const std::string name = json.str("name");
    if (name.empty()) {
      reply_json(client, 400, json_error("缺少游戏目录名"));
      return;
    }
    if (target == "/api/name") {
      std::string display = json.str("display_name");
      const auto begin = display.find_first_not_of(" \t\r\n");
      display = begin == std::string::npos ? "" : display.substr(begin, display.find_last_not_of(" \t\r\n") - begin + 1);
      if (display.size() > 400) {
        reply_json(client, 400, json_error("名称太长"));
        return;
      }
      const std::string error = save_name_override(name_key(name, panplay::find_title_id(name)), display);
      if (!error.empty()) {
        reply_json(client, 500, json_error(error));
        return;
      }
      library_changed();
    } else {
      const std::string title_id = panplay::find_title_id(name);
      const panplay::CatalogMatch match = panplay::GameCatalog::instance().lookup(title_id, name);
      const auto library = shelf_library();
      const std::int64_t concept_id = store_concept_of(match, library ? library->find(title_id) : nullptr);
      if (concept_id <= 0 || store_meta == nullptr) {
        reply_json(client, 400, json_error("这款游戏没有 conceptId，无法从商店获取"));
        return;
      }
      store_meta->refresh(concept_id);
    }
    reply_json(client, 200, meta_json(name, load_name_overrides(), true));
    return;
  }
  if (method == "GET" && target == "/api/catalog") {
    reply_json(client, 200, catalog_status_json());
    return;
  }
  if (method == "GET" && target == "/api/shelf-library") {
    reply_json(client, 200, shelf_library_status_json(load_settings().library_path()));
    return;
  }
  // 详情页「⋯ › 让盘架重新分析」。
  if (method == "POST" && target == "/api/shelf-feedback") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    if (!parse_error.empty() || path.empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    const std::string note = trimmed(json.str("note"));
    if (!report_to_shelf(path, "manual", note.empty() ? "用户要求重新分析" : note.substr(0, 300))) {
      reply_json(client, 400, json_error("盘架资源库里没有这种格式，通知不了盘架"));
      return;
    }
    reply_json(client, 200, "{\"ok\":true}");
    return;
  }
  if (method == "POST" && target == "/api/catalog/reload") {
    std::string catalog_error;
    const bool ok = panplay::GameCatalog::instance().reload(&catalog_error);
    if (!ok) {
      reply_json(client, 500, json_error(catalog_error.empty() ? "资料库加载失败" : catalog_error));
      return;
    }
    library_changed();
    reply_json(client, 200, catalog_status_json());
    return;
  }
  // 正在装的游戏停下后回到队首，下次启动接着装。
  // 停好以后仍用 _Exit：走正常退出时，系统库在析构全局对象时抛 resource deadlock would occur 并中止进程。
  if (method == "POST" && target == "/api/quit") {
    reply_json(client, 200, "{}");
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      stop_for_quit();
      if (on_quit) on_quit();
      std::fflush(nullptr);
      std::_Exit(0);
    }).detach();
    return;
  }
  if (method == "GET" && target == "/api/settings") {
    reply_json(client, 200, settings_json(load_settings()));
    return;
  }
  // 刚换过文件时新日志只有几行，把上一份接在前面一起给。
  if (method == "GET" && target == "/api/log") {
    const std::filesystem::path dir = panplay::app_data_dir();
    const std::string bytes = read_file(dir / "panplay.old.log") + read_file(dir / "panplay.log");
    if (bytes.empty()) {
      reply_json(client, 404, json_error("还没有日志"));
      return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "panplay-%Y%m%d-%H%M%S.log", &tm);
    reply(client, 200, "text/plain; charset=utf-8", bytes,
          std::string("Content-Disposition: attachment; filename=\"") + name + "\"\r\nCache-Control: no-store\r\n");
    return;
  }
  if (method == "POST" && target == "/api/settings") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty()) {
      reply_json(client, 400, json_error(parse_error));
      return;
    }
    AppSettings settings = load_settings();
    std::string invalid = apply_settings(json, &settings);
    if (!invalid.empty()) {
      reply_json(client, 400, json_error(invalid));
      return;
    }
    const std::string error = save_settings(settings);
    if (error.empty() && (settings.verbose_log ? panplay::LogLevel::debug : panplay::LogLevel::info) != panplay::log_level()) {
      panplay::set_log_level(settings.verbose_log ? panplay::LogLevel::debug : panplay::LogLevel::info);
      panplay::log_info(std::string("详细日志已") + (settings.verbose_log ? "打开" : "关闭"));
    }
    if (error.empty()) library_changed();
    reply_json(client, error.empty() ? 200 : 500, error.empty() ? "{\"ok\":true}" : json_error(error));
    return;
  }
  // 下载测速在后台跑，界面轮询近况；装游戏时也能测，两边会分带宽。
  if (method == "GET" && target == "/api/speedtest") {
    reply_json(client, 200, speed_test_json());
    return;
  }
  if (method == "POST" && target == "/api/speedtest/start") {
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      if (installs.current) {
        reply_json(client, 409, json_error("正在安装，先暂停再测网速"));
        return;
      }
    }
    auto drive = open_drive();
    if (!drive->session_valid()) {
      reply_json(client, 400, json_error("先登录网盘"));
      return;
    }
    SpeedReference reference = find_speed_reference();
    const std::string library_path = load_settings().library_path();
    std::string error;
    {
      // 拿着锁开始，轮询不会在新测速里读到上一次的参照。
      std::lock_guard<std::mutex> lock(speed_reference_mu);
      error = speed_test.start(drive, library_path);
      if (error.empty()) speed_reference = std::move(reference);
    }
    if (!error.empty()) {
      reply_json(client, 409, json_error(error));
      return;
    }
    panplay::log_info("开始下载测速");
    reply_json(client, 200, speed_test_json());
    return;
  }
  if (method == "POST" && target == "/api/speedtest/stop") {
    speed_test.stop();
    reply_json(client, 200, speed_test_json());
    return;
  }
  // 硬盘测速和安装抢同一块盘，装着游戏时不测。
  if (method == "GET" && target == "/api/disktest") {
    reply_json(client, 200, disk_test_json());
    return;
  }
  if (method == "POST" && target == "/api/disktest/start") {
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      if (installs.current) {
        reply_json(client, 409, json_error("正在安装，先暂停再测硬盘"));
        return;
      }
    }
    const AppSettings settings = load_settings();
    panplay::Destination destination;
    std::string error;
    if (settings.dest().empty() || !panplay::parse_destination(settings.dest(), &destination, &error)) {
      reply_json(client, 400, json_error(error.empty() ? "先在设置里填写存放位置" : error));
      return;
    }
    destination.prefix = settings.prefix();
    if (error = disk_test.start(destination); !error.empty()) {
      reply_json(client, 409, json_error(error));
      return;
    }
    panplay::log_info("开始硬盘读写测速");
    reply_json(client, 200, disk_test_json());
    return;
  }
  if (method == "POST" && target == "/api/disktest/stop") {
    disk_test.stop();
    reply_json(client, 200, disk_test_json());
    return;
  }
  if (method == "GET" && target == "/api/session") {
    auto drive = open_drive();
    reply_json(client, 200, std::string("{\"logged_in\":") + (drive->session_valid() ? "true" : "false") + "}");
    return;
  }
  if (method == "POST" && target == "/api/login/start") {
    auto drive = open_drive();
    if (drive->session_valid()) {
      reply_json(client, 200, "{\"already\":true}");
      return;
    }
    const panplay::LoginChallenge challenge = drive->start_login();
    if (!challenge.error.empty()) {
      reply_json(client, 400, json_error(challenge.error));
      return;
    }
    std::ofstream out(login_challenge_file(), std::ios::trunc);
    out << "{\"device_code\":\"" << panplay::json_escape(challenge.device_code) << "\",\"interval\":"
        << challenge.interval_seconds << ",\"expires_in\":" << challenge.expires_in << ",\"qrcode_url\":\""
        << panplay::json_escape(challenge.qrcode_url) << "\",\"user_code\":\"" << panplay::json_escape(challenge.user_code)
        << "\"}";
    reply_json(client, 200,
               "{\"qrcode_url\":\"" + panplay::json_escape(challenge.qrcode_url) + "\",\"user_code\":\"" +
                   panplay::json_escape(challenge.user_code) + "\",\"interval\":" + std::to_string(challenge.interval_seconds) +
                   ",\"expires_in\":" + std::to_string(challenge.expires_in) + "}");
    return;
  }
  if (method == "POST" && target == "/api/login/poll") {
    auto drive = open_drive();
    if (drive->session_valid()) {
      reply_json(client, 200, "{\"done\":true,\"expired\":false,\"error\":\"\",\"retry_after\":0}");
      return;
    }
    std::ifstream in(login_challenge_file());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const panplay::Json saved = panplay::Json::parse(buffer.str(), nullptr);
    panplay::LoginChallenge challenge;
    challenge.device_code = saved.str("device_code");
    challenge.interval_seconds = static_cast<int>(saved.num("interval"));
    challenge.expires_in = static_cast<int>(saved.num("expires_in"));
    if (challenge.device_code.empty()) {
      reply_json(client, 400, json_error("还没有开始登录"));
      return;
    }
    static std::mutex poll_gate;
    static std::chrono::steady_clock::time_point next_baidu_poll{};
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(poll_gate);
      if (now < next_baidu_poll) {
        const int retry = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(next_baidu_poll - now).count()));
        reply_json(client, 200,
                   std::string("{\"done\":false,\"expired\":false,\"error\":\"\",\"retry_after\":") + std::to_string(retry) + "}");
        return;
      }
    }
    const panplay::LoginPoll poll = drive->poll_login(challenge);
    const int retry = poll.retry_after_seconds > 0 ? poll.retry_after_seconds : std::max(challenge.interval_seconds, 5);
    {
      std::lock_guard<std::mutex> lock(poll_gate);
      next_baidu_poll = std::chrono::steady_clock::now() + std::chrono::seconds(retry);
    }
    reply_json(client, 200,
               std::string("{\"done\":") + (poll.done ? "true" : "false") + ",\"expired\":" + (poll.expired ? "true" : "false") +
                   ",\"scanned\":" + (poll.scanned ? "true" : "false") + ",\"error\":\"" + panplay::json_escape(poll.error) + "\",\"retry_after\":" + std::to_string(retry) + "}");
    return;
  }
  if (method == "POST" && target == "/api/game") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty() || json.str("path").empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    const std::string payload = one_game_json(json.str("path"), json.boolean_or("cached", false));
    reply_json(client, payload.rfind("{\"error\"", 0) == 0 ? 400 : 200, payload);
    return;
  }
  if (method == "GET" && (target == "/api/games" || target.rfind("/api/games?", 0) == 0)) {
    refresh_reviews_if_stale();
    refresh_shelf_library_if_stale();
    auto drive = open_drive();
    const auto mark = target.find('?');
    const std::string json = games_json(*drive, mark == std::string::npos ? "" : target.substr(mark + 1));
    reply_json(client, json.rfind("{\"error\"", 0) == 0 ? 400 : 200, json);
    return;
  }
  if (method == "GET" && target == "/api/queue") {
    reply_json(client, 200, queue_json());
    return;
  }
  if (method == "POST" && target == "/api/queue/clear") {
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      installs.finished.clear();
    }
    reply_json(client, 200, queue_json());
    return;
  }
  // 全部暂停：正在装的停在能续装的地方、回到队首，排队的都不开始；全部继续：从队首接着装。
  if (method == "POST" && (target == "/api/queue/pause" || target == "/api/queue/resume")) {
    const bool pause = target == "/api/queue/pause";
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      installs.paused = pause;
      installs.pause_reason.clear();
      if (pause && installs.current && installs.current->cancel) {
        installs.current->pausing = true;
        installs.current->cancel->cancel();
        installs.current->result.message = "正在暂停…";
      }
      start_worker_locked();
      save_queue_locked();
    }
    panplay::log_info(pause ? "安装队列全部暂停" : "安装队列全部继续");
    reply_json(client, 200, queue_json());
    return;
  }
  // 加入队列（install）、移出或取消（remove）、排到下一个（first）都按游戏目录指定。
  if (method == "POST" && (target == "/api/install" || target == "/api/queue/remove" || target == "/api/queue/first")) {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    if (!parse_error.empty() || path.empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      const bool running = installs.current && installs.current->path == path;
      const auto waiting = std::find_if(installs.waiting.begin(), installs.waiting.end(),
                                        [&](const InstallTask& task) { return task.path == path; });
      if (target == "/api/install") {
        if (running || waiting != installs.waiting.end()) {
          reply_json(client, 409, json_error("这个游戏已经在安装队列里"));
          return;
        }
        if (installs.discarding.count(path) != 0) {
          reply_json(client, 409, json_error("正在删除或注册这个游戏，稍等再装"));
          return;
        }
        // 带了 skip 就是用户刚选过要装哪些；没带（继续、重试）沿用上次的选择。
        if (const panplay::Json* skip = json.get("skip")) {
          std::set<std::string> skipped;
          for (const auto& id : skip->array) {
            if (!id.text.empty()) skipped.insert(id.text);
          }
          if (const std::string save_error = save_skipped_sets(path, skipped); !save_error.empty()) {
            reply_json(client, 500, json_error(save_error));
            return;
          }
        }
        erase_path(installs.finished, path);
        InstallTask task;
        task.path = path;
        installs.waiting.push_back(std::move(task));
        start_worker_locked();
      } else if (target == "/api/queue/first") {
        if (waiting == installs.waiting.end()) {
          reply_json(client, 409, json_error("这个游戏不在排队"));
          return;
        }
        InstallTask task = std::move(*waiting);
        installs.waiting.erase(waiting);
        installs.waiting.push_front(std::move(task));
      } else if (running) {
        installs.current->cancel->cancel();
        installs.current->result.message = "正在暂停…";
      } else if (waiting != installs.waiting.end()) {
        installs.waiting.erase(waiting);
        library_changed();
      } else if (!erase_path(installs.finished, path)) {
        reply_json(client, 409, json_error("这个游戏不在安装队列里"));
        return;
      }
      save_queue_locked();
    }
    reply_json(client, 200, queue_json());
    return;
  }
  // discard 删掉没装完的游戏已经写到存放位置的文件，uninstall 删掉整个游戏文件夹；都会把它移出队列。
  // 正在装或排队的要先取消。
  // 记下这个版本能不能玩，或者清掉评价。按详情页上次读到的压缩包算内容指纹，所以要先打开过详情。
  if (method == "POST" && (target == "/api/review" || target == "/api/review/remove")) {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    if (!parse_error.empty() || path.empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    const auto detail = cached_game_detail(shelf_key(load_settings()), path);
    if (!detail || detail->fingerprint.empty()) {
      reply_json(client, 400, json_error("还没读到这个游戏的压缩包，等详情页读完再评价"));
      return;
    }
    if (target == "/api/review/remove") {
      const std::string error = remove_review(detail->fingerprint);
      if (error.empty()) {
        review_changed();
        library_changed();
      }
      reply_json(client, error.empty() ? 200 : 500, error.empty() ? "{\"review\":null}" : json_error(error));
      return;
    }
    GameReview review;
    review.fingerprint = detail->fingerprint;
    review.title_id = panplay::find_title_id(folder_name_of(path));
    review.name = game_display_name(path);
    review.format = review_format(path);
    review.playable = json.boolean_or("playable", true);
    review.note = trimmed(json.str("note"));
    if (review.note.size() > 300) {
      std::size_t cut = 300;
      while (cut > 0 && (static_cast<unsigned char>(review.note[cut]) & 0xC0) == 0x80) --cut;
      review.note.resize(cut);
    }
#if defined(PANPLAY_PS5)
    review.firmware = firmware_text();
#endif
    review.at = static_cast<std::int64_t>(std::time(nullptr));
    review.layers = review_layers(*detail);
    review.parts = review_parts(*detail);
    const std::string error = save_review(review);
    if (!error.empty()) {
      reply_json(client, 500, json_error(error));
      return;
    }
    review_changed();
    library_changed();
    if (!review.playable) report_to_shelf(path, "bad_review", review.note.empty() ? "装好后标了有问题" : review.note);
    panplay::log_info(std::string("评价 ") + review.name + "：" + (review.playable ? "能玩" : "有问题 " + review.note) +
                      (review.firmware.empty() ? "" : "，固件 " + review.firmware));
    reply_json(client, 200, "{\"review\":" + review_json(find_review(review.fingerprint).value_or(review)) + "}");
    return;
  }
#if defined(PANPLAY_PS5)
  // 把装好的游戏重新注册到主机桌面（自动注册失败、图标被删掉、挂载丢了时用）。
  if (method == "POST" && target == "/api/console/register") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    const std::string game_dir = console_game_dir(load_settings(), path);
    if (!parse_error.empty() || path.empty() || game_dir.empty()) {
      reply_json(client, 400, json_error("只有装在主机本机文件夹里的游戏能注册"));
      return;
    }
    if (const std::string busy = claim_console_path(path, "这个游戏还在安装队列里，装完会自动注册"); !busy.empty()) {
      reply_json(client, 409, json_error(busy));
      return;
    }
    std::string error;
    {
      const ConsolePathClaim claim{path};
      error = register_console_title(game_dir);
      // 以前装好的游戏目录里带着激活包的，重新注册时顺便装上。
      if (error.empty()) install_console_side_packages(path);
    }
    remember_console_error(path, error);
    if (!error.empty()) {
      reply_json(client, 500, json_error(error));
      return;
    }
    reply_json(client, 200, "{\"console\":" + console_json(path, game_dir) + "}");
    return;
  }
#endif
  // 把游戏文件夹从网盘删掉腾空间：装好了不用再留，或者装不了、玩不了。主机上装好的游戏不动，评价按内容指纹记着也不丢。
  // 只删游戏库目录下列出来的游戏文件夹；主机上留着没装完的文件时不删，不然没地方再删那些文件。
  if (method == "POST" && target == "/api/drive/remove") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    if (!parse_error.empty() || path.empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    auto drive = open_drive();
    std::string error;
    const auto listing = shelf_listing(*drive, false, &error);
    if (!listing) {
      reply_json(client, 500, json_error(error));
      return;
    }
    // 按格式分了的游戏可以只删一种格式（变体目录）。
    const std::string game_dir = panplay::game_dir_of(path);
    const auto folder = std::find_if(listing->folders.begin(), listing->folders.end(),
                                     [&](const ShelfListing::Folder& item) { return item.path == game_dir; });
    if (folder == listing->folders.end()) {
      reply_json(client, 400, json_error("游戏库目录里没有这个文件夹，可能已经从网盘删掉了"));
      return;
    }
    // 游戏库设成网盘根目录时，盘游自己用的目录（评价、待删除都在 /盘游 里）和旧版的待删除目录也会列成游戏。
    if (path == "/盘游" || path == "/盘游待删除" || path == "/盘架待删除" || path == "/盘架临时") {
      reply_json(client, 400, json_error("这是盘游自己用的文件夹，不能当游戏删除"));
      return;
    }
    if (listing->sink_ready && shelf_state(listing->placement, folder->name, folder->title_id, path) == "continue") {
      reply_json(client, 409, json_error("存放位置里还留着这个游戏没装完的文件，先在「⋯」里删除已装的部分，再从网盘删除"));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      const auto hit = [&](const std::string& task_path) {
        return task_path == path || panplay::game_dir_of(task_path) == path;
      };
      const bool queued = (installs.current && hit(installs.current->path)) ||
                          std::any_of(installs.waiting.begin(), installs.waiting.end(),
                                      [&](const InstallTask& task) { return hit(task.path); });
      if (queued) {
        reply_json(client, 409, json_error("这个游戏还在安装队列里，先取消再从网盘删除"));
        return;
      }
      if (!installs.discarding.insert(path).second) {
        reply_json(client, 409, json_error("正在处理这个游戏，稍等再试"));
        return;
      }
    }
    std::string moved_to;
    panplay::log_info("从网盘删除 " + path);
    error = drive->remove_path(path, &moved_to);
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      installs.discarding.erase(path);
      if (error.empty()) erase_path(installs.finished, path);
    }
    if (!error.empty()) {
      panplay::log_warn("从网盘删除失败 " + path + "：" + error);
      reply_json(client, 500, json_error(error));
      return;
    }
    if (!moved_to.empty()) panplay::log_warn("百度要求安全验证、不能直接删除，已移到 " + moved_to + "：" + path);
    forget_install_progress(path);
    forget_game_detail(path);
    {
      std::lock_guard<std::mutex> lock(shelf_mu);
      shelf_cache.reset();
    }
    library_changed();
    reply_json(client, 200, "{\"moved_to\":\"" + panplay::json_escape(moved_to) + "\"}");
    return;
  }
  if (method == "POST" && (target == "/api/discard" || target == "/api/uninstall")) {
    const bool whole = target == "/api/uninstall";
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    const std::string path = json.str("path");
    if (!parse_error.empty() || path.empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      // 几种格式装在同一个文件夹，另一种格式在排队也不能删。
      const auto same_game = [&](const std::string& other) { return panplay::game_dir_of(other) == panplay::game_dir_of(path); };
      const bool queued = (installs.current && same_game(installs.current->path)) ||
                          std::any_of(installs.waiting.begin(), installs.waiting.end(),
                                      [&](const InstallTask& task) { return same_game(task.path); });
      if (queued) {
        reply_json(client, 409, json_error("这个游戏还在安装队列里，先取消再删除"));
        return;
      }
      if (!installs.discarding.insert(path).second) {
        reply_json(client, 409, json_error("正在处理这个游戏，稍等再试"));
        return;
      }
    }
    std::string error;
    bool running = false;
    try {
      const AppSettings settings = load_settings();
      if (auto sink = open_sink(settings, &error)) {
        panplay::log_info(std::string(whole ? "删除游戏 " : "删除没装完的文件 ") + path);
#if defined(PANPLAY_PS5)
        // 游戏开着时删文件，游戏会崩；ShadowMountPlus 管着、主机上没挂盘游的挂载时，卸挂载那一步也拦不住，所以先查一次。
        const std::string game_dir = console_game_dir(settings, path);
        running = !game_dir.empty() && console_title_running(game_dir);
        // 先从主机注销、卸掉挂载，注销不成就不删文件，免得留下打不开的图标。
        if (!running && whole && !game_dir.empty()) {
          error = unregister_console_title(game_dir);
          running = error == kTitleBusyError;
          if (!error.empty()) error = "没能从主机注销，先不删除：" + error;
          remember_console_error(path, "");
        }
        if (running) error = "「" + game_display_name(path) + "」正在主机上运行，先在主机上关掉游戏再删除";
#endif
        if (error.empty()) {
          error = whole ? panplay::uninstall_game(*sink, settings.prefix(), path)
                        : panplay::discard_install(*sink, settings.prefix(), path);
        }
      }
    } catch (const std::exception& ex) {
      error = std::string("删除失败: ") + ex.what();
    }
    if (!error.empty()) panplay::log_warn("删除失败 " + path + "：" + error);
    {
      std::lock_guard<std::mutex> lock(installs.mu);
      installs.discarding.erase(path);
      if (error.empty()) erase_path(installs.finished, path);
    }
    if (error.empty()) forget_install_progress(path);
    if (error.empty() && whole) forget_console_packages(path);
    if (!error.empty()) {
      reply_json(client, running ? 409 : 500, json_error(error));
      return;
    }
    forget_game_detail(path);
    refresh_shelf_placement();
    library_changed();
    reply_json(client, 200, queue_json());
    return;
  }
  reply_json(client, 404, json_error("没有这个页面"));
}

// 本机在局域网里的地址：向外连一个 UDP 套接字（不发包），看系统选了哪个本地地址。
std::string lan_address() {
  SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (probe == INVALID_SOCKET) return "";
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  std::string address;
  if (connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
    sockaddr_in local{};
    panplay::socklen length = sizeof(local);
    char text[INET_ADDRSTRLEN]{};
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
        inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr) {
      address = text;
    }
  }
  closesocket(probe);
  return address;
}

}  // namespace

// 在所有网卡上监听这个端口，失败返回 INVALID_SOCKET（errno 是原因）。
static SOCKET open_listener(int port) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return INVALID_SOCKET;
  panplay::set_listen_exclusive(listener);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
    const int code = errno;
    closesocket(listener);
    errno = code;
    return INVALID_SOCKET;
  }
  return listener;
}

int run_server(const ServerOptions& options) {
  const int port = options.port;
  on_quit = options.on_quit;
  panplay::log_info("数据目录 " + panplay::app_data_dir().u8string());
  {
    const AppSettings settings = load_settings();
    panplay::set_log_level(panplay::log_level_from_env(settings.verbose_log ? panplay::LogLevel::debug
                                                                            : panplay::LogLevel::info));
    lan.required = options.lan_required;
    lan.enabled = lan.required || options.lan.value_or(settings.lan_access);
    lan.require_code = settings.require_code;
    lan.port = port;
    PANPLAY_DEBUG("读取访问口令");
    lan.code = load_or_create_access_code();
    panplay::log_info(std::string("设置：局域网访问 ") + (lan.enabled ? "开" : "关") + "，口令 " +
                      (lan.require_code ? "开" : "关") + "，详细日志 " + (settings.verbose_log ? "开" : "关"));
  }
  {
    std::string catalog_error;
    const auto catalog_start = std::chrono::steady_clock::now();
    PANPLAY_DEBUG("加载游戏资料库");
    if (!panplay::GameCatalog::instance().load_default(&catalog_error)) {
      panplay::log_warn("游戏资料库未加载（将使用目录名占位，后台自动同步中）: " + catalog_error);
    }
    panplay::log_info("游戏资料库 " + std::to_string(panplay::GameCatalog::instance().size()) + " 条，来源 " +
                      panplay::GameCatalog::instance().meta().path + "，耗时 " +
                      std::to_string(panplay::ms_since(catalog_start)) + " ms，" + panplay::memory_summary());
    PANPLAY_DEBUG("启动资料库同步线程");
    panplay::CatalogSync::instance().start(7, 24);
    PANPLAY_DEBUG("读取商店资料缓存");
    store_meta = new panplay::StoreMetaCache(panplay::app_data_dir());
    PANPLAY_DEBUG("启动商店资料线程");
    store_meta->start();
  }

  std::string net_error;
  if (!panplay::net_startup(&net_error)) {
    panplay::log_error("网络初始化失败: " + net_error);
    return 1;
  }
  PANPLAY_DEBUG("监听端口 " + std::to_string(port));
  SOCKET listener = open_listener(port);
  if (listener == INVALID_SOCKET) {
    panplay::log_error("无法在端口 " + std::to_string(port) + " 监听，errno " + std::to_string(errno));
    return 1;
  }
  const std::string local_url = "http://127.0.0.1:" + std::to_string(port);
  const std::string other_url = lan_url();
  panplay::log_info("服务就绪：本机 " + local_url + "，局域网 " + (other_url.empty() ? std::string("（取不到地址）") : other_url));
  const std::string code = lan.require_code ? lan.code : "";
#if !defined(PANPLAY_PS5)
  // 给开着控制台窗口的人看的。PS5 上标准输出接在日志文件里，上面那行日志已经记了。
  std::cout << "盘游界面 " << local_url << "\n";
  if (!other_url.empty()) std::cout << "局域网里打开 " << other_url << "\n";
  if (!lan.enabled) std::cout << "局域网访问已关闭，可在设置里打开\n";
  if (!code.empty()) std::cout << "访问口令 " << code << "（" << access_code_path() << "）\n";
  std::cout << "按 Ctrl+C 结束" << std::endl;
#endif
  if (options.on_ready) options.on_ready(local_url, other_url, code);
  load_queue();
  start_review_sync(open_drive);
  start_size_scan(open_drive, library_changed);
  start_shelf_library_sync(open_drive, library_changed);
  start_drive_usage_sync(open_drive, installed_game_dirs);
  start_shelf_feedback_sync(open_drive);
  PANPLAY_DEBUG("安装队列恢复完，开始接受连接");
  while (true) {
    sockaddr_in peer{};
    panplay::socklen peer_length = sizeof(peer);
    SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (client == INVALID_SOCKET) {
      const int code = errno;
      if (code == EINTR || code == ECONNABORTED) continue;
      // 主机休眠唤醒后监听套接字会坏掉（实测 errno 163）。照 PS5 上 ftpsrv 的做法关掉重开，网络恢复前一直重试，不退出。
      panplay::log_warn("accept 失败 errno " + std::to_string(code) + "，3 秒后重新监听");
      closesocket(listener);
      for (int attempt = 1;; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        listener = open_listener(port);
        if (listener != INVALID_SOCKET) break;
        if (attempt % 20 == 1) panplay::log_warn("重新监听端口 " + std::to_string(port) + " 失败 errno " + std::to_string(errno) + "，继续重试");
      }
      const std::string address_now = lan_url();
      panplay::log_info("已重新监听：局域网 " + (address_now.empty() ? std::string("（取不到地址）") : address_now));
      continue;
    }
    const bool local_peer = peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
    panplay::set_socket_timeout(client, 30000);
    std::thread([client, local_peer] {
      try {
        handle_client(client, local_peer);
      } catch (const std::exception& e) {
        panplay::log_error(std::string("处理请求时出错: ") + e.what());
        try {
          reply_json(client, 500, json_error("服务器内部错误"));
        } catch (...) {
        }
      } catch (...) {
        try {
          reply_json(client, 500, json_error("服务器内部错误"));
        } catch (...) {
        }
      }
      closesocket(client);
    }).detach();
  }
  return 0;
}
