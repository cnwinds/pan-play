#pragma once

#include <string>

enum class DashboardIcon { unchanged, installed, failed };

// 桌面图标那个只打开网页的应用。主机浏览器里开着的盘游页面在系统看来就是它在运行。
extern const char* const kDashboardTitleId;

// 在主机桌面装一个「盘游」图标，点开用系统浏览器打开本机这个端口上的盘游界面。
// 已经装好、内容也没变时什么都不做；被删掉或内容变了就重装。失败时原因写进 *error。
DashboardIcon install_dashboard_icon(int port, std::string* error);
