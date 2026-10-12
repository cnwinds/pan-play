#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace panplay {

struct CatalogEntry {
  std::string name;
  std::string cover_url;
  std::string region;
  std::string publisher_id;
  std::string platform;
  std::int64_t concept_id = 0;
};

struct CatalogMatch {
  CatalogEntry entry;
  std::string match_by;
  bool found = false;
};

struct CatalogMeta {
  std::string path;
  std::string source;
  std::string generated_at;
  std::string generated_from;
  int version = 0;
  std::int64_t file_mtime_unix = 0;
};

class GameCatalog {
 public:
  static GameCatalog& instance();
  // 路径都是 UTF-8。自动同步写在数据目录（随程序发布的目录在 PS5 上可能只读），读取时取较新的那份。
  static std::string default_path();
  static std::string synced_path();

  bool load_default(std::string* error = nullptr);
  bool load_file(const std::string& path, std::string* error = nullptr);
  bool reload(std::string* error = nullptr);
  CatalogMatch lookup(const std::string& title_id, const std::string& folder_name) const;
  CatalogMeta meta() const;
  bool is_stale(int max_age_days = 30) const;
  bool loaded() const;
  std::size_t size() const;

 private:
  GameCatalog() = default;

  bool load_file_unlocked(const std::string& path, std::string* error);

  mutable std::shared_mutex mu_;
  bool loaded_ = false;
  CatalogMeta meta_;
  std::unordered_map<std::string, CatalogEntry> by_title_id_;
};

}  // namespace panplay
