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

}  // namespace panplay
