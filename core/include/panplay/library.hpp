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

// 同一款游戏的几种格式（exFAT 镜像、文件夹……）在游戏目录下各占一个 @格式 子目录（@exfat、@folder、@other），
// 每个子目录就是一个变体，安装、评价都按变体目录走。见 docs/adr/0007。
struct GameVariant {
  std::string format;  // 子目录名去掉 @；游戏目录里直接放着压缩包时为空
  std::string path;
};

std::string join_storage_path(std::string prefix, const std::string& relative);
std::string find_title_id(const std::string& name);
bool is_variant_dir(const std::string& name);
// 变体目录的格式（"@exfat" → "exfat"），不是变体目录时为空。
std::string variant_format(const std::string& path);
// 变体目录所在的游戏目录；本身就是游戏目录时原样返回。
std::string game_dir_of(const std::string& path);
std::vector<ArchiveSet> group_archives(const std::vector<DriveEntry>& files);
// 游戏目录和下一层子目录里的压缩包；@格式 子目录是别的变体，不算。
std::vector<DriveEntry> collect_archives(DrivePlugin& drive, const std::string& dir, std::string* error);
// 游戏目录下的各个变体，按 exfat、folder、其他的顺序。没分格式时为空；分了格式、游戏目录里又直接放着压缩包时，
// 游戏目录本身也算一个（format 为空），排在最后。
std::vector<GameVariant> list_variants(DrivePlugin& drive, const std::string& game_dir, std::string* error);
std::vector<GameFolder> scan_library(DrivePlugin& drive, const std::string& root, std::string* error);

// 一组压缩包的身份：各分卷的文件名和大小。安装记录、补丁选择都按它认。
std::string archive_set_id(const ArchiveSet& set);
// 多组压缩包合起来的身份，和每组的先后无关。
std::string archive_sets_signature(const std::vector<ArchiveSet>& sets);
// 一组压缩包的内容指纹：只看各分卷的内容校验值，不看文件名和目录。改名、挪目录、删了再转存回来都不变，
// 不同来源、不同打包的不一样。评价按它认。网盘没给校验值的分卷按大小算，开头写 s 表示没那么可靠，否则开头是 m。
std::string content_fingerprint(const ArchiveSet& set);
// 压缩包在游戏目录下的哪个子目录里，直接放在游戏目录里时为空。
std::string archive_folder(std::string game_dir, const std::string& archive_path);
// 不分大小写，连续数字按数值比，所以 "补丁2" 排在 "补丁10" 前面。返回负数、0、正数。
int natural_compare(const std::string& left, const std::string& right);

}  // namespace panplay
