#include "panplay/library.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace panplay {
namespace {

std::string lower_copy(std::string value) {
  for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool parse_z_volume(const std::string& lower, std::string* stem, int* order) {
  if (lower.size() < 5) return false;
  const auto dot = lower.rfind('.');
  if (dot == std::string::npos || lower.size() - dot != 4) return false;
  if (lower[dot + 1] != 'z' || !std::isdigit(static_cast<unsigned char>(lower[dot + 2])) ||
      !std::isdigit(static_cast<unsigned char>(lower[dot + 3]))) {
    return false;
  }
  *stem = lower.substr(0, dot);
  *order = (lower[dot + 2] - '0') * 10 + (lower[dot + 3] - '0');
  return true;
}

bool parse_zip_parts(const std::string& lower, std::string* stem, int* order) {
  const auto marker = lower.rfind(".zip.");
  if (marker == std::string::npos) return false;
  const std::string number = lower.substr(marker + 5);
  if (number.empty()) return false;
  for (char ch : number) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
  }
  *stem = lower.substr(0, marker);
  *order = std::stoi(number);
  return true;
}

bool parse_part_rar(const std::string& lower, std::string* stem, int* order) {
  if (!ends_with(lower, ".rar")) return false;
  const auto pos = lower.rfind(".part");
  if (pos == std::string::npos || pos + 5 >= lower.size() - 4) return false;
  const std::string digits = lower.substr(pos + 5, lower.size() - 4 - (pos + 5));
  if (digits.empty()) return false;
  for (char ch : digits) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
  }
  *stem = lower.substr(0, pos);
  *order = std::stoi(digits);
  return true;
}

bool parse_old_rar_volume(const std::string& lower, std::string* stem, int* order) {
  if (lower.size() < 5 || lower[lower.size() - 4] != '.' || lower[lower.size() - 3] != 'r') return false;
  if (!std::isdigit(static_cast<unsigned char>(lower[lower.size() - 2])) ||
      !std::isdigit(static_cast<unsigned char>(lower[lower.size() - 1]))) {
    return false;
  }
  *stem = lower.substr(0, lower.size() - 4);
  *order = (lower[lower.size() - 2] - '0') * 10 + (lower[lower.size() - 1] - '0') + 2;
  return true;
}

bool is_archive_name(const std::string& name) {
  const std::string lower = lower_copy(name);
  std::string stem;
  int order = 0;
  if (parse_z_volume(lower, &stem, &order) || parse_zip_parts(lower, &stem, &order)) return true;
  if (parse_part_rar(lower, &stem, &order) || parse_old_rar_volume(lower, &stem, &order)) return true;
  if (ends_with(lower, ".zip") || ends_with(lower, ".7z") || ends_with(lower, ".rar")) return true;
  return lower.find(".7z.") != std::string::npos;
}

std::string archive_format(const std::string& name) {
  const std::string lower = lower_copy(name);
  std::string stem;
  int order = 0;
  if (parse_zip_parts(lower, &stem, &order) || parse_z_volume(lower, &stem, &order) || ends_with(lower, ".zip")) {
    return "zip";
  }
  if (ends_with(lower, ".7z") || lower.find(".7z.") != std::string::npos) return "7z";
  return "rar";
}

}  // namespace

std::string join_storage_path(std::string prefix, const std::string& relative) {
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  std::string rest = relative;
  while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
  if (prefix.empty()) return "/" + rest;
  if (rest.empty()) return prefix;
  return prefix + "/" + rest;
}

std::string find_title_id(const std::string& name) {
  const std::string upper = [&] {
    std::string value = name;
    for (char& ch : value) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return value;
  }();
  for (std::size_t i = 0; i + 9 <= upper.size(); ++i) {
    const bool prefix = upper.compare(i, 4, "CUSA") == 0 || upper.compare(i, 4, "PPSA") == 0;
    if (!prefix) continue;
    bool digits = true;
    for (int k = 0; k < 5; ++k) {
      if (!std::isdigit(static_cast<unsigned char>(upper[i + 4 + k]))) digits = false;
    }
    if (digits) return upper.substr(i, 9);
  }
  return "";
}

