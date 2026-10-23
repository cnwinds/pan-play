#include "panplay/install.hpp"

#include "panplay/dummy_ftp.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/speed_test.hpp"

#include "big_memory.hpp"
#include "install_clock.hpp"
#include "install_common.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace panplay {
namespace {

using Generator = std::function<void(std::int64_t offset, std::uint8_t* dst, std::size_t size)>;

std::string shell_quoted(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

// 用 RAR 命令行生成测试压缩包：Windows 上是 WinRAR，其他平台是 PATH 里的 rar，也可以用 PANPLAY_RAR 指定。
bool run_rar(const std::string& args) {
#if defined(_WIN32)
  const std::string command = "\"\"C:\\Program Files\\WinRAR\\Rar.exe\" " + args + "\"";
#else
  const char* custom = std::getenv("PANPLAY_RAR");
  const std::string command = std::string("\"") + (custom != nullptr && *custom != 0 ? custom : "rar") + "\" " + args;
#endif
  return std::system(command.c_str()) == 0;
}

// 用 7-Zip 命令行生成测试 7z：Windows 上默认装在 Program Files，其他平台是 PATH 里的 7z，也可以用 PANPLAY_7Z 指定。
bool run_7z(const std::string& args) {
  const char* custom = std::getenv("PANPLAY_7Z");
#if defined(_WIN32)
  const std::string program = custom != nullptr && *custom != 0 ? custom : "C:\\Program Files\\7-Zip\\7z.exe";
  const std::string command = "\"\"" + program + "\" " + args + " >NUL 2>&1\"";
#else
  const std::string program = custom != nullptr && *custom != 0 ? custom : "7z";
  const std::string command = "\"" + program + "\" " + args + " >/dev/null 2>&1";
#endif
  return std::system(command.c_str()) == 0;
}

struct FakeFile {
  std::int64_t size = 0;
  Generator fill;
};

// 内容按偏移现算的假网盘，统计请求次数，可以模拟延迟和断线。
class FakeDrive final : public DrivePlugin {
 public:
  void add(const std::string& path, std::vector<std::uint8_t> bytes) {
    auto shared = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    add_generated(path, static_cast<std::int64_t>(shared->size()), [shared](std::int64_t offset, std::uint8_t* dst, std::size_t size) {
      std::memcpy(dst, shared->data() + offset, size);
    });
  }

  void add_generated(const std::string& path, std::int64_t size, Generator fill) {
    files_[path] = FakeFile{size, std::move(fill)};
    std::string cursor = path;
    while (true) {
      const auto slash = cursor.rfind('/');
      if (slash == std::string::npos || slash == 0) break;
      cursor = cursor.substr(0, slash);
      dirs_.insert(cursor);
    }
  }

  std::atomic<int> read_at_calls{0};
  std::atomic<int> stream_opens{0};
  std::atomic<int> latency_ms{0};
  // 下一次打开的流读到这么多字节后断开，只生效一次。
  std::atomic<std::int64_t> break_next_stream_after{-1};
  // 限速（KB/s，0 是不限）：每个流各自的上限（像百度按连接限速），和所有流加起来的上限（像主机的网络）。
  std::atomic<int> stream_kbps{0};
  std::atomic<int> total_kbps{0};
  // 下一次打开的流读到这么多字节后每 stall_piece_ms 毫秒只给 16 KB，只生效一次。
  // 默认每秒 16 KB 是卡住；间隔短时是一直有数据、只是很慢。
  std::atomic<std::int64_t> stall_next_stream_after{-1};
  std::atomic<int> stall_piece_ms{1000};

  std::string id() const override { return "memory"; }
  std::string display_name() const override { return "内存"; }
  LoginChallenge start_login() override {
    LoginChallenge challenge;
    challenge.error = "内存盘不需要登录";
    return challenge;
  }
  LoginPoll poll_login(const LoginChallenge&) override { return {}; }
  bool session_valid() override { return true; }
  void logout() override {}

  std::vector<DriveEntry> list(const std::string& path, std::string*) override {
    const std::string dir = trim(path);
    const std::string prefix = dir == "/" ? std::string("/") : dir + "/";
    std::map<std::string, DriveEntry> children;
    auto consider = [&](const std::string& full, bool is_dir, std::int64_t size) {
      if (full.rfind(prefix, 0) != 0) return;
      const std::string rest = full.substr(prefix.size());
      if (rest.empty()) return;
      const auto slash = rest.find('/');
      const std::string name = slash == std::string::npos ? rest : rest.substr(0, slash);
      DriveEntry entry;
      entry.name = name;
      entry.is_dir = slash != std::string::npos || is_dir;
      if (entry.is_dir) {
        entry.path = dir == "/" ? "/" + name : dir + "/" + name;
        entry.file_id = entry.path;
        entry.size = 0;
      } else {
        entry.path = full;
        entry.file_id = full;
        entry.size = size;
      }
      children[name] = entry;
    };
    for (const auto& dir_path : dirs_) consider(dir_path, true, 0);
    for (const auto& [file_path, file] : files_) consider(file_path, false, file.size);
    std::vector<DriveEntry> out;
    for (const auto& [name, entry] : children) {
      (void)name;
      out.push_back(entry);
    }
    return out;
  }

  ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) override {
    ++read_at_calls;
    delay();
    ReadResult result;
    const auto it = files_.find(file_id);
    if (it == files_.end()) {
      result.error = "没有这个文件";
      return result;
    }
    if (offset < 0 || length == 0 || offset > it->second.size) {
      result.error = "读取范围无效";
      return result;
    }
    const auto n = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(length), it->second.size - offset));
    result.bytes.resize(n);
    it->second.fill(offset, result.bytes.data(), n);
    result.status = 206;
    return result;
  }

  // 每个文件被顺序读过的最小起点；没读过的不在里面。
  std::map<std::string, std::int64_t> streamed_from() {
    std::lock_guard<std::mutex> lock(streamed_mu_);
    return streamed_from_;
  }
  void forget_streams() {
    std::lock_guard<std::mutex> lock(streamed_mu_);
    streamed_from_.clear();
  }

  std::unique_ptr<ByteStream> open_stream(const std::string& file_id, std::int64_t offset, std::string* error,
                                          AbortSignal* abort) override {
    ++stream_opens;
    {
      std::lock_guard<std::mutex> lock(streamed_mu_);
      const auto it = streamed_from_.find(file_id);
      if (it == streamed_from_.end() || offset < it->second) streamed_from_[file_id] = offset;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(latency_ms.load());
    while (std::chrono::steady_clock::now() < until) {
      if (abort != nullptr && abort->aborted()) {
        *error = "下载已中止";
        return nullptr;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto it = files_.find(file_id);
    if (it == files_.end() || offset < 0 || offset > it->second.size) {
      *error = "没有这个文件";
      return nullptr;
    }
    return std::make_unique<Stream>(*this, it->second, offset, break_next_stream_after.exchange(-1),
                                    stall_next_stream_after.exchange(-1));
  }

 private:
  // 按限速给这次读排时间：每个流、所有流各有一条时间线，睡到两条都轮到为止。
  // 允许追回 50 毫秒睡过头的时间，平均速度才准。
  void pace(std::size_t bytes, std::chrono::steady_clock::time_point* own) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    auto until = now;
    const auto slot = [&](int kbps) {
      return microseconds(static_cast<long long>(bytes) * 1000000 / (static_cast<long long>(kbps) * 1024));
    };
    if (const int kbps = stream_kbps.load(); kbps > 0) {
      *own = std::max(*own, now - milliseconds(50)) + slot(kbps);
      until = std::max(until, *own);
    }
    if (const int kbps = total_kbps.load(); kbps > 0) {
      std::lock_guard<std::mutex> lock(pace_mu_);
      total_next_ = std::max(total_next_, now - milliseconds(50)) + slot(kbps);
      until = std::max(until, total_next_);
    }
    std::this_thread::sleep_until(until);
  }

  class Stream final : public ByteStream {
   public:
    Stream(FakeDrive& owner, const FakeFile& file, std::int64_t offset, std::int64_t break_after, std::int64_t stall_after)
        : owner_(owner), file_(file), offset_(offset), break_after_(break_after), stall_after_(stall_after) {}

    std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error) override {
      if (aborted_) {
        *error = "下载已中止";
        return 0;
      }
      if (break_after_ >= 0 && delivered_ >= break_after_) {
        *error = "模拟断线";
        return 0;
      }
      std::int64_t n = std::min<std::int64_t>(static_cast<std::int64_t>(size), file_.size - offset_);
      if (break_after_ >= 0) n = std::min(n, break_after_ - delivered_);
      if (n <= 0) return 0;
      if (stall_after_ >= 0 && delivered_ >= stall_after_) {
        n = std::min<std::int64_t>(n, 16 << 10);
        const int slices = std::max(1, owner_.stall_piece_ms.load() / 10);
        for (int i = 0; i < slices && !aborted_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (aborted_) {
          *error = "下载已中止";
          return 0;
        }
      } else if (owner_.stream_kbps > 0 || owner_.total_kbps > 0) {
        n = std::min<std::int64_t>(n, 64 << 10);
        owner_.pace(static_cast<std::size_t>(n), &next_);
      }
      file_.fill(offset_, dst, static_cast<std::size_t>(n));
      offset_ += n;
      delivered_ += n;
      return static_cast<std::size_t>(n);
    }

    void abort() override { aborted_ = true; }

   private:
    FakeDrive& owner_;
    const FakeFile& file_;
    std::int64_t offset_;
    std::int64_t break_after_;
    std::int64_t stall_after_;
    std::int64_t delivered_ = 0;
    std::atomic<bool> aborted_{false};
    std::chrono::steady_clock::time_point next_{};
  };

  std::mutex pace_mu_;
  std::chrono::steady_clock::time_point total_next_{};

  void delay() const {
    if (latency_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
  }

  std::mutex streamed_mu_;
  std::map<std::string, std::int64_t> streamed_from_;

  static std::string trim(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path.empty() ? "/" : path;
  }

  std::map<std::string, FakeFile> files_;
  std::set<std::string> dirs_;
};

class CountingSink final : public InstallSink {
 public:
  explicit CountingSink(InstallSink& inner) : inner_(inner) {}

  int writes_ending(const std::string& suffix) const {
    int count = 0;
    for (const auto& [path, hits] : writes_) {
      if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) count += hits;
    }
    return count;
  }
  int writers_opened() const { return writers_; }

  void reset() {
    writes_.clear();
    writers_ = 0;
  }

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) override {
    writes_[path] += 1;
    ++writers_;
    return inner_.open_writer(path, size, error);
  }
  std::string ensure_dir(const std::string& path) override { return inner_.ensure_dir(path); }
  std::int64_t file_size(const std::string& path, std::string* error) override { return inner_.file_size(path, error); }
  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    writes_[path] += 1;
    return inner_.store(path, data, size);
  }
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    ++appends_;
    return inner_.append(path, data, size);
  }
  int appends() const { return appends_; }
  std::string rename_path(const std::string& from, const std::string& to) override { return inner_.rename_path(from, to); }
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    return inner_.read_file(path, out, max_bytes);
  }
  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    return inner_.list_children(path, error);
  }
  std::string remove_file(const std::string& path) override { return inner_.remove_file(path); }
  std::string remove_dir(const std::string& path) override { return inner_.remove_dir(path); }
  std::string remove_tree(const std::string& path) override { return inner_.remove_tree(path); }

 private:
  InstallSink& inner_;
  std::map<std::string, int> writes_;
  int writers_ = 0;
  int appends_ = 0;
};

// 只记大小的存放位置，大文件不落盘。1 MB 以内的文件保留内容，方便读回进度日志和完成标记。
class MemorySink final : public InstallSink {
 public:
  struct File {
    std::int64_t size = 0;
    std::vector<std::uint8_t> bytes;
  };

  std::map<std::string, File> files;

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t, std::string*) override {
    files[path] = File{};
    return std::make_unique<Writer>(files[path]);
  }
  std::string ensure_dir(const std::string&) override { return ""; }
  std::int64_t file_size(const std::string& path, std::string*) override {
    const auto it = files.find(path);
    return it == files.end() ? -1 : it->second.size;
  }
  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    files[path] = File{static_cast<std::int64_t>(size), std::vector<std::uint8_t>(data, data + size)};
    return "";
  }
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    File& file = files[path];
    file.bytes.insert(file.bytes.end(), data, data + size);
    file.size += static_cast<std::int64_t>(size);
    return "";
  }
  std::string rename_path(const std::string& from, const std::string& to) override {
    const auto it = files.find(from);
    if (it == files.end()) return "没有这个文件";
    files[to] = std::move(it->second);
    files.erase(from);
    return "";
  }
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t) override {
    const auto it = files.find(path);
    if (it == files.end()) return "missing";
    *out = it->second.bytes;
    return "";
  }
  std::vector<std::string> list_children(const std::string&, std::string*) override { return {}; }
  std::string remove_file(const std::string& path) override {
    files.erase(path);
    return "";
  }
  std::string remove_dir(const std::string&) override { return ""; }
  std::string remove_tree(const std::string& path) override {
    const std::string inside = path + "/";
    for (auto it = files.begin(); it != files.end();) {
      it = it->first.rfind(inside, 0) == 0 ? files.erase(it) : std::next(it);
    }
    return "";
  }

 private:
  class Writer final : public FileWriter {
   public:
    explicit Writer(File& file) : file_(file) {}
    std::string write(const std::uint8_t* data, std::size_t size) override {
      file_.size += static_cast<std::int64_t>(size);
      if (file_.size <= (1 << 20)) file_.bytes.insert(file_.bytes.end(), data, data + size);
      else file_.bytes.clear();
      return "";
    }
    std::string finish() override { return ""; }

   private:
    File& file_;
  };
};

