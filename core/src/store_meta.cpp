#include "panplay/store_meta.hpp"

#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace panplay {
namespace {

constexpr const char* kLocales[] = {"zh-hans-hk", "zh-hant-hk", "en-us"};
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
constexpr std::int64_t kFoundTtlSeconds = 30LL * 24 * 3600;
constexpr std::int64_t kMissingTtlSeconds = 7LL * 24 * 3600;
constexpr auto kOfflineRetry = std::chrono::minutes(10);

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

bool is_language_note(const std::string& inner) {
  static const char* marks[] = {"中文", "语", "語", "文版", "日文", "韩文", "韓文", "英文",
                                "English", "Chinese", "Japanese", "Korean"};
  for (const char* mark : marks) {
    if (inner.find(mark) != std::string::npos) return true;
  }
  return false;
}

void append_utf8(std::string& out, unsigned long cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string lower_ascii(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::string strip_markup(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '<') {
      const auto close = text.find('>', i);
      if (close == std::string::npos) break;
      const std::string tag = lower_ascii(text.substr(i + 1, close - i - 1));
      const std::string tag_name = tag.substr(0, tag.find_first_of(" /\t", tag.empty() || tag[0] != '/' ? 0 : 1));
      if (tag_name == "br") out += '\n';
      else if (tag_name == "/p") out += "\n\n";
      else if (tag_name == "li") out += "• ";
      else if (tag_name == "/li") out += '\n';
      i = close + 1;
      continue;
    }
    if (text[i] == '&') {
      static const std::pair<const char*, const char*> entities[] = {
          {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
      bool replaced = false;
      for (const auto& [from, to] : entities) {
        if (text.compare(i, std::char_traits<char>::length(from), from) == 0) {
          out += to;
          i += std::char_traits<char>::length(from);
          replaced = true;
          break;
        }
      }
      if (replaced) continue;
      const auto semi = text.find(';', i);
      if (semi != std::string::npos && semi - i <= 9 && i + 2 < semi && text[i + 1] == '#') {
        const bool hex = text[i + 2] == 'x' || text[i + 2] == 'X';
        const std::string digits = text.substr(i + (hex ? 3 : 2), semi - i - (hex ? 3 : 2));
        char* end = nullptr;
        const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
        if (!digits.empty() && end != nullptr && *end == '\0' && cp > 0) {
          append_utf8(out, cp);
          i = semi + 1;
          continue;
        }
      }
    }
    out += text[i++];
  }
  std::string collapsed;
  collapsed.reserve(out.size());
  int newlines = 0;
  for (char ch : out) {
    if (ch == '\r') continue;
    newlines = ch == '\n' ? newlines + 1 : 0;
    if (newlines <= 2) collapsed += ch;
  }
  return trim(collapsed);
}

using Cache = std::map<std::string, Json>;

Cache merged_cache(const std::string& html) {
  Cache cache;
  std::size_t pos = 0;
  while ((pos = html.find("<script", pos)) != std::string::npos) {
    const auto tag_end = html.find('>', pos);
    if (tag_end == std::string::npos) break;
    const std::string tag = html.substr(pos, tag_end - pos);
    pos = tag_end + 1;
    if (tag.find("id=\"env:") == std::string::npos || tag.find("application/json") == std::string::npos) continue;
    const auto close = html.find("</script>", pos);
    if (close == std::string::npos) break;
    const Json root = Json::parse(html.substr(pos, close - pos), nullptr);
    pos = close;
    const Json* nodes = root.get("cache");
    if (nodes == nullptr || nodes->type != Json::Type::object) continue;
    for (const auto& [key, node] : nodes->object) {
      Json& slot = cache[key];
      if (slot.type != Json::Type::object) slot = node;
      else for (const auto& [field, value] : node.object) slot.object[field] = value;
    }
  }
  return cache;
}

const Json* ref_target(const Cache& cache, const Json& node, const std::string& field) {
  const Json* link = node.get(field);
  if (link == nullptr) return nullptr;
  const auto it = cache.find(link->str("__ref"));
  return it == cache.end() ? nullptr : &it->second;
}

std::string pick_image(const Json* node) {
  if (node == nullptr) return "";
  const Json* media = node->get("media");
  if (media == nullptr) return "";
  for (const char* role : {"MASTER", "PORTRAIT_BANNER", "EDITION_KEY_ART", "GAMEHUB_COVER_ART"}) {
    for (const Json& item : media->array) {
      if (item.str("type") == "IMAGE" && item.str("role") == role && !item.str("url").empty()) return item.str("url");
    }
  }
  return "";
}

std::string joined_genres(const Json* node) {
  if (node == nullptr) return "";
  const Json* genres = node->get("localizedGenres");
  if (genres == nullptr) return "";
  std::string out;
  for (const Json& genre : genres->array) {
    const std::string value = genre.str("value");
    if (value.empty() || out.find(value) != std::string::npos) continue;
    if (!out.empty()) out += " / ";
    out += value;
  }
  return out;
}

std::string pick_description(const Json* node) {
  if (node == nullptr) return "";
  const Json* items = node->get("descriptions");
  if (items == nullptr) return "";
  for (const char* type : {"LONG", "SHORT"}) {
    for (const Json& item : items->array) {
      if (item.str("type") == type && !item.str("value").empty()) return strip_markup(item.str("value"));
    }
  }
  return "";
}

std::string first_of(const Json* a, const Json* b, const std::string& field) {
  if (a != nullptr && !a->str(field).empty()) return a->str(field);
  if (b != nullptr) return b->str(field);
  return "";
}

std::string release_of(const Json* node) {
  if (node == nullptr) return "";
  const Json* release = node->get("releaseDate");
  if (release == nullptr) return "";
  return release->str("value").substr(0, 10);
}

// 商店数据里的数字有时写成字符串，布尔有时写成 "true"。
std::string scalar_text(const Json* node) {
  if (node == nullptr) return "";
  if (node->type == Json::Type::string || node->type == Json::Type::number) return node->text;
  if (node->type == Json::Type::boolean) return node->boolean ? "true" : "false";
  return "";
}

double number_of(const Json* node) {
  const std::string text = scalar_text(node);
  return text.empty() ? 0 : std::strtod(text.c_str(), nullptr);
}

const Json* field_of(const Cache& cache, const Json* node, const std::string& field) {
  if (node == nullptr) return nullptr;
  const Json* value = node->get(field);
  if (value == nullptr) return nullptr;
  if (value->type == Json::Type::object && value->get("__ref") != nullptr) {
    const auto it = cache.find(value->str("__ref"));
    return it == cache.end() ? nullptr : &it->second;
  }
  return value;
}

// 先看商品再看概念，取第一个有内容的。
const Json* first_field(const Cache& cache, const Json* product, const Json* concept_node, const std::string& field) {
  for (const Json* node : {product, concept_node}) {
    const Json* value = field_of(cache, node, field);
    if (value == nullptr || value->type == Json::Type::null) continue;
    if (value->type == Json::Type::array && value->array.empty()) continue;
    if (value->type == Json::Type::string && value->text.empty()) continue;
    return value;
  }
  return nullptr;
}

std::vector<std::string> string_list(const Json* node) {
  std::vector<std::string> out;
  if (node == nullptr) return out;
  for (const Json& item : node->array) {
    const std::string text = scalar_text(&item);
    if (!text.empty() && std::find(out.begin(), out.end(), text) == out.end()) out.push_back(text);
  }
  return out;
}

void parse_rating(const Json* star, StoreMeta* meta) {
  if (star == nullptr) return;
  const double count = number_of(star->get("totalRatingsCount"));
  if (count <= 0) return;
  meta->rating = number_of(star->get("averageRating"));
  meta->rating_count = static_cast<std::int64_t>(count);
  const Json* dist = star->get("ratingsDistribution");
  if (dist == nullptr || dist->array.empty()) return;
  std::map<int, int> percent;
  for (const Json& item : dist->array) {
    percent[static_cast<int>(number_of(item.get("rating")))] =
        static_cast<int>(number_of(item.get("percentageRaw")) + 0.5);
  }
  for (int stars = 5; stars >= 1; --stars) meta->rating_dist.push_back(percent[stars]);
}

// 人数、联机、会员和内购写在兼容性提示里，商品的覆盖概念的。
void parse_notices(const Json* concept_node, const Json* product, StoreMeta* meta) {
  std::map<std::string, std::string> notices;
  for (const Json* node : {concept_node, product}) {
    if (node == nullptr) continue;
    const Json* by_platform = node->get("compatibilityNoticesByPlatform");
    const Json* common = by_platform == nullptr ? nullptr : by_platform->get("Common");
    if (common == nullptr) continue;
    for (const Json& item : common->array) notices[item.str("type")] = scalar_text(item.get("value"));
  }
  const int players = std::atoi(notices["NO_OF_PLAYERS"].c_str());
  meta->players = players > 0 ? players : 0;
  meta->online_play = notices["ONLINE_PLAY_MODE"];
  meta->ps_plus_required = notices["PS_PLUS_REQUIRED"] == "true";
  const std::string purchases = notices["IN_GAME_PURCHASES"];
  meta->in_game_purchases = !purchases.empty() && purchases != "NONE" && purchases != "false";
}

bool any_chinese(const std::vector<std::string>& langs) {
  for (const std::string& code : langs) {
    const std::string lower = lower_ascii(code);
    if (starts_with(lower, "zh") || starts_with(lower, "ch")) return true;
  }
  return false;
}

std::string json_strings(const std::vector<std::string>& items) {
  std::string out = "[";
  for (const std::string& item : items) {
    if (out.size() > 1) out += ",";
    out += "\"" + json_escape(item) + "\"";
  }
  return out + "]";
}

std::string json_ints(const std::vector<int>& items) {
  std::string out = "[";
  for (const int item : items) {
    if (out.size() > 1) out += ",";
    out += std::to_string(item);
  }
  return out + "]";
}

std::string format_rating(double rating) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", rating);
  return buffer;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool looks_like_image(const std::string& body) {
  if (body.size() < 4) return false;
  const auto* b = reinterpret_cast<const unsigned char*>(body.data());
  const bool jpeg = b[0] == 0xFF && b[1] == 0xD8;
  const bool png = b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
  const bool webp = body.size() >= 12 && body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0;
  const bool avif = body.size() >= 12 && (body.compare(4, 8, "ftypavif") == 0 || body.compare(4, 8, "ftypavis") == 0);
  return jpeg || png || webp || avif;
}

// PS5 浏览器认不出 AVIF 和 WebP。
bool ps5_unreadable_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  char head[12] = {};
  if (!in.read(head, sizeof(head))) return false;
  const std::string bytes(head, sizeof(head));
  return (bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) || bytes.compare(4, 4, "ftyp") == 0;
}

HttpRequest store_request(const std::string& url) {
  HttpRequest request;
  request.url = url;
  request.headers.push_back({"User-Agent", kUserAgent});
  request.timeout_ms = 30000;
  return request;
}

}  // namespace

const char* store_meta_state_name(StoreMetaState state) {
  switch (state) {
    case StoreMetaState::pending: return "pending";
    case StoreMetaState::ready: return "ready";
    case StoreMetaState::missing: return "missing";
    case StoreMetaState::offline: return "offline";
  }
  return "missing";
}

std::string clean_store_name(const std::string& name) {
  std::string out = trim(name);
  for (;;) {
    const bool ascii = ends_with(out, ")");
    if (!ascii && !ends_with(out, "）")) break;
    const std::size_t close = ascii ? out.size() - 1 : out.size() - std::string("）").size();
    const auto open_ascii = out.rfind('(', close);
    const auto open_wide = out.rfind("（", close);
    std::size_t open = std::string::npos;
    std::size_t open_len = 0;
    if (open_ascii != std::string::npos) open = open_ascii, open_len = 1;
    if (open_wide != std::string::npos && (open == std::string::npos || open_wide > open)) {
      open = open_wide;
      open_len = std::string("（").size();
    }
    if (open == std::string::npos || open == 0) break;
    if (!is_language_note(out.substr(open + open_len, close - open - open_len))) break;
    out = trim(out.substr(0, open));
  }
  const std::string left = "《";
  const std::string right = "》";
  if (starts_with(out, left) && ends_with(out, right) && out.find(left, left.size()) == std::string::npos &&
      out.size() > left.size() + right.size()) {
    out = trim(out.substr(left.size(), out.size() - left.size() - right.size()));
  }
  return out;
}

StoreMeta parse_concept_page(const std::string& html, std::int64_t concept_id) {
  StoreMeta meta;
  meta.concept_id = concept_id;
  const Cache cache = merged_cache(html);
  const auto it = cache.find("Concept:" + std::to_string(concept_id));
  if (it == cache.end()) return meta;
  const Json* concept_node = &it->second;
  const Json* product = ref_target(cache, *concept_node, "defaultProduct");

  meta.name = clean_store_name(first_of(concept_node, product, "name"));
  if (meta.name.empty()) return meta;
  meta.found = true;
  meta.cover_url = pick_image(concept_node);
  if (meta.cover_url.empty()) meta.cover_url = pick_image(product);
  meta.publisher = first_of(concept_node, product, "publisherName");
  meta.genres = joined_genres(concept_node);
  if (meta.genres.empty()) meta.genres = joined_genres(product);
  meta.release_date = release_of(concept_node);
  if (meta.release_date.empty()) meta.release_date = release_of(product);
  meta.description = pick_description(concept_node);
  if (meta.description.empty()) meta.description = pick_description(product);

  parse_rating(first_field(cache, product, concept_node, "starRating"), &meta);
  meta.screen_langs = string_list(first_field(cache, product, concept_node, "screenLanguages"));
  meta.spoken_langs = string_list(first_field(cache, product, concept_node, "spokenLanguages"));
  meta.platforms = string_list(first_field(cache, product, concept_node, "platforms"));
  parse_notices(concept_node, product, &meta);
  meta.classification = scalar_text(first_field(cache, product, concept_node, "storeDisplayClassification"));
  if (const Json* edition = first_field(cache, product, concept_node, "edition")) meta.edition = edition->str("name");
  meta.meta_ver = kStoreMetaVersion;
  return meta;
}

bool StoreMeta::has_cn_sub() const { return any_chinese(screen_langs); }
bool StoreMeta::has_cn_voice() const { return any_chinese(spoken_langs); }

bool fetch_store_meta(std::int64_t concept_id, const HttpFetch& fetch, StoreMeta* out, std::string* error,
                      const std::function<void()>& before_request) {
  StoreMeta meta;
  meta.concept_id = concept_id;
  for (const char* locale : kLocales) {
    if (before_request) before_request();
    const HttpResponse response = fetch(store_request("https://store.playstation.com/" + std::string(locale) +
                                                      "/concept/" + std::to_string(concept_id)));
    if (!response.error.empty() || (response.status != 200 && response.status != 404)) {
      if (error) {
        *error = response.error.empty() ? "商店返回 HTTP " + std::to_string(response.status) : "连接商店失败: " + response.error;
      }
      return false;
    }
    if (response.status != 200) continue;
    StoreMeta parsed = parse_concept_page(response.body, concept_id);
    if (parsed.found) {
      parsed.locale = locale;
      meta = parsed;
      break;
    }
  }
  meta.meta_ver = kStoreMetaVersion;
  meta.fetched_at = unix_now();
  *out = meta;
  return true;
}

StoreMetaCache::StoreMetaCache(std::filesystem::path dir, HttpFetch fetch, int min_interval_ms)
    : dir_(std::move(dir)), fetch_(std::move(fetch)), min_interval_ms_(min_interval_ms) {
  std::error_code ec;
  std::filesystem::create_directories(dir_ / "covers", ec);
  load();
}

StoreMetaCache::~StoreMetaCache() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void StoreMetaCache::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (worker_.joinable()) return;
  worker_ = std::thread([this] { worker_loop(); });
}

