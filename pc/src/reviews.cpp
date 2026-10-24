#include "reviews.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace {

struct Device {
  std::string name;
  std::map<std::string, GameReview> reviews;
};

std::mutex mu;
bool loaded = false;
std::map<std::string, GameReview> reviews;
std::map<std::string, Device> devices;
// 本机评价每改一次加一；传到网盘后记下传的是哪一次，两者不等就要再传。
std::uint64_t revision = 0;
std::uint64_t uploaded_revision = 0;

std::filesystem::path reviews_path() { return panplay::app_data_dir() / "reviews.json"; }
std::filesystem::path devices_path() { return panplay::app_data_dir() / "reviews-devices.json"; }

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string write_text(const std::filesystem::path& path, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) return "评价没保存上";
  }
  std::filesystem::rename(tmp, path, ec);
  return ec ? "评价没保存上" : "";
}

GameReview parse_review(const panplay::Json& node) {
  GameReview review;
  review.fingerprint = node.str("fingerprint");
  review.title_id = node.str("title_id");
  review.name = node.str("name");
  review.playable = node.boolean_or("playable", true);
  review.note = node.str("note");
  review.firmware = node.str("firmware");
  review.format = node.str("format");
  review.at = static_cast<std::int64_t>(node.num("at"));
  review.removed = node.boolean_or("removed", false);
  if (const panplay::Json* layers = node.get("layers")) {
    for (const auto& layer : layers->array) review.layers.push_back({layer.str("fingerprint"), layer.str("name")});
  }
  if (const panplay::Json* parts = node.get("parts")) {
    for (const auto& part : parts->array) {
      review.parts.push_back({part.str("md5"), static_cast<std::int64_t>(part.num("size")), part.str("name")});
    }
  }
  return review;
}

std::string reviews_array(const std::map<std::string, GameReview>& list) {
  std::string json = "[";
  for (const auto& [fingerprint, review] : list) {
    if (json.size() > 1) json += ",";
    json += review_json(review);
  }
  return json + "]";
}

void load_locked() {
  if (loaded) return;
  loaded = true;
  if (const std::string text = read_text(reviews_path()); !text.empty()) {
    std::string error;
    const panplay::Json json = panplay::Json::parse(text, &error);
    const panplay::Json* list = json.get("reviews");
    if (!error.empty() || list == nullptr) {
      panplay::log_warn("评价文件读不了，当作空的：" + error);
    } else {
      for (const auto& node : list->array) {
        GameReview review = parse_review(node);
        if (!review.fingerprint.empty()) reviews[review.fingerprint] = std::move(review);
      }
      // 同步之前的版本存的没有这一项，里面的评价都还没传过。
      if (json.boolean_or("pending_upload", !reviews.empty())) revision = 1;
    }
  }
  if (const std::string text = read_text(devices_path()); !text.empty()) {
    const panplay::Json json = panplay::Json::parse(text, nullptr);
    if (const panplay::Json* list = json.get("devices")) {
      for (const auto& node : list->array) {
        Device& device = devices[node.str("id")];
        device.name = node.str("name");
        if (const panplay::Json* items = node.get("reviews")) {
          for (const auto& item : items->array) {
            GameReview review = parse_review(item);
            review.device = device.name;
            if (!review.fingerprint.empty()) device.reviews[review.fingerprint] = std::move(review);
          }
        }
      }
    }
  }
}

std::string save_locked() {
  return write_text(reviews_path(), std::string("{\"version\":2,\"pending_upload\":") +
                                        (revision != uploaded_revision ? "true" : "false") +
                                        ",\"reviews\":" + reviews_array(reviews) + "}");
}

void save_devices_locked() {
  using panplay::json_escape;
  std::string json = "{\"devices\":[";
  bool first = true;
  for (const auto& [id, device] : devices) {
    if (!first) json += ",";
    first = false;
    json += "{\"id\":\"" + json_escape(id) + "\",\"name\":\"" + json_escape(device.name) +
            "\",\"reviews\":" + reviews_array(device.reviews) + "}";
  }
  write_text(devices_path(), json + "]}");
}

// 这个指纹现在算数的那条：各设备里最后评的，同一时刻本机的优先。可能是清除记录。
const GameReview* latest_locked(const std::string& fingerprint) {
  const GameReview* best = nullptr;
  if (const auto it = reviews.find(fingerprint); it != reviews.end()) best = &it->second;
  for (const auto& [id, device] : devices) {
    const auto it = device.reviews.find(fingerprint);
    if (it != device.reviews.end() && (best == nullptr || it->second.at > best->at)) best = &it->second;
  }
  return best;
}

}  // namespace

std::optional<GameReview> find_review(const std::string& fingerprint) {
  if (fingerprint.empty()) return std::nullopt;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const GameReview* review = latest_locked(fingerprint);
  if (review == nullptr || review->removed) return std::nullopt;
  return *review;
}

std::vector<GameReview> reviews_of_title(const std::string& title_id, const std::string& except_fingerprint) {
  std::vector<GameReview> found;
  if (title_id.empty()) return found;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  std::map<std::string, const GameReview*> candidates;
  const auto consider = [&](const std::map<std::string, GameReview>& list) {
    for (const auto& [fingerprint, review] : list) {
      if (review.title_id == title_id && fingerprint != except_fingerprint) candidates[fingerprint] = nullptr;
    }
  };
  consider(reviews);
  for (const auto& [id, device] : devices) consider(device.reviews);
  for (const auto& [fingerprint, unused] : candidates) {
    (void)unused;
    const GameReview* review = latest_locked(fingerprint);
    if (review != nullptr && !review->removed) found.push_back(*review);
  }
  std::sort(found.begin(), found.end(), [](const GameReview& a, const GameReview& b) { return a.at > b.at; });
  return found;
}

