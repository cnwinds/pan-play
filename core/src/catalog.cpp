#include "panplay/catalog.hpp"

#include "panplay/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "panplay/app_paths.hpp"

#if defined(PANPLAY_EMBED_CATALOG)
#include <zlib.h>

// 构建时生成：data/panplay-catalog.json 的 gzip 数据 kCatalogGz。
#include "panplay_catalog_gz.inc"
#endif

namespace panplay {
namespace {

#if defined(PANPLAY_EMBED_CATALOG)
bool gunzip(const unsigned char* data, std::size_t size, std::string* out) {
  z_stream stream{};
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  std::vector<char> buffer(1 << 16);
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    status = inflate(&stream, Z_NO_FLUSH);
    if (status == Z_OK || status == Z_STREAM_END) out->append(buffer.data(), buffer.size() - stream.avail_out);
  }
  inflateEnd(&stream);
  return status == Z_STREAM_END;
}
#endif

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string short_title_id(const std::string& value) {
  if (value.size() >= 9) {
    const std::string head = value.substr(0, 9);
    const bool prefix = head.compare(0, 4, "CUSA") == 0 || head.compare(0, 4, "PPSA") == 0;
    if (prefix) {
      bool digits = true;
      for (int i = 4; i < 9; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(head[i]))) digits = false;
      }
      if (digits) return head;
    }
  }
  return value;
}

std::string platform_of(const std::string& title_id) {
  if (title_id.size() >= 4 && title_id.compare(0, 4, "PPSA") == 0) return "ps5";
  if (title_id.size() >= 4 && title_id.compare(0, 4, "CUSA") == 0) return "ps4";
  return "";
}

CatalogEntry parse_entry(const Json& node, const std::string& title_id) {
  CatalogEntry entry;
  entry.name = node.str("name");
  if (entry.name.empty()) entry.name = node.str("n");
  entry.cover_url = node.str("cover_url");
  if (entry.cover_url.empty()) entry.cover_url = node.str("u");
  entry.region = node.str("region");
  if (entry.region.empty()) entry.region = node.str("r");
  entry.publisher_id = node.str("publisher_id");
  if (entry.publisher_id.empty()) entry.publisher_id = node.str("p");
  entry.platform = node.str("platform");
  if (entry.platform.empty()) entry.platform = node.str("x");
  if (entry.platform.empty()) entry.platform = platform_of(title_id);
  entry.concept_id = static_cast<std::int64_t>(node.num("concept_id"));
  if (entry.concept_id == 0) entry.concept_id = static_cast<std::int64_t>(node.num("c"));
  return entry;
}

std::filesystem::path bundled_catalog_path() {
  const std::filesystem::path relative = std::filesystem::path("data") / "panplay-catalog.json";
  std::vector<std::filesystem::path> roots;
  const std::filesystem::path exe = executable_dir();
  if (!exe.empty()) roots.insert(roots.end(), {exe, exe / "..", exe / ".." / ".."});
  roots.insert(roots.end(), {".", "..", std::filesystem::path("..") / ".."});
  for (const auto& root : roots) {
    std::error_code ec;
    if (std::filesystem::exists(root / relative, ec)) return root / relative;
  }
  return {};
}

}  // namespace

GameCatalog& GameCatalog::instance() {
  static GameCatalog catalog;
  return catalog;
}

std::string GameCatalog::synced_path() { return (app_data_dir() / "panplay-catalog.json").u8string(); }

std::string GameCatalog::default_path() {
  const std::string synced = synced_path();
  const std::filesystem::path bundled = bundled_catalog_path();
  if (bundled.empty()) return synced;
  std::error_code synced_ec;
  std::error_code bundled_ec;
  const auto synced_time = std::filesystem::last_write_time(std::filesystem::u8path(synced), synced_ec);
  const auto bundled_time = std::filesystem::last_write_time(bundled, bundled_ec);
  if (!synced_ec && (bundled_ec || synced_time >= bundled_time)) return synced;
  return bundled.u8string();
}

bool GameCatalog::load_default(std::string* error) {
  if (load_file(default_path(), error)) return true;
  return load_builtin(nullptr);
}

bool GameCatalog::reload(std::string* error) {
  std::shared_lock<std::shared_mutex> read_lock(mu_);
  const bool fallback = meta_.path.empty() || meta_.builtin;
  const std::string path = meta_.path;
  read_lock.unlock();
  return fallback ? load_default(error) : load_file(path, error);
}