void append16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

void append64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

std::uint32_t crc_of(const std::vector<std::uint8_t>& data) {
  return static_cast<std::uint32_t>(
      ::crc32(::crc32(0L, Z_NULL, 0), data.empty() ? Z_NULL : data.data(), static_cast<uInt>(data.size())));
}

std::vector<std::uint8_t> raw_deflate(const std::vector<std::uint8_t>& data) {
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
  std::vector<std::uint8_t> out(deflateBound(&stream, static_cast<uLong>(data.size())) + 64);
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  if (result != Z_STREAM_END) {
    deflateEnd(&stream);
    return {};
  }
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

struct ZipInput {
  std::string name;
  std::vector<std::uint8_t> data;
  bool deflate = false;
};

struct ZipLayout {
  // 强制写 ZIP64 字段和 ZIP64 目录结束记录。
  bool zip64 = false;
  // 本地文件头不写大小，数据后面跟数据描述符。
  bool descriptor = false;
  // 大于 0 时按这个大小切成跨卷 ZIP（.z01 … .zip），偏移按卷号分开记。
  std::size_t span = 0;
};

// 生成测试 ZIP。跨卷时返回每一卷，否则只有一个元素。
std::vector<std::vector<std::uint8_t>> make_zip(const std::vector<ZipInput>& members, const ZipLayout& layout) {
  struct Meta {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t comp_size = 0;
    std::uint64_t uncomp_size = 0;
    std::uint64_t local = 0;
  };
  std::vector<std::uint8_t> out;
  if (layout.span > 0) append32(out, 0x08074b50);
  const auto disk_of = [&](std::uint64_t position) -> std::uint32_t {
    return layout.span > 0 ? static_cast<std::uint32_t>(position / layout.span) : 0;
  };
  const auto offset_in_disk = [&](std::uint64_t position) -> std::uint64_t {
    return layout.span > 0 ? position % layout.span : position;
  };
  const std::uint16_t flags = layout.descriptor ? 8 : 0;
  std::vector<Meta> metas;
  for (const auto& member : members) {
    Meta meta;
    meta.name = member.name;
    meta.uncomp_size = member.data.size();
    meta.crc = crc_of(member.data);
    const std::vector<std::uint8_t> payload = member.deflate ? raw_deflate(member.data) : member.data;
    meta.method = member.deflate ? 8 : 0;
    meta.comp_size = payload.size();
    meta.local = out.size();
    append32(out, 0x04034b50);
    append16(out, 45);
    append16(out, flags);
    append16(out, meta.method);
    append16(out, 0);
    append16(out, 0);
    append32(out, layout.descriptor ? 0 : meta.crc);
    append32(out, layout.descriptor ? 0 : static_cast<std::uint32_t>(meta.comp_size));
    append32(out, layout.descriptor ? 0 : static_cast<std::uint32_t>(meta.uncomp_size));
    append16(out, static_cast<std::uint16_t>(meta.name.size()));
    append16(out, 0);
    out.insert(out.end(), meta.name.begin(), meta.name.end());
    out.insert(out.end(), payload.begin(), payload.end());
    if (layout.descriptor) {
      append32(out, 0x08074b50);
      append32(out, meta.crc);
      append32(out, static_cast<std::uint32_t>(meta.comp_size));
      append32(out, static_cast<std::uint32_t>(meta.uncomp_size));
    }
    metas.push_back(std::move(meta));
  }
  const std::uint64_t central = out.size();
  for (const auto& meta : metas) {
    append32(out, 0x02014b50);
    append16(out, 45);
    append16(out, 45);
    append16(out, flags);
    append16(out, meta.method);
    append16(out, 0);
    append16(out, 0);
    append32(out, meta.crc);
    append32(out, layout.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(meta.comp_size));
    append32(out, layout.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(meta.uncomp_size));
    append16(out, static_cast<std::uint16_t>(meta.name.size()));
    append16(out, static_cast<std::uint16_t>(layout.zip64 ? 4 + 28 : 0));
    append16(out, 0);
    append16(out, layout.zip64 ? 0xffff : static_cast<std::uint16_t>(disk_of(meta.local)));
    append16(out, 0);
    append32(out, 0);
    append32(out, layout.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(offset_in_disk(meta.local)));
    out.insert(out.end(), meta.name.begin(), meta.name.end());
    if (layout.zip64) {
      append16(out, 0x0001);
      append16(out, 28);
      append64(out, meta.uncomp_size);
      append64(out, meta.comp_size);
      append64(out, offset_in_disk(meta.local));
      append32(out, disk_of(meta.local));
    }
  }
  const std::uint64_t central_size = out.size() - central;
  const std::uint64_t zip64_record = out.size();
  const std::uint64_t end_record = zip64_record + (layout.zip64 ? 56 + 20 : 0);
  const std::uint32_t last_disk = disk_of(end_record);
  if (layout.zip64) {
    append32(out, 0x06064b50);
    append64(out, 44);
    append16(out, 45);
    append16(out, 45);
    append32(out, last_disk);
    append32(out, disk_of(central));
    append64(out, metas.size());
    append64(out, metas.size());
    append64(out, central_size);
    append64(out, offset_in_disk(central));
    append32(out, 0x07064b50);
    append32(out, disk_of(zip64_record));
    append64(out, offset_in_disk(zip64_record));
    append32(out, last_disk + 1);
  }
  append32(out, 0x06054b50);
  append16(out, layout.zip64 ? 0xffff : static_cast<std::uint16_t>(last_disk));
  append16(out, layout.zip64 ? 0xffff : static_cast<std::uint16_t>(disk_of(central)));
  append16(out, layout.zip64 ? 0xffff : static_cast<std::uint16_t>(metas.size()));
  append16(out, layout.zip64 ? 0xffff : static_cast<std::uint16_t>(metas.size()));
  append32(out, layout.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(central_size));
  append32(out, layout.zip64 ? 0xffffffffu : static_cast<std::uint32_t>(offset_in_disk(central)));
  append16(out, 0);

  if (layout.span == 0) return {out};
  if (disk_of(out.size() - 1) != last_disk) return {};
  std::vector<std::vector<std::uint8_t>> volumes;
  for (std::size_t offset = 0; offset < out.size(); offset += layout.span) {
    const std::size_t n = std::min(layout.span, out.size() - offset);
    volumes.emplace_back(out.begin() + static_cast<std::ptrdiff_t>(offset), out.begin() + static_cast<std::ptrdiff_t>(offset + n));
  }
  return volumes;
}

void add_spanned(FakeDrive& drive, const std::string& dir, const std::string& stem,
                 const std::vector<std::vector<std::uint8_t>>& volumes) {
  for (std::size_t i = 0; i < volumes.size(); ++i) {
    char name[64];
    if (i + 1 == volumes.size()) {
      std::snprintf(name, sizeof(name), "%s.zip", stem.c_str());
    } else {
      std::snprintf(name, sizeof(name), "%s.z%02d", stem.c_str(), static_cast<int>(i + 1));
    }
    drive.add(dir + "/" + name, volumes[i]);
  }
}

int fail(const std::string& message) {
  std::cerr << "install: " << message << "\n";
  return 1;
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 8));
  return bytes;
}

int resume_on(InstallSink& raw, FakeDrive& drive, const std::vector<std::uint8_t>& param, const std::vector<std::uint8_t>& eboot,
              const std::filesystem::path& disk_root) {
  CountingSink sink(raw);
  InstallRequest request;
  request.source_dir = "/apps/panplay/Demo CUSA12345";
  request.dest_prefix = "/data/homebrew";
  request.stop_after_output_bytes = 150;
  InstallResult first = install_game(drive, sink, request);
  if (first.status != "interrupted" || first.files_done != 1) return fail("中断后状态不对: " + first.status + " " + first.message);
  std::string error;
  const std::string partial = "/data/homebrew/.panplay-staging/CUSA12345/eboot.bin";
  if (sink.file_size(partial, &error) != 50 || !error.empty()) return fail("中断时第二个文件不是 50 字节");
  std::vector<std::uint8_t> journal;
  error = sink.read_file("/data/homebrew/.panplay-staging/CUSA12345/.panplay-journal.jsonl", &journal, 1 << 20);
  if (!error.empty()) return fail(error);
  const std::string journal_text(journal.begin(), journal.end());
  if (journal_text.find("param.sfo") == std::string::npos || journal_text.find("eboot.bin") != std::string::npos) {
    return fail("进度记录没有只留下已校验文件");
  }

  for (int round = 0; round < 2; ++round) {
    if (auto discard_error = discard_install(sink, request.dest_prefix, request.source_dir); !discard_error.empty()) {
      return fail("删除没装完的文件失败: " + discard_error);
    }
  }
  error.clear();
  const auto left = sink.list_children(request.dest_prefix, &error);
  if (!error.empty() || !left.empty() || sink.file_size(partial, &error) >= 0) return fail("删除后暂存区还有东西");
  first = install_game(drive, sink, request);
  if (first.status != "interrupted" || first.files_done != 1) return fail("删除后重新安装没有从头开始: " + first.message);

  sink.reset();
  request.stop_after_output_bytes = -1;
  bool reported_check = false;
  request.on_progress = [&](const InstallResult& snapshot) {
    if (snapshot.message == "正在核对" && snapshot.files_checked == 1 && snapshot.files_to_check == 1) reported_check = true;
  };
  InstallResult second = install_game(drive, sink, request);
  if (second.status != "installed" || second.files_done != 2) return fail("继续后没有装完: " + second.message);
  if (!reported_check) return fail("继续安装时没有报告正在核对已装好的文件");
  if (sink.writes_ending("/param.sfo") != 0) return fail("已校验文件被重写了");
  if (sink.writes_ending("/eboot.bin") != 1) return fail("未完成文件没有从头重写一次");

  std::vector<std::uint8_t> got_param;
  std::vector<std::uint8_t> got_eboot;
  error = sink.read_file("/data/homebrew/CUSA12345/param.sfo", &got_param, 1 << 20);
  if (!error.empty() || got_param != param) return fail("param.sfo 内容不对");
  error = sink.read_file("/data/homebrew/CUSA12345/eboot.bin", &got_eboot, 1 << 20);
  if (!error.empty() || got_eboot != eboot) return fail("eboot.bin 内容不对");

  sink.reset();
  InstallResult third = install_game(drive, sink, request);
  if (third.status != "already") return fail("完成后再次安装没有显示已安装");
  if (sink.writes_ending("/param.sfo") != 0 || sink.writes_ending("/eboot.bin") != 0) return fail("已安装后又写了游戏文件");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(disk_root)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() >= 4 && (name.compare(name.size() - 4, 4, ".zip") == 0 || name.compare(name.size() - 4, 4, ".z01") == 0)) {
      return fail("压缩包被写进了存放目录");
    }
  }
  return 0;
}

