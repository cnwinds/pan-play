#include "shelf_library.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

// 盘架按这个位置写，改了要两边一起改。
const std::string kLibraryDir = "/盘游";
const std::string kLibraryName = "资源库.json";
constexpr auto kRefreshEvery = std::chrono::minutes(30);
constexpr auto kStaleAfter = std::chrono::minutes(5);
constexpr std::int64_t kMaxFileBytes = 32 << 20;

std::mutex mu;
std::condition_variable wake;
bool started = false;
bool refresh_wanted = true;
Clock::time_point refreshed_at;
std::shared_ptr<const ShelfLibrary> current;
// 网盘上的文件上次读到的 fs_id；盘架覆盖写一次 fs_id 就变，没变的不重读。
std::string seen_file_id;
std::int64_t fetched_at = 0;
std::string last_error;

std::filesystem::path cache_file() { return panplay::app_data_dir() / "shelf-library.json"; }

std::string read_cache() {
  std::ifstream in(cache_file(), std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_cache(const std::string& text) {
  const auto path = cache_file();
  auto temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
}

// 返回出错原因；网盘上还没有这个文件（盘架没开或还没发布过）不算错。
std::string pull(panplay::DrivePlugin& drive, const std::function<void()>& on_change) {
  std::string error;
  const auto entries = drive.list(kLibraryDir, &error);
  if (!error.empty()) return error.find("errno=-9") == std::string::npos ? error : "";
  for (const auto& entry : entries) {
    if (entry.is_dir || entry.name != kLibraryName) continue;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (entry.file_id == seen_file_id) return "";
    }
    if (entry.size <= 0 || entry.size > kMaxFileBytes) return "资源库文件大小不对：" + std::to_string(entry.size);
    const panplay::ReadResult read = drive.read_at(entry.file_id, 0, static_cast<std::size_t>(entry.size));
    if (!read.error.empty()) return "读资源库失败：" + read.error;
    const std::string text(read.bytes.begin(), read.bytes.end());
    std::string parse_error;
    auto library = parse_shelf_library(text, &parse_error);
    if (!library) return "资源库读不懂：" + parse_error;
    write_cache(text);
    const std::size_t count = library->games.size();
    {
      std::lock_guard<std::mutex> lock(mu);
      current = std::move(library);
      seen_file_id = entry.file_id;
      fetched_at = static_cast<std::int64_t>(std::time(nullptr));
    }
    panplay::log_info("读到盘架的资源库：" + std::to_string(count) + " 款游戏");
    on_change();
    return "";
  }
  return "";
}

void sync_loop(const std::function<std::shared_ptr<panplay::DrivePlugin>()>& open_drive,
               const std::function<void()>& on_change) {
  bool failing = false;
  std::this_thread::sleep_for(std::chrono::seconds(3));
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mu);
      while (!refresh_wanted && Clock::now() < refreshed_at + kRefreshEvery) {
        wake.wait_until(lock, refreshed_at + kRefreshEvery);
      }
      refresh_wanted = false;
      refreshed_at = Clock::now();
    }
    const auto drive = open_drive();
    if (!drive || !drive->session_valid()) continue;
    const std::string error = pull(*drive, on_change);
    {
      std::lock_guard<std::mutex> lock(mu);
      last_error = error;
    }
    // 断网时每 30 分钟都会失败，只在第一次记警告，恢复时再记一行。
    if (!error.empty() && !failing) panplay::log_warn("没能读到盘架的资源库：" + error + "（恢复之前不再重复记）");
    else if (error.empty() && failing) panplay::log_info("又能读到盘架的资源库了");
    failing = !error.empty();
  }
}

}  // namespace

const ShelfGame* ShelfLibrary::find(const std::string& title_id) const {
  if (title_id.empty()) return nullptr;
  for (const auto& game : games) {
    if (game.title_id == title_id) return &game;
  }
  return nullptr;
}

const ShelfVariant* ShelfLibrary::variant(const std::string& title_id, const std::string& variant_format) const {
  const ShelfGame* game = find(title_id);
  if (game == nullptr || variant_format.empty()) return nullptr;
  const std::string format = shelf_format_of(variant_format);
  for (const auto& item : game->variants) {
    if (item.format == format) return &item;
  }
  return nullptr;
}