std::filesystem::path StoreMetaCache::cover_file(std::int64_t concept_id) const {
  return dir_ / "covers" / (std::to_string(concept_id) + ".img");
}

StoreMetaState StoreMetaCache::lookup(std::int64_t concept_id, StoreMeta* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (concept_id <= 0) return StoreMetaState::missing;
  const auto now = std::chrono::steady_clock::now();
  const auto retry = retry_after_.find(concept_id);
  const bool backing_off = retry != retry_after_.end() && now < retry->second;
  const auto busy = [&] { return queued_.count(concept_id) != 0 || in_flight_.count(concept_id) != 0; };
  const auto it = entries_.find(concept_id);
  if (it != entries_.end()) {
    const StoreMeta& meta = it->second;
    if (out) *out = meta;
    const bool stale = unix_now() - meta.fetched_at > (meta.found ? kFoundTtlSeconds : kMissingTtlSeconds);
    const bool cover_missing = meta.found && !meta.has_cover && !meta.cover_url.empty();
    const bool outdated = meta.found && meta.meta_ver < kStoreMetaVersion;
    if (!backing_off && !busy() && (stale || cover_missing || outdated)) enqueue_locked(concept_id);
    // 已有资料在后台更新时照常显示；用户点了重新获取才算补齐中。
    const bool user_refresh = forced_.count(concept_id) != 0 || forced_running_.count(concept_id) != 0;
    if (busy() && (!meta.found || user_refresh)) return StoreMetaState::pending;
    return meta.found ? StoreMetaState::ready : StoreMetaState::missing;
  }
  if (busy()) return StoreMetaState::pending;
  if (backing_off) return StoreMetaState::offline;
  enqueue_locked(concept_id);
  return StoreMetaState::pending;
}

