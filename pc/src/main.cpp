#include "panplay/app_paths.hpp"
#include "panplay/catalog.hpp"
#include "panplay/drive.hpp"
#include "panplay/dummy_ftp.hpp"
#include "panplay/http.hpp"
#include "panplay/install.hpp"
#include "panplay/json.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/sink.hpp"
#include "panplay/store_meta.hpp"
#include "panplay/trace.hpp"

#include "server.hpp"
#include "settings.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#endif

#if defined(PANPLAY_PS5)
#include "ps5_dashboard.hpp"
#include "ps5_thread.hpp"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

extern "C" int sceNotificationSend(int user_id, bool is_logged, const char* payload);
#endif

namespace {

std::vector<std::string> utf8_args() {
  std::vector<std::string> out;
#if defined(_WIN32)
  int count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) return out;
  for (int i = 0; i < count; ++i) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string arg(size > 0 ? size - 1 : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
    out.push_back(std::move(arg));
  }
  LocalFree(wide);
#endif
  return out;
}

#if defined(PANPLAY_PS5)
// 主机右上角弹一条通知。
void notify(const std::string& text) {
  constexpr int kSystemUser = 0xFE;
  const std::string payload =
      "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
      "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
      "\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
      "\"message\":{\"body\":\"" + panplay::json_escape(text) + "\"}}},"
      "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"localNotificationId\":\"73726\"}";
  sceNotificationSend(kSystemUser, true, payload.c_str());
}

// 装图标要加载系统库、调系统接口，在有的固件上会一直卡住。卡在服务线程里时，连新线程都开不了，
// 所以放在启动服务之前做，并在数据目录留个标记：上次没做完（卡住了）就跳过一次，盘游照常启动。
void ensure_dashboard_icon(int port) {
  const std::filesystem::path pending = panplay::app_data_dir() / "dashboard-icon.pending";
  std::error_code ec;
  if (std::filesystem::exists(pending, ec)) {
    std::filesystem::remove(pending, ec);
    panplay::trace("上次安装桌面图标没有做完，这次跳过");
    notify("上次安装盘游桌面图标时卡住了，这次跳过；再发送一次 elf 会重试");
    return;
  }
  std::ofstream(pending.string()) << "1";
  std::string error;
  const DashboardIcon result = install_dashboard_icon(port, &error);
  std::filesystem::remove(pending, ec);
  switch (result) {
    case DashboardIcon::unchanged:
      panplay::trace("桌面图标已在");
      break;
    case DashboardIcon::installed:
      panplay::trace("已安装桌面图标");
      notify("已在主机桌面添加「盘游」图标，点它就能在主机上打开盘游");
      break;
    case DashboardIcon::failed:
      panplay::trace("桌面图标没装上：" + error);
      notify("盘游桌面图标没装上：" + error);
      break;
  }
}

// 一启动就把输出改写到数据目录的 panplay.log。加载器（elfldr 等）把输出接到管道或套接字上，
// 对面不再读时写满就会卡住正在打印的线程。追加写：再次启动不会冲掉上次崩溃前的记录，超过 2 MB 时换新文件。
void redirect_output_to_log() {
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  const std::filesystem::path path = panplay::app_data_dir() / "panplay.log";
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > (2u << 20) && !ec) {
    std::filesystem::rename(path, panplay::app_data_dir() / "panplay.old.log", ec);
  }
  const int fd = open(path.string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  close(fd);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
}
#endif

#if defined(PANPLAY_PS5) || defined(_WIN32)
// 本机这个端口上已经开着盘游时，取它的局域网地址和口令（不要口令时为空）。
// 没开着，或者占着端口的不是盘游，返回 false。
bool running_instance(int port, std::string* lan_url, std::string* code) {
  panplay::HttpRequest request;
  request.url = "http://127.0.0.1:" + std::to_string(port) + "/api/access";
  request.timeout_ms = 3000;
  const panplay::HttpResponse response = panplay::http_send(request);
  if (response.status != 200) return false;
  const panplay::Json json = panplay::Json::parse(response.body, nullptr);
  if (json.type != panplay::Json::Type::object || json.get("require_code") == nullptr) return false;
  *lan_url = json.str("url");
  *code = json.boolean_or("require_code", false) ? json.str("code") : "";
  return true;
}
#endif

// 排查网络用：打印状态码、长度和跳转后的地址，不打印内容。
int http_get(const std::string& url) {
  panplay::HttpRequest request;
  request.url = url;
  const panplay::HttpResponse response = panplay::http_send(request);
  if (!response.error.empty()) {
    std::cerr << response.error << "\n";
    return 1;
  }
  std::cout << "HTTP " << response.status << "，" << response.body.size() << " 字节，" << response.final_url << "\n";
  return response.status >= 200 && response.status < 400 ? 0 : 1;
}

