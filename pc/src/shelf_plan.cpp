#include "shelf_plan.hpp"

#include "panplay/library.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace {

constexpr int kMaxDepth = 3;

std::string lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

struct Found {
  panplay::DriveEntry entry;
  // 相对 variant_dir 的路径。
  std::string rel;
};

void collect(panplay::DrivePlugin& drive, const std::string& dir, const std::string& rel, int depth,
             std::vector<Found>* out, std::string* error) {
  const auto entries = drive.list(dir, error);
  if (!error->empty()) return;
  for (const auto& entry : entries) {
    const std::string path = entry.path.empty() ? panplay::join_storage_path(dir, entry.name) : entry.path;
    const std::string child = rel.empty() ? entry.name : rel + "/" + entry.name;
    if (entry.is_dir) {
      if (depth < kMaxDepth) collect(drive, path, child, depth + 1, out, error);
      if (!error->empty()) return;
      continue;
    }
    panplay::DriveEntry copy = entry;
    copy.path = path;
    out->push_back({std::move(copy), child});
  }
}

}  // namespace

ResolvedPlan resolve_plan(panplay::DrivePlugin& drive, const std::string& variant_dir, const ShelfPlan& plan,
                          std::string* error) {
  ResolvedPlan resolved;
  std::vector<Found> files;
  collect(drive, variant_dir, "", 0, &files, error);
  if (!error->empty()) return resolved;
  std::set<std::size_t> used;
  const auto match = [&](const ShelfPlanFile& want) -> const panplay::DriveEntry* {
    const auto take = [&](auto&& accept) -> const panplay::DriveEntry* {
      for (std::size_t i = 0; i < files.size(); ++i) {
        if (used.count(i) == 0 && files[i].entry.size == want.size && accept(files[i])) {
          used.insert(i);
          return &files[i].entry;
        }
      }
      return nullptr;
    };
    if (!want.md5.empty()) {
      const std::string md5 = lower(want.md5);
      if (auto found = take([&](const Found& file) { return lower(file.entry.content_hash) == md5; })) return found;
    }
    return take([&](const Found& file) { return file.rel == want.path; });
  };

  for (const auto& layer : plan.layers) {
    std::vector<panplay::DriveEntry> parts;
    for (const auto& file : layer.files) {
      if (const auto* found = match(file)) parts.push_back(*found);
      else resolved.missing.push_back(file.path);
    }
    if (parts.size() != layer.files.size()) continue;
    // 卷序和格式按盘游自己的规则认，一层只能是一组。
    const auto sets = panplay::group_archives(parts);
    if (sets.size() != 1 || sets.front().parts.size() != parts.size()) {
      resolved.missing.push_back(layer.name + "（分卷认不成一组）");
      continue;
    }
    resolved.plan.layers.push_back(sets.front());
  }
  for (const auto& copy : plan.copies) {
    if (const auto* found = match(copy.from)) resolved.plan.copies.push_back({*found, copy.to});
    else resolved.missing.push_back(copy.from.path);
  }
  for (const auto& package : plan.packages) {
    if (const auto* found = match(package)) resolved.plan.packages.push_back(*found);
    else resolved.missing.push_back(package.path);
  }
  return resolved;
}

std::vector<panplay::ArchiveSet> plan_archive_sets(const std::string& variant_dir, const ShelfPlan& plan) {
  std::vector<panplay::ArchiveSet> layers;
  for (const auto& layer : plan.layers) {
    std::vector<panplay::DriveEntry> parts;
    for (const auto& file : layer.files) {
      panplay::DriveEntry entry;
      entry.path = panplay::join_storage_path(variant_dir, file.path);
      entry.name = file.path.substr(file.path.find_last_of('/') + 1);
      entry.size = file.size;
      entry.content_hash = lower(file.md5);
      parts.push_back(std::move(entry));
    }
    const auto sets = panplay::group_archives(parts);
    if (sets.size() == 1 && sets.front().parts.size() == parts.size()) layers.push_back(sets.front());
  }
  return layers;
}

std::vector<panplay::ArchiveSet> share_archive_sets(const std::string& variant_dir,
                                                    const std::vector<panplay::ShareFile>& files) {
  if (files.empty()) return {};
  std::string common = files.front().path.substr(0, files.front().path.find_last_of('/') + 1);
  for (const auto& file : files) {
    while (!common.empty() && file.path.compare(0, common.size(), common) != 0) {
      common.pop_back();
      common.erase(common.find_last_of('/') + 1);
    }
  }
  std::vector<panplay::DriveEntry> entries;
  for (const auto& file : files) {
    const std::string rel = file.path.substr(common.size());
    const auto slash = rel.find('/');
    if (slash != std::string::npos && (rel.find('/', slash + 1) != std::string::npos || panplay::is_variant_dir(rel.substr(0, slash)))) {
      continue;
    }
    panplay::DriveEntry entry;
    entry.path = panplay::join_storage_path(variant_dir, rel);
    entry.name = rel.substr(rel.find_last_of('/') + 1);
    entry.size = file.size;
    entry.content_hash = file.content_hash;
    entries.push_back(std::move(entry));
  }
  return panplay::group_archives(entries);
}
