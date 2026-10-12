#include "panplay/drive.hpp"
#include "panplay/dummy_ftp.hpp"
#include "panplay/install.hpp"
#include "panplay/json.hpp"
#include "panplay/sink.hpp"

#include "server.hpp"
#include "settings.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::string session_path() {
  const std::string root = env_or_empty("APPDATA");
  std::filesystem::path path = root.empty() ? std::filesystem::path("session") : std::filesystem::path(root) / "panplay";
  std::filesystem::create_directories(path);
  return (path / "baidu-session.json").string();
}

void print_usage() {
  std::cout << "盘游 Panplay\n"
            << "  panplay serve [端口]\n"
            << "  panplay self-test\n"
            << "  panplay dummy-ftp <端口> <根目录>\n"
            << "  panplay login\n"
            << "  panplay ls [目录]\n"
            << "  panplay scan <游戏库目录>\n"
            << "  panplay install <游戏目录> --to <本机目录或 ftp://主机:端口>\n"
            << "  panplay read <fsid> <offset> <length>\n"
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const std::string command = argv[1];
  if (command == "serve") return run_server(argc >= 3 ? std::stoi(argv[2]) : 8734);
  if (command == "self-test") {
    if (const int json = self_test()) return json;
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
  print_usage();
  return 1;
}
