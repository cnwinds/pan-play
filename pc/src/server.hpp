#pragma once

#include <functional>
#include <optional>
#include <string>

struct ServerOptions {
  int port = 8734;
  // 这次启动是否允许局域网访问。不设时按设置（默认允许），设置页可以随时改。
  std::optional<bool> lan;
  // PS5 上界面只能从别的设备打开，不让关掉局域网访问，免得把自己关在外面。
  bool lan_required = false;
  // 开始监听后调用：本机地址、局域网地址（不允许局域网访问或找不到网卡时为空）、访问口令（不要口令时为空）。
  std::function<void(const std::string& local_url, const std::string& lan_url, const std::string& code)> on_ready;
};

int run_server(const ServerOptions& options);
