#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace panplay {

enum class CatalogSyncPhase { idle, updating };

struct CatalogSyncInfo {
  CatalogSyncPhase phase = CatalogSyncPhase::idle;
  std::string last_error;
  std::int64_t last_attempt_unix = 0;
  std::int64_t last_success_unix = 0;
  int stale_days = 7;
  int check_interval_hours = 24;
};

class CatalogSync {
 public:
  static CatalogSync& instance();

  void start(int stale_days = 7, int check_interval_hours = 24);
  CatalogSyncInfo info() const;

 private:
  CatalogSync() = default;

  bool needs_update() const;
  void sync_once();
  void run_loop();

  int stale_days_ = 7;
  int check_interval_hours_ = 24;
  bool started_ = false;
  bool stop_ = false;
  CatalogSyncInfo info_;
  mutable std::mutex mu_;
  std::thread worker_;
};

bool fetch_and_write_catalog(const std::string& output_path, const std::string& source_url, std::string* error);

}  // namespace panplay