namespace {

ShelfPlanFile parse_plan_file(const panplay::Json& json) {
  return {json.str("path"), static_cast<std::int64_t>(json.num("size")), json.str("md5")};
}

// 编排里缺东西（没有主游戏、文件没路径）时整个不用，照旧按规则装。
// 资源库第 2 版起只放摘要（layers 是名字、copies 是个数），这时只留版本号和提醒，layers 为空。
std::optional<ShelfPlan> parse_plan(const panplay::Json* json) {
  if (json == nullptr || json->type != panplay::Json::Type::object) return std::nullopt;
  ShelfPlan plan;
  plan.rev = static_cast<int>(json->num("rev"));
  const panplay::Json* layer_list = json->get("layers");
  const bool summary = layer_list == nullptr || layer_list->array.empty() ||
                       layer_list->array.front().type != panplay::Json::Type::object;
  if (summary) {
    if (const panplay::Json* notes = json->get("notes")) {
      for (const auto& note : notes->array) {
        if (note.type == panplay::Json::Type::string && !note.text.empty()) plan.notes.push_back(note.text);
      }
    }
    if (plan.rev <= 0) return std::nullopt;
    return plan;
  }
  if (const panplay::Json* layers = layer_list) {
    for (const auto& item : layers->array) {
      ShelfPlanLayer layer{item.str("role"), item.str("name"), {}};
      if (const panplay::Json* files = item.get("files")) {
        for (const auto& file : files->array) layer.files.push_back(parse_plan_file(file));
      }
      if (layer.files.empty()) return std::nullopt;
      plan.layers.push_back(std::move(layer));
    }
  }
  if (const panplay::Json* copies = json->get("copies")) {
    for (const auto& item : copies->array) {
      const panplay::Json* from = item.get("from");
      if (from == nullptr || item.str("to").empty()) return std::nullopt;
      plan.copies.push_back({parse_plan_file(*from), item.str("to")});
    }
  }
  if (const panplay::Json* notes = json->get("notes")) {
    for (const auto& note : notes->array) {
      if (note.type == panplay::Json::Type::string && !note.text.empty()) plan.notes.push_back(note.text);
    }
  }
  if (plan.layers.empty()) return std::nullopt;
  for (const auto& layer : plan.layers) {
    for (const auto& file : layer.files) {
      if (file.path.empty()) return std::nullopt;
    }
  }
  return plan;
}

}  // namespace

std::shared_ptr<ShelfLibrary> parse_shelf_library(const std::string& text, std::string* error) {
  std::string parse_error;
  const panplay::Json json = panplay::Json::parse(text, &parse_error);
  const panplay::Json* games = json.get("games");
  if (!parse_error.empty() || games == nullptr || games->type != panplay::Json::Type::array) {
    if (error) *error = parse_error.empty() ? "没有 games" : parse_error;
    return nullptr;
  }
  auto library = std::make_shared<ShelfLibrary>();
  library->generated_at = static_cast<std::int64_t>(json.num("generated_at"));
  library->library_dir = json.str("library_dir");
  for (const auto& item : games->array) {
    ShelfGame game;
    game.title_id = item.str("title_id");
    game.concept_id = static_cast<std::int64_t>(item.num("concept_id"));
    game.name = item.str("name");
    game.added_at = static_cast<std::int64_t>(item.num("added_at"));
    if (const panplay::Json* variants = item.get("variants")) {
      for (const auto& entry : variants->array) {
        ShelfVariant variant;
        variant.format = entry.str("format");
        variant.surl = entry.str("surl");
        variant.pwd = entry.str("pwd");
        variant.version = entry.str("version");
        variant.firmware = entry.str("firmware");
        variant.size = static_cast<std::int64_t>(entry.num("size"));
        if (const panplay::Json* alive = entry.get("alive"); alive && alive->type == panplay::Json::Type::boolean) {
          variant.alive = alive->boolean ? 1 : 0;
        }
        variant.checked_at = static_cast<std::int64_t>(entry.num("checked_at"));
        if (const panplay::Json* review = entry.get("review"); review && review->type == panplay::Json::Type::object) {
          variant.reviewed = true;
          variant.playable = review->boolean_or("playable", true);
          variant.review_note = review->str("note");
        }
        variant.plan = parse_plan(entry.get("plan"));
        if (variant.format.empty() || variant.surl.empty()) continue;
        game.variants.push_back(std::move(variant));
      }
    }
    if (game.title_id.empty() || game.variants.empty()) continue;
    library->games.push_back(std::move(game));
  }
  return library;
}

