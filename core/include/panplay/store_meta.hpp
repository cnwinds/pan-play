#pragma once

#include "panplay/http.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace panplay {

struct StoreMeta {
  std::int64_t concept_id = 0;
  bool found = false;
  bool has_cover = false;
  std::string locale;
  std::string name;
  std::string cover_url;
  std::string publisher;
  std::string genres;
  std::string release_date;
  std::string description;
  std::int64_t fetched_at = 0;
};

enum class StoreMetaState { pending, ready, missing, offline };

const char* store_meta_state_name(StoreMetaState state);

using HttpFetch = std::function<HttpResponse(const HttpRequest&)>;

std::string clean_store_name(const std::string& name);
StoreMeta parse_concept_page(const std::string& html, std::int64_t concept_id);

// 依次试港服简体、港服繁体、美服。网络失败或被拒时返回 false，没收录时返回 true 且 found 为 false。
bool fetch_store_meta(std::int64_t concept_id, const HttpFetch& fetch, StoreMeta* out, std::string* error,
                      const std::function<void()>& before_request = {});

class StoreMetaCache {
 public:
  explicit StoreMetaCache(std::filesystem::path dir, HttpFetch fetch = http_send, int min_interval_ms = 1000);
  ~StoreMetaCache();

  void start();
  StoreMetaState lookup(std::int64_t concept_id, StoreMeta* out);
  void refresh(std::int64_t concept_id);
  std::filesystem::path cover_file(std::int64_t concept_id) const;
  bool run_once();

 private:
  void enqueue_locked(std::int64_t concept_id);
  void process(std::int64_t concept_id);
  void throttle();
  void load();
  void save_locked();
  void worker_loop();

  std::filesystem::path dir_;
  HttpFetch fetch_;
  int min_interval_ms_;
  std::chrono::steady_clock::time_point last_request_{};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::int64_t, StoreMeta> entries_;
  std::map<std::int64_t, std::chrono::steady_clock::time_point> retry_after_;
  std::deque<std::int64_t> queue_;
  std::set<std::int64_t> queued_;
  std::set<std::int64_t> in_flight_;
  std::set<std::int64_t> forced_;
  bool stop_ = false;
  std::thread worker_;
};

int run_store_meta_self_test();
int run_store_meta_cli(const std::string& concept_id);

}  // namespace panplay