void StoreMetaCache::refresh(std::int64_t concept_id) {
  if (concept_id <= 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  retry_after_.erase(concept_id);
  forced_.insert(concept_id);
  // 用户点的刷新插到最前面，不用等游戏库里排着的其他游戏。
  if (queued_.count(concept_id) != 0) queue_.erase(std::remove(queue_.begin(), queue_.end(), concept_id), queue_.end());
  queued_.insert(concept_id);
  queue_.push_front(concept_id);
  cv_.notify_one();
}

void StoreMetaCache::enqueue_locked(std::int64_t concept_id) {
  if (!queued_.insert(concept_id).second) return;
  queue_.push_back(concept_id);
  cv_.notify_one();
}

bool StoreMetaCache::run_once() {
  std::int64_t concept_id = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) return false;
    concept_id = queue_.front();
    queue_.pop_front();
    queued_.erase(concept_id);
    in_flight_.insert(concept_id);
  }
  process(concept_id);
  return true;
}

void StoreMetaCache::worker_loop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
    }
    run_once();
  }
}

void StoreMetaCache::throttle() {
  const auto next = last_request_ + std::chrono::milliseconds(min_interval_ms_);
  const auto now = std::chrono::steady_clock::now();
  if (now < next) std::this_thread::sleep_for(next - now);
  last_request_ = std::chrono::steady_clock::now();
}

