#include "reviews.hpp"

#include "panplay/app_paths.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace {

std::mutex mu;
bool loaded = false;
std::map<std::string, GameReview> reviews;

std::filesystem::path reviews_path() { return panplay::app_data_dir() / "reviews.json"; }

void load_locked() {
  if (loaded) return;
  loaded = true;
  std::ifstream in(reviews_path(), std::ios::binary);
  if (!in) return;
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string error;
  const panplay::Json json = panplay::Json::parse(buffer.str(), &error);
  const panplay::Json* list = json.get("reviews");
  if (!error.empty() || list == nullptr) {
    panplay::log_warn("评价文件读不了，当作空的：" + error);
    return;
  }
  for (const auto& node : list->array) {
    GameReview review;
    review.fingerprint = node.str("fingerprint");
    if (review.fingerprint.empty()) continue;
    review.title_id = node.str("title_id");
    review.name = node.str("name");
    review.playable = node.boolean_or("playable", true);
    review.note = node.str("note");
    review.firmware = node.str("firmware");
    review.at = static_cast<std::int64_t>(node.num("at"));
    if (const panplay::Json* layers = node.get("layers")) {
      for (const auto& layer : layers->array) review.layers.push_back({layer.str("fingerprint"), layer.str("name")});
    }
    reviews[review.fingerprint] = std::move(review);
  }
}

std::string save_locked() {
  std::string json = "{\"version\":1,\"reviews\":[";
  bool first = true;
  for (const auto& [fingerprint, review] : reviews) {
    if (!first) json += ",";
    first = false;
    json += review_json(review);
  }
  json += "]}";
  std::error_code ec;
  std::filesystem::create_directories(reviews_path().parent_path(), ec);
  std::filesystem::path tmp = reviews_path();
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return "评价没保存上";
  }
  std::filesystem::rename(tmp, reviews_path(), ec);
  return ec ? "评价没保存上" : "";
}

}  // namespace

std::optional<GameReview> find_review(const std::string& fingerprint) {
  if (fingerprint.empty()) return std::nullopt;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  const auto it = reviews.find(fingerprint);
  if (it == reviews.end()) return std::nullopt;
  return it->second;
}

std::vector<GameReview> reviews_of_title(const std::string& title_id, const std::string& except_fingerprint) {
  std::vector<GameReview> found;
  if (title_id.empty()) return found;
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  for (const auto& [fingerprint, review] : reviews) {
    if (review.title_id == title_id && fingerprint != except_fingerprint) found.push_back(review);
  }
  std::sort(found.begin(), found.end(), [](const GameReview& a, const GameReview& b) { return a.at > b.at; });
  return found;
}

std::string save_review(const GameReview& review) {
  if (review.fingerprint.empty()) return "还不知道这个游戏的压缩包，先打开详情读一下";
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  reviews[review.fingerprint] = review;
  return save_locked();
}

std::string remove_review(const std::string& fingerprint) {
  std::lock_guard<std::mutex> lock(mu);
  load_locked();
  if (reviews.erase(fingerprint) == 0) return "";
  return save_locked();
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
  return json + "]}";
}