std::string save_review(const GameReview& review) {
  if (review.fingerprint.empty()) return "还不知道这个游戏的压缩包，先打开详情读一下";
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  GameReview mine = review;
  mine.device.clear();
  mine.removed = false;
  // 别的设备时钟快了也要压过它刚评的那条，不然这次改了不算数。
  if (const GameReview* current = latest_locked(review.fingerprint)) mine.at = std::max(mine.at, current->at + 1);
  reviews[review.fingerprint] = std::move(mine);
  ++revision;
  return save_locked();
}

std::string remove_review(const std::string& fingerprint) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const GameReview* current = latest_locked(fingerprint);
  if (current == nullptr || current->removed) return "";
  GameReview tombstone;
  tombstone.fingerprint = fingerprint;
  tombstone.title_id = current->title_id;
  tombstone.removed = true;
  tombstone.at = std::max<std::int64_t>(static_cast<std::int64_t>(std::time(nullptr)), current->at + 1);
  reviews[fingerprint] = std::move(tombstone);
  ++revision;
  return save_locked();
}

bool fill_review_parts(const std::string& fingerprint, const std::vector<ReviewPart>& parts) {
  if (parts.empty()) return false;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const auto it = reviews.find(fingerprint);
  if (it == reviews.end() || it->second.removed || !it->second.parts.empty()) return false;
  it->second.parts = parts;
  ++revision;
  save_locked();
  return true;
}

std::string review_json(const GameReview& review) {
  using panplay::json_escape;
  std::string json = "{\"fingerprint\":\"" + json_escape(review.fingerprint) + "\",\"title_id\":\"" +
                     json_escape(review.title_id) + "\",\"name\":\"" + json_escape(review.name) + "\",\"playable\":" +
                     (review.playable ? "true" : "false") + ",\"note\":\"" + json_escape(review.note) +
                     "\",\"firmware\":\"" + json_escape(review.firmware) + "\",\"at\":" + std::to_string(review.at) +
                     ",\"layers\":[";
  for (std::size_t i = 0; i < review.layers.size(); ++i) {
    if (i > 0) json += ",";
    json += "{\"fingerprint\":\"" + json_escape(review.layers[i].fingerprint) + "\",\"name\":\"" +
            json_escape(review.layers[i].name) + "\"}";
  }
  json += "],\"parts\":[";
  for (std::size_t i = 0; i < review.parts.size(); ++i) {
    if (i > 0) json += ",";
    json += "{\"md5\":\"" + json_escape(review.parts[i].md5) + "\",\"size\":" + std::to_string(review.parts[i].size) +
            ",\"name\":\"" + json_escape(review.parts[i].name) + "\"}";
  }
  json += "]";
  if (!review.format.empty()) json += ",\"format\":\"" + json_escape(review.format) + "\"";
  if (review.removed) json += ",\"removed\":true";
  if (!review.device.empty()) json += ",\"device\":\"" + json_escape(review.device) + "\"";
  return json + "}";
}

std::optional<ReviewUpload> pending_review_upload(const std::string& device_id, const std::string& device_name) {
  using panplay::json_escape;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (revision == uploaded_revision) return std::nullopt;
  ReviewUpload upload;
  upload.revision = revision;
  upload.document = "{\"version\":1,\"device\":{\"id\":\"" + json_escape(device_id) + "\",\"name\":\"" +
                    json_escape(device_name) + "\"},\"updated_at\":" +
                    std::to_string(static_cast<std::int64_t>(std::time(nullptr))) +
                    ",\"reviews\":" + reviews_array(reviews) + "}";
  return upload;
}

void mark_reviews_uploaded(std::uint64_t uploaded) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (uploaded <= uploaded_revision) return;
  uploaded_revision = uploaded;
  save_locked();
}

void set_device_reviews(const std::string& device_id, const std::string& device_name,
                        const std::vector<GameReview>& list) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  Device device;
  device.name = device_name;
  for (GameReview review : list) {
    if (review.fingerprint.empty()) continue;
    review.device = device_name;
    device.reviews[review.fingerprint] = std::move(review);
  }
  devices[device_id] = std::move(device);
  save_devices_locked();
}

void keep_devices(const std::vector<std::string>& device_ids) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  bool changed = false;
  for (auto it = devices.begin(); it != devices.end();) {
    if (std::find(device_ids.begin(), device_ids.end(), it->first) == device_ids.end()) {
      it = devices.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) save_devices_locked();
}

std::vector<GameReview> parse_reviews(const std::string& document, std::string* device_id, std::string* device_name,
                                      std::string* error) {
  std::vector<GameReview> out;
  std::string parse_error;
  const panplay::Json json = panplay::Json::parse(document, &parse_error);
  const panplay::Json* device = json.get("device");
  const panplay::Json* list = json.get("reviews");
  if (!parse_error.empty() || device == nullptr || list == nullptr) {
    if (error) *error = parse_error.empty() ? "不是盘游的评价文件" : parse_error;
    return out;
  }
  if (device_id) *device_id = device->str("id");
  if (device_name) *device_name = device->str("name");
  for (const auto& node : list->array) {
    GameReview review = parse_review(node);
    if (!review.fingerprint.empty()) out.push_back(std::move(review));
  }
  return out;
}