void StoreMetaCache::process(std::int64_t concept_id) {
  const std::filesystem::path cover_path = cover_file(concept_id);
  StoreMeta meta;
  StoreMeta previous;
  bool full = true;
  // upgrade：资料没过期，只是缺新版本的字段，重抓页面但保留封面和旧资料。
  bool upgrade = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const bool forced = forced_.erase(concept_id) > 0;
    if (forced) forced_running_.insert(concept_id);
    const auto it = entries_.find(concept_id);
    if (!forced && it != entries_.end() && it->second.found) {
      const bool stale = unix_now() - it->second.fetched_at > kFoundTtlSeconds;
      if (!stale) {
        meta = it->second;
        previous = it->second;
        full = it->second.meta_ver < kStoreMetaVersion;
        upgrade = full;
      }
    }
  }

  if (full) {
    std::string error;
    PANPLAY_DEBUG("商店资料：抓取 " + std::to_string(concept_id));
    if (!fetch_store_meta(concept_id, fetch_, &meta, &error, [this] { throttle(); })) {
      log_warn("商店资料 " + std::to_string(concept_id) + " 暂时取不到: " + error);
      std::lock_guard<std::mutex> lock(mu_);
      retry_after_[concept_id] = std::chrono::steady_clock::now() + kOfflineRetry;
      in_flight_.erase(concept_id);
      forced_running_.erase(concept_id);
      return;
    }
    if (upgrade && !meta.found) {
      meta = previous;
      meta.meta_ver = kStoreMetaVersion;
    }
  }

  bool cover_offline = false;
  if (meta.found && !meta.cover_url.empty() && ((full && !upgrade) || !std::filesystem::exists(cover_path))) {
    throttle();
    // 图床按源图回 AVIF 或 WebP，与后缀和请求头无关，只有 fmt=jpg 能让它转成 JPEG。
    const std::string url =
        meta.cover_url + (meta.cover_url.find('?') == std::string::npos ? "?w=440&fmt=jpg" : "&w=440&fmt=jpg");
    const HttpResponse image = fetch_(store_request(url));
    if (image.error.empty() && image.status == 200 && looks_like_image(image.body)) {
      std::filesystem::path tmp = cover_path;
      tmp += ".tmp";
      std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
      file.write(image.body.data(), static_cast<std::streamsize>(image.body.size()));
      file.close();
      std::error_code ec;
      if (file) std::filesystem::rename(tmp, cover_path, ec);
      else std::filesystem::remove(tmp, ec);
    } else if (image.error.empty() && (image.status == 200 || image.status == 404)) {
      meta.cover_url.clear();
    } else {
      cover_offline = true;
    }
  }
  meta.has_cover = meta.found && std::filesystem::exists(cover_path);
  PANPLAY_DEBUG("商店资料：" + std::to_string(concept_id) + (meta.found ? " 已更新" : " 商店没有收录") +
                (meta.has_cover ? "，有封面" : "，无封面"));

  std::lock_guard<std::mutex> lock(mu_);
  entries_[concept_id] = meta;
  if (cover_offline) retry_after_[concept_id] = std::chrono::steady_clock::now() + kOfflineRetry;
  else retry_after_.erase(concept_id);
  in_flight_.erase(concept_id);
  forced_running_.erase(concept_id);
  save_locked();
}