int prefetch_tests() {
  FakeDrive drive;
  const std::int64_t size = (64ll << 20) + 12345;
  const auto byte_at = [](std::int64_t offset) { return static_cast<std::uint8_t>((offset * 131) ^ (offset >> 13)); };
  drive.add_generated("/big.bin", size, [&](std::int64_t offset, std::uint8_t* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] = byte_at(offset + static_cast<std::int64_t>(i));
  });
  PrefetchOptions options;
  options.retry_base_ms = 1;
  options.connections = 1;
  options.block_size = 4u << 20;
  options.max_blocks = 8;

  {
    PrefetchReader reader(drive, "/big.bin", size, nullptr, options);
    std::vector<std::uint8_t> buffer(7919);
    std::int64_t at = 0;
    while (true) {
      std::string error;
      const std::size_t got = reader.read(buffer.data(), buffer.size(), &error);
      if (!error.empty()) return fail(error);
      if (got == 0) break;
      for (std::size_t i = 0; i < got; i += 997) {
        if (buffer[i] != byte_at(at + static_cast<std::int64_t>(i))) return fail("预读数据不对");
      }
      at += static_cast<std::int64_t>(got);
    }
    if (at != size) return fail("预读没有读到文件结尾");
    if (drive.stream_opens != 1 || drive.read_at_calls != 0) {
      return fail("顺序读 64 MB 打开了 " + std::to_string(drive.stream_opens.load()) + " 次流");
    }
  }

  drive.stream_opens = 0;
  drive.break_next_stream_after = 10ll << 20;
  {
    PrefetchReader reader(drive, "/big.bin", size, nullptr, options);
    if (auto error = reader.seek(1000); !error.empty()) return fail(error);
    std::vector<std::uint8_t> buffer(1 << 20);
    std::int64_t at = 1000;
    while (true) {
      std::string error;
      const std::size_t got = reader.read(buffer.data(), buffer.size(), &error);
      if (!error.empty()) return fail("断线后没有自动重连: " + error);
      if (got == 0) break;
      if (buffer[0] != byte_at(at) || buffer[got - 1] != byte_at(at + static_cast<std::int64_t>(got) - 1)) {
        return fail("断线重连后数据错位");
      }
      at += static_cast<std::int64_t>(got);
    }
    if (at != size) return fail("断线重连后没有读完");
    if (drive.stream_opens != 2) return fail("断线一次应该重连一次，实际打开 " + std::to_string(drive.stream_opens.load()) + " 次");
  }

  drive.stream_opens = 0;
  {
    PrefetchReader reader(drive, "/big.bin", size, nullptr, options);
    std::uint8_t byte = 0;
    std::string error;
    reader.read(&byte, 1, &error);
    if (auto seek_error = reader.seek(100); !seek_error.empty()) return fail(seek_error);
    reader.read(&byte, 1, &error);
    if (!error.empty() || byte != byte_at(100)) return fail("缓冲内向前跳转后数据不对");
    if (auto seek_error = reader.seek(3ll << 20); !seek_error.empty()) return fail(seek_error);
    reader.read(&byte, 1, &error);
    if (!error.empty() || byte != byte_at(3ll << 20)) return fail("近距离向前跳转后数据不对");
    if (drive.stream_opens != 1) return fail("近距离向前跳转不该重连");
    if (auto seek_error = reader.seek((3ll << 20) - 50); !seek_error.empty()) return fail(seek_error);
    reader.read(&byte, 1, &error);
    if (!error.empty() || byte != byte_at((3ll << 20) - 50)) return fail("块内往回跳后数据不对");
    if (auto seek_error = reader.seek((4ll << 20) + 20); !seek_error.empty()) return fail(seek_error);
    reader.read(&byte, 1, &error);
    if (auto seek_error = reader.seek((4ll << 20) - 30); !seek_error.empty()) return fail(seek_error);
    std::vector<std::uint8_t> span(100);
    std::size_t got_span = 0;
    while (got_span < span.size()) {
      const std::size_t got = reader.read(span.data() + got_span, span.size() - got_span, &error);
      if (got == 0) break;
      got_span += got;
    }
    for (std::size_t i = 0; i < span.size(); ++i) {
      if (span[i] != byte_at((4ll << 20) - 30 + static_cast<std::int64_t>(i))) return fail("跨块往回跳后数据不对");
    }
    if (drive.stream_opens != 1) return fail("往回跳一小段不该重连");
    if (auto seek_error = reader.seek(60ll << 20); !seek_error.empty()) return fail(seek_error);
    reader.read(&byte, 1, &error);
    if (!error.empty() || byte != byte_at(60ll << 20)) return fail("远距离跳转后数据不对");
    if (drive.stream_opens != 2) return fail("跳转次数不对");
  }

  for (const int connections : {2, 4}) {
    PrefetchOptions multi = options;
    multi.connections = connections;
    multi.adaptive = false;
    multi.max_blocks = static_cast<std::size_t>(connections) * 2;
    drive.stream_opens = 0;
    drive.latency_ms = 2;
    drive.break_next_stream_after = 3ll << 20;
    PrefetchReader reader(drive, "/big.bin", size, nullptr, multi);
    if (auto error = reader.seek(777); !error.empty()) return fail(error);
    std::vector<std::uint8_t> buffer(300000);
    std::int64_t at = 777;
    bool jumped = false;
    while (true) {
      std::string error;
      const std::size_t got = reader.read(buffer.data(), buffer.size(), &error);
      if (!error.empty()) return fail("多连接读取出错: " + error);
      if (got == 0) break;
      for (std::size_t i = 0; i < got; i += 4099) {
        if (buffer[i] != byte_at(at + static_cast<std::int64_t>(i))) return fail("多连接数据顺序不对");
      }
      if (buffer[got - 1] != byte_at(at + static_cast<std::int64_t>(got) - 1)) return fail("多连接数据顺序不对");
      at += static_cast<std::int64_t>(got);
      if (!jumped && at > (20ll << 20)) {
        jumped = true;
        at = 5ll << 20;
        if (auto seek_error = reader.seek(at); !seek_error.empty()) return fail(seek_error);
      }
    }
    drive.latency_ms = 0;
    if (at != size) return fail("多连接没有读到文件结尾");
  }

  {
    CancelToken cancel;
    drive.latency_ms = 50;
    PrefetchReader reader(drive, "/big.bin", size, &cancel, options);
    cancel.cancel();
    std::uint8_t byte = 0;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    reader.read(&byte, 1, &error);
    const auto spent = std::chrono::steady_clock::now() - started;
    drive.latency_ms = 0;
    if (error != "interrupted") return fail("取消后没有返回中断");
    if (spent > std::chrono::seconds(1)) return fail("取消后没有很快停下");
  }

  for (const int connections : {1, 4}) {
    PrefetchOptions slow = options;
    slow.connections = connections;
    slow.adaptive = false;
    const auto started = std::chrono::steady_clock::now();
    {
      PrefetchReader reader(drive, "/big.bin", size, nullptr, slow);
      std::uint8_t byte = 0;
      std::string error;
      reader.read(&byte, 1, &error);
      drive.latency_ms = 10000;
      if (auto seek_error = reader.seek(60ll << 20); !seek_error.empty()) return fail(seek_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      drive.latency_ms = 0;
      if (auto seek_error = reader.seek(50ll << 20); !seek_error.empty()) return fail(seek_error);
      reader.read(&byte, 1, &error);
      if (!error.empty() || byte != byte_at(50ll << 20)) return fail("连接中跳转后数据不对");
      drive.latency_ms = 10000;
      if (auto seek_error = reader.seek(10ll << 20); !seek_error.empty()) return fail(seek_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    drive.latency_ms = 0;
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) return fail("连接还没建立时跳转或关闭要等很久");
  }
  return 0;
}

// 先单连接、一条吃不满才加（ADR 0005）：带宽限在主机这边时试过要退回 1 条，服务器按连接限速时往上加，
// 解压器等着的那段卡住时很快重派。调速用的时间都调小了。
int tuning_tests() {
  FakeDrive drive;
  const std::int64_t size = 512ll << 20;
  const auto byte_at = [](std::int64_t offset) { return static_cast<std::uint8_t>(offset * 7 + (offset >> 20)); };
  drive.add_generated("/tune.bin", size, [&](std::int64_t offset, std::uint8_t* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] = byte_at(offset + static_cast<std::int64_t>(i));
  });
  const auto base = [] {
    PrefetchOptions options;
    options.retry_base_ms = 1;
    options.block_size = 1u << 20;
    options.max_blocks = 32;
    options.connections = 8;
    options.settle_ms = 200;
    options.measure_ms = 400;
    options.review_ms = 400;
    options.backoff_ms = 600000;
    options.segment_ms = 300;
    options.stall_ms = 300;
    return options;
  };
  // 一直读到 limit 字节或者超时，边读边核对内容，返回读到哪。
  const auto drain = [&](PrefetchReader& reader, std::int64_t limit, double seconds, std::string* error) {
    std::vector<std::uint8_t> buffer(1 << 20);
    std::int64_t at = reader.position();
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (at < limit && std::chrono::steady_clock::now() < until) {
      const std::size_t got = reader.read(buffer.data(), buffer.size(), error);
      if (!error->empty() || got == 0) break;
      for (std::size_t i = 0; i < got; i += 4093) {
        if (buffer[i] != byte_at(at + static_cast<std::int64_t>(i))) {
          *error = "数据不对 @" + std::to_string(at + static_cast<std::int64_t>(i));
          return at;
        }
      }
      at += static_cast<std::int64_t>(got);
    }
    return at;
  };
  const auto reset_limits = [&] {
    drive.total_kbps = 0;
    drive.stream_kbps = 0;
    drive.stall_next_stream_after = -1;
    drive.stall_piece_ms = 1000;
  };

  {
    reset_limits();
    drive.total_kbps = 24 * 1024;
    PrefetchOptions options = base();
    PrefetchReader reader(drive, "/tune.bin", size, nullptr, options);
    std::string error;
    drain(reader, size, 3.0, &error);
    if (!error.empty()) return fail("总带宽受限时读取出错: " + error);
    int failures = 0;
    {
      std::lock_guard<std::mutex> lock(options.tuning->mu);
      failures = options.tuning->failures;
    }
    if (failures < 1) return fail("总带宽受限、解压器一直在等时应该试过多开");
    if (reader.connections() != 1) {
      return fail("带宽限在主机这边时多开只是分流，应该退回 1 条，实际 " + std::to_string(reader.connections()) + " 条");
    }
  }

  {
    reset_limits();
    drive.stream_kbps = 6 * 1024;
    PrefetchOptions options = base();
    PrefetchReader reader(drive, "/tune.bin", size, nullptr, options);
    std::string error;
    drain(reader, size, 5.0, &error);
    if (!error.empty()) return fail("按连接限速时读取出错: " + error);
    if (reader.connections() < 3) {
      return fail("服务器按连接限速时应该加到 3 条以上，实际 " + std::to_string(reader.connections()) + " 条");
    }
  }

  // 卡住（每秒 16 KB）和一直有数据但很慢（每 40 毫秒 16 KB，400 KB/s，高于卡住的下限）：
  // 很慢的那段每次只交一点，解压器拿到一点又等，不能只看它连续等了多久。
  for (const int piece_ms : {1000, 40}) {
    for (const int connections : {1, 4}) {
      reset_limits();
      drive.stall_piece_ms = piece_ms;
      if (connections == 1) {
        drive.total_kbps = 32 * 1024;
        drive.stall_next_stream_after = 3ll << 20;
      } else {
        drive.stream_kbps = 4 * 1024;
        drive.stall_next_stream_after = 256 << 10;
      }
      PrefetchOptions options = base();
      options.adaptive = false;
      options.connections = connections;
      PrefetchReader reader(drive, "/tune.bin", size, nullptr, options);
      std::string error;
      const auto started = std::chrono::steady_clock::now();
      const std::int64_t got = drain(reader, 24ll << 20, 8.0, &error);
      const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      const std::string what = std::to_string(connections) + " 条连接时请求" + (piece_ms < 1000 ? "很慢" : "卡住");
      if (!error.empty()) return fail(what + "后读取出错: " + error);
      if (got < (24ll << 20) || spent > 5.0) {
        return fail(what + "没有及时重派：" + std::to_string(spent) + " 秒读到 " + std::to_string(got >> 20) + " MB");
      }
    }
  }
  reset_limits();
  return 0;
}

