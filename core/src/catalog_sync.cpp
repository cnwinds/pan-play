#include "panplay/catalog_sync.hpp"

#include "panplay/catalog.hpp"
#include "panplay/http.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace panplay {
namespace {

constexpr const char* kDefaultUpstream =
    "https://raw.githubusercontent.com/andshrew/PlayStation-Titles/main/Json/All_Titles.json";

std::string upstream_url() {
  const char* custom = std::getenv("PANPLAY_CATALOG_URL");
  return custom != nullptr && *custom != '\0' ? custom : kDefaultUpstream;
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string utc_now_iso8601() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
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

}  // namespace

bool fetch_and_write_catalog(const std::string& output_path, const std::string& source_url, std::string* error) {
  HttpRequest request;
  request.url = source_url.empty() ? kDefaultUpstream : source_url;
  request.headers.push_back({"User-Agent", "panplay-catalog-sync/1.0"});
  request.timeout_ms = 120000;

  PANPLAY_DEBUG("资料库同步：开始下载 " + request.url + "，" + memory_summary());
  const auto download_start = std::chrono::steady_clock::now();
  const HttpResponse response = http_send(request);
  PANPLAY_DEBUG("资料库同步：下载结束 HTTP " + std::to_string(response.status) + "，" + std::to_string(response.body.size()) +
        " 字节，耗时 " + std::to_string(ms_since(download_start)) + " ms" +
        (response.error.empty() ? "" : "，错误 " + response.error) + "，" + memory_summary());
  if (!response.error.empty()) {
    if (error) *error = "拉取上游失败: " + response.error;
    return false;
  }
  if (response.status != 200) {
    if (error) *error = "拉取上游失败: HTTP " + std::to_string(response.status);
    return false;
  }

  std::string parse_error;
  PANPLAY_DEBUG("资料库同步：开始解析 JSON");
  const auto parse_start = std::chrono::steady_clock::now();
  const Json root = Json::parse(response.body, &parse_error);
  PANPLAY_DEBUG("资料库同步：解析结束，" + std::to_string(root.array.size()) + " 条，耗时 " +
        std::to_string(ms_since(parse_start)) + " ms" + (parse_error.empty() ? "" : "，错误 " + parse_error) + "，" +
        memory_summary());
  if (!parse_error.empty()) {
    if (error) *error = "解析上游 JSON 失败: " + parse_error;
    return false;
  }
  if (root.type != Json::Type::array) {
    if (error) *error = "上游 JSON 格式不符合预期";
    return false;
  }

  const std::filesystem::path out_path = std::filesystem::u8path(output_path);
  std::error_code ec;
  std::filesystem::create_directories(out_path.parent_path(), ec);
  std::filesystem::path tmp_path = out_path;
  tmp_path += ".tmp";

  PANPLAY_DEBUG("资料库同步：写入 " + tmp_path.u8string());
  std::ofstream out(tmp_path, std::ios::binary);
  if (!out) {
    if (error) *error = "无法写入临时 catalog 文件";
    return false;
  }

  const std::string generated_at = utc_now_iso8601();
  const std::string source =
      "PlayStation-Titles (MIT) — https://github.com/andshrew/PlayStation-Titles";
  const std::string upstream = source_url.empty() ? kDefaultUpstream : source_url;

  out << "{\"version\":1,\"source\":\"" << json_escape(source) << "\",\"generated_at\":\"" << generated_at
      << "\",\"generated_from\":\"" << json_escape(upstream) << "\",\"titles\":{";

  bool first = true;
  std::size_t count = 0;
  for (const Json& row : root.array) {
    const std::string title_id = short_title_id(row.str("titleId"));
    if (title_id.empty()) continue;
    const std::string name = row.str("name");
    if (name.empty()) continue;

    if (!first) out << ',';
    first = false;
    ++count;

    out << '\"' << json_escape(title_id) << "\":{\"n\":\"" << json_escape(name) << '\"';
    const double concept_id = row.num("conceptId");
    if (concept_id != 0) out << ",\"c\":" << static_cast<std::int64_t>(concept_id);
    const std::string region = row.str("region");
    if (!region.empty()) out << ",\"r\":\"" << json_escape(region) << '\"';
    const std::string publisher = row.str("publisherId");
    if (!publisher.empty()) out << ",\"p\":\"" << json_escape(publisher) << '\"';
    const std::string platform = platform_of(title_id);
    if (!platform.empty()) out << ",\"x\":\"" << platform << '\"';
    out << '}';
  }
  out << "},\"count\":" << count << '}';
  out.close();
  if (!out) {
    std::filesystem::remove(tmp_path, ec);
    if (error) *error = "写入 catalog 文件失败";
    return false;
  }
  if (count == 0) {
    std::filesystem::remove(tmp_path, ec);
    if (error) *error = "上游没有可用 Title ID";
    return false;
  }

  std::filesystem::rename(tmp_path, out_path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    if (error) *error = "替换 catalog 文件失败";
    return false;
  }
  PANPLAY_DEBUG("资料库同步：写入完成 " + std::to_string(count) + " 条");
  return true;
}

CatalogSync& CatalogSync::instance() {
  static CatalogSync sync;
  return sync;
}

void CatalogSync::start(int stale_days, int check_interval_hours) {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) return;
  started_ = true;
  stale_days_ = stale_days;
  check_interval_hours_ = check_interval_hours;
  info_.stale_days = stale_days;
  info_.check_interval_hours = check_interval_hours;
  worker_ = std::thread([this] { run_loop(); });
  worker_.detach();
}

CatalogSyncInfo CatalogSync::info() const {
  std::lock_guard<std::mutex> lock(mu_);
  return info_;
}

bool CatalogSync::needs_update() const {
  const GameCatalog& catalog = GameCatalog::instance();
  if (!catalog.loaded()) return true;
  return catalog.is_stale(stale_days_);
}

void CatalogSync::sync_once() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (info_.phase == CatalogSyncPhase::updating) return;
    info_.phase = CatalogSyncPhase::updating;
    info_.last_error.clear();
    info_.last_attempt_unix = unix_now();
  }

  const std::string path = GameCatalog::synced_path();
  std::string sync_error;
  const bool ok = fetch_and_write_catalog(path, upstream_url(), &sync_error);

  if (ok) {
    std::string reload_error;
    PANPLAY_DEBUG("资料库同步：重新加载 " + path);
    if (GameCatalog::instance().load_file(path, &reload_error)) {
      log_info("游戏资料库已自动同步，共 " + std::to_string(GameCatalog::instance().size()) + " 条，" + memory_summary());
      std::lock_guard<std::mutex> lock(mu_);
      info_.phase = CatalogSyncPhase::idle;
      info_.last_success_unix = unix_now();
      info_.last_error.clear();
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      info_.phase = CatalogSyncPhase::idle;
      info_.last_error = reload_error.empty() ? "重载资料库失败" : reload_error;
      log_warn("资料库已下载但重载失败: " + info_.last_error);
    }
  } else {
    std::lock_guard<std::mutex> lock(mu_);
    info_.phase = CatalogSyncPhase::idle;
    info_.last_error = sync_error;
    log_warn("自动同步资料库失败（保留现有数据）: " + sync_error);
  }
}

void CatalogSync::run_loop() {
  const bool update = needs_update();
  PANPLAY_DEBUG(std::string("资料库同步线程启动，") + (update ? "资料库过期，马上同步" : "资料库够新，暂不同步"));
  if (update) sync_once();

  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::hours(check_interval_hours_));
    if (stop_) break;
    if (needs_update()) sync_once();
  }
}

}  // namespace panplay