std::vector<ArchiveSet> group_archives(const std::vector<DriveEntry>& files) {
  std::set<std::string> z_stems;
  std::set<std::string> old_rar_stems;
  for (const auto& file : files) {
    std::string stem;
    int order = 0;
    const std::string lower = lower_copy(file.name);
    if (parse_z_volume(lower, &stem, &order)) z_stems.insert(stem);
    if (parse_old_rar_volume(lower, &stem, &order)) old_rar_stems.insert(stem);
  }

  struct Item {
    DriveEntry entry;
    std::string key;
    int order = 0;
    std::string format;
  };
  std::vector<Item> items;
  int single = 0;
  for (const auto& file : files) {
    if (!is_archive_name(file.name)) continue;
    Item item;
    item.entry = file;
    item.format = archive_format(file.name);
    const std::string lower = lower_copy(file.name);
    std::string stem;
    int order = 0;
    if (parse_zip_parts(lower, &stem, &order)) {
      item.key = "zipnum:" + stem;
      item.order = order;
    } else if (parse_z_volume(lower, &stem, &order)) {
      item.key = "zvol:" + stem;
      item.order = order;
    } else if (ends_with(lower, ".zip")) {
      stem = lower.substr(0, lower.size() - 4);
      if (z_stems.count(stem) != 0) {
        item.key = "zvol:" + stem;
        item.order = 100000;
      } else {
        item.key = "single:" + std::to_string(single++);
        item.order = 0;
      }
    } else if (parse_part_rar(lower, &stem, &order)) {
      item.key = "rarpart:" + stem;
      item.order = order;
    } else if (parse_old_rar_volume(lower, &stem, &order)) {
      item.key = "rarold:" + stem;
      item.order = order;
    } else if (ends_with(lower, ".rar")) {
      stem = lower.substr(0, lower.size() - 4);
      if (old_rar_stems.count(stem) != 0) {
        item.key = "rarold:" + stem;
        item.order = 1;
      } else {
        item.key = "single:" + std::to_string(single++);
        item.order = 0;
      }
    } else {
      item.key = "other:" + std::to_string(single++);
      item.order = 0;
    }
    items.push_back(std::move(item));
  }

  std::map<std::string, std::vector<Item>> groups;
  for (auto& item : items) groups[item.key].push_back(std::move(item));
  std::vector<ArchiveSet> sets;
  for (auto& [key, group] : groups) {
    (void)key;
    std::sort(group.begin(), group.end(), [](const Item& left, const Item& right) {
      if (left.order != right.order) return left.order < right.order;
      return left.entry.name < right.entry.name;
    });
    ArchiveSet set;
    set.format = group.front().format;
    for (const auto& item : group) set.parts.push_back(item.entry);
    sets.push_back(std::move(set));
  }
  return sets;
}

std::vector<DriveEntry> collect_archives(DrivePlugin& drive, const std::string& dir, std::string* error) {
  std::vector<DriveEntry> files;
  const auto entries = drive.list(dir, error);
  if (error && !error->empty()) return {};
  for (const auto& entry : entries) {
    const std::string path = entry.path.empty() ? join_storage_path(dir, entry.name) : entry.path;
    if (entry.is_dir) {
      const auto nested = drive.list(path, error);
      if (error && !error->empty()) return {};
      for (auto nested_entry : nested) {
        if (nested_entry.is_dir || !is_archive_name(nested_entry.name)) continue;
        if (nested_entry.path.empty()) nested_entry.path = join_storage_path(path, nested_entry.name);
        files.push_back(std::move(nested_entry));
      }
    } else if (is_archive_name(entry.name)) {
      DriveEntry copy = entry;
      if (copy.path.empty()) copy.path = path;
      files.push_back(std::move(copy));
    }
  }
  return files;
}

std::vector<GameFolder> scan_library(DrivePlugin& drive, const std::string& root, std::string* error) {
  std::vector<GameFolder> games;
  const auto children = drive.list(root, error);
  if (error && !error->empty()) return {};
  for (const auto& child : children) {
    if (!child.is_dir) continue;
    GameFolder game;
    game.name = child.name;
    game.path = child.path.empty() ? join_storage_path(root, child.name) : child.path;
    game.title_id = find_title_id(child.name);
    std::string list_error;
    const auto files = collect_archives(drive, game.path, &list_error);
    if (!list_error.empty()) {
      if (error) *error = list_error;
      return {};
    }
    game.sets = group_archives(files);
    for (const auto& set : game.sets) {
      for (const auto& part : set.parts) game.archive_bytes += part.size;
    }
    games.push_back(std::move(game));
  }
  return games;
}

}  // namespace panplay