bool GameCatalog::load_builtin(std::string* error) {
#if defined(PANPLAY_EMBED_CATALOG)
  std::string text;
  if (!gunzip(kCatalogGz, sizeof(kCatalogGz), &text)) {
    if (error) *error = "内置资料库解压失败";
    return false;
  }
  std::unique_lock<std::shared_mutex> lock(mu_);
  if (!load_text_unlocked(text, error)) return false;
  meta_.path = "（内置）";
  meta_.builtin = true;
  return true;
#else
  if (error) *error = "这个版本没有打包资料库";
  return false;
#endif
}

bool GameCatalog::load_file(const std::string& path, std::string* error) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  return load_file_unlocked(path, error);
}

bool GameCatalog::load_file_unlocked(const std::string& path, std::string* error) {
  const std::string text = read_text_file(std::filesystem::u8path(path));
  if (text.empty()) {
    by_title_id_.clear();
    loaded_ = false;
    meta_ = {};
    if (error) *error = "找不到 catalog 文件: " + path;
    return false;
  }
  if (!load_text_unlocked(text, error)) return false;
  meta_.path = path;
  try {
    // 文件时钟的起点不是 1970 年，按两个时钟的当前时间差换算成 Unix 时间。
    const auto mtime = std::filesystem::last_write_time(std::filesystem::u8path(path));
    const auto system_mtime = std::chrono::system_clock::now() +
                              std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                  mtime - std::filesystem::file_time_type::clock::now());
    meta_.file_mtime_unix =
        std::chrono::duration_cast<std::chrono::seconds>(system_mtime.time_since_epoch()).count();
  } catch (...) {
    meta_.file_mtime_unix = 0;
  }
  return true;
}

bool GameCatalog::load_text_unlocked(const std::string& text, std::string* error) {
  by_title_id_.clear();
  loaded_ = false;
  meta_ = {};

  std::string parse_error;
  const Json root = Json::parse(text, &parse_error);
  if (!parse_error.empty()) {
    if (error) *error = parse_error;
    return false;
  }

  meta_.source = root.str("source");
  meta_.generated_at = root.str("generated_at");
  meta_.generated_from = root.str("generated_from");
  meta_.version = static_cast<int>(root.num("version"));

  const Json* titles = root.get("titles");
  if (titles == nullptr) titles = root.get("t");
  if (titles != nullptr && titles->type == Json::Type::object) {
    for (const auto& [title_id, node] : titles->object) {
      CatalogEntry entry = parse_entry(node, title_id);
      if (entry.name.empty()) continue;
      const std::string id = short_title_id(title_id);
      by_title_id_[id] = entry;
    }
  } else if (titles != nullptr && titles->type == Json::Type::array) {
    for (const Json& item : titles->array) {
      const std::string title_id = short_title_id(item.str("title_id"));
      if (title_id.empty()) continue;
      CatalogEntry entry = parse_entry(item, title_id);
      if (entry.name.empty()) entry.name = item.str("name");
      if (entry.name.empty()) continue;
      by_title_id_[title_id] = entry;
    }
  }

  loaded_ = !by_title_id_.empty();
  if (!loaded_ && error) *error = "catalog 里没有可用条目";
  return loaded_;
}

bool GameCatalog::loaded() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return loaded_;
}

std::size_t GameCatalog::size() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return by_title_id_.size();
}

CatalogMeta GameCatalog::meta() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return meta_;
}

bool GameCatalog::is_stale(int max_age_days) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  if (loaded_ && meta_.builtin) return true;
  if (!loaded_ || max_age_days <= 0 || meta_.file_mtime_unix <= 0) return false;
  const auto now = std::chrono::system_clock::now();
  const auto mtime = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(meta_.file_mtime_unix));
  return now - mtime > std::chrono::hours(24 * max_age_days);
}

CatalogMatch GameCatalog::lookup(const std::string& title_id, const std::string&) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  CatalogMatch match;
  if (title_id.empty()) return match;
  const auto it = by_title_id_.find(title_id);
  if (it == by_title_id_.end()) return match;
  match.entry = it->second;
  match.match_by = "title_id";
  match.found = true;
  return match;
}

}  // namespace panplay