void StoreMetaCache::load() {
  const std::string text = read_text(dir_ / "store-meta.json");
  PANPLAY_DEBUG("商店资料缓存 " + std::to_string(text.size()) + " 字节");
  if (text.empty()) return;
  const Json root = Json::parse(text, nullptr);
  const Json* concepts = root.get("concepts");
  if (concepts == nullptr || concepts->type != Json::Type::object) return;
  // 版本 1 把 AVIF 封面当成坏图，清掉了封面地址
  const bool drop_coverless = root.num("version") < 2;
  for (const auto& [key, node] : concepts->object) {
    StoreMeta meta;
    meta.concept_id = std::strtoll(key.c_str(), nullptr, 10);
    if (meta.concept_id <= 0) continue;
    meta.found = node.boolean_or("found", false);
    meta.locale = node.str("locale");
    meta.name = node.str("name");
    meta.cover_url = node.str("cover_url");
    meta.publisher = node.str("publisher");
    meta.genres = node.str("genres");
    meta.release_date = node.str("release_date");
    meta.description = node.str("description");
    meta.rating = node.num("rating");
    meta.rating_count = static_cast<std::int64_t>(node.num("rating_count"));
    if (const Json* dist = node.get("rating_dist")) {
      for (const Json& item : dist->array) meta.rating_dist.push_back(static_cast<int>(item.number));
    }
    meta.screen_langs = string_list(node.get("screen_langs"));
    meta.spoken_langs = string_list(node.get("spoken_langs"));
    meta.platforms = string_list(node.get("platforms"));
    meta.players = static_cast<int>(node.num("players"));
    meta.online_play = node.str("online_play");
    meta.ps_plus_required = node.boolean_or("ps_plus_required", false);
    meta.in_game_purchases = node.boolean_or("in_game_purchases", false);
    meta.classification = node.str("classification");
    meta.edition = node.str("edition");
    meta.meta_ver = static_cast<int>(node.num("meta_ver"));
    meta.fetched_at = static_cast<std::int64_t>(node.num("fetched_at"));
    // 早先存下的 AVIF、WebP 封面删掉，查询时会按缺封面补下 JPEG。
    std::error_code ec;
    if (meta.found && !meta.cover_url.empty() && ps5_unreadable_file(cover_file(meta.concept_id))) {
      std::filesystem::remove(cover_file(meta.concept_id), ec);
    }
    meta.has_cover = meta.found && std::filesystem::exists(cover_file(meta.concept_id));
    if (drop_coverless && meta.found && !meta.has_cover) meta.fetched_at = 0;
    entries_[meta.concept_id] = meta;
  }
}

