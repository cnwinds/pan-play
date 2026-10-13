#pragma once

#include "panplay/drive.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace panplay {

struct ArchiveSet {
  std::string format;
  std::vector<DriveEntry> parts;
};

struct GameFolder {
  std::string name;
  std::string path;
  std::string title_id;
  std::int64_t archive_bytes = 0;
  std::vector<ArchiveSet> sets;
};

std::string join_storage_path(std::string prefix, const std::string& relative);
std::string find_title_id(const std::string& name);
std::vector<ArchiveSet> group_archives(const std::vector<DriveEntry>& files);
std::vector<DriveEntry> collect_archives(DrivePlugin& drive, const std::string& dir, std::string* error);
std::vector<GameFolder> scan_library(DrivePlugin& drive, const std::string& root, std::string* error);

// 一组压缩包的身份：各分卷的文件名和大小。安装记录、补丁选择都按它认。
std::string archive_set_id(const ArchiveSet& set);
// 多组压缩包合起来的身份，和每组的先后无关。
std::string archive_sets_signature(const std::vector<ArchiveSet>& sets);
// 压缩包在游戏目录下的哪个子目录里，直接放在游戏目录里时为空。
std::string archive_folder(std::string game_dir, const std::string& archive_path);
// 不分大小写，连续数字按数值比，所以 "补丁2" 排在 "补丁10" 前面。返回负数、0、正数。
int natural_compare(const std::string& left, const std::string& right);

}  // namespace panplay