// panplay serve [端口] [--lan|--local]
int serve(int argc, char** argv) {
  ServerOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--lan") {
      options.lan = true;
    } else if (arg == "--local") {
      options.lan = false;
    } else {
      options.port = std::stoi(arg);
    }
  }
  return run_server(options);
}

void print_usage() {
  std::cout << "盘游 Panplay\n"
            << "  panplay serve [端口] [--lan|--local]   默认按设置（局域网可访问）；--local 这次只许本机打开\n"
            << "  panplay self-test\n"
            << "  panplay bench-prefetch\n"
            << "  panplay bench-download <网盘目录> [秒] [连接数] [块MB]\n"
            << "  panplay dummy-ftp <端口> <根目录> [--zftpd]\n"
            << "  panplay login\n"
            << "  panplay ls [目录]\n"
            << "  panplay scan <游戏库目录>\n"
            << "  panplay install <游戏目录> --to <本机目录或 ftp://主机:端口>\n"
            << "  panplay read <fsid> <offset> <length>\n"
            << "  panplay store-meta <conceptId>\n"
            << "  panplay http-get <地址>\n";
}

int self_test() {
  std::string error;
  panplay::Json json = panplay::Json::parse(R"({"name":"\u76d8\u6e38","n":2,"ok":true,"list":[{"fs_id":12}]})", &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  if (json.str("name") != "盘游" || json.num("n") != 2 || !json.boolean_or("ok", false)) {
    std::cerr << "json mismatch\n";
    return 1;
  }
  const panplay::Json* list = json.get("list");
  if (!list || list->array.size() != 1 || list->array[0].str("fs_id") != "12") {
    std::cerr << "json list mismatch\n";
    return 1;
  }
#if defined(PANPLAY_EMBED_CATALOG)
  panplay::GameCatalog& catalog = panplay::GameCatalog::instance();
  if (!catalog.load_builtin(&error) || catalog.size() < 1000 || !catalog.meta().builtin || !catalog.is_stale(7)) {
    std::cerr << "builtin catalog: " << error << " size=" << catalog.size() << "\n";
    return 1;
  }
#endif
  return 0;
}

std::unique_ptr<panplay::DrivePlugin> open_drive() {
  const AppSettings settings = load_settings();
  return panplay::make_drive(settings.drive, settings.current_drive().values, drive_session_path(settings.drive));
}

int login() {
  auto drive = open_drive();
  panplay::LoginChallenge challenge = drive->start_login();
  if (!challenge.error.empty()) {
    std::cerr << challenge.error << "\n";
    return 1;
  }
  std::cout << "在浏览器打开下面的二维码，用百度网盘 App 扫码:\n" << challenge.qrcode_url << "\n";
  if (!challenge.user_code.empty()) std::cout << "用户码: " << challenge.user_code << "\n";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(challenge.expires_in > 0 ? challenge.expires_in : 300);
  bool scanned = false;
  while (std::chrono::steady_clock::now() < deadline) {
    panplay::LoginPoll poll = drive->poll_login(challenge);
    if (poll.scanned && !scanned) {
      std::cout << "已扫码，请在手机上确认登录\n";
      scanned = true;
    }
    if (poll.done) {
      std::cout << "登录成功\n";
      return 0;
    }
    if (poll.expired) {
      std::cerr << poll.error << "\n";
      return 1;
    }
    if (!poll.error.empty()) {
      std::cerr << poll.error << "\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(std::max(1, poll.retry_after_seconds)));
  }
  std::cerr << "等待扫码超时\n";
  return 1;
}

int list_dir(const std::string& path) {
  auto drive = open_drive();
  std::string error;
  std::vector<panplay::DriveEntry> entries = drive->list(path, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  for (const auto& entry : entries) {
    std::cout << (entry.is_dir ? "d " : "f ") << entry.file_id << " " << entry.size << " " << entry.name << "\n";
  }
  return 0;
}

int read_range(const std::string& file_id, const std::string& offset_text, const std::string& length_text) {
  auto drive = open_drive();
  const auto offset = std::stoll(offset_text);
  const auto length = static_cast<std::size_t>(std::stoll(length_text));
  panplay::ReadResult result = drive->read_at(file_id, offset, length);
  if (!result.error.empty()) {
    std::cerr << result.error << "\n";
    return 1;
  }
  std::cout << "status " << result.status << " bytes " << result.bytes.size() << "\n";
  return 0;
}

int bench_prefetch_download(panplay::DrivePlugin& drive, const panplay::DriveEntry& file, int seconds, int connections,
                            int block_mb) {
  panplay::PrefetchOptions options;
  options.connections = connections;
  options.block_size = static_cast<std::size_t>(block_mb) << 20;
  options.max_blocks = std::max<std::size_t>(8, static_cast<std::size_t>(connections) * 2);
  panplay::PrefetchReader reader(drive, file.file_id, file.size, nullptr, options);
  std::vector<std::uint8_t> buffer(1 << 20);
  const auto start = std::chrono::steady_clock::now();
  std::int64_t total = 0;
  std::int64_t window = 0;
  auto window_start = start;
  std::string error;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
    const std::size_t got = reader.read(buffer.data(), buffer.size(), &error);
    if (got == 0) break;
    total += static_cast<std::int64_t>(got);
    window += static_cast<std::int64_t>(got);
    const auto now = std::chrono::steady_clock::now();
    const double span = std::chrono::duration<double>(now - window_start).count();
    if (span >= 2.0) {
      std::cout << "  " << static_cast<double>(window) / span / (1 << 20) << " MB/s" << std::endl;
      window = 0;
      window_start = now;
    }
  }
  const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!error.empty()) std::cerr << error << "\n";
  std::cout << connections << " 条连接，块 " << block_mb << " MB：共 " << (total >> 20) << " MB，" << spent << " 秒，平均 "
            << static_cast<double>(total) / spent / (1 << 20) << " MB/s" << std::endl;
  return 0;
}

int bench_download(const std::string& dir, int seconds, int connections, int block_mb) {
  auto drive = open_drive();
  std::string error;
  const auto entries = drive->list(dir, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  const panplay::DriveEntry* biggest = nullptr;
  for (const auto& entry : entries) {
    if (!entry.is_dir && (!biggest || entry.size > biggest->size)) biggest = &entry;
  }
  if (!biggest) {
    std::cerr << "目录里没有文件\n";
    return 1;
  }
  std::cout << biggest->name << "，" << (biggest->size >> 20) << " MB" << std::endl;
  if (connections > 0) return bench_prefetch_download(*drive, *biggest, seconds, connections, block_mb);
  const auto opened_at = std::chrono::steady_clock::now();
  auto stream = drive->open_stream(biggest->file_id, 0, &error);
  if (!stream) {
    std::cerr << error << "\n";
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  std::cout << "打开连接 " << std::chrono::duration<double>(start - opened_at).count() << " 秒" << std::endl;
  std::vector<std::uint8_t> buffer(1 << 20);
  std::int64_t total = 0;
  std::int64_t window = 0;
  auto window_start = start;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
    const std::size_t got = stream->read(buffer.data(), buffer.size(), &error);
    if (got == 0) break;
    total += static_cast<std::int64_t>(got);
    window += static_cast<std::int64_t>(got);
    const auto now = std::chrono::steady_clock::now();
    const double span = std::chrono::duration<double>(now - window_start).count();
    if (span >= 2.0) {
      std::cout << "  " << static_cast<double>(window) / span / (1 << 20) << " MB/s" << std::endl;
      window = 0;
      window_start = now;
    }
  }
  const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!error.empty()) std::cerr << error << "\n";
  std::cout << "共 " << (total >> 20) << " MB，" << spent << " 秒，平均 " << static_cast<double>(total) / spent / (1 << 20)
            << " MB/s" << std::endl;
  stream.reset();
  for (int i = 0; i < 6; ++i) {
    const auto asked = std::chrono::steady_clock::now();
    const auto result = drive->read_at(biggest->file_id, (biggest->size / 7) * (i + 1), 64 << 10);
    std::cout << "随机读 64 KB：" << std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count() << " 秒"
              << (result.error.empty() ? "" : " " + result.error) << std::endl;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // 输出都是 UTF-8；中文系统的控制台默认按 GBK 显示，不切过来就是乱码。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#else
  // 对方先关了连接时 send 会触发 SIGPIPE，默认会结束进程。
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::vector<std::string> args = utf8_args();
  std::vector<char*> utf8_argv;
  if (static_cast<int>(args.size()) == argc) {
    for (auto& arg : args) utf8_argv.push_back(arg.data());
    utf8_argv.push_back(nullptr);
    argv = utf8_argv.data();
  }
#if defined(PANPLAY_PS5)
  redirect_output_to_log();
  panplay::enable_trace();
  panplay::install_crash_trace();
  panplay::trace("==== 盘游启动 pid " + std::to_string(getpid()) + "，参数 " + std::to_string(argc) + " 个，编译于 " +
                 __DATE__ + " " + __TIME__ + "，" + panplay::memory_summary());
  panplay::trace("线程栈：系统默认 " + std::to_string(ps5_default_thread_stack() >> 10) + " KB，盘游改用 " +
                 std::to_string(kPs5ThreadStack >> 10) + " KB");
  panplay::start_trace_heartbeat();
  // 载荷由 ELF 加载器启动，通常不带参数：直接开局域网界面。
  if (argc < 2) {
    ServerOptions options;
    options.lan_required = true;
    // 已经在运行时也检查一遍：图标被删了，重新发送一次载荷就能装回来。
    ensure_dashboard_icon(options.port);
    std::string running_url;
    std::string running_code;
    panplay::trace("检查端口 " + std::to_string(options.port) + " 上是否已有盘游");
    if (running_instance(options.port, &running_url, &running_code)) {
      panplay::trace("已有盘游在运行 " + running_url + "，本次退出");
      notify("盘游已经在运行 " + running_url + (running_code.empty() ? "" : " 口令 " + running_code));
      // 走正常退出时，系统库在析构全局对象时抛 resource deadlock would occur 并中止进程，这里什么都不用清理。
      std::fflush(nullptr);
      std::_Exit(0);
    }
    panplay::trace("没有已在运行的盘游，开始启动服务");
    options.on_ready = [](const std::string&, const std::string& url, const std::string& code) {
      panplay::trace("服务就绪，弹出启动通知 " + url);
      notify("盘游已启动 " + url + (code.empty() ? "" : " 口令 " + code));
    };
    options.on_quit = [] {
      panplay::trace("用户在设置页退出盘游");
      notify("盘游已退出");
    };
    const int code = run_server(options);
    panplay::trace("run_server 返回 " + std::to_string(code));
    if (code != 0) notify("盘游没能启动：端口 " + std::to_string(options.port) + " 可能已被占用");
    return code;
  }
#else
  if (std::getenv("PANPLAY_TRACE") != nullptr) {
    panplay::enable_trace();
    panplay::install_crash_trace();
  }
#endif
#if defined(_WIN32)
  // 双击 exe 时不带参数：直接开本机界面，并用默认浏览器打开。窗口一关程序就退出，所以出错时停住让人看到原因。
  if (argc < 2) {
    ServerOptions options;
    std::string running_url;
    std::string running_code;
    if (running_instance(options.port, &running_url, &running_code)) {
      const std::string local_url = "http://127.0.0.1:" + std::to_string(options.port);
      std::cout << "盘游已经在运行，已打开 " << local_url << std::endl;
      ShellExecuteA(nullptr, "open", local_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return 0;
    }
    options.on_ready = [](const std::string& url, const std::string&, const std::string&) {
      ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    const int code = run_server(options);
    if (code != 0) {
      std::cout << "端口 " << options.port << " 可能已被占用，盘游是不是已经开着了？按回车关闭" << std::endl;
      std::cin.get();
    }
    return code;
  }
#endif
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const std::string command = argv[1];
  if (command == "serve") return serve(argc, argv);
  if (command == "bench-prefetch") return panplay::run_prefetch_bench();
  if (command == "bench-download" && argc >= 3) {
    return bench_download(argv[2], argc >= 4 ? std::stoi(argv[3]) : 20, argc >= 5 ? std::stoi(argv[4]) : 0,
                          argc >= 6 ? std::stoi(argv[5]) : 4);
  }
  if (command == "self-test") {
    if (const int json = self_test()) return json;
    if (const int store = panplay::run_store_meta_self_test()) return store;
    if (const int sink = panplay::run_install_sink_self_test()) return sink;
    return panplay::run_install_pipeline_self_test();
  }
  if (command == "dummy-ftp" && argc >= 4) {
    const bool zftpd = argc >= 5 && std::string(argv[4]) == "--zftpd";
    return panplay::run_dummy_ftp(static_cast<std::uint16_t>(std::stoi(argv[2])), argv[3], zftpd);
  }
  if (command == "login") return login();
  if (command == "ls") return list_dir(argc >= 3 ? argv[2] : "/");
  if (command == "scan") {
    auto drive = open_drive();
    return panplay::run_scan_cli(*drive, argc >= 3 ? argv[2] : "/");
  }
  if (command == "install") {
    auto drive = open_drive();
    return panplay::run_install_cli(*drive, argc, argv);
  }
  if (command == "read" && argc >= 5) return read_range(argv[2], argv[3], argv[4]);
  if (command == "store-meta" && argc >= 3) return panplay::run_store_meta_cli(argv[2]);
  if (command == "http-get" && argc >= 3) return http_get(argv[2]);
  print_usage();
  return 1;
}
