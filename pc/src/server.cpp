#include "server.hpp"

#include "settings.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/catalog.hpp"
#include "panplay/catalog_sync.hpp"
#include "panplay/drive.hpp"
#include "panplay/install.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"
#include "panplay/store_meta.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

std::string session_path() { return (panplay::app_data_dir() / "baidu-session.json").u8string(); }

// 进程内只留一个百度插件实例，下载地址缓存和令牌刷新都在它里面；密钥改了才换新实例。
std::shared_ptr<panplay::DrivePlugin> open_drive() {
  static std::mutex mu;
  static std::shared_ptr<panplay::DrivePlugin> drive;
  static std::string key;
  const AppSettings settings = load_settings();
  const std::string wanted = settings.app_key + "\n" + settings.secret_key;
  std::lock_guard<std::mutex> lock(mu);
  if (!drive || key != wanted) {
    panplay::BaiduConfig config;
    config.app_key = settings.app_key;
    config.secret_key = settings.secret_key;
    config.session_path = session_path();
    drive = panplay::make_baidu_drive(std::move(config));
    key = wanted;
  }
  return drive;
}

std::filesystem::path ui_root() {
  const std::filesystem::path found = panplay::executable_dir();
  const std::filesystem::path exe = found.empty() ? std::filesystem::path(".") : found;
  const std::filesystem::path candidates[] = {exe / "ui", exe / ".." / "ui", exe / ".." / ".." / "ui", std::filesystem::path("ui")};
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate / "index.html")) return candidate;
  }
  return "ui";
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

struct Job {
  std::mutex mu;
  bool running = false;
  std::string path;
  panplay::InstallResult result;
  std::shared_ptr<panplay::CancelToken> cancel;
  std::thread worker;
};

Job job;

void send_all(SOCKET socket, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int n = ::send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (n <= 0) return;
    sent += static_cast<std::size_t>(n);
  }
}

void reply(SOCKET socket, int status, const std::string& type, const std::string& body, const std::string& extra = "") {
  const std::string text = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                           "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
                           extra + "Connection: close\r\n\r\n" + body;
  send_all(socket, text);
}

void reply_json(SOCKET socket, int status, const std::string& body) { reply(socket, status, "application/json; charset=utf-8", body); }

std::string json_error(const std::string& message) { return "{\"error\":\"" + panplay::json_escape(message) + "\"}"; }

std::string game_state(panplay::InstallSink& sink, const std::string& prefix, const panplay::GameFolder& game) {
  bool only_unsupported = !game.sets.empty();
  for (const auto& set : game.sets) {
    if (set.format == "zip" || set.format == "rar") only_unsupported = false;
  }
  if (game.sets.empty()) return "empty";
  if (only_unsupported) return "unsupported";
  const std::string folder = game.title_id.empty() ? game.name : game.title_id;
  const std::string marker = panplay::join_storage_path(panplay::join_storage_path(prefix, folder), ".panplay-done.json");
  std::string error;
  if (sink.file_size(marker, &error) >= 0 && error.empty()) return "installed";
  const std::string journal =
      panplay::join_storage_path(panplay::join_storage_path(prefix, ".panplay-staging/" + folder), ".panplay-journal.jsonl");
  error.clear();
  if (sink.file_size(journal, &error) >= 0 && error.empty()) return "continue";
  return "install";
}

std::string formats_of(const panplay::GameFolder& game) {
  std::string formats;
  for (const auto& set : game.sets) {
    if (formats.find(set.format) == std::string::npos) {
      if (!formats.empty()) formats += "+";
      formats += set.format;
    }
  }
  return formats.empty() ? "-" : formats;
}

struct ShelfPlacement {
  std::unordered_set<std::string> installed_dirs;
  std::unordered_set<std::string> staging_dirs;
  std::string active_path;
};

ShelfPlacement read_shelf_placement(panplay::InstallSink& sink, const std::string& prefix) {
  ShelfPlacement placement;
  {
    std::lock_guard<std::mutex> lock(job.mu);
    if (job.running) placement.active_path = job.path;
  }
  std::string error;
  for (const auto& name : sink.list_children(prefix, &error)) {
    if (name != ".panplay-staging") placement.installed_dirs.insert(name);
  }
  error.clear();
  for (const auto& name : sink.list_children(panplay::join_storage_path(prefix, ".panplay-staging"), &error)) {
    placement.staging_dirs.insert(name);
  }
  return placement;
}

