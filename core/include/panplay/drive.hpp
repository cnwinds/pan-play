#pragma once

#include "panplay/abort.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace panplay {

class CancelToken {
 public:
  void cancel() { cancelled_.store(true); }
  bool cancelled() const { return cancelled_.load(); }

 private:
  std::atomic<bool> cancelled_{false};
};

// 从某个偏移开始顺序读一个网盘文件。
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  // 返回读到的字节数，0 表示文件结束。出错时设置 error。
  virtual std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) = 0;
  // 可以从其他线程调用，让阻塞中的 read 尽快返回。
  virtual void abort() {}
};

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
  // 手机已扫码，还没点确认。
  bool scanned = false;
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
  // 少量随机读，例如 ZIP 末尾目录。大段数据用 open_stream。
  virtual ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) = 0;
  // 默认实现用 read_at 按 1 MB 分段读；网盘插件应该覆盖成一次请求顺序读到底。
  // abort 可选：另一个线程调用 abort->abort() 时，还在建立中的请求立即失败返回。
  virtual std::unique_ptr<ByteStream> open_stream(const std::string& file_id, std::int64_t offset, std::string* error,
                                                  AbortSignal* abort = nullptr);
  // 只读 [offset, offset + length)。读完整段后连接可以复用。默认实现直接用 open_stream，由调用方只读到 length。
  virtual std::unique_ptr<ByteStream> open_range(const std::string& file_id, std::int64_t offset, std::int64_t length,
                                                 std::string* error, AbortSignal* abort = nullptr);
};

struct BaiduConfig {
  std::string session_path;
};

std::unique_ptr<DrivePlugin> make_baidu_drive(BaiduConfig config);

// 插件在设置页要用户填的一项参数。
struct DriveField {
  std::string key;
  std::string label;
  bool secret = false;
  std::string hint;
};

struct DriveKind {
  std::string id;
  std::string name;
  std::string intro;
  std::vector<DriveField> fields;
};

// 随版本发布的全部网盘插件，第一个是默认插件。
const std::vector<DriveKind>& drive_kinds();
const DriveKind* find_drive_kind(const std::string& id);
// values 按 DriveField::key 取值。id 不认识时返回空指针。
std::unique_ptr<DrivePlugin> make_drive(const std::string& id, const std::map<std::string, std::string>& values,
                                        const std::string& session_path);

}  // namespace panplay
