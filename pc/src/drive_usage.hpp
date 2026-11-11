#pragma once

#include "panplay/drive.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

// 在用记录（ADR 0011）：每台设备在网盘 /盘游/在用/<设备 ID>.json 记着正在装哪些、主机上装着哪些、最近用过哪些。
// 盘游腾网盘空间之前读一遍所有设备的：别的设备正在装的不删，已经装在主机上的先退。
struct DriveUsage {
  // 正在装的变体目录（心跳 15 分钟内的）。
  std::set<std::string> installing;
  // 装在某台设备主机上的游戏目录。
  std::set<std::string> installed;
  // 变体目录最近一次转存或开始安装的时间（各设备取最新）。
  std::map<std::string, std::int64_t> used;
};

// installed_dirs 返回本机主机上装着的游戏目录，上传时调用。
void start_drive_usage_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive,
                            std::function<std::set<std::string>()> installed_dirs);
void usage_install_started(const std::string& path);
void usage_install_stopped(const std::string& path);
// 转存或开始安装时调用。
void usage_used(const std::string& path);
// 重新读所有设备的在用记录，和本机的合在一起。别的设备的读不到时 error 写原因，返回本机的部分。
DriveUsage read_drive_usage(panplay::DrivePlugin& drive, std::string* error);