std::string shelf_state(const ShelfPlacement& placement, const std::string& name, const std::string& title_id,
                        const std::string& path) {
  const std::string folder = title_id.empty() ? name : title_id;
  if (!placement.active_path.empty() && placement.active_path == path) return "installing";
  if (placement.staging_dirs.count(folder) != 0) return "continue";
  if (placement.installed_dirs.count(folder) != 0) return "installed";
  return "install";
}

std::string catalog_status_json() {
  const panplay::GameCatalog& catalog = panplay::GameCatalog::instance();
  const panplay::CatalogMeta meta = catalog.meta();
  const panplay::CatalogSyncInfo sync = panplay::CatalogSync::instance().info();
  const char* phase = sync.phase == panplay::CatalogSyncPhase::updating ? "updating" : "idle";
  return std::string("{\"loaded\":") + (catalog.loaded() ? "true" : "false") + ",\"count\":" +
         std::to_string(catalog.size()) + ",\"stale\":" + (catalog.is_stale(sync.stale_days) ? "true" : "false") +
         ",\"version\":" + std::to_string(meta.version) + ",\"path\":\"" + panplay::json_escape(meta.path) +
         "\",\"source\":\"" + panplay::json_escape(meta.source) + "\",\"generated_at\":\"" +
         panplay::json_escape(meta.generated_at) + "\",\"generated_from\":\"" +
         panplay::json_escape(meta.generated_from) + "\",\"file_mtime\":" + std::to_string(meta.file_mtime_unix) +
         ",\"sync\":{\"phase\":\"" + std::string(phase) + "\",\"last_error\":\"" +
         panplay::json_escape(sync.last_error) + "\",\"last_attempt\":" + std::to_string(sync.last_attempt_unix) +
         ",\"last_success\":" + std::to_string(sync.last_success_unix) + ",\"auto\":true,\"stale_days\":" +
         std::to_string(sync.stale_days) + ",\"check_interval_hours\":" + std::to_string(sync.check_interval_hours) +
         "}}";
}

panplay::StoreMetaCache* store_meta = nullptr;

using NameOverrides = std::map<std::string, std::string>;

std::string name_key(const std::string& name, const std::string& title_id) { return title_id.empty() ? name : title_id; }

// 展示名：手动改名 > 商店名称 > PlayStation-Titles 英文名 > 目录名（由页面兜底）。
std::string game_meta_fields(const std::string& name, const std::string& title_id, const NameOverrides& overrides,
                             bool detail) {
  panplay::CatalogMatch match;
  if (!title_id.empty()) match = panplay::GameCatalog::instance().lookup(title_id, name);
  const std::int64_t concept_id = match.found ? match.entry.concept_id : 0;

  panplay::StoreMeta meta;
  std::string meta_state = "none";
  if (concept_id > 0 && store_meta != nullptr) meta_state = panplay::store_meta_state_name(store_meta->lookup(concept_id, &meta));

  std::string display_name;
  std::string name_source;
  const auto custom = overrides.find(name_key(name, title_id));
  if (custom != overrides.end()) {
    display_name = custom->second;
    name_source = "user";
  } else if (meta.found) {
    display_name = meta.name;
    name_source = "store";
  } else if (match.found) {
    display_name = match.entry.name;
    name_source = "catalog";
  }

  std::string cover_url;
  if (meta.has_cover) cover_url = "/api/cover/" + std::to_string(concept_id) + "?v=" + std::to_string(meta.fetched_at);
  else if (match.found) cover_url = match.entry.cover_url;

  std::string json = ",\"display_name\":\"" + panplay::json_escape(display_name) + "\",\"name_source\":\"" + name_source +
                     "\",\"store_locale\":\"" + panplay::json_escape(meta.found ? meta.locale : "") +
                     "\",\"catalog_name\":\"" + panplay::json_escape(match.found ? match.entry.name : "") +
                     "\",\"cover_url\":\"" + panplay::json_escape(cover_url) + "\",\"meta_state\":\"" + meta_state +
                     "\",\"concept_id\":" + std::to_string(concept_id) + ",\"region\":\"" +
                     panplay::json_escape(match.entry.region) + "\",\"publisher_id\":\"" +
                     panplay::json_escape(match.entry.publisher_id) + "\",\"platform\":\"" +
                     panplay::json_escape(match.entry.platform) + "\",\"catalog_matched\":" +
                     (match.found ? "true" : "false") + ",\"catalog_match_by\":\"" + panplay::json_escape(match.match_by) + "\"";
  if (detail) {
    json += ",\"publisher\":\"" + panplay::json_escape(meta.publisher) + "\",\"genres\":\"" +
            panplay::json_escape(meta.genres) + "\",\"release_date\":\"" + panplay::json_escape(meta.release_date) +
            "\",\"description\":\"" + panplay::json_escape(meta.description) + "\"";
  }
  return json;
}