void StoreMetaCache::save_locked() {
  std::string json = "{\"version\":3,\"concepts\":{";
  bool first = true;
  for (const auto& [id, meta] : entries_) {
    if (!first) json += ",";
    first = false;
    json += "\"" + std::to_string(id) + "\":{\"found\":" + (meta.found ? "true" : "false") + ",\"locale\":\"" +
            json_escape(meta.locale) + "\",\"name\":\"" + json_escape(meta.name) + "\",\"cover_url\":\"" +
            json_escape(meta.cover_url) + "\",\"publisher\":\"" + json_escape(meta.publisher) + "\",\"genres\":\"" +
            json_escape(meta.genres) + "\",\"release_date\":\"" + json_escape(meta.release_date) +
            "\",\"description\":\"" + json_escape(meta.description) + "\",\"rating\":" + format_rating(meta.rating) +
            ",\"rating_count\":" + std::to_string(meta.rating_count) + ",\"rating_dist\":" + json_ints(meta.rating_dist) +
            ",\"screen_langs\":" + json_strings(meta.screen_langs) + ",\"spoken_langs\":" +
            json_strings(meta.spoken_langs) + ",\"platforms\":" + json_strings(meta.platforms) +
            ",\"players\":" + std::to_string(meta.players) + ",\"online_play\":\"" + json_escape(meta.online_play) +
            "\",\"ps_plus_required\":" + (meta.ps_plus_required ? "true" : "false") +
            ",\"in_game_purchases\":" + (meta.in_game_purchases ? "true" : "false") + ",\"classification\":\"" +
            json_escape(meta.classification) + "\",\"edition\":\"" + json_escape(meta.edition) +
            "\",\"meta_ver\":" + std::to_string(meta.meta_ver) + ",\"fetched_at\":" + std::to_string(meta.fetched_at) +
            "}";
  }
  json += "}}";
  const std::filesystem::path path = dir_ / "store-meta.json";
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) log_warn("写入商店资料缓存失败: " + ec.message());
}

