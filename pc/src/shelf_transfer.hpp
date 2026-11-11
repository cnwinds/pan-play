#pragma once

#include "settings.hpp"

#include "panplay/drive.hpp"

#include <functional>
#include <set>
#include <string>

struct PrepareResult {
  std::string error;
  // 链接失效这类再试也没用的为 false。
  bool retryable = true;
  // 网络、限流这类过一会儿自己就好的，交给安装队列的自动续装。
  bool auto_resume = false;
  // 这次转存了：游戏库目录变了，列表要重列。
  bool transferred = false;
};

// 安装前调用（ADR 0011）：path 这个变体网盘里还没有、盘架资源库里有时，先核对分享链接，网盘空间不够就把能再转回来的
// 游戏移出网盘，然后转存进来。网盘里已经有了什么都不做。busy 是本机安装队列里的变体目录，不会被移出。
// sizes_key 是容量缓存的 key；say 报告现在在做什么，显示在安装进度上。
// 自测：资源库解析，用内存里的假网盘跑转存和腾空间。返回 0 表示通过。
int run_shelf_self_test();

PrepareResult prepare_on_drive(panplay::DrivePlugin& drive, const AppSettings& settings, const std::string& sizes_key,
                               const std::string& path, const std::set<std::string>& busy, panplay::CancelToken& cancel,
                               const std::function<void(const std::string&)>& say);