std::string meta_json(const std::string& name, const NameOverrides& overrides, bool detail) {
  const std::string title_id = panplay::find_title_id(name);
  return "{\"name\":\"" + panplay::json_escape(name) + "\",\"title_id\":\"" + panplay::json_escape(title_id) + "\"" +
         game_meta_fields(name, title_id, overrides, detail) + "}";
}

std::string folder_json(const std::string& name, const std::string& path, const std::string& state,
                        const NameOverrides& overrides) {
  const std::string title_id = panplay::find_title_id(name);
  return "{\"name\":\"" + panplay::json_escape(name) + "\",\"path\":\"" + panplay::json_escape(path) + "\",\"title_id\":\"" +
         panplay::json_escape(title_id) + "\",\"archive_bytes\":0,\"formats\":\"\",\"state\":\"" + state + "\"" +
         game_meta_fields(name, title_id, overrides, false) + "}";
}

std::string folder_name_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string image_type(const std::string& bytes) {
  if (bytes.size() >= 4 && bytes.compare(1, 3, "PNG") == 0) return "image/png";
  if (bytes.size() >= 12 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
  return "image/jpeg";
}

std::string games_json(panplay::DrivePlugin& drive) {
  const AppSettings settings = load_settings();
  std::string error;
  const auto children = drive.list(settings.library_path, &error);
  if (!error.empty()) return json_error(error);
  panplay::Destination destination;
  const bool dest_ready = !settings.dest.empty() && panplay::parse_destination(settings.dest, &destination, &error);
  std::unique_ptr<panplay::InstallSink> sink;
  ShelfPlacement placement;
  if (dest_ready) {
    sink = panplay::open_destination(destination, &error);
    if (sink) placement = read_shelf_placement(*sink, settings.prefix);
  }
  const NameOverrides overrides = load_name_overrides();
  std::string json = "{\"games\":[";
  bool first = true;
  for (const auto& child : children) {
    if (!child.is_dir) continue;
    if (!first) json += ",";
    first = false;
    const std::string path = child.path.empty() ? panplay::join_storage_path(settings.library_path, child.name) : child.path;
    const std::string title_id = panplay::find_title_id(child.name);
    std::string state = "need-settings";
    if (sink) state = shelf_state(placement, child.name, title_id, path);
    else if (dest_ready) state = "install";
    json += folder_json(child.name, path, state, overrides);
  }
  json += "]}";
  return json;
}

std::string one_game_json(panplay::DrivePlugin& drive, const std::string& path) {
  const AppSettings settings = load_settings();
  std::string error;
  const auto files = panplay::collect_archives(drive, path, &error);
  if (!error.empty()) return json_error(error);
  panplay::GameFolder game;
  game.name = folder_name_of(path);
  game.path = path;
  game.title_id = panplay::find_title_id(game.name);
  game.sets = panplay::group_archives(files);
  for (const auto& set : game.sets) {
    for (const auto& part : set.parts) game.archive_bytes += part.size;
  }
  std::string state = "need-settings";
  std::unique_ptr<panplay::InstallSink> sink;
  panplay::Destination destination;
  if (!settings.dest.empty() && panplay::parse_destination(settings.dest, &destination, &error)) {
    sink = panplay::open_destination(destination, &error);
  }
  if (sink) state = game_state(*sink, settings.prefix, game);
  else if (!settings.dest.empty()) state = "install";
  {
    std::lock_guard<std::mutex> lock(job.mu);
    if (job.running && job.path == path) state = "installing";
  }
  return "{\"name\":\"" + panplay::json_escape(game.name) + "\",\"path\":\"" + panplay::json_escape(game.path) +
         "\",\"title_id\":\"" + panplay::json_escape(game.title_id) + "\",\"archive_bytes\":" +
         std::to_string(game.archive_bytes) + ",\"formats\":\"" + panplay::json_escape(formats_of(game)) +
         "\",\"state\":\"" + state + "\"" + game_meta_fields(game.name, game.title_id, load_name_overrides(), true) + "}";
}

void publish_progress(const panplay::InstallResult& snapshot) {
  std::lock_guard<std::mutex> lock(job.mu);
  if (!job.running || (job.cancel && job.cancel->cancelled())) return;
  job.result.message = snapshot.message;
  job.result.files_done = snapshot.files_done;
  job.result.files_total = snapshot.files_total;
}

void run_install(std::string path, std::shared_ptr<panplay::CancelToken> cancel) {
  panplay::InstallResult result;
  try {
    panplay::InstallResult starting;
    starting.message = "正在读取压缩包…";
    publish_progress(starting);
    auto drive = open_drive();
    const AppSettings settings = load_settings();
    std::string error;
    panplay::Destination destination;
    if (!panplay::parse_destination(settings.dest, &destination, &error)) {
      result.status = "error";
      result.message = error.empty() ? "先在设置里填写存放位置" : error;
    } else {
      auto sink = panplay::open_destination(destination, &error);
      if (!sink) {
        result.status = "error";
        result.message = error.empty() ? "无法连接存放位置" : error;
      } else {
        panplay::InstallRequest request;
        request.source_dir = path;
        request.dest_prefix = settings.prefix;
        request.on_progress = publish_progress;
        request.cancel = cancel.get();
        result = panplay::install_game(*drive, *sink, request);
      }
    }
  } catch (const std::exception& ex) {
    result.status = "error";
    result.message = std::string("安装失败: ") + ex.what();
  } catch (...) {
    result.status = "error";
    result.message = "安装失败";
  }
  if (result.status.empty()) {
    result.status = "error";
    result.message = result.message.empty() ? "安装失败" : result.message;
  }
  std::lock_guard<std::mutex> lock(job.mu);
  job.result = result;
  job.running = false;
}

std::string request_target(const std::string& header) {
  const auto line_end = header.find("\r\n");
  const std::string line = header.substr(0, line_end);
  const auto first = line.find(' ');
  const auto second = line.find(' ', first == std::string::npos ? 0 : first + 1);
  if (first == std::string::npos || second == std::string::npos) return "/";
  return line.substr(first + 1, second - first - 1);
}

std::string request_method(const std::string& header) {
  const auto space = header.find(' ');
  if (space == std::string::npos) return "";
  return header.substr(0, space);
}

std::string header_value(const std::string& header, const std::string& name) {
  std::size_t start = header.find("\r\n");
  while (start != std::string::npos) {
    start += 2;
    const std::size_t end = header.find("\r\n", start);
    const std::string line = header.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const std::size_t colon = line.find(':');
    if (colon == name.size()) {
      bool same = true;
      for (std::size_t i = 0; i < colon; ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
          same = false;
          break;
        }
      }
      if (same) {
        std::size_t value = colon + 1;
        while (value < line.size() && line[value] == ' ') ++value;
        return line.substr(value);
      }
    }
    start = end;
  }
  return "";
}