// 下载测速：每档都量；服务器按连接限速时 8 条比 1 条快得多，带宽限在本机这边时多开不更快；中途停下很快结束。
int speed_test_tests() {
  auto drive = std::make_shared<FakeDrive>();
  const auto fill = [](std::int64_t, std::uint8_t* dst, std::size_t n) { std::memset(dst, 7, n); };
  drive->add_generated("/lib/Game PPSA00001/big.part1.rar", 256ll << 20, fill);
  drive->add_generated("/lib/Game PPSA00001/big.part2.rar", 64ll << 20, fill);
  drive->add_generated("/lib/readme.txt", 1 << 10, fill);
  SpeedTestOptions options;
  options.settle_ms = 100;
  options.single_ms = 600;
  options.step_ms = 400;
  options.connect_ms = 2000;
  const auto finish = [](SpeedTest& test, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (test.running() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return !test.running();
  };

  {
    drive->stream_kbps = 4 * 1024;
    SpeedTest test;
    if (const std::string error = test.start(drive, "/lib", options); !error.empty()) return fail(error);
    if (!finish(test, 15)) return fail("测速没有按时结束");
    const SpeedTestStatus status = test.status();
    if (!status.error.empty()) return fail("按连接限速时测速出错: " + status.error);
    if (status.file_name != "big.part1.rar") return fail("测速没有挑最大的文件，挑了 " + status.file_name);
    if (status.steps.size() != 4 || !status.ended.empty()) {
      return fail("按连接限速时应该一直加到 8 条，实际量了 " + std::to_string(status.steps.size()) + " 档");
    }
    if (status.steps.back().rate < status.steps.front().rate * 4) return fail("按连接限速时 8 条应该比 1 条快得多");
    for (const auto& step : status.steps) {
      if (step.connected != step.connections) return fail("测速的连接没有全部连上: " + std::to_string(step.connections) + " 条档");
    }
  }

  {
    drive->stream_kbps = 0;
    drive->total_kbps = 8 * 1024;
    SpeedTest test;
    if (const std::string error = test.start(drive, "/lib", options); !error.empty()) return fail(error);
    if (!finish(test, 15)) return fail("测速没有按时结束");
    const SpeedTestStatus status = test.status();
    if (!status.error.empty()) return fail("总带宽受限时测速出错: " + status.error);
    if (status.steps.size() != 4) return fail("总带宽受限时也要每档都量，实际量了 " + std::to_string(status.steps.size()) + " 档");
    if (status.steps.back().rate > status.steps.front().rate * 1.3) return fail("总带宽受限时多开不该更快");
  }

  {
    drive->total_kbps = 16 * 1024;
    SpeedTest test;
    if (const std::string error = test.start(drive, "/lib", options); !error.empty()) return fail(error);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (test.start(drive, "/lib", options).empty()) return fail("测速进行中又开了一次");
    test.stop();
    if (!finish(test, 1)) return fail("停下测速后没有很快结束");
    if (test.status().ended != "stopped") return fail("停下的测速没有标成中途停下");
  }
  drive->total_kbps = 0;

  {
    SpeedTest test;
    if (const std::string error = test.start(drive, "/lib", options); !error.empty()) return fail(error);
    test.stop();
    if (!finish(test, 1)) return fail("找文件时停下测速后没有很快结束");
    const SpeedTestStatus status = test.status();
    if (status.phase != "done" || status.ended != "stopped" || !status.error.empty()) {
      return fail("刚开始就停下的测速应该标成中途停下，实际 " + status.phase + " " + status.error);
    }
  }

  {
    SpeedTest test;
    if (const std::string error = test.start(drive, "/nothing", options); !error.empty()) return fail(error);
    if (!finish(test, 5)) return fail("测速没有按时结束");
    if (test.status().phase != "error") return fail("游戏库里没有文件时测速应该报错");
  }
  return 0;
}

// 安装耗时：嵌套时只记最里面那项，别的线程不记，写入经 timed_sink 记到写入、新建文件记到开关文件，没开始解压时没有总账。
int install_clock_test() {
  const auto nap = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
  {
    InstallClock idle;
    PhaseScope listing(InstallPhase::download);
    if (!idle.summary("空", "出错").empty()) return fail("没开始解压就有了安装耗时总账");
  }
  const auto root = std::filesystem::temp_directory_path() / "panplay-clock-test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  auto local = make_local_sink(root.u8string());
  InstallClock clock;
  {
    const auto sink = timed_sink(*local);
    PhaseScope decoding(InstallPhase::decode);
    nap(60);
    {
      PhaseScope waiting(InstallPhase::download);
      nap(200);
      std::thread other([] {
        PhaseScope elsewhere(InstallPhase::write);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      });
      other.join();
    }
    std::string error;
    auto writer = sink->open_writer("/a.bin", 4, &error);
    if (!writer) return fail("计时的 sink 开不了文件: " + error);
    const std::uint8_t bytes[4] = {1, 2, 3, 4};
    if (!writer->write(bytes, sizeof(bytes)).empty() || !writer->finish().empty()) return fail("计时的 sink 写不进去");
  }
  std::filesystem::remove_all(root, ec);
  const std::string line = clock.summary("测试", "已装好");
  const auto percent = [&](const std::string& name) {
    const auto at = line.find("，" + name + " ");
    return at == std::string::npos ? -1 : std::atoi(line.c_str() + at + std::strlen("，") + name.size() + 1);
  };
  if (line.find("时间大多花在等下载") == std::string::npos || line.find("新建 1 个文件") == std::string::npos ||
      line.find("写了 1 KB") == std::string::npos) {
    return fail("安装耗时总账不对: " + line);
  }
  if (percent("等下载") < 60 || percent("等下载") > 85 || percent("解压") < 12) return fail("安装耗时占比不对: " + line);
  return 0;
}

int game_root_test() {
  const auto param = pattern(300, 1);
  const auto eboot = pattern(5000, 2);
  const auto prx = pattern(700, 3);
  FakeDrive drive;
  drive.add("/apps/panplay/Wrapped PPSA70000/game.zip",
            make_zip({{"Release/PPSA70000-app0/sce_sys/param.json", param, true},
                      {"Release/PPSA70000-app0/eboot.bin", eboot, true},
                      {"Release/PPSA70000-app0/sce_module/lib.prx", prx, false},
                      {"Release/PPSA70000-app0/extra/sce_sys/param.json", prx, false},
                      {"Release/readme.txt", pattern(20, 4), false}},
                     {})
                .front());
  MemorySink sink;
  const std::string old_file = "/data/homebrew/PPSA70000/PPSA70000-app0/eboot.bin";
  const std::string old_marker =
      "{\"entries\":[{\"crc32\":1,\"name\":\"PPSA70000-app0/eboot.bin\",\"size\":5000}],\"plugin\":\"fake\",\"title_id\":\"PPSA70000\"}";
  sink.store(old_file, eboot.data(), eboot.size());
  sink.store("/data/homebrew/PPSA70000/.panplay-done.json", reinterpret_cast<const std::uint8_t*>(old_marker.data()),
             old_marker.size());
  InstallRequest request;
  request.source_dir = "/apps/panplay/Wrapped PPSA70000";
  request.prefetch.connections = 1;
  const InstallResult result = install_game(drive, sink, request);
  if (result.status != "installed") return fail("外面包了目录的游戏没装上: " + result.message);
  if (result.files_total != 4) return fail("游戏目录外的文件不该算进去");
  if (result.message.find("1 个文件没有装") == std::string::npos) return fail("没有提示跳过了游戏目录外的文件");
  const std::string root = "/data/homebrew/PPSA70000/";
  if (sink.files[root + "sce_sys/param.json"].bytes != param) return fail("sce_sys 没有放在游戏目录第一层");
  if (sink.files[root + "eboot.bin"].bytes != eboot) return fail("eboot.bin 没有放在游戏目录第一层");
  if (sink.files[root + "sce_module/lib.prx"].bytes != prx) return fail("子目录没有跟着游戏根目录一起上移");
  for (const auto& [path, file] : sink.files) {
    if (path.find("Release/") != std::string::npos || path.find("readme.txt") != std::string::npos) {
      return fail("外层目录或目录外的文件被装进去了: " + path);
    }
  }
  if (sink.files.count(old_file) != 0) return fail("旧目录结构留下的文件没有清掉");

  FakeDrive two;
  two.add("/apps/panplay/Two PPSA70001/game.zip", make_zip({{"a/sce_sys/param.json", param, false},
                                                           {"b/sce_sys/param.json", param, false}},
                                                          {})
                                                     .front());
  MemorySink unused;
  InstallRequest ambiguous;
  ambiguous.source_dir = "/apps/panplay/Two PPSA70001";
  const InstallResult refused = install_game(two, unused, ambiguous);
  if (refused.status != "error" || refused.message.find("多个游戏目录") == std::string::npos) {
    return fail("有两个游戏目录时应该拒绝安装");
  }

  // 主游戏在根层，正式补丁各占一个子目录按名字里的数字排，固件补丁最后叠上去。
  const auto bytes = [](const char* text) { return std::vector<std::uint8_t>(text, text + std::strlen(text)); };
  const std::string dir = "/apps/panplay/Layers PPSA71000";
  const auto game_zip = make_zip({{"sce_sys/param.json", bytes("param-1.00"), false},
                                  {"eboot.bin", bytes("eboot-1.00"), true},
                                  {"data/a.bin", bytes("a-1.00"), true},
                                  {"data/keep.bin", bytes("keep"), false}},
                                 {})
                            .front();
  const auto patch10 = make_zip({{"PPSA71000-patch/sce_sys/param.json", bytes("param-1.10"), false},
                                 {"PPSA71000-patch/eboot.bin", bytes("eboot-1.10"), true}},
                                {})
                           .front();
  const auto patch2 = make_zip({{"sce_sys/param.json", bytes("param-1.05"), false},
                                {"eboot.bin", bytes("eboot-1.05"), true},
                                {"data/a.bin", bytes("a-1.05"), true}},
                               {})
                          .front();
  const auto backport = make_zip({{"eboot.bin", bytes("eboot-fix"), true}}, {}).front();
  const auto library = [&](bool with_patch2, bool with_patch10) {
    auto drive = std::make_unique<FakeDrive>();
    drive->add(dir + "/game.zip", game_zip);
    drive->add(dir + "/backport.zip", backport);
    if (with_patch2) drive->add(dir + "/补丁2 v1.05/patch.zip", patch2);
    if (with_patch10) drive->add(dir + "/补丁10 v1.10/patch.zip", patch10);
    return drive;
  };
  MemorySink console;
  CountingSink counted(console);
  InstallRequest layered_request;
  layered_request.source_dir = dir;
  const std::string out = "/data/homebrew/PPSA71000/";
  const auto expect = [&](const std::string& name, const char* text, const std::string& why) {
    return console.files[out + name].bytes == bytes(text) ? 0 : fail(why + "（" + name + "）");
  };
  const auto staging_clean = [&]() {
    for (const auto& entry : console.files) {
      if (entry.first.find(".panplay-staging") != std::string::npos) return fail("暂存区有文件留下: " + entry.first);
    }
    return 0;
  };

  std::string scan_error;
  // 先装只有一个补丁的版本，再放第二个补丁：只叠补丁，主游戏的文件一个都不重写。
  auto shelf = library(true, false);
  InstallResult stacked = install_game(*shelf, counted, layered_request);
  if (stacked.status != "installed") return fail("主游戏加补丁没装上: " + stacked.message);
  if (install_outdated(counted, "/data/homebrew", dir, scan_library(*shelf, "/apps/panplay", &scan_error).front().sets)) {
    return fail("刚装好就说有更新");
  }
  shelf = library(true, true);
  if (!install_outdated(counted, "/data/homebrew", dir, scan_library(*shelf, "/apps/panplay", &scan_error).front().sets)) {
    return fail("网盘里多了补丁没有显示有更新");
  }
  counted.reset();
  stacked = install_game(*shelf, counted, layered_request);
  if (stacked.status != "installed" || stacked.message.find("已更新") == std::string::npos) {
    return fail("多了补丁没有只装补丁: " + stacked.message);
  }
  if (counted.writes_ending("/keep.bin") != 0 || counted.writes_ending("/a.bin") != 0) return fail("只装补丁时重写了主游戏的文件");
  if (int code = expect("sce_sys/param.json", "param-1.10", "param 不是最后一个补丁的"); code != 0) return code;
  if (int code = expect("eboot.bin", "eboot-fix", "固件补丁没有最后叠上去"); code != 0) return code;
  if (int code = expect("data/a.bin", "a-1.05", "前一个补丁的文件被丢了"); code != 0) return code;
  if (int code = expect("data/keep.bin", "keep", "补丁没碰的文件丢了"); code != 0) return code;
  if (int code = staging_clean(); code != 0) return code;
  if (install_game(*shelf, counted, layered_request).status != "already") return fail("叠完补丁再装没有显示已安装");

  // 去掉一个补丁就分不出原来的内容了，只能整个重装。
  shelf = library(false, true);
  stacked = install_game(*shelf, counted, layered_request);
  if (stacked.status != "installed" || stacked.message.find("已更新") != std::string::npos) {
    return fail("少了补丁应该整个重装: " + stacked.message);
  }
  if (int code = expect("data/a.bin", "a-1.00", "去掉补丁重装后没有恢复主游戏的文件"); code != 0) return code;
  if (int code = expect("eboot.bin", "eboot-fix", "重装后固件补丁没有叠上去"); code != 0) return code;
  if (int code = staging_clean(); code != 0) return code;

  // 用户选了不装的补丁：装的时候跳过，也不算有更新。
  shelf = library(true, true);
  const auto sets = scan_library(*shelf, "/apps/panplay", &scan_error).front().sets;
  const auto set_in = [&](const std::string& folder) {
    for (const auto& set : sets) {
      if (archive_folder(dir, set.parts.front().path) == folder) return archive_set_id(set);
    }
    return std::string();
  };
  const std::string skip2 = set_in("补丁2 v1.05");
  const std::string skip10 = set_in("补丁10 v1.10");
  if (skip2.empty() || skip10.empty()) return fail("找不到补丁对应的压缩包组");
  if (!install_outdated(counted, "/data/homebrew", dir, sets)) return fail("没跳过补丁时应该显示有更新");
  if (install_outdated(counted, "/data/homebrew", dir, sets, {skip2})) return fail("跳过的补丁不该算更新");
  layered_request.skip_sets = {skip2};
  counted.reset();
  if (install_game(*shelf, counted, layered_request).status != "already") return fail("跳过新补丁后应该显示已安装");
  if (counted.writes_ending(".bin") != 0) return fail("跳过新补丁后还写了文件");

  // 不装已经装上的补丁：整个重装，回到没有这个补丁的样子。
  layered_request.skip_sets = {skip2, skip10};
  stacked = install_game(*shelf, counted, layered_request);
  if (stacked.status != "installed" || stacked.message.find("已更新") != std::string::npos) {
    return fail("去掉已装的补丁应该整个重装: " + stacked.message);
  }
  if (int code = expect("sce_sys/param.json", "param-1.00", "跳过的补丁还留在游戏里"); code != 0) return code;
  if (int code = expect("eboot.bin", "eboot-fix", "跳过补丁后固件补丁没有叠上去"); code != 0) return code;
  const auto layers = installed_sets(counted, "/data/homebrew", dir);
  if (layers.size() != 2 || std::find(layers.begin(), layers.end(), skip10) != layers.end()) {
    return fail("完成标记里记的层不对");
  }

  // 再勾回来：只叠这一个补丁。
  layered_request.skip_sets = {skip2};
  counted.reset();
  stacked = install_game(*shelf, counted, layered_request);
  if (stacked.status != "installed" || stacked.message.find("已更新") == std::string::npos) {
    return fail("勾回补丁应该只装补丁: " + stacked.message);
  }
  if (counted.writes_ending("/a.bin") != 0) return fail("勾回补丁时重写了主游戏的文件");
  if (int code = expect("sce_sys/param.json", "param-1.10", "勾回的补丁没装上"); code != 0) return code;

  std::set<std::string> everything;
  for (const auto& set : sets) everything.insert(archive_set_id(set));
  layered_request.skip_sets = everything;
  if (install_game(*shelf, counted, layered_request).message != "没有选中要安装的压缩包") return fail("全跳过时应该拒绝");
  return staging_clean();
}

// 只有 .pkg 安装包的压缩包要尽早拒绝；各个压缩包里内容一样的同名文件（资源站的说明文件）留一份。
int package_and_duplicate_test() {
  const auto ad = pattern(200, 11);
  const std::vector<std::uint8_t> pkg(static_cast<std::size_t>(kInstallPackageMinBytes), 0);
  const auto has_pkg = [](const MemorySink& sink) {
    for (const auto& entry : sink.files) {
      if (entry.first.size() > 4 && entry.first.compare(entry.first.size() - 4, 4, ".pkg") == 0) return true;
    }
    return false;
  };

  FakeDrive drive;
  drive.add("/apps/panplay/Pkg CUSA20000/base.zip",
            make_zip({{"2468c.com.txt", ad, false}, {"Game_CUSA20000_v1.00.pkg", pkg, true}}, {}).front());

  const auto rar_dir = std::filesystem::temp_directory_path() / "panplay-pkg-src";
  std::error_code ignored;
  std::filesystem::remove_all(rar_dir, ignored);
  std::filesystem::create_directories(rar_dir);
  {
    std::ofstream(rar_dir / "2468c.com.txt", std::ios::binary).write(reinterpret_cast<const char*>(ad.data()), 200);
    std::ofstream big(rar_dir / "Game_CUSA20002_v1.07.pkg", std::ios::binary);
    big.write(reinterpret_cast<const char*>(pkg.data()), static_cast<std::streamsize>(pkg.size()));
  }
  const auto rar_path = rar_dir / "patch.rar";
  if (!run_rar("a -ep -idq -y -ma5 -m1 " + shell_quoted(rar_path) + " " + shell_quoted(rar_dir / "2468c.com.txt") + " " +
               shell_quoted(rar_dir / "Game_CUSA20002_v1.07.pkg"))) {
    return fail("RAR 没能生成 PKG 测试 RAR");
  }
  {
    std::ifstream in(rar_path, std::ios::binary);
    drive.add("/apps/panplay/Pkg CUSA20002/patch.rar", std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {}));
  }
  std::filesystem::remove_all(rar_dir, ignored);

  for (const char* dir : {"/apps/panplay/Pkg CUSA20000", "/apps/panplay/Pkg CUSA20002"}) {
    MemorySink sink;
    InstallRequest request;
    request.source_dir = dir;
    const InstallResult refused = install_game(drive, sink, request);
    if (refused.status != "error" || refused.message.find("PKG 安装包") == std::string::npos) {
      return fail(std::string("PKG 安装包没有被认出来: ") + dir + " " + refused.message);
    }
    if (refused.retryable) return fail(std::string("PKG 安装包失败不该能重试: ") + dir);
    std::string list_error;
    const auto sets = group_archives(collect_archives(drive, dir, &list_error));
    if (sets.size() != 1 || refused.install_package_set != archive_set_id(sets.front())) {
      return fail(std::string("PKG 安装包失败要带上是哪组压缩包: ") + dir + " " + refused.install_package_set);
    }
    if (has_pkg(sink)) return fail(std::string("认出 PKG 之前就把它写到主机上了: ") + dir);
  }

  const auto a = pattern(3000, 12);
  const auto b = pattern(4000, 13);
  drive.add("/apps/panplay/Dup CUSA20001/a.zip", make_zip({{"data/a.bin", a, true}, {"2468c.com.txt", ad, false}}, {}).front());
  drive.add("/apps/panplay/Dup CUSA20001/b.zip", make_zip({{"data/b.bin", b, true}, {"2468c.com.txt", ad, false}}, {}).front());
  MemorySink sink;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Dup CUSA20001";
  const InstallResult installed = install_game(drive, sink, request);
  if (installed.status != "installed" || installed.files_total != 3) {
    return fail("内容一样的同名文件不该算重复: " + installed.message);
  }
  const std::string root = "/data/homebrew/CUSA20001/";
  if (sink.files[root + "data/a.bin"].bytes != a || sink.files[root + "data/b.bin"].bytes != b ||
      sink.files[root + "2468c.com.txt"].bytes != ad) {
    return fail("去重后的文件内容不对");
  }
  for (const auto& entry : sink.files) {
    if (entry.first.find(".panplay-staging") != std::string::npos) return fail("去掉的重复文件留在了暂存区: " + entry.first);
  }

  drive.add("/apps/panplay/Clash CUSA20003/a.zip", make_zip({{"data/a.bin", a, true}, {"readme.txt", ad, false}}, {}).front());
  drive.add("/apps/panplay/Clash CUSA20003/b.zip", make_zip({{"data/b.bin", b, true}, {"readme.txt", b, false}}, {}).front());
  MemorySink clash_sink;
  request.source_dir = "/apps/panplay/Clash CUSA20003";
  const InstallResult clash = install_game(drive, clash_sink, request);
  if (clash.status != "error" || clash.message.find("重复文件") == std::string::npos) {
    return fail("内容不同的同名文件应该报重复");
  }
  if (!clash.retryable) return fail("普通失败应该能重试");
  if (!clash.install_package_set.empty()) return fail("不是 PKG 的失败不该标成 PKG 安装包");
  return 0;
}

