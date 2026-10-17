#pragma once

#include <string>

// 主机右上角弹一条文字通知。
void notify(const std::string& text);

// 游戏装好了。已经注册到主机的，发系统自带的「已安装」通知，系统自己配上游戏图标和名字，和正版游戏装完一样；
// 发不出去或没注册时，用 icon_path 的图配上「《name》已安装完成」。
void notify_game_installed(const std::string& title_id, bool registered, const std::string& name,
                           const std::string& icon_path);