bool is_loopback_host(std::string host) {
  if (host.rfind("http://", 0) == 0) host = host.substr(7);
  const std::size_t colon = host.rfind(':');
  if (colon != std::string::npos && host.find(']') == std::string::npos) host = host.substr(0, colon);
  return host == "127.0.0.1" || host == "localhost";
}

// 只服务本机页面：Host 挡 DNS 重绑定；POST 要求 JSON 让浏览器跨站请求必须预检，而这里不响应预检。
bool trusted_request(const std::string& header, const std::string& method) {
  if (!is_loopback_host(header_value(header, "Host"))) return false;
  if (method != "POST") return true;
  const std::string origin = header_value(header, "Origin");
  if (!origin.empty() && !is_loopback_host(origin)) return false;
  return header_value(header, "Content-Type").rfind("application/json", 0) == 0;
}

void handle_client(SOCKET client) {
  std::string data;
  char buffer[4096];
  while (data.find("\r\n\r\n") == std::string::npos && data.size() < 65536) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) return;
    data.append(buffer, buffer + n);
  }
  const auto split = data.find("\r\n\r\n");
  if (split == std::string::npos) return;
  const std::string header = data.substr(0, split);
  std::string body = data.substr(split + 4);
  std::size_t length = 0;
  const auto content = header.find("Content-Length:");
  if (content != std::string::npos) {
    length = static_cast<std::size_t>(std::strtoul(header.c_str() + content + 15, nullptr, 10));
  }
  while (body.size() < length && body.size() < 1024 * 1024) {
    const int n = recv(client, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    body.append(buffer, buffer + n);
  }
  body.resize(std::min(body.size(), length));

  const std::string method = request_method(header);
  const std::string target = request_target(header);
  const std::filesystem::path ui = ui_root();
  if (!trusted_request(header, method)) {
    reply_json(client, 403, json_error("只接受本机页面的请求"));
    return;
  }

  if (method == "GET" && (target == "/" || target == "/index.html")) {
    reply(client, 200, "text/html; charset=utf-8", read_file(ui / "index.html"));
    return;
  }
  if (method == "GET" && target == "/app.css") {
    reply(client, 200, "text/css; charset=utf-8", read_file(ui / "app.css"));
    return;
  }
  if (method == "GET" && target == "/app.js") {
    reply(client, 200, "text/javascript; charset=utf-8", read_file(ui / "app.js"));
    return;
  }
  if (method == "GET" && target.rfind("/api/cover/", 0) == 0) {
    const std::string id = target.substr(11, target.find('?') == std::string::npos ? std::string::npos : target.find('?') - 11);
    const bool digits = !id.empty() && id.size() < 19 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); });
    const std::string bytes = digits && store_meta != nullptr ? read_file(store_meta->cover_file(std::stoll(id))) : "";
    if (bytes.empty()) {
      reply_json(client, 404, json_error("没有这张封面"));
      return;
    }
    reply(client, 200, image_type(bytes), bytes, "Cache-Control: max-age=86400\r\n");
    return;
  }
  if (method == "POST" && (target == "/api/meta" || target == "/api/name" || target == "/api/meta/refresh")) {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty()) {
      reply_json(client, 400, json_error(parse_error));
      return;
    }
    if (target == "/api/meta") {
      const NameOverrides overrides = load_name_overrides();
      std::string out = "{\"items\":[";
      const panplay::Json* names = json.get("names");
      if (names != nullptr) {
        for (std::size_t i = 0; i < names->array.size() && i < 500; ++i) {
          if (i > 0) out += ",";
          out += meta_json(names->array[i].text, overrides, false);
        }
      }
      reply_json(client, 200, out + "]}");
      return;
    }
    const std::string name = json.str("name");
    if (name.empty()) {
      reply_json(client, 400, json_error("缺少游戏目录名"));
      return;
    }
    if (target == "/api/name") {
      std::string display = json.str("display_name");
      const auto begin = display.find_first_not_of(" \t\r\n");
      display = begin == std::string::npos ? "" : display.substr(begin, display.find_last_not_of(" \t\r\n") - begin + 1);
      if (display.size() > 400) {
        reply_json(client, 400, json_error("名称太长"));
        return;
      }
      const std::string error = save_name_override(name_key(name, panplay::find_title_id(name)), display);
      if (!error.empty()) {
        reply_json(client, 500, json_error(error));
        return;
      }
    } else {
      const panplay::CatalogMatch match = panplay::GameCatalog::instance().lookup(panplay::find_title_id(name), name);
      if (!match.found || match.entry.concept_id <= 0 || store_meta == nullptr) {
        reply_json(client, 400, json_error("这款游戏没有 conceptId，无法从商店获取"));
        return;
      }
      store_meta->refresh(match.entry.concept_id);
    }
    reply_json(client, 200, meta_json(name, load_name_overrides(), true));
    return;
  }
  if (method == "GET" && target == "/api/catalog") {
    reply_json(client, 200, catalog_status_json());
    return;
  }
  if (method == "POST" && target == "/api/catalog/reload") {
    std::string catalog_error;
    const bool ok = panplay::GameCatalog::instance().reload(&catalog_error);
    if (!ok) {
      reply_json(client, 500, json_error(catalog_error.empty() ? "资料库加载失败" : catalog_error));
      return;
    }
    reply_json(client, 200, catalog_status_json());
    return;
  }
  if (method == "GET" && target == "/api/settings") {
    const AppSettings settings = load_settings();
    reply_json(client, 200,
               "{\"app_key\":\"" + panplay::json_escape(settings.app_key) + "\",\"secret_key\":\"" +
                   panplay::json_escape(settings.secret_key) + "\",\"library_path\":\"" +
                   panplay::json_escape(settings.library_path) + "\",\"dest\":\"" + panplay::json_escape(settings.dest) +
                   "\",\"prefix\":\"" + panplay::json_escape(settings.prefix) + "\"}");
    return;
  }
  if (method == "POST" && target == "/api/settings") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty()) {
      reply_json(client, 400, json_error(parse_error));
      return;
    }
    AppSettings settings = load_settings();
    if (!json.str("app_key").empty() || body.find("app_key") != std::string::npos) settings.app_key = json.str("app_key");
    if (!json.str("secret_key").empty() || body.find("secret_key") != std::string::npos) {
      settings.secret_key = json.str("secret_key");
    }
    if (!json.str("library_path").empty()) settings.library_path = json.str("library_path");
    if (body.find("dest") != std::string::npos) settings.dest = json.str("dest");
    if (!json.str("prefix").empty()) settings.prefix = json.str("prefix");
    const std::string error = save_settings(settings);
    reply_json(client, error.empty() ? 200 : 500, error.empty() ? "{\"ok\":true}" : json_error(error));
    return;
  }
  if (method == "GET" && target == "/api/session") {
    auto drive = open_drive();
    reply_json(client, 200, std::string("{\"logged_in\":") + (drive->session_valid() ? "true" : "false") + "}");
    return;
  }
  if (method == "POST" && target == "/api/login/start") {
    auto drive = open_drive();
    if (drive->session_valid()) {
      reply_json(client, 200, "{\"already\":true}");
      return;
    }
    const panplay::LoginChallenge challenge = drive->start_login();
    if (!challenge.error.empty()) {
      reply_json(client, 400, json_error(challenge.error));
      return;
    }
    std::ofstream out(std::filesystem::path(session_path()).parent_path() / "login-challenge.json", std::ios::trunc);
    out << "{\"device_code\":\"" << panplay::json_escape(challenge.device_code) << "\",\"interval\":"
        << challenge.interval_seconds << ",\"expires_in\":" << challenge.expires_in << ",\"qrcode_url\":\""
        << panplay::json_escape(challenge.qrcode_url) << "\",\"user_code\":\"" << panplay::json_escape(challenge.user_code)
        << "\"}";
    reply_json(client, 200,
               "{\"qrcode_url\":\"" + panplay::json_escape(challenge.qrcode_url) + "\",\"user_code\":\"" +
                   panplay::json_escape(challenge.user_code) + "\",\"interval\":" + std::to_string(challenge.interval_seconds) +
                   ",\"expires_in\":" + std::to_string(challenge.expires_in) + "}");
    return;
  }
  if (method == "POST" && target == "/api/login/poll") {
    auto drive = open_drive();
    if (drive->session_valid()) {
      reply_json(client, 200, "{\"done\":true,\"expired\":false,\"error\":\"\",\"retry_after\":0}");
      return;
    }
    std::ifstream in(std::filesystem::path(session_path()).parent_path() / "login-challenge.json");
    std::stringstream buffer;
    buffer << in.rdbuf();
    const panplay::Json saved = panplay::Json::parse(buffer.str(), nullptr);
    panplay::LoginChallenge challenge;
    challenge.device_code = saved.str("device_code");
    challenge.interval_seconds = static_cast<int>(saved.num("interval"));
    challenge.expires_in = static_cast<int>(saved.num("expires_in"));
    if (challenge.device_code.empty()) {
      reply_json(client, 400, json_error("还没有开始登录"));
      return;
    }
    static std::mutex poll_gate;
    static std::chrono::steady_clock::time_point next_baidu_poll{};
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(poll_gate);
      if (now < next_baidu_poll) {
        const int retry = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(next_baidu_poll - now).count()));
        reply_json(client, 200,
                   std::string("{\"done\":false,\"expired\":false,\"error\":\"\",\"retry_after\":") + std::to_string(retry) + "}");
        return;
      }
    }
    const panplay::LoginPoll poll = drive->poll_login(challenge);
    const int retry = poll.retry_after_seconds > 0 ? poll.retry_after_seconds : std::max(challenge.interval_seconds, 5);
    {
      std::lock_guard<std::mutex> lock(poll_gate);
      next_baidu_poll = std::chrono::steady_clock::now() + std::chrono::seconds(retry);
    }
    reply_json(client, 200,
               std::string("{\"done\":") + (poll.done ? "true" : "false") + ",\"expired\":" + (poll.expired ? "true" : "false") +
                   ",\"error\":\"" + panplay::json_escape(poll.error) + "\",\"retry_after\":" + std::to_string(retry) + "}");
    return;
  }
  if (method == "POST" && target == "/api/game") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty() || json.str("path").empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    auto drive = open_drive();
    const std::string payload = one_game_json(*drive, json.str("path"));
    reply_json(client, payload.rfind("{\"error\"", 0) == 0 ? 400 : 200, payload);
    return;
  }
  if (method == "GET" && target == "/api/games") {
    auto drive = open_drive();
    const std::string json = games_json(*drive);
    reply_json(client, json.rfind("{\"error\"", 0) == 0 ? 400 : 200, json);
    return;
  }
  if (method == "GET" && target == "/api/job") {
    std::lock_guard<std::mutex> lock(job.mu);
    reply_json(client, 200,
               std::string("{\"running\":") + (job.running ? "true" : "false") + ",\"path\":\"" +
                   panplay::json_escape(job.path) + "\",\"status\":\"" + panplay::json_escape(job.result.status) +
                   "\",\"message\":\"" + panplay::json_escape(job.result.message) + "\",\"files_done\":" +
                   std::to_string(job.result.files_done) + ",\"files_total\":" + std::to_string(job.result.files_total) + "}");
    return;
  }
  if (method == "POST" && target == "/api/install") {
    std::string parse_error;
    const panplay::Json json = panplay::Json::parse(body, &parse_error);
    if (!parse_error.empty() || json.str("path").empty()) {
      reply_json(client, 400, json_error(parse_error.empty() ? "缺少游戏目录" : parse_error));
      return;
    }
    std::thread previous;
    {
      std::lock_guard<std::mutex> lock(job.mu);
      if (job.running) {
        reply_json(client, 409, json_error("已经有一个安装在进行"));
        return;
      }
      if (job.worker.joinable()) previous = std::move(job.worker);
      job.running = true;
      job.path = json.str("path");
      job.result = {};
      job.cancel = std::make_shared<panplay::CancelToken>();
      job.worker = std::thread(run_install, job.path, job.cancel);
    }
    if (previous.joinable()) previous.join();
    reply_json(client, 200, "{\"ok\":true}");
    return;
  }
  if (method == "POST" && target == "/api/cancel") {
    std::lock_guard<std::mutex> lock(job.mu);
    if (!job.running || !job.cancel) {
      reply_json(client, 409, json_error("没有正在进行的安装"));
      return;
    }
    job.cancel->cancel();
    job.result.message = "正在取消…";
    reply_json(client, 200, "{\"ok\":true}");
    return;
  }
  reply_json(client, 404, json_error("没有这个页面"));
}

}  // namespace