int many_files_test() {
  FakeDrive drive;
  std::vector<ZipInput> inputs;
  for (int i = 0; i < 3000; ++i) {
    inputs.push_back({"data/file" + std::to_string(i) + ".bin", pattern(static_cast<std::size_t>(100 + i % 700), static_cast<std::uint8_t>(i)),
                      i % 3 == 0});
  }
  const auto zip = make_zip(inputs, {});
  drive.add("/apps/panplay/Many CUSA30000/game.zip", zip.front());
  MemorySink memory;
  CountingSink sink(memory);
  InstallRequest request;
  request.source_dir = "/apps/panplay/Many CUSA30000";
  request.prefetch.connections = 1;
  const InstallResult result = install_game(drive, sink, request);
  if (result.status != "installed" || result.files_total != 3000) return fail("3000 个小文件没有装完: " + result.message);
  if (drive.read_at_calls > 4) return fail("读 ZIP 目录用了 " + std::to_string(drive.read_at_calls.load()) + " 次随机读");
  if (drive.stream_opens > 2) return fail("顺序解压打开了 " + std::to_string(drive.stream_opens.load()) + " 次流");
  if (sink.writers_opened() != 3000) return fail("每个文件应该只开一次写入流");
  if (sink.appends() > 3000 / 100 + 5) return fail("进度日志追加了 " + std::to_string(sink.appends()) + " 次，没有攒批");
  const auto& sample = memory.files["/data/homebrew/CUSA30000/file2999.bin"];
  if (sample.bytes != inputs.back().data) return fail("最后一个文件内容不对");
  return 0;
}

int zip64_split_test() {
  FakeDrive drive;
  const auto first = pattern(300000, 3);
  const auto second = pattern(5000, 11);
  ZipLayout layout;
  layout.zip64 = true;
  layout.descriptor = true;
  const auto zip = make_zip({{"sce_sys/param.json", second, true}, {"eboot.bin", first, false}}, layout);
  const auto& bytes = zip.front();
  const std::size_t half = bytes.size() / 2;
  drive.add("/apps/panplay/Raw PPSA40000/game.zip.001", std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(half)));
  drive.add("/apps/panplay/Raw PPSA40000/game.zip.002", std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(half), bytes.end()));
  MemorySink sink;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Raw PPSA40000";
  const InstallResult result = install_game(drive, sink, request);
  if (result.status != "installed") return fail("ZIP64 加数据描述符的切分包没有装上: " + result.message);
  if (sink.files["/data/homebrew/PPSA40000/eboot.bin"].bytes != first) return fail("ZIP64 包里的大文件内容不对");
  if (sink.files["/data/homebrew/PPSA40000/sce_sys/param.json"].bytes != second) return fail("ZIP64 包里的压缩文件内容不对");
  return 0;
}

int large_file_test() {
  // 一个超过 4 GB 的文件，存储方式，ZIP64。内容是重复的 1 MB 块，CRC 用 crc32_combine 直接算。
  constexpr std::int64_t kBlock = 1 << 20;
  constexpr std::int64_t kBlocks = 4 * 1024 + 300;
  constexpr std::int64_t kSize = kBlock * kBlocks;
  const auto block = pattern(static_cast<std::size_t>(kBlock), 5);
  const auto block_crc = static_cast<std::uint32_t>(::crc32(::crc32(0L, Z_NULL, 0), block.data(), static_cast<uInt>(block.size())));
  uLong crc = block_crc;
  for (std::int64_t i = 1; i < kBlocks; ++i) crc = ::crc32_combine(crc, block_crc, static_cast<z_off_t>(kBlock));
  const std::string name = "image0.dat";

  std::vector<std::uint8_t> head;
  append32(head, 0x04034b50);
  append16(head, 45);
  append16(head, 0);
  append16(head, 0);
  append32(head, 0);
  append32(head, static_cast<std::uint32_t>(crc));
  append32(head, 0xffffffffu);
  append32(head, 0xffffffffu);
  append16(head, static_cast<std::uint16_t>(name.size()));
  append16(head, 20);
  head.insert(head.end(), name.begin(), name.end());
  append16(head, 0x0001);
  append16(head, 16);
  append64(head, static_cast<std::uint64_t>(kSize));
  append64(head, static_cast<std::uint64_t>(kSize));

  std::vector<std::uint8_t> tail;
  append32(tail, 0x02014b50);
  append16(tail, 45);
  append16(tail, 45);
  append16(tail, 0);
  append16(tail, 0);
  append32(tail, 0);
  append32(tail, static_cast<std::uint32_t>(crc));
  append32(tail, 0xffffffffu);
  append32(tail, 0xffffffffu);
  append16(tail, static_cast<std::uint16_t>(name.size()));
  append16(tail, 4 + 24);
  append16(tail, 0);
  append16(tail, 0);
  append16(tail, 0);
  append32(tail, 0);
  append32(tail, 0xffffffffu);
  tail.insert(tail.end(), name.begin(), name.end());
  append16(tail, 0x0001);
  append16(tail, 24);
  append64(tail, static_cast<std::uint64_t>(kSize));
  append64(tail, static_cast<std::uint64_t>(kSize));
  append64(tail, 0);
  const std::uint64_t central = head.size() + static_cast<std::uint64_t>(kSize);
  const std::uint64_t central_size = tail.size();
  const std::uint64_t zip64_record = central + central_size;
  append32(tail, 0x06064b50);
  append64(tail, 44);
  append16(tail, 45);
  append16(tail, 45);
  append32(tail, 0);
  append32(tail, 0);
  append64(tail, 1);
  append64(tail, 1);
  append64(tail, central_size);
  append64(tail, central);
  append32(tail, 0x07064b50);
  append32(tail, 0);
  append64(tail, zip64_record);
  append32(tail, 1);
  append32(tail, 0x06054b50);
  append16(tail, 0);
  append16(tail, 0);
  append16(tail, 0xffff);
  append16(tail, 0xffff);
  append32(tail, 0xffffffffu);
  append32(tail, 0xffffffffu);
  append16(tail, 0);

  const std::int64_t head_size = static_cast<std::int64_t>(head.size());
  const std::int64_t total = head_size + kSize + static_cast<std::int64_t>(tail.size());
  FakeDrive drive;
  drive.add_generated("/apps/panplay/Huge PPSA50000/game.zip", total, [&](std::int64_t offset, std::uint8_t* dst, std::size_t n) {
    while (n > 0) {
      std::size_t step = 0;
      if (offset < head_size) {
        step = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(n), head_size - offset));
        std::memcpy(dst, head.data() + offset, step);
      } else if (offset < head_size + kSize) {
        const std::int64_t in_block = (offset - head_size) % kBlock;
        step = static_cast<std::size_t>(std::min<std::int64_t>({static_cast<std::int64_t>(n), kBlock - in_block,
                                                                head_size + kSize - offset}));
        std::memcpy(dst, block.data() + in_block, step);
      } else {
        const std::int64_t at = offset - head_size - kSize;
        step = n;
        std::memcpy(dst, tail.data() + at, step);
      }
      dst += step;
      offset += static_cast<std::int64_t>(step);
      n -= step;
    }
  });
  MemorySink sink;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Huge PPSA50000";
  request.prefetch.connections = 1;
  const auto started = std::chrono::steady_clock::now();
  const InstallResult result = install_game(drive, sink, request);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (result.status != "installed") return fail("超过 4 GB 的文件没有装上: " + result.message);
  if (sink.files["/data/homebrew/PPSA50000/image0.dat"].size != kSize) return fail("超过 4 GB 的文件大小不对");
  if (drive.stream_opens != 1) return fail("5 GB 文件打开了 " + std::to_string(drive.stream_opens.load()) + " 次流");
  std::cout << "  超过 4 GB 的单文件：" << (kSize >> 20) << " MB，" << seconds << " 秒，网盘随机读 " << drive.read_at_calls
            << " 次，读取流 " << drive.stream_opens << " 次\n";
  return 0;
}

int cancel_install_test() {
  FakeDrive drive;
  std::vector<ZipInput> inputs;
  for (int i = 0; i < 20; ++i) inputs.push_back({"f" + std::to_string(i), pattern(5000, static_cast<std::uint8_t>(i)), false});
  drive.add("/apps/panplay/Stop CUSA60000/game.zip", make_zip(inputs, {}).front());
  MemorySink sink;
  CancelToken cancel;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Stop CUSA60000";
  request.cancel = &cancel;
  request.on_progress = [&](const InstallResult& snapshot) {
    if (snapshot.files_done >= 3) cancel.cancel();
  };
  const InstallResult stopped = install_game(drive, sink, request);
  if (stopped.status != "interrupted" || stopped.files_done < 3 || stopped.files_done >= 20) {
    return fail("取消安装后状态不对: " + stopped.status + " " + std::to_string(stopped.files_done));
  }
  InstallRequest again;
  again.source_dir = request.source_dir;
  const InstallResult finished = install_game(drive, sink, again);
  if (finished.status != "installed") return fail("取消后继续没有装完: " + finished.message);
  return 0;
}

