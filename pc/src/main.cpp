#include "panplay/app_paths.hpp"
#include "panplay/drive.hpp"
#include "panplay/dummy_ftp.hpp"
#include "panplay/http.hpp"
#include "panplay/install.hpp"
#include "panplay/json.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/sink.hpp"
#include "panplay/store_meta.hpp"

#include "server.hpp"
#include "settings.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

// panplay serve [端口] [--lan]
int serve(int argc, char** argv) {
  ServerOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--lan") {
      options.lan = true;
    } else {
      options.port = std::stoi(arg);
    }
  }
  return run_server(options);
}

void print_usage() {
  std::cout << "盘游 Panplay\n"
            << "  panplay serve [端口] [--lan]   --lan 让局域网里的手机也能打开，要访问口令\n"
            << "  panplay self-test\n"
            << "  panplay bench-prefetch\n"
            << "  panplay bench-download <网盘目录> [秒] [连接数] [块MB]\n"
            << "  panplay dummy-ftp <端口> <根目录>\n"
            << "  panplay login\n"
            << "  panplay ls [目录]\n"
            << "  panplay scan <游戏库目录>\n"
            << "  panplay install <游戏目录> --to <本机目录或 ftp://主机:端口>\n"
            << "  panplay read <fsid> <offset> <length>\n"
            << "  panplay store-meta <conceptId>\n"
            << "  panplay http-get <地址>\n"
            << "\n密钥在界面的设置页保存。开发时环境变量 PANPLAY_BAIDU_APP_KEY 和 PANPLAY_BAIDU_SECRET_KEY 仍可覆盖。\n";
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
  std::cout << "用百度 App、百度网盘或微信扫描:\n" << challenge.qrcode_url << "\n";
  if (!challenge.user_code.empty()) std::cout << "用户码: " << challenge.user_code << "\n";
  const int rounds = challenge.expires_in > 0 ? challenge.expires_in / challenge.interval_seconds : 60;
  for (int i = 0; i < rounds; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(challenge.interval_seconds));
    panplay::LoginPoll poll = drive->poll_login(challenge);
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
  // 载荷由 ELF 加载器启动，通常不带参数：直接开局域网界面。
  if (argc < 2) {
    ServerOptions options;
    options.lan = true;
    options.on_ready = [](const std::string& url, const std::string& code) {
      notify("盘游已启动 " + url + " 口令 " + code);
    };
    const int code = run_server(options);
    if (code != 0) notify("盘游没能启动：端口 " + std::to_string(options.port) + " 可能已被占用");
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
    return panplay::run_dummy_ftp(static_cast<std::uint16_t>(std::stoi(argv[2])), argv[3]);
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