int run_server(int port) {
  {
    std::string catalog_error;
    if (panplay::GameCatalog::instance().load_default(&catalog_error)) {
      std::cout << "已加载游戏资料库 " << panplay::GameCatalog::instance().size() << " 条\n";
    } else {
      std::cerr << "游戏资料库未加载（将使用目录名占位，后台自动同步中）: " << catalog_error << "\n";
    }
    panplay::CatalogSync::instance().start(7, 24);
    store_meta = new panplay::StoreMetaCache(panplay::app_data_dir());
    store_meta->start();
  }

  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::cerr << "Winsock 初始化失败\n";
    return 1;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL reuse = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port));
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
    std::cerr << "无法在 127.0.0.1:" << port << " 监听\n";
    return 1;
  }
  std::cout << "盘游界面 http://127.0.0.1:" << port << "\n按 Ctrl+C 结束" << std::endl;
  while (true) {
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) break;
    const DWORD timeout_ms = 30000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    std::thread([client] {
      try {
        handle_client(client);
      } catch (const std::exception&) {
        try {
          reply_json(client, 500, json_error("服务器内部错误"));
        } catch (...) {
        }
      } catch (...) {
        try {
          reply_json(client, 500, json_error("服务器内部错误"));
        } catch (...) {
        }
      }
      closesocket(client);
    }).detach();
  }
  return 0;
}