// 压缩不了的随机内容，用来把 7z 撑出多个分卷。
std::vector<std::uint8_t> noise(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> bytes(size);
  std::uint32_t state = seed * 2654435761u + 1;
  for (auto& byte : bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<std::uint8_t>(state >> 24);
  }
  return bytes;
}

std::vector<std::uint8_t> read_disk_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void write_disk_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

int sevenzip_test() {
  const auto work = std::filesystem::temp_directory_path() / "panplay-7z-test";
  std::error_code ignored;
  std::filesystem::remove_all(work, ignored);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code remove_error;
      std::filesystem::remove_all(path, remove_error);
    }
  } cleanup{work};

  // 游戏根目录是 Game/，外面的 readme.txt 不装。eboot.bin 里塞了很多 E8，让 x86 过滤器真的有东西可转。
  const auto param = pattern(300, 1);
  auto eboot = pattern(300000, 2);
  for (std::size_t i = 0; i < eboot.size(); i += 37) eboot[i] = 0xE8;
  const auto big = noise(400000, 3);
  const auto src = work / "src";
  write_disk_file(src / "Game" / "sce_sys" / "param.json", param);
  write_disk_file(src / "Game" / "eboot.bin", eboot);
  write_disk_file(src / "Game" / "data" / "big.bin", big);
  write_disk_file(src / "Game" / "data" / "empty.bin", {});
  std::filesystem::create_directories(src / "Game" / "data" / "emptydir");
  write_disk_file(src / "readme.txt", pattern(50, 4));
  const std::map<std::string, const std::vector<std::uint8_t>*> expected{
      {"sce_sys/param.json", &param}, {"eboot.bin", &eboot}, {"data/big.bin", &big}};

  FakeDrive drive;
  // 打包成 7z，按名字顺序把各分卷放进网盘目录。返回分卷数，失败时为 0。
  const auto pack = [&](const std::string& name, const std::string& options, const std::string& dir) -> std::size_t {
    const auto out_dir = work / name;
    std::filesystem::create_directories(out_dir);
    if (!run_7z("a -y -bd " + options + " " + shell_quoted(out_dir / "game.7z") + " " + shell_quoted(src / "Game") + " " +
                shell_quoted(src / "readme.txt"))) {
      return 0;
    }
    std::vector<std::filesystem::path> volumes;
    for (const auto& entry : std::filesystem::directory_iterator(out_dir)) volumes.push_back(entry.path());
    std::sort(volumes.begin(), volumes.end());
    for (const auto& volume : volumes) drive.add(dir + "/" + volume.filename().string(), read_disk_file(volume));
    return volumes.size();
  };

  struct Case {
    const char* name;
    const char* options;
    std::size_t min_volumes;
  };
  const Case cases[] = {
      {"lzma2-bcj-split", "-mx=5 -mf=BCJ -v100k", 4},
      {"lzma-arm64", "-m0=ARM64 -m1=LZMA:d=64k -ms=off", 1},
      {"delta-lzma2", "-m0=Delta:4 -m1=LZMA2 -ms=on", 1},
      {"copy", "-m0=Copy", 1},
  };
  int index = 0;
  for (const auto& test : cases) {
    const std::string id = "CUSA3000" + std::to_string(index++);
    const std::string dir = std::string("/apps/panplay/Seven ") + id;
    const std::size_t volumes = pack(test.name, test.options, dir);
    if (volumes == 0) return fail(std::string("7-Zip 没能生成测试 7z（可以用 PANPLAY_7Z 指定 7z 程序）: ") + test.name);
    if (volumes < test.min_volumes) return fail(std::string("测试 7z 分卷数不对: ") + test.name);

    std::string error;
    const auto games = scan_library(drive, "/apps/panplay", &error);
    const GameFolder* game = nullptr;
    for (const auto& found : games) {
      if (found.title_id == id) game = &found;
    }
    if (game == nullptr || game->sets.size() != 1 || game->sets[0].format != "7z" || game->sets[0].parts.size() != volumes) {
      return fail(std::string("没有把 7z 分卷认成一组: ") + test.name);
    }

    const auto local_root = work / ("install-" + std::string(test.name));
    auto local = make_local_sink(local_root.u8string());
    CountingSink counted(*local);
    InstallRequest request;
    request.source_dir = dir;
    request.dest_prefix = "/data/homebrew";
    std::int64_t last_bytes = -1;
    std::int64_t bytes_total = 0;
    bool bytes_backwards = false;
    request.on_progress = [&](const InstallResult& snapshot) {
      if (snapshot.bytes_done < last_bytes) bytes_backwards = true;
      last_bytes = snapshot.bytes_done;
      bytes_total = snapshot.bytes_total;
    };
    const bool resume = test.min_volumes > 1;
    if (resume) {
      // 中途断开，第二次接着装，装好的文件不再重写。
      request.stop_after_output_bytes = 650000;
      const InstallResult stopped = install_game(drive, counted, request);
      if (stopped.status != "interrupted") return fail("7z 中途断开后状态不对: " + stopped.status + " " + stopped.message);
      request.stop_after_output_bytes = -1;
      counted.reset();
      last_bytes = -1;
    }
    const InstallResult installed = install_game(drive, counted, request);
    if (installed.status != "installed" || installed.files_total != 4) {
      return fail(std::string("7z 没装好: ") + test.name + " " + installed.message);
    }
    if (resume && (counted.writers_opened() >= 4 || installed.files_checked == 0)) return fail("7z 续装时重写了已装好的文件");
    if (bytes_total <= 0 || last_bytes != bytes_total || bytes_backwards) {
      return fail(std::string("7z 按字节的安装进度不对: ") + test.name);
    }
    const auto game_dir = local_root / "data" / "homebrew" / id;
    for (const auto& [name, bytes] : expected) {
      if (read_disk_file(game_dir / name) != *bytes) return fail(std::string("7z 解压内容不对: ") + test.name + " " + name);
    }
    if (!std::filesystem::exists(game_dir / "data" / "empty.bin") || std::filesystem::file_size(game_dir / "data" / "empty.bin") != 0) {
      return fail(std::string("7z 里的空文件没装上: ") + test.name);
    }
    if (!std::filesystem::is_directory(game_dir / "data" / "emptydir")) return fail(std::string("7z 里的空目录没建出来: ") + test.name);
    if (std::filesystem::exists(game_dir / "readme.txt") || std::filesystem::exists(game_dir / "Game")) {
      return fail(std::string("7z 游戏根目录外的文件被装进去了: ") + test.name);
    }
    if (std::filesystem::exists(local_root / "data" / "homebrew" / ".panplay-staging")) {
      return fail(std::string("7z 装完后暂存区没有清干净: ") + test.name);
    }
    if (install_game(drive, *local, request).status != "already") return fail(std::string("7z 装完后没有显示已安装: ") + test.name);

    if (std::string(test.name) == "copy") continue;
    MemorySink limited;
    request.on_progress = nullptr;
    request.max_dictionary = 1;
    const InstallResult too_big = install_game(drive, limited, request);
    if (too_big.status != "error" || too_big.message.find("超过上限") == std::string::npos || !too_big.retryable) {
      return fail(std::string("字典超过上限的 7z 没有被拒绝: ") + test.name + " " + too_big.message);
    }
    if (!limited.files.empty()) return fail(std::string("拒绝 7z 之前写了文件: ") + test.name);

    MemorySink starved_sink;
    request.max_dictionary = 0;
    set_big_memory_limit(1);
    const InstallResult starved = install_game(drive, starved_sink, request);
    set_big_memory_limit(0);
    if (starved.status != "error" || !is_out_of_memory_error(starved.message)) {
      return fail(std::string("7z 要不到字典内存时没有报内存不够: ") + test.name + " " + starved.message);
    }
    if (!starved.retryable || starved.auto_resume) return fail(std::string("7z 内存不够应该交给用户重试，不该自动再装: ") + test.name);
    if (big_memory_in_use() != 0) return fail(std::string("7z 解压结束后字典内存没有还掉: ") + test.name);
  }

  // 不支持的方法在读目录时就拒绝，不写任何文件。精简版 7zr 不能压 PPMd，生成不了就跳过这一项。
  struct Refused {
    const char* name;
    const char* options;
    const char* word;
    bool optional;
  };
  const Refused refused[] = {{"ppmd", "-m0=PPMd", "PPMd", true},
                             {"bcj2", "-mf=BCJ2", "BCJ2", false},
                             {"password", "-ppanplay", "密码", false},
                             {"password-header", "-ppanplay -mhe=on", "密码", false}};
  int refused_index = 0;
  for (const auto& test : refused) {
    const std::string dir = "/apps/panplay/Refused CUSA3100" + std::to_string(refused_index++);
    if (pack(test.name, test.options, dir) == 0) {
      if (test.optional) continue;
      return fail(std::string("7-Zip 没能生成测试 7z: ") + test.name);
    }
    MemorySink sink;
    InstallRequest request;
    request.source_dir = dir;
    const InstallResult result = install_game(drive, sink, request);
    if (result.status != "error" || result.message.find(test.word) == std::string::npos ||
        result.message.find("ZIP / RAR") == std::string::npos) {
      return fail(std::string("不支持的 7z 没有明确拒绝: ") + test.name + " " + result.message);
    }
    if (result.retryable) return fail(std::string("不支持的 7z 失败不该能重试: ") + test.name);
    if (!sink.files.empty()) return fail(std::string("拒绝 7z 之前写了文件: ") + test.name);
  }
  return 0;
}

}  // namespace

