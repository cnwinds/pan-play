#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

struct DriveEntry {
  std::string file_id;
  std::string name;
  std::string path;
  bool is_dir = false;
  std::int64_t size = 0;
};

struct LoginChallenge {
  std::string qrcode_url;
  std::string user_code;
  std::string verification_url;
  std::string device_code;
  int interval_seconds = 5;
  int expires_in = 0;
  std::string error;
};

struct LoginPoll {
  bool done = false;
  bool expired = false;
  int retry_after_seconds = 0;
  std::string error;
};

struct ReadResult {
  std::vector<std::uint8_t> bytes;
  int status = 0;
  std::string error;
};

class DrivePlugin {
 public:
  virtual ~DrivePlugin() = default;
  virtual std::string id() const = 0;
  virtual std::string display_name() const = 0;
  virtual LoginChallenge start_login() = 0;
  virtual LoginPoll poll_login(const LoginChallenge& challenge) = 0;
  virtual bool session_valid() = 0;
  virtual void logout() = 0;
  virtual std::vector<DriveEntry> list(const std::string& path, std::string* error) = 0;
  virtual ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) = 0;
};

struct BaiduConfig {
  std::string app_key;
  std::string secret_key;
  std::string session_path;
};

std::unique_ptr<DrivePlugin> make_baidu_drive(BaiduConfig config);

}  // namespace panplay
