#pragma once

#include "panplay/drive.hpp"
#include "panplay/library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 游戏库格子上显示的容量。列表只列游戏库一级目录、看不到压缩包，所以由后台线程逐个游戏目录量好存起来
// （数据目录 game-sizes.json），详情页实时读到的也写进来。key 是网盘和游戏库目录，换了就全部重新量。
struct GameSize {
  // 直接放在这个目录（和它下一层非 @格式 子目录）里的压缩包一共多大。
  std::int64_t bytes = 0;
  // 分了格式的游戏目录：各个变体，顺序同 list_variants；变体自己的容量按变体目录另记一项。
  std::vector<panplay::GameVariant> variants;
};

// on_change 在量到新的容量后调用（连着量时十秒一次），页面据此重取游戏库。
void start_size_scan(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive, std::function<void()> on_change);
// 列表重新列过网盘目录后调用：没量过的、量过超过一天的游戏目录在后台排队去量，已经不在网盘上的记录删掉。
void want_game_sizes(const std::string& key, const std::vector<std::string>& game_dirs);
// 没量过时为空。
std::optional<GameSize> cached_game_size(const std::string& key, const std::string& path);
// 详情页实时读到的。返回容量有没有变。
bool store_game_size(const std::string& key, const std::string& path, const GameSize& size);
