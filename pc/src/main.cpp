#include "panplay/app_paths.hpp"
#include "panplay/drive.hpp"
#include "panplay/dummy_ftp.hpp"
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

#include <windows.h>
#include <shellapi.h>

namespace {

std::vector<std::string> utf8_args() {
  std::vector<std::string> out;
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
  return out;
}

std::string session_path() { return (panplay::app_data_dir() / "baidu-session.json").u8string(); }

void print_usage() {
  std::cout << "盘游 Panplay\n"
            << "  panplay serve [端口]\n"
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

std::unique_ptr<panplay::DrivePlugin> open_baidu() {
  const AppSettings settings = load_settings();
  panplay::BaiduConfig config;
  config.app_key = settings.app_key;
  config.secret_key = settings.secret_key;
  config.session_path = session_path();
  return panplay::make_baidu_drive(std::move(config));
}

int login() {
  auto drive = open_baidu();
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
  auto drive = open_baidu();
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
  auto drive = open_baidu();
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
  auto drive = open_baidu();
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
  std::vector<std::string> args = utf8_args();
  std::vector<char*> utf8_argv;
  if (static_cast<int>(args.size()) == argc) {
    for (auto& arg : args) utf8_argv.push_back(arg.data());
    utf8_argv.push_back(nullptr);
    argv = utf8_argv.data();
  }
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const std::string command = argv[1];
  if (command == "serve") return run_server(argc >= 3 ? std::stoi(argv[2]) : 8734);
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
    auto drive = open_baidu();
    return panplay::run_scan_cli(*drive, argc >= 3 ? argv[2] : "/");
  }
  if (command == "install") {
    auto drive = open_baidu();
    return panplay::run_install_cli(*drive, argc, argv);
  }
  if (command == "read" && argc >= 5) return read_range(argv[2], argv[3], argv[4]);
  if (command == "store-meta" && argc >= 3) return panplay::run_store_meta_cli(argv[2]);
  print_usage();
  return 1;
}
