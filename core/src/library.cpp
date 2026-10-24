#include "panplay/library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

// 直接切开的分卷：name.zip.001、name.7z.001 这样，marker 是 ".zip." 或 ".7z."。
bool parse_numbered_parts(const std::string& lower, const std::string& marker_text, std::string* stem, int* order) {
  const auto marker = lower.rfind(marker_text);
  if (marker == std::string::npos) return false;
  const std::string number = lower.substr(marker + marker_text.size());
  if (number.empty() || number.size() > 6) return false;
  for (char ch : number) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
  }
  *stem = lower.substr(0, marker);
  *order = std::stoi(number);
  return true;
}

bool parse_zip_parts(const std::string& lower, std::string* stem, int* order) {
  return parse_numbered_parts(lower, ".zip.", stem, order);
}

bool parse_7z_parts(const std::string& lower, std::string* stem, int* order) {
  return parse_numbered_parts(lower, ".7z.", stem, order);
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
  return parse_7z_parts(lower, &stem, &order);
}

std::string archive_format(const std::string& name) {
  const std::string lower = lower_copy(name);
  std::string stem;
  int order = 0;
  if (parse_zip_parts(lower, &stem, &order) || parse_z_volume(lower, &stem, &order) || ends_with(lower, ".zip")) {
    return "zip";
  }
  if (ends_with(lower, ".7z") || parse_7z_parts(lower, &stem, &order)) return "7z";
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

bool is_variant_dir(const std::string& name) {
  if (name.size() < 2 || name[0] != '@') return false;
  return std::all_of(name.begin() + 1, name.end(), [](char ch) {
    return std::islower(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch));
  });
}

std::string variant_format(const std::string& path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
  const auto slash = trimmed.rfind('/');
  const std::string name = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
  return is_variant_dir(name) ? name.substr(1) : "";
}

std::string game_dir_of(const std::string& path) {
  if (variant_format(path).empty()) return path;
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
  const auto slash = trimmed.rfind('/');
  if (slash == std::string::npos) return path;
  return slash == 0 ? "/" : trimmed.substr(0, slash);
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
    } else if (parse_7z_parts(lower, &stem, &order)) {
      item.key = "7znum:" + stem;
      item.order = order;
    } else {
      item.key = "single:" + std::to_string(single++);
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
      if (is_variant_dir(entry.name)) continue;
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

std::vector<GameVariant> list_variants(DrivePlugin& drive, const std::string& game_dir, std::string* error) {
  std::vector<GameVariant> variants;
  const auto entries = drive.list(game_dir, error);
  if (error && !error->empty()) return {};
  bool loose = false;
  for (const auto& entry : entries) {
    if (!entry.is_dir) {
      loose = loose || is_archive_name(entry.name);
      continue;
    }
    if (!is_variant_dir(entry.name)) continue;
    variants.push_back({entry.name.substr(1), entry.path.empty() ? join_storage_path(game_dir, entry.name) : entry.path});
  }
  const auto rank = [](const std::string& format) { return format == "exfat" ? 0 : format == "folder" ? 1 : 2; };
  std::sort(variants.begin(), variants.end(), [&](const GameVariant& left, const GameVariant& right) {
    if (rank(left.format) != rank(right.format)) return rank(left.format) < rank(right.format);
    return left.format < right.format;
  });
  if (!variants.empty() && loose) variants.push_back({"", game_dir});
  return variants;
}

std::string archive_sets_signature(const std::vector<ArchiveSet>& sets) {
  std::vector<std::string> parts;
  for (const auto& set : sets) {
    for (const auto& part : set.parts) parts.push_back(part.name + ":" + std::to_string(part.size));
  }
  std::sort(parts.begin(), parts.end());
  std::string signature;
  for (const auto& part : parts) {
    if (!signature.empty()) signature += "|";
    signature += part;
  }
  return signature;
}

std::string archive_set_id(const ArchiveSet& set) { return archive_sets_signature({set}); }

std::string content_fingerprint(const ArchiveSet& set) {
  std::vector<std::string> parts;
  bool all_hashed = !set.parts.empty();
  for (const auto& part : set.parts) {
    if (part.content_hash.empty()) all_hashed = false;
    parts.push_back(part.content_hash.empty() ? "size:" + std::to_string(part.size) : part.content_hash);
  }
  std::sort(parts.begin(), parts.end());
  // FNV-1a 64 位，算两遍（第二遍换初值）拼成 32 个十六进制字符，本机记评价用，撞车可以不管。
  const auto fnv = [&](std::uint64_t hash) {
    for (const auto& part : parts) {
      for (const unsigned char c : part + "\n") {
        hash ^= c;
        hash *= 1099511628211ull;
      }
    }
    return hash;
  };
  char text[40];
  std::snprintf(text, sizeof(text), "%c%016llx%016llx", all_hashed ? 'm' : 's',
                static_cast<unsigned long long>(fnv(14695981039346656037ull)),
                static_cast<unsigned long long>(fnv(0x84222325cbf29ce4ull)));
  return text;
}

std::string archive_folder(std::string game_dir, const std::string& archive_path) {
  while (game_dir.size() > 1 && game_dir.back() == '/') game_dir.pop_back();
  const auto slash = archive_path.rfind('/');
  if (slash == std::string::npos) return "";
  const std::string parent = archive_path.substr(0, slash);
  if (parent == game_dir || parent.rfind(game_dir + "/", 0) != 0) return "";
  return parent.substr(game_dir.size() + 1);
}

int natural_compare(const std::string& left, const std::string& right) {
  const auto digit = [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; };
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    if (digit(left[i]) && digit(right[j])) {
      while (i < left.size() && left[i] == '0') ++i;
      while (j < right.size() && right[j] == '0') ++j;
      std::size_t end_i = i;
      std::size_t end_j = j;
      while (end_i < left.size() && digit(left[end_i])) ++end_i;
      while (end_j < right.size() && digit(right[end_j])) ++end_j;
      if (end_i - i != end_j - j) return end_i - i < end_j - j ? -1 : 1;
      if (const int order = left.compare(i, end_i - i, right, j, end_j - j); order != 0) return order < 0 ? -1 : 1;
      i = end_i;
      j = end_j;
      continue;
    }
    const int a = std::tolower(static_cast<unsigned char>(left[i]));
    const int b = std::tolower(static_cast<unsigned char>(right[j]));
    if (a != b) return a < b ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < left.size()) return 1;
  if (j < right.size()) return -1;
  return 0;
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