int run_store_meta_cli(const std::string& concept_id) {
  const std::int64_t id = std::strtoll(concept_id.c_str(), nullptr, 10);
  if (id <= 0) {
    std::cerr << "conceptId 应该是正整数\n";
    return 1;
  }
  StoreMeta meta;
  std::string error;
  if (!fetch_store_meta(id, http_send, &meta, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!meta.found) {
    std::cout << "港服和美服都没有收录 " << id << "\n";
    return 0;
  }
  std::size_t cut = std::min<std::size_t>(meta.description.size(), 300);
  while (cut > 0 && cut < meta.description.size() && (static_cast<unsigned char>(meta.description[cut]) & 0xC0) == 0x80) --cut;
  std::cout << "地区     " << meta.locale << "\n名称     " << meta.name << "\n发行商   " << meta.publisher << "\n类型     "
            << meta.genres << "\n发售日   " << meta.release_date << "\n封面     " << meta.cover_url << "\n";
  const auto join = [](const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
    return out;
  };
  if (meta.rating_count > 0) std::cout << "评分     " << format_rating(meta.rating) << "（" << meta.rating_count << " 人）\n";
  std::cout << "字幕     " << join(meta.screen_langs) << "\n语音     " << join(meta.spoken_langs) << "\n平台     "
            << join(meta.platforms) << "\n人数     " << meta.players << "\n联机     " << meta.online_play
            << (meta.ps_plus_required ? " 需 PS Plus" : "") << (meta.in_game_purchases ? " 含内购" : "") << "\n版本     "
            << meta.edition << " " << meta.classification << "\n简介     " << meta.description.substr(0, cut) << "\n";
  return 0;
}

}  // namespace panplay