int run_install_pipeline_self_test() {
  std::vector<std::uint8_t> param(100);
  for (std::size_t i = 0; i < param.size(); ++i) param[i] = static_cast<std::uint8_t>(1 + i);
  std::vector<std::uint8_t> eboot(8000);
  for (std::size_t i = 0; i < eboot.size(); ++i) eboot[i] = static_cast<std::uint8_t>(9 + i);
  const std::vector<std::uint8_t> hello{'h', 'e', 'l', 'l', 'o'};
  std::vector<std::vector<std::uint8_t>> parts;
  for (std::size_t span = 97; span < 400 && parts.size() < 3; span += 13) {
    ZipLayout spanned;
    spanned.span = span;
    parts = make_zip({{"param.sfo", param, false}, {"eboot.bin", eboot, true}}, spanned);
  }
  if (parts.size() < 3) return fail("跨卷 ZIP 没有切开");
  const auto single_zip = make_zip({{"hello.txt", hello, false}}, {}).front();

  FakeDrive drive;
  add_spanned(drive, "/apps/panplay/Demo CUSA12345", "disc", parts);
  drive.add("/apps/panplay/Nested CUSA99999/inner/alone.zip", single_zip);
  drive.add("/apps/panplay/Old/game.7z", std::vector<std::uint8_t>{1, 2, 3, 4});
  drive.add("/apps/panplay/Single CUSA00001/game.zip", single_zip);

  const auto rar_dir = std::filesystem::temp_directory_path() / "panplay-rar-src";
  std::error_code rar_remove;
  std::filesystem::remove_all(rar_dir, rar_remove);
  std::filesystem::create_directories(rar_dir);
  {
    std::ofstream note(rar_dir / "note.txt", std::ios::binary);
    note << "rar-ok";
  }
  const auto rar_file = rar_dir / "game.rar";
  if (!run_rar("a -ep -idq -y -ma5 " + shell_quoted(rar_file) + " " + shell_quoted(rar_dir / "note.txt"))) {
    return fail("RAR 没能生成测试 RAR");
  }
  std::vector<std::uint8_t> rar_bytes;
  {
    std::ifstream rar_in(rar_file, std::ios::binary);
    rar_bytes.assign(std::istreambuf_iterator<char>(rar_in), {});
  }
  std::filesystem::remove(rar_file);
  if (rar_bytes.size() < 7 || std::string(rar_bytes.begin(), rar_bytes.begin() + 4) != "Rar!") return fail("测试 RAR 是空的");
  drive.add("/apps/panplay/Rar CUSA77777/game.rar", std::move(rar_bytes));

  // note.txt 太小，RAR 直接存储、解压时不分配字典；这一个压缩过，测要不到字典内存。
  std::string packed_text;
  for (int i = 0; i < 4096; ++i) packed_text += "panplay packed line " + std::to_string(i % 7) + "\n";
  std::ofstream(rar_dir / "packed.txt", std::ios::binary) << packed_text;
  const auto packed_rar = rar_dir / "packed.rar";
  if (!run_rar("a -ep -idq -y -ma5 -m3 " + shell_quoted(packed_rar) + " " + shell_quoted(rar_dir / "packed.txt"))) {
    return fail("RAR 没能生成压缩过的测试 RAR");
  }
  std::vector<std::uint8_t> packed_bytes;
  {
    std::ifstream packed_in(packed_rar, std::ios::binary);
    packed_bytes.assign(std::istreambuf_iterator<char>(packed_in), {});
  }
  if (packed_bytes.size() >= packed_text.size()) return fail("测试 RAR 没有压缩");
  drive.add("/apps/panplay/Packed CUSA75555/game.rar", std::move(packed_bytes));

  const auto wrap_dir = rar_dir / "Wrap";
  std::filesystem::create_directories(wrap_dir / "PPSA88888-app0" / "sce_sys");
  {
    std::ofstream(wrap_dir / "PPSA88888-app0" / "sce_sys" / "param.json", std::ios::binary) << "{\"titleId\":\"PPSA88888\"}";
    std::ofstream(wrap_dir / "PPSA88888-app0" / "eboot.bin", std::ios::binary) << "eboot-ok";
    std::ofstream(wrap_dir / "readme.txt", std::ios::binary) << "skip me";
  }
  const auto wrap_rar = rar_dir / "wrap.rar";
  if (!run_rar("a -r -ep1 -idq -y -ma5 " + shell_quoted(wrap_rar) + " " + shell_quoted(wrap_dir))) {
    return fail("RAR 没能生成带外层目录的测试 RAR");
  }
  std::vector<std::uint8_t> wrap_bytes;
  {
    std::ifstream wrap_in(wrap_rar, std::ios::binary);
    wrap_bytes.assign(std::istreambuf_iterator<char>(wrap_in), {});
  }
  const auto split_dir = rar_dir / "Split";
  std::filesystem::create_directories(split_dir);
  const auto split_data = pattern(150000, 5);
  const auto split_head = pattern(30000, 6);
  const auto split_tail = pattern(30000, 7);
  {
    std::ofstream head(split_dir / "a.bin", std::ios::binary);
    head.write(reinterpret_cast<const char*>(split_head.data()), static_cast<std::streamsize>(split_head.size()));
    std::ofstream big(split_dir / "big.bin", std::ios::binary);
    big.write(reinterpret_cast<const char*>(split_data.data()), static_cast<std::streamsize>(split_data.size()));
    std::ofstream tail(split_dir / "c.bin", std::ios::binary);
    tail.write(reinterpret_cast<const char*>(split_tail.data()), static_cast<std::streamsize>(split_tail.size()));
  }
  if (!run_rar("a -ep -idq -y -ma5 -m0 -v40k " + shell_quoted(split_dir / "split.rar") + " " + shell_quoted(split_dir / "a.bin") + " " +
               shell_quoted(split_dir / "big.bin") + " " + shell_quoted(split_dir / "c.bin"))) {
    return fail("RAR 没能生成跨卷测试 RAR");
  }
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> split_volumes;
  for (const auto& entry : std::filesystem::directory_iterator(split_dir)) {
    if (entry.path().extension() != ".rar") continue;
    std::ifstream part_in(entry.path(), std::ios::binary);
    split_volumes.emplace_back(entry.path().filename().string(),
                               std::vector<std::uint8_t>(std::istreambuf_iterator<char>(part_in), {}));
  }
  if (split_volumes.size() < 3 || split_volumes.size() > 9) return fail("跨卷测试 RAR 分卷数不对");
  std::sort(split_volumes.begin(), split_volumes.end());
  for (std::size_t i = 0; i < split_volumes.size(); ++i) {
    drive.add("/apps/panplay/Split CUSA66666/" + split_volumes[i].first, split_volumes[i].second);
    if (i + 1 < split_volumes.size()) drive.add("/apps/panplay/Cut CUSA66667/" + split_volumes[i].first, split_volumes[i].second);
  }
  // WinRAR 把目录条目都放在压缩包最后。
  const auto many_dir = rar_dir / "Many";
  std::filesystem::create_directories(many_dir / "pack" / "a_empty");
  std::vector<std::vector<std::uint8_t>> many_files;
  const auto many_name = [](int i) { return std::string(i < 3 ? "d1" : "d2") + "/f" + std::to_string(i) + ".bin"; };
  for (int i = 0; i < 6; ++i) {
    many_files.push_back(pattern(20000, static_cast<std::uint8_t>(20 + i)));
    std::filesystem::create_directories(many_dir / "pack" / (i < 3 ? "d1" : "d2"));
    std::ofstream out(many_dir / "pack" / many_name(i), std::ios::binary);
    out.write(reinterpret_cast<const char*>(many_files.back().data()), static_cast<std::streamsize>(many_files.back().size()));
  }
  if (!run_rar("a -r -ep1 -idq -y -ma5 -m0 -v40k " + shell_quoted(many_dir / "many.rar") + " " +
               shell_quoted(many_dir / "pack" / "*"))) {
    return fail("RAR 没能生成续装点测试 RAR");
  }
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> many_volumes;
  for (const auto& entry : std::filesystem::directory_iterator(many_dir)) {
    if (entry.path().extension() != ".rar") continue;
    std::ifstream part_in(entry.path(), std::ios::binary);
    many_volumes.emplace_back(entry.path().filename().string(),
                              std::vector<std::uint8_t>(std::istreambuf_iterator<char>(part_in), {}));
  }
  if (many_volumes.size() < 3 || many_volumes.size() > 9) return fail("续装点测试 RAR 分卷数不对");
  std::sort(many_volumes.begin(), many_volumes.end());
  for (const auto& volume : many_volumes) drive.add("/apps/panplay/Many CUSA55555/" + volume.first, volume.second);
  std::filesystem::remove_all(rar_dir, rar_remove);
  drive.add("/apps/panplay/Wrapped PPSA88888/game.rar", std::move(wrap_bytes));
  const std::vector<std::uint8_t> fixed_eboot{'e', 'b', 'o', 'o', 't', '-', 'f', 'i', 'x'};
  drive.add("/apps/panplay/Wrapped PPSA88888/fix.zip",
            make_zip({{"eboot.bin", fixed_eboot, true}, {"sce_sys/about/right.sprx", pattern(40, 9), false}}, {}).front());

  std::string error;
  const auto games = scan_library(drive, "/apps/panplay", &error);
  if (!error.empty()) return fail(error);
  const GameFolder* demo = nullptr;
  const GameFolder* nested = nullptr;
  const GameFolder* old = nullptr;
  for (const auto& game : games) {
    if (game.title_id == "CUSA12345") demo = &game;
    if (game.title_id == "CUSA99999") nested = &game;
    if (game.name == "Old") old = &game;
  }
  if (!demo || demo->sets.size() != 1 || demo->sets[0].format != "zip" || demo->sets[0].parts.size() != parts.size()) {
    return fail("没有识别出分卷 ZIP");
  }
  if (demo->sets[0].parts.front().name != "disc.z01" || demo->sets[0].parts.back().name != "disc.zip") {
    return fail("分卷顺序不对");
  }
  if (!nested || nested->archive_bytes == 0) return fail("没有扫到下一层的压缩包");
  if (!old || old->sets.size() != 1 || old->sets[0].format != "7z") return fail("没有认出 7z");

  Destination parsed;
  if (!parse_destination("ftp://127.0.0.1:2121", &parsed, &error) || !parsed.ftp || parsed.endpoint.port != 2121) {
    return fail("FTP 地址没有解析出来");
  }

  InstallRequest rejected;
  rejected.source_dir = "/apps/panplay/Old";
  auto local_probe = make_local_sink(std::filesystem::temp_directory_path().u8string());
  InstallResult only_7z = install_game(drive, *local_probe, rejected);
  if (only_7z.status != "error" || only_7z.message.find("7z") == std::string::npos) return fail("坏的 7z 没有报错");

  const auto root = std::filesystem::temp_directory_path() / "panplay-install-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code remove_error;
      std::filesystem::remove_all(path, remove_error);
    }
  } cleanup{root};

  {
    const auto local_root = root / "local";
    auto local = make_local_sink(local_root.u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Single CUSA00001";
    request.dest_prefix = "/data/homebrew";
    bool saw_reading_before_total = false;
    bool saw_total = false;
    request.on_progress = [&](const InstallResult& snapshot) {
      if (!saw_total && snapshot.message == "正在读取压缩包…" && snapshot.files_total == 0) saw_reading_before_total = true;
      if (snapshot.files_total > 0) saw_total = true;
    };
    InstallResult installed = install_game(drive, *local, request);
    if (!saw_reading_before_total || !saw_total) return fail("安装过程没有先报告正在读取压缩包");
    if (installed.status != "installed") return fail(installed.message);
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA00001/hello.txt", &got, 100);
    if (!error.empty() || got != hello) return fail("单卷 ZIP 解压内容不对");
    InstallResult again = install_game(drive, *local, request);
    if (again.status != "already") return fail("单卷安装后没有显示已安装");
  }

  {
    const auto ftp_root = root / "ftp";
    DummyFtp server(ftp_root, 0);
    if (!server.start(&error)) return fail(error);
    FtpEndpoint endpoint;
    endpoint.port = server.port();
    auto ftp = make_ftp_sink(endpoint, &error);
    if (!ftp) return fail(error);
    const int code = resume_on(*ftp, drive, param, eboot, ftp_root);
    ftp.reset();
    server.stop();
    if (code != 0) return code;
  }

  {
    const auto resume_root = root / "resume-local";
    auto local = make_local_sink(resume_root.u8string());
    if (const int code = resume_on(*local, drive, param, eboot, resume_root); code != 0) return code;
    for (const char* whole : {"/", "", "/mnt/.."}) {
      if (local->remove_tree(whole).empty() || !std::filesystem::is_directory(resume_root)) {
        return fail(std::string("删除整个存放位置没有被拦住: ") + whole);
      }
    }
  }

  {
    auto local = make_local_sink((root / "rar").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Rar CUSA77777";
    request.dest_prefix = "/data/homebrew";
    const InstallResult installed = install_game(drive, *local, request);
    if (installed.status != "installed") return fail(installed.message);
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA77777/note.txt", &got, 100);
    const std::vector<std::uint8_t> expect{'r', 'a', 'r', '-', 'o', 'k'};
    if (!error.empty() || got != expect) return fail("RAR 解压内容不对");
    const InstallResult again = install_game(drive, *local, request);
    if (again.status != "already") return fail("RAR 安装后没有显示已安装");

    if (auto remove_error = uninstall_game(*local, request.dest_prefix, request.source_dir); !remove_error.empty()) {
      return fail("删除装好的游戏失败: " + remove_error);
    }
    error.clear();
    if (local->file_size("/data/homebrew/CUSA77777/note.txt", &error) >= 0) return fail("删除游戏后文件还在");
    const InstallResult reinstalled = install_game(drive, *local, request);
    if (reinstalled.status != "installed") return fail("删除游戏后没有重新安装: " + reinstalled.status);
  }

  {
    auto local = make_local_sink((root / "rar-dict").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Rar CUSA77777";
    request.dest_prefix = "/data/homebrew";
    request.max_dictionary = 1;
    const InstallResult refused = install_game(drive, *local, request);
    if (refused.status != "error" || refused.message.find("超过上限") == std::string::npos) {
      return fail("字典超过上限的 RAR 没有被拒绝: " + refused.status + " " + refused.message);
    }
    if (!refused.retryable) return fail("字典超过上限调高后可以重试，不该标成不能重试");
    error.clear();
    if (local->file_size("/data/homebrew/CUSA77777/note.txt", &error) >= 0) return fail("字典超过上限还是装了文件");
  }

  {
    auto local = make_local_sink((root / "rar-no-memory").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Packed CUSA75555";
    request.dest_prefix = "/data/homebrew";
    set_big_memory_limit(1);
    const InstallResult starved = install_game(drive, *local, request);
    set_big_memory_limit(0);
    if (starved.status != "error" || !is_out_of_memory_error(starved.message)) {
      return fail("RAR 要不到字典内存时没有报内存不够: " + starved.status + " " + starved.message);
    }
    if (!starved.retryable || starved.auto_resume) return fail("RAR 内存不够应该交给用户重试，不该自动再装");
    if (big_memory_in_use() != 0) return fail("RAR 解压结束后字典内存没有还掉");
    const InstallResult installed = install_game(drive, *local, request);
    if (installed.status != "installed") return fail("RAR 内存不够之后重试没装好: " + installed.message);
    if (big_memory_in_use() != 0) return fail("RAR 装完后字典内存没有还掉");
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA75555/packed.txt", &got, 1 << 20);
    if (!error.empty() || std::string(got.begin(), got.end()) != packed_text) return fail("压缩过的 RAR 解压内容不对");
  }

  {
    auto local = make_local_sink((root / "rar-split").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Split CUSA66666";
    request.dest_prefix = "/data/homebrew";
    const InstallResult installed = install_game(drive, *local, request);
    if (installed.status != "installed") return fail("跨卷文件的 RAR 没装好: " + installed.message);
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA66666/big.bin", &got, 1 << 20);
    if (!error.empty() || got != split_data) return fail("跨卷文件内容不对");
  }

  // 一次停在跨卷文件中间；一次停在它之后，续装时要跳过已装好的跨卷文件。
  for (const std::int64_t stop : {std::int64_t{30000 + 5000}, std::int64_t{30000 + 150000 + 5000}}) {
    auto local = make_local_sink((root / ("rar-split-resume-" + std::to_string(stop))).u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Split CUSA66666";
    request.dest_prefix = "/data/homebrew";
    request.stop_after_output_bytes = stop;
    const InstallResult first = install_game(drive, *local, request);
    if (first.status != "interrupted") return fail("跨卷 RAR 没有在中途停下: " + first.status + " " + first.message);
    request.stop_after_output_bytes = -1;
    const InstallResult resumed = install_game(drive, *local, request);
    if (resumed.status != "installed") return fail("跨卷 RAR 继续安装失败: " + resumed.message);
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA66666/big.bin", &got, 1 << 20);
    if (!error.empty() || got != split_data) return fail("继续安装后跨卷文件内容不对");
    error = local->read_file("/data/homebrew/CUSA66666/c.bin", &got, 1 << 20);
    if (!error.empty() || got != split_tail) return fail("继续安装后跨卷文件后面的文件不对");
  }

  // 续装时直接从上次停下的那一卷接着读，前面的卷不再读；续装点坏了时退回从第一卷核对，照样装对。
  for (const bool spoil : {false, true}) {
    const auto local_root = root / (spoil ? "rar-resume-spoiled" : "rar-resume-point");
    auto local = make_local_sink(local_root.u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Many CUSA55555";
    request.dest_prefix = "/data/homebrew";
    request.stop_after_output_bytes = 20000 * 4 + 5000;
    const InstallResult first = install_game(drive, *local, request);
    if (first.status != "interrupted") return fail("多卷 RAR 没有在中途停下: " + first.status + " " + first.message);
    std::filesystem::path journal_file;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(local_root)) {
      if (entry.path().filename() == ".panplay-journal.jsonl") journal_file = entry.path();
    }
    if (journal_file.empty()) return fail("中途停下后没有进度日志");
    std::string journal_text;
    {
      std::ifstream in(journal_file, std::ios::binary);
      journal_text.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (journal_text.find("\"next\":") == std::string::npos) return fail("进度日志里没有记续装点");
    // 模拟续装点之前已经走过的目录：一个包里有、续装途中还会再遇到，一个只在日志里。
    const std::string first_file = "d1/f0.bin\"";
    const auto first_at = journal_text.find(first_file);
    const auto name_at = journal_text.rfind("\"name\":\"", first_at);
    if (first_at == std::string::npos || name_at == std::string::npos) return fail("进度日志里没有第一个文件");
    const std::string prefix = journal_text.substr(name_at + 8, first_at - name_at - 8);
    journal_text = "{\"dir\":\"" + prefix + "a_empty\"}\n{\"dir\":\"" + prefix + "zz_journal_only\"}\n" + journal_text;
    std::ofstream(journal_file, std::ios::binary | std::ios::trunc) << journal_text;
    if (spoil) {
      std::string spoiled;
      std::size_t at = 0;
      for (std::size_t key; (key = journal_text.find("\"next\":", at)) != std::string::npos;) {
        const std::size_t digits = key + 7;
        std::size_t end = digits;
        while (end < journal_text.size() && std::isdigit(static_cast<unsigned char>(journal_text[end]))) ++end;
        spoiled += journal_text.substr(at, digits - at) + std::to_string(std::stoll(journal_text.substr(digits, end - digits)) + 7);
        at = end;
      }
      spoiled += journal_text.substr(at);
      std::ofstream(journal_file, std::ios::binary | std::ios::trunc) << spoiled;
    }
    drive.forget_streams();
    request.stop_after_output_bytes = -1;
    // 安装速度从第一次报“正在安装”算起，那之后涨的字节只能是这次真正解压的文件，续装点之前的不能算进去。
    std::int64_t install_from = -1;
    std::int64_t files_left = 0;
    std::int64_t last_bytes = 0;
    request.on_progress = [&](const InstallResult& snapshot) {
      if (install_from < 0 && snapshot.message == "正在安装") {
        install_from = snapshot.bytes_done;
        files_left = 6 - snapshot.files_done;
      }
      last_bytes = snapshot.bytes_done;
    };
    const InstallResult resumed = install_game(drive, *local, request);
    request.on_progress = nullptr;
    if (resumed.status != "installed") return fail("多卷 RAR 继续安装失败: " + resumed.message);
    if (install_from < 0 || last_bytes - install_from > files_left * 20000 + 4096) {
      return fail("续装时开始解压前的进度没跟上，已装的部分会算进安装速度: 从 " + std::to_string(install_from) + " 到 " +
                  std::to_string(last_bytes) + "，要装 " + std::to_string(files_left) + " 个文件");
    }
    const bool read_first = drive.streamed_from().count("/apps/panplay/Many CUSA55555/" + many_volumes.front().first) > 0;
    if (!spoil && read_first) return fail("续装时又从第一卷读起");
    if (spoil && !read_first) return fail("续装点坏了却没有退回从第一卷核对");
    std::vector<std::uint8_t> got;
    for (int i = 0; i < 6; ++i) {
      error = local->read_file("/data/homebrew/CUSA55555/" + many_name(i), &got, 1 << 20);
      if (!error.empty() || got != many_files[static_cast<std::size_t>(i)]) {
        return fail("继续安装后第 " + std::to_string(i) + " 个文件不对");
      }
    }
    const auto game_dir = local_root / "data" / "homebrew" / "CUSA55555";
    if (!std::filesystem::is_directory(game_dir / "a_empty")) return fail("继续安装后空目录没了");
    if (!spoil && !std::filesystem::is_directory(game_dir / "zz_journal_only")) {
      return fail("续装点之前的目录没有按进度日志补回来");
    }
  }

  // 少了最后一卷时 UnRAR 只报压缩包结束，不能把已经读到的文件当作装完。
  {
    auto local = make_local_sink((root / "rar-cut").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Cut CUSA66667";
    const InstallResult cut = install_game(drive, *local, request);
    if (cut.status != "error") return fail("少了最后一卷的 RAR 不该装成功: " + cut.status + " " + cut.message);
    std::vector<std::uint8_t> marker;
    if (local->read_file("/data/homebrew/CUSA66667/.panplay-done.json", &marker, 1 << 20).empty()) {
      return fail("少了最后一卷的 RAR 写了安装完成标记");
    }
  }

  {
    const auto local_root = root / "rar-wrap";
    auto local = make_local_sink(local_root.u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Wrapped PPSA88888";
    request.dest_prefix = "/data/homebrew";
    std::int64_t last_bytes = -1;
    std::int64_t bytes_total = 0;
    bool bytes_backwards = false;
    request.on_progress = [&](const InstallResult& snapshot) {
      if (snapshot.bytes_done < last_bytes) bytes_backwards = true;
      last_bytes = snapshot.bytes_done;
      bytes_total = snapshot.bytes_total;
    };
    const InstallResult installed = install_game(drive, *local, request);
    if (installed.status != "installed" || installed.files_total != 3) {
      return fail("带外层目录的 RAR 加补丁没装好: " + installed.message);
    }
    if (bytes_total <= 0 || last_bytes != bytes_total || bytes_backwards) return fail("按字节的安装进度不对");
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/PPSA88888/eboot.bin", &got, 100);
    if (!error.empty() || got != fixed_eboot) return fail("补丁里的 eboot.bin 没有覆盖游戏本体的");
    const auto game_dir = local_root / "data" / "homebrew" / "PPSA88888";
    if (!std::filesystem::exists(game_dir / "sce_sys" / "param.json")) return fail("RAR 的 sce_sys 不在第一层");
    if (!std::filesystem::exists(game_dir / "sce_sys" / "about" / "right.sprx")) return fail("补丁的 sce_sys 没有合进游戏目录");
    if (std::filesystem::exists(game_dir / "Wrap") || std::filesystem::exists(game_dir / "PPSA88888-app0") ||
        std::filesystem::exists(game_dir / "readme.txt")) {
      return fail("RAR 外层目录被装进去了");
    }
    if (std::filesystem::exists(local_root / "data" / "homebrew" / ".panplay-staging")) {
      return fail("装完后暂存区没有清干净");
    }
    const InstallResult again = install_game(drive, *local, request);
    if (again.status != "already" || again.files_total != 3) return fail("RAR 加补丁装完后没有显示已安装");

    // 出了正式补丁：RAR 本体不用再解压，补丁叠上去后固件补丁还在最上面。
    const std::vector<std::uint8_t> patched_param{'p', 'a', 't', 'c', 'h', 'e', 'd'};
    drive.add("/apps/panplay/Wrapped PPSA88888/补丁1/patch.zip",
              make_zip({{"sce_sys/param.json", patched_param, false}, {"eboot.bin", pattern(30, 5), true}}, {}).front());
    CountingSink counted(*local);
    const InstallResult patched = install_game(drive, counted, request);
    if (patched.status != "installed" || patched.message.find("已更新") == std::string::npos) {
      return fail("RAR 游戏出了补丁没有只装补丁: " + patched.message);
    }
    if (counted.writes_ending("/param.json") != 1) return fail("只装补丁时又解压了 RAR 本体");
    error = local->read_file("/data/homebrew/PPSA88888/eboot.bin", &got, 100);
    if (!error.empty() || got != fixed_eboot) return fail("叠上正式补丁后固件补丁没有留在最上面");
    error = local->read_file("/data/homebrew/PPSA88888/sce_sys/param.json", &got, 100);
    if (!error.empty() || got != patched_param) return fail("正式补丁的 param.json 没有装上");
    if (install_game(drive, *local, request).status != "already") return fail("RAR 游戏叠完补丁后没有显示已安装");
  }

  if (const int code = prefetch_tests()) return code;
  if (const int code = tuning_tests()) return code;
  if (const int code = speed_test_tests()) return code;
  if (const int code = install_clock_test()) return code;
  if (const int code = game_root_test()) return code;
  if (const int code = package_and_duplicate_test()) return code;
  if (const int code = many_files_test()) return code;
  if (const int code = zip64_split_test()) return code;
  if (const int code = cancel_install_test()) return code;
  if (const int code = large_file_test()) return code;
  if (const int code = sevenzip_test()) return code;

  std::cout << "self-test ok\n";
  return 0;
}

namespace {

// 模拟网盘下载：首包延迟、带宽上限、偶尔卡顿。
class SimDrive final : public DrivePlugin {
 public:
  SimDrive(double mb_per_second, std::int64_t size) : rate_(mb_per_second * 1024 * 1024), size_(size) {}
  std::string id() const override { return "sim"; }
  std::string display_name() const override { return "sim"; }
  LoginChallenge start_login() override { return {}; }
  LoginPoll poll_login(const LoginChallenge&) override { return {}; }
  bool session_valid() override { return true; }
  void logout() override {}
  std::vector<DriveEntry> list(const std::string&, std::string*) override { return {}; }
  ReadResult read_at(const std::string&, std::int64_t, std::size_t) override { return {}; }
  std::unique_ptr<ByteStream> open_stream(const std::string&, std::int64_t offset, std::string*, AbortSignal*) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return std::make_unique<Stream>(rate_, size_ - offset);
  }

 private:
  class Stream final : public ByteStream {
   public:
    Stream(double rate, std::int64_t left) : rate_(rate), left_(left), ready_(std::chrono::steady_clock::now()) {}
    std::size_t read(std::uint8_t*, std::size_t size, std::string*) override {
      const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(size), left_));
      if (n == 0) return 0;
      sent_ += static_cast<std::int64_t>(n);
      left_ -= static_cast<std::int64_t>(n);
      double cost = static_cast<double>(n) / rate_;
      // 每 48 MB 卡 400 ms，模拟 CDN 抖动。
      if (sent_ / (48 << 20) != (sent_ - static_cast<std::int64_t>(n)) / (48 << 20)) cost += 0.4;
      // 两次读之间隔得久，说明是缓冲满了在等，这段带宽浪费掉，不能攒着用。
      const auto now = std::chrono::steady_clock::now();
      if (now - returned_ > std::chrono::milliseconds(5)) ready_ = std::max(ready_, now);
      ready_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(cost));
      std::this_thread::sleep_until(ready_);
      returned_ = std::chrono::steady_clock::now();
      return n;
    }

   private:
    double rate_;
    std::int64_t left_;
    std::int64_t sent_ = 0;
    std::chrono::steady_clock::time_point ready_;
    std::chrono::steady_clock::time_point returned_;
  };

  double rate_;
  std::int64_t size_;
};

}  // namespace

int run_prefetch_bench() {
  constexpr std::int64_t kSize = 256ll << 20;
  struct Scene {
    double download;
    double write;
  };
  struct Config {
    std::size_t block;
    std::size_t blocks;
  };
  const Scene scenes[] = {{40.0, 60.0}, {60.0, 60.0}};
  const Config configs[] = {{256u << 10, 1}, {4u << 20, 1}, {4u << 20, 2}, {4u << 20, 4}, {4u << 20, 8}, {4u << 20, 16}, {8u << 20, 8}};
  for (const auto& scene : scenes) {
    std::cout << "下载 " << scene.download << " MB/s（每 48 MB 卡 0.4 秒），写入 " << scene.write
              << " MB/s（每 32 MB 换文件卡 0.3 秒），共 " << (kSize >> 20) << " MB，理想 "
              << static_cast<double>(kSize >> 20) / scene.download + 5 * 0.4 << " 秒\n";
    for (const auto& config : configs) {
      SimDrive drive(scene.download, kSize);
      PrefetchOptions options;
      options.connections = 1;
      options.block_size = config.block;
      options.max_blocks = config.blocks;
      PrefetchReader reader(drive, "x", kSize, nullptr, options);
      std::vector<std::uint8_t> buffer(64u << 10);
      const auto start = std::chrono::steady_clock::now();
      auto ready = start;
      std::int64_t consumed = 0;
      while (true) {
        std::string error;
        const auto asked = std::chrono::steady_clock::now();
        const std::size_t got = reader.read(buffer.data(), buffer.size(), &error);
        if (got == 0) break;
        const auto now = std::chrono::steady_clock::now();
        if (now - asked > std::chrono::milliseconds(2)) ready = std::max(ready, now);
        consumed += static_cast<std::int64_t>(got);
        double cost = static_cast<double>(got) / (scene.write * 1024 * 1024);
        if (consumed / (32 << 20) != (consumed - static_cast<std::int64_t>(got)) / (32 << 20)) cost += 0.3;
        ready += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(cost));
        std::this_thread::sleep_until(ready);
      }
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::cout << "  块 " << (config.block >> 10) << " KB × " << config.blocks << " = "
                << ((config.block * config.blocks) >> 10) << " KB：" << seconds << " 秒，"
                << static_cast<double>(kSize >> 20) / seconds << " MB/s" << std::endl;
    }
  }
  return 0;
}

}  // namespace panplay
