#pragma once

#include <filesystem>

namespace panplay {

// 可写的数据目录：设置、登录会话、同步下来的资料库、商店资料缓存和封面都放在这里。
// 环境变量 PANPLAY_DATA_DIR 优先；否则 Windows 用 %APPDATA%\panplay，PS5 用 /data/panplay，
// 其他系统用 $XDG_DATA_HOME/panplay 或 ~/.local/share/panplay。目录不存在时会创建。
std::filesystem::path app_data_dir();

// 可执行文件所在目录，用来找随程序发布的界面和资料库；取不到时为空。
std::filesystem::path executable_dir();

}  // namespace panplay