std::optional<ShelfPlan> load_shelf_plan(panplay::DrivePlugin& drive, const std::string& title_id,
                                         const ShelfVariant& variant, std::string* error) {
  if (!variant.plan) return std::nullopt;
  if (variant.plan->complete()) return variant.plan;
  const int rev = variant.plan->rev;
  const auto parse = [&](const std::string& text) -> std::optional<ShelfPlan> {
    const panplay::Json json = panplay::Json::parse(text, nullptr);
    auto plan = parse_plan(&json);
    if (!plan || !plan->complete() || plan->rev != rev) return std::nullopt;
    return plan;
  };
  const auto cache = panplay::app_data_dir() / "shelf-plans" / (variant.surl + ".json");
  {
    std::ifstream in(cache, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (auto plan = parse(buffer.str())) return plan;
  }
  std::string list_error;
  const auto entries = drive.list(kLibraryDir + "/编排/" + title_id, &list_error);
  if (!list_error.empty()) {
    if (error) *error = "取编排失败：" + list_error;
    return std::nullopt;
  }
  for (const auto& entry : entries) {
    if (entry.is_dir || entry.name != variant.surl + ".json") continue;
    if (entry.size <= 0 || entry.size > kMaxFileBytes) break;
    const panplay::ReadResult read = drive.read_at(entry.file_id, 0, static_cast<std::size_t>(entry.size));
    if (!read.error.empty()) {
      if (error) *error = "读编排失败：" + read.error;
      return std::nullopt;
    }
    const std::string text(read.bytes.begin(), read.bytes.end());
    auto plan = parse(text);
    if (!plan) {
      if (error) *error = "网盘上的编排和资源库的版本对不上";
      return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache.parent_path(), ec);
    std::ofstream(cache, std::ios::binary | std::ios::trunc) << text;
    return plan;
  }
  if (error) *error = "网盘上没有这份编排";
  return std::nullopt;
}

std::string shelf_variant_dir(const std::string& format) { return format == "unknown" ? "@other" : "@" + format; }

std::string shelf_format_of(const std::string& variant_format) {
  return variant_format == "other" ? "unknown" : variant_format;
}

void start_shelf_library_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive,
                              std::function<void()> on_change) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) return;
    started = true;
    refreshed_at = Clock::now();
    if (auto cached = parse_shelf_library(read_cache(), nullptr)) current = std::move(cached);
  }
  std::thread([open_drive = std::move(open_drive), on_change = std::move(on_change)] {
    sync_loop(open_drive, on_change);
  }).detach();
}

void refresh_shelf_library_if_stale() {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (refresh_wanted || Clock::now() - refreshed_at < kStaleAfter) return;
    refresh_wanted = true;
  }
  wake.notify_all();
}

std::shared_ptr<const ShelfLibrary> shelf_library() {
  std::lock_guard<std::mutex> lock(mu);
  return current;
}

void use_shelf_library(std::shared_ptr<const ShelfLibrary> library) {
  std::lock_guard<std::mutex> lock(mu);
  current = std::move(library);
}

std::string shelf_library_status_json(const std::string& library_path) {
  std::lock_guard<std::mutex> lock(mu);
  const auto trimmed = [](std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
  };
  const bool mismatch = current && !current->library_dir.empty() && trimmed(current->library_dir) != trimmed(library_path);
  return "{\"loaded\":" + std::string(current ? "true" : "false") +
         ",\"count\":" + std::to_string(current ? current->games.size() : 0) +
         ",\"generated_at\":" + std::to_string(current ? current->generated_at : 0) +
         ",\"fetched_at\":" + std::to_string(fetched_at) + ",\"library_dir\":\"" +
         panplay::json_escape(current ? current->library_dir : "") + "\",\"mismatch\":" + (mismatch ? "true" : "false") +
         ",\"error\":\"" + panplay::json_escape(last_error) + "\"}";
}
