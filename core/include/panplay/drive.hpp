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
  // 网盘按文件内容算的校验值（百度的 md5）：改名、挪目录、删了再从同一个分享转存都不变。网盘不给时为空。
  std::string content_hash;
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
  // 写一个几 KB 的小文件（评价同步用），已有的覆盖，上级目录没有时网盘自己建。返回出错原因。
  virtual std::string write_small_file(const std::string& path, const std::string& bytes);
  // 从网盘删掉一个文件或整个文件夹，删掉的进网盘回收站。网盘不让直接删时可以改为移进一个待删除目录，
  // 这时 moved_to 写这个目录，要用户自己去网盘里清空才释放空间。返回出错原因。
  virtual std::string remove_path(const std::string& path, std::string* moved_to);
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
