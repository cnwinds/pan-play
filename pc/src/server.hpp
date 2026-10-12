#pragma once

#include <functional>
#include <string>

struct ServerOptions {
  int port = 8734;
  // 监听所有网卡，要访问口令。PS5 上默认打开，好让手机打开界面。
  bool lan = false;
  // 开始监听后调用，参数是界面地址和访问口令（本机模式口令为空）。
  std::function<void(const std::string& url, const std::string& code)> on_ready;
};

int run_server(const ServerOptions& options);
