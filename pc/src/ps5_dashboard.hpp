#pragma once

#include <string>

enum class DashboardIcon { unchanged, installed, failed };

// 在主机桌面装一个「盘游」图标，点开用系统浏览器打开本机这个端口上的盘游界面。
// 已经装好、内容也没变时什么都不做；被删掉或内容变了就重装。失败时原因写进 *error。
DashboardIcon install_dashboard_icon(int port, std::string* error);
