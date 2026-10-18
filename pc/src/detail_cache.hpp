#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 详情页里要连网盘、连主机才知道的部分。打开详情时先显示这份，同时实时去读，读到的不一样再更新。
// 存在数据目录里，程序重开后还在；key 是网盘、游戏库目录和存放位置，换了设置就全部作废。
struct GameDetail {
  std::string state;
  std::int64_t archive_bytes = 0;
  std::string formats;
  // 各压缩包组的 JSON 数组原文，详情页直接用。
  std::string layers;
  // 已装好的游戏叠了哪些压缩包组（archive_set_id）。
  std::vector<std::string> installed;
  // 主游戏那组压缩包的内容指纹（content_fingerprint），评价按它认。
  std::string fingerprint;

  bool operator==(const GameDetail& other) const {
    return state == other.state && archive_bytes == other.archive_bytes && formats == other.formats &&
           layers == other.layers && installed == other.installed && fingerprint == other.fingerprint;
  }
};

std::optional<GameDetail> cached_game_detail(const std::string& key, const std::string& path);
// 只取主游戏的内容指纹，游戏库列表每个格子都要查一次，不复制整份详情。没读过时为空。
std::string cached_game_fingerprint(const std::string& key, const std::string& path);
// 内容变了才写盘，返回有没有变。
bool store_game_detail(const std::string& key, const std::string& path, const GameDetail& detail);
// 装完、删除之后主机上的情况变了，下次打开详情以实时读到的为准。
void forget_game_detail(const std::string& path);
