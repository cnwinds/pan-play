#include "panplay/install.hpp"

#include "panplay/dummy_ftp.hpp"
#include "panplay/library.hpp"
#include "panplay/download.hpp"
#include "panplay/speed_test.hpp"

#include "big_memory.hpp"
#include "download_sim.hpp"
#include "install_clock.hpp"
#include "install_common.hpp"
#include "write_behind.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include <stdexcept>
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
    stream_order_.clear();
  }
  // 各文件第一次被顺序读的先后。
  std::vector<std::string> stream_order() {
    std::lock_guard<std::mutex> lock(streamed_mu_);
    return stream_order_;
  }

  std::unique_ptr<ByteStream> open_stream(const std::string& file_id, std::int64_t offset, std::string* error,
                                          AbortSignal* abort) override {
    ++stream_opens;
    {
      std::lock_guard<std::mutex> lock(streamed_mu_);
      const auto it = streamed_from_.find(file_id);
      if (it == streamed_from_.end()) stream_order_.push_back(file_id);
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
  std::vector<std::string> stream_order_;

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
  // 文件内容还能写多少字节，写超了报存放位置空间不够；负数不限。
  std::int64_t room = -1;

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t, std::string*) override {
    files[path] = File{};
    return std::make_unique<Writer>(files[path], room);
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
    Writer(File& file, std::int64_t& room) : file_(file), room_(room) {}
    std::string write(const std::uint8_t* data, std::size_t size) override {
      if (room_ >= 0) {
        if (static_cast<std::int64_t>(size) > room_) return disk_full_error("盘写满了");
        room_ -= static_cast<std::int64_t>(size);
      }
      file_.size += static_cast<std::int64_t>(size);
      if (file_.size <= (1 << 20)) file_.bytes.insert(file_.bytes.end(), data, data + size);
      else file_.bytes.clear();
      return "";
    }
    std::string finish() override { return ""; }

   private:
    File& file_;
    std::int64_t& room_;
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

// 下载会话的 I/O 外壳（ADR 0009）：数据顺序、断线重连、跳转、取消、多段拼接、卡住或很慢的连接及时让出。
// 调条数的逻辑在 download_sim_tests 里用虚拟时间测。
int download_tests() {
  FakeDrive drive;
  const std::int64_t size = (64ll << 20) + 12345;
  const auto byte_at = [](std::int64_t offset) { return static_cast<std::uint8_t>((offset * 131) ^ (offset >> 13)); };
  drive.add_generated("/big.bin", size, [&](std::int64_t offset, std::uint8_t* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] = byte_at(offset + static_cast<std::int64_t>(i));
  });
  const std::vector<DownloadExtent> whole{DownloadExtent{"/big.bin", 0, size, size}};
  DownloadOptions options;
  options.retry_base_ms = 1;
  options.max_connections = 1;
  options.adaptive = false;
  options.window_max = 16ll << 20;
  // 一直读到 limit 或超时，边读边核对，返回读到哪。
  const auto drain = [](DownloadSession& session, const std::function<std::uint8_t(std::int64_t)>& expect, std::int64_t limit,
                        double seconds, std::size_t chunk, std::string* error) {
    std::vector<std::uint8_t> buffer(chunk);
    std::int64_t at = session.position();
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (at < limit && std::chrono::steady_clock::now() < until) {
      const std::size_t want = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(chunk), limit - at));
      const std::size_t got = session.read(buffer.data(), want, error);
      if (!error->empty() || got == 0) break;
      for (std::size_t i = 0; i < got; i += 4093) {
        if (buffer[i] != expect(at + static_cast<std::int64_t>(i))) {
          *error = "数据不对 @" + std::to_string(at + static_cast<std::int64_t>(i));
          return at;
        }
      }
      if (buffer[got - 1] != expect(at + static_cast<std::int64_t>(got) - 1)) {
        *error = "数据不对 @" + std::to_string(at + static_cast<std::int64_t>(got) - 1);
        return at;
      }
      at += static_cast<std::int64_t>(got);
    }
    return at;
  };

  {
    DownloadSession session(drive, whole, nullptr, options);
    std::string error;
    const std::int64_t at = drain(session, byte_at, size, 30, 7919, &error);
    if (!error.empty()) return fail("单连接顺序读: " + error);
    if (at != size) return fail("单连接没有读到结尾");
    std::uint8_t byte = 0;
    if (session.read(&byte, 1, &error) != 0 || !error.empty()) return fail("读完以后还有数据");
    if (drive.stream_opens != 1 || drive.read_at_calls != 0) {
      return fail("单连接顺序读 64 MB 打开了 " + std::to_string(drive.stream_opens.load()) + " 次流");
    }
  }

  drive.stream_opens = 0;
  drive.break_next_stream_after = 10ll << 20;
  {
    DownloadSession session(drive, whole, nullptr, options);
    if (auto error = session.seek(1000); !error.empty()) return fail(error);
    std::string error;
    const std::int64_t at = drain(session, byte_at, size, 30, 1 << 20, &error);
    if (!error.empty()) return fail("断线后没有自动重连: " + error);
    if (at != size) return fail("断线重连后没有读完");
    if (drive.stream_opens != 2) return fail("断线一次应该重连一次，实际打开 " + std::to_string(drive.stream_opens.load()) + " 次");
  }

  drive.stream_opens = 0;
  {
    DownloadSession session(drive, whole, nullptr, options);
    std::uint8_t byte = 0;
    std::string error;
    const auto read_at = [&](std::int64_t offset) {
      if (auto seek_error = session.seek(offset); !seek_error.empty()) return seek_error;
      if (session.read(&byte, 1, &error) != 1 || !error.empty()) return "跳到 " + std::to_string(offset) + " 后没读到: " + error;
      if (byte != byte_at(offset)) return "跳到 " + std::to_string(offset) + " 后数据不对";
      return std::string();
    };
    session.read(&byte, 1, &error);
    if (auto message = read_at(100); !message.empty()) return fail(message);
    // 等下载跑到前面：往前跳到已下好的地方，再往回跳一小段，都用缓冲里的。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const std::int64_t offset : {3ll << 20, (3ll << 20) - 50, (4ll << 20) + 20}) {
      if (auto message = read_at(offset); !message.empty()) return fail(message);
    }
    if (drive.stream_opens != 1) return fail("近处跳转不该重连");
    if (auto seek_error = session.seek((4ll << 20) - 30); !seek_error.empty()) return fail(seek_error);
    if (drain(session, byte_at, (4ll << 20) + 70, 5, 100, &error) != (4ll << 20) + 70 || !error.empty()) {
      return fail("往回跳一小段后数据不对: " + error);
    }
    if (drive.stream_opens != 1) return fail("往回跳一小段不该重连");
    if (auto message = read_at(60ll << 20); !message.empty()) return fail(message);
    if (drive.stream_opens != 2) return fail("远处跳转应该重连一次，实际打开 " + std::to_string(drive.stream_opens.load()) + " 次");
  }

  for (const int connections : {2, 4}) {
    DownloadOptions multi = options;
    multi.max_connections = connections;
    drive.stream_opens = 0;
    drive.latency_ms = 2;
    drive.break_next_stream_after = 3ll << 20;
    DownloadSession session(drive, whole, nullptr, multi);
    if (auto error = session.seek(777); !error.empty()) return fail(error);
    std::string error;
    std::int64_t at = drain(session, byte_at, 20ll << 20, 30, 300000, &error);
    if (!error.empty()) return fail(std::to_string(connections) + " 条连接读取出错: " + error);
    if (auto seek_error = session.seek(5ll << 20); !seek_error.empty()) return fail(seek_error);
    at = drain(session, byte_at, size, 30, 300000, &error);
    drive.latency_ms = 0;
    if (!error.empty()) return fail(std::to_string(connections) + " 条连接往回跳后出错: " + error);
    if (at != size) return fail(std::to_string(connections) + " 条连接没有读到结尾");
    if (session.connections() != connections) return fail("固定条数时条数变了");
  }

  {
    CancelToken cancel;
    drive.latency_ms = 50;
    DownloadSession session(drive, whole, &cancel, options);
    cancel.cancel();
    std::uint8_t byte = 0;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    session.read(&byte, 1, &error);
    const auto spent = std::chrono::steady_clock::now() - started;
    drive.latency_ms = 0;
    if (error != "interrupted") return fail("取消后没有返回中断: " + error);
    if (spent > std::chrono::seconds(1)) return fail("取消后没有很快停下");
  }

  for (const int connections : {1, 4}) {
    DownloadOptions slow = options;
    slow.max_connections = connections;
    const auto started = std::chrono::steady_clock::now();
    {
      DownloadSession session(drive, whole, nullptr, slow);
      std::uint8_t byte = 0;
      std::string error;
      session.read(&byte, 1, &error);
      drive.latency_ms = 10000;
      if (auto seek_error = session.seek(60ll << 20); !seek_error.empty()) return fail(seek_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      drive.latency_ms = 0;
      if (auto seek_error = session.seek(50ll << 20); !seek_error.empty()) return fail(seek_error);
      session.read(&byte, 1, &error);
      if (!error.empty() || byte != byte_at(50ll << 20)) return fail("连接中跳转后数据不对");
      drive.latency_ms = 10000;
      if (auto seek_error = session.seek(10ll << 20); !seek_error.empty()) return fail(seek_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    drive.latency_ms = 0;
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) return fail("连接还没建立时跳转或关闭要等很久");
  }

  // 几段首尾相接：两个文件，中间那段从文件中间开始、不到结尾，最后一段到文件结尾。
  {
    const std::int64_t other_size = 9ll << 20;
    const auto other_at = [](std::int64_t offset) { return static_cast<std::uint8_t>(offset * 7 + (offset >> 16) + 1); };
    drive.add_generated("/other.bin", other_size, [&](std::int64_t offset, std::uint8_t* dst, std::size_t n) {
      for (std::size_t i = 0; i < n; ++i) dst[i] = other_at(offset + static_cast<std::int64_t>(i));
    });
    const std::vector<DownloadExtent> parts{DownloadExtent{"/big.bin", 0, 5ll << 20, size},
                                            DownloadExtent{"/other.bin", 1ll << 20, 6ll << 20, other_size},
                                            DownloadExtent{"/big.bin", 7ll << 20, size - (7ll << 20), size}};
    const auto expect = [&](std::int64_t at) {
      if (at < (5ll << 20)) return byte_at(at);
      at -= 5ll << 20;
      if (at < (6ll << 20)) return other_at(at + (1ll << 20));
      return byte_at(at - (6ll << 20) + (7ll << 20));
    };
    for (const int connections : {1, 3}) {
      DownloadOptions multi = options;
      multi.max_connections = connections;
      drive.stream_opens = 0;
      DownloadSession session(drive, parts, nullptr, multi);
      if (session.size() != size + (4ll << 20)) return fail("几段拼起来的长度不对");
      std::string error;
      const std::int64_t at = drain(session, expect, session.size(), 30, 65536 + 17, &error);
      if (!error.empty()) return fail(std::to_string(connections) + " 条连接读几段: " + error);
      if (at != session.size()) return fail(std::to_string(connections) + " 条连接没读完几段");
      if (connections == 1 && drive.stream_opens != 3) {
        return fail("单连接读三段应该请求三次，实际 " + std::to_string(drive.stream_opens.load()) + " 次");
      }
    }
  }

  // 文件打不开：重试几次后报错，不会一直等。
  {
    DownloadOptions broken = options;
    broken.max_retries = 2;
    DownloadSession session(drive, {DownloadExtent{"/missing.bin", 0, 1 << 20, 1 << 20}}, nullptr, broken);
    std::uint8_t byte = 0;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    if (session.read(&byte, 1, &error) != 0 || error.empty()) return fail("文件打不开时没有报错");
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(3)) return fail("文件打不开时报错太慢");
  }

  // 卡住（每秒 16 KB）和一直有数据但很慢（每 40 毫秒 16 KB）：读取方要的那段要及时让别的连接接手或重连。
  const auto reset_limits = [&] {
    drive.total_kbps = 0;
    drive.stream_kbps = 0;
    drive.stall_next_stream_after = -1;
    drive.stall_piece_ms = 1000;
  };
  for (const int piece_ms : {1000, 40}) {
    for (const int connections : {1, 4}) {
      reset_limits();
      drive.stall_piece_ms = piece_ms;
      // 只有一条时没有别的连接可比，要先正常下一阵（16 MB，半秒），才知道它后来慢了。
      std::int64_t want = 24ll << 20;
      if (connections == 1) {
        drive.total_kbps = 32 * 1024;
        drive.stall_next_stream_after = 16ll << 20;
        want = 40ll << 20;
      } else {
        drive.stream_kbps = 4 * 1024;
        drive.stall_next_stream_after = 256 << 10;
      }
      DownloadOptions stalled = options;
      stalled.max_connections = connections;
      stalled.window_max = 0;
      DownloadSession session(drive, whole, nullptr, stalled);
      std::string error;
      const auto started = std::chrono::steady_clock::now();
      const std::int64_t got = drain(session, byte_at, want, 10.0, 1 << 20, &error);
      const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      const std::string what = std::to_string(connections) + " 条连接时请求" + (piece_ms < 1000 ? "很慢" : "卡住");
      if (!error.empty()) return fail(what + "后读取出错: " + error);
      if (got < want || spent > 7.0) {
        return fail(what + "没有及时让出：" + std::to_string(spent) + " 秒读到 " + std::to_string(got >> 20) + " MB");
      }
    }
  }
  reset_limits();

  {
    DownloadMemory memory;
    load_download_memory("{\"lanes\":4,\"lane_mbps\":7.5}", &memory);
    if (memory.lanes != 4 || std::abs(memory.lane_rate - 7.5 * 1024 * 1024) > 1) return fail("读 download.json 不对");
    DownloadMemory again;
    load_download_memory(download_memory_json(memory), &again);
    if (again.lanes != 4 || std::abs(again.lane_rate - memory.lane_rate) > 1024) return fail("download.json 存了再读不一样");
    DownloadMemory junk;
    load_download_memory("{\"lanes\":99}", &junk);
    if (junk.lanes < 1 || junk.lanes > 8) return fail("download.json 里的条数没有限制范围");
  }
  return 0;
}

// 调条数、分段、让出慢连接的决策（ADR 0009），用虚拟时间跑 download_sim_scenarios 的各个场景。
int download_sim_tests() {
  const auto scenarios = download_sim_scenarios();
  for (const SimScenario& scenario : scenarios) {
    const SimResult result = run_download_sim(scenario);
    const auto bad = [&](const std::string& what) { return fail("下载模拟「" + scenario.name + "」" + what + "\n" + describe_sim(scenario, result)); };
    const auto is = [&](const char* prefix) { return scenario.name.rfind(prefix, 0) == 0; };
    if (!result.error.empty()) return bad("出错: " + result.error);
    const auto within = [&](double factor) { return result.seconds <= result.bound * factor + 0.5; };
    if (is("单条够快")) {
      if (result.totals.requests != 3 || result.final_lanes != 1 || !within(1.05)) return bad("应该一卷一个请求、一条到底");
    } else if (is("每条限速")) {
      if (result.most_lanes < 6 || !within(1.3)) return bad("应该加到 6 条以上");
    } else if (is("共享瓶颈")) {
      if (result.final_lanes != 1 || result.memory.lanes != 1 || !within(1.05)) return bad("多开只是分流，应该退回 1 条");
    } else if (is("解压慢")) {
      if (result.final_lanes > 3 || !within(1.05)) return bad("解压跟不上时应该减条数");
    } else if (is("固定")) {
      if (result.totals.steals != 0 || result.totals.aborts != 0 || !within(1.2)) return bad("没有故障时不该分走或断开");
    } else if (is("慢连接")) {
      if (result.totals.steals < 1 || !within(1.35)) return bad("应该把慢连接的后半段分走");
    } else if (is("卡住")) {
      if (result.totals.aborts < 1 || !within(1.3)) return bad("应该断开卡住的连接");
    } else if (is("分卷")) {
      if (result.totals.requests > 14 || !within(1.1)) return bad("跨卷不该多出请求或等待");
    } else if (is("记下")) {
      if (result.final_lanes > 3 || !within(1.1)) return bad("开头复核应该把记下的条数减下来");
    } else if (is("总带宽")) {
      if (!within(1.45)) return bad("带宽波动时太慢");
    } else if (is("出错")) {
      if (result.totals.failures < 1 || !within(1.4)) return bad("出错后应该重试并读完");
    } else if (is("百度")) {
      if (result.connections > 45 || !within(1.25)) return bad("连接忽快忽慢时不该频繁断开重连");
    }
  }
  SimScenario broken;
  broken.name = "一直出错";
  broken.extents = {64ll << 20};
  broken.error_chance = 1;
  broken.options.retry_base = 0.1;
  const SimResult result = run_download_sim(broken);
  if (result.error.empty()) return fail("下载模拟：一直出错时应该报错\n" + describe_sim(broken, result));
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
int big_memory_test() {
  constexpr std::size_t kSize = 8u << 20;
  auto* window = static_cast<std::uint8_t*>(big_alloc(kSize));
  if (window == nullptr) return fail("要不到 8 MB 的字典内存");
  std::memset(window, 0xAB, kSize);
  const std::size_t dropped = big_discard(window, kSize);
  const std::uint64_t total = big_memory_discarded();
  if (dropped == 0 || dropped > kSize) {
    big_free(window);
    return fail("字典内存没能交给系统丢掉: " + std::to_string(dropped));
  }
  if (window_reuse(window, 1u << 20)) {
    big_free(window);
    return fail("字典只写了 1 MB 也被丢掉了");
  }
  std::memset(window, 0x5A, kSize);
  const bool kept = window[0] == 0x5A && window[kSize - 1] == 0x5A;
  big_free(window);
  if (!kept) return fail("字典内存丢掉内容后写不进去");
  if (total < dropped) return fail("没记下丢掉了多少字典内存");
  if (big_memory_in_use() != 0) return fail("字典内存测试后没有还掉");
  return 0;
}

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
  if (percent("等下载") < 60 || percent("等下载") > 90 || percent("解压") < 12) return fail("安装耗时占比不对: " + line);
  if (line.find("等内存换页") != std::string::npos || line.find("主机上在运行") != std::string::npos ||
      line.find("字典") != std::string::npos) {
    return fail("没换页、没在玩游戏、不用字典时总账不该提这些: " + line);
  }
  InstallClock::note_dictionary(64u << 20);
  if (clock.dictionary() != (64u << 20)) return fail("安装计时没记下字典大小");
  if (!clock.slow().empty()) return fail("不到 10 秒就判断了慢在哪: " + clock.slow());
  set_running_game_probe([] { return std::string("PPSA00001"); });
  std::string playing_line;
  {
    InstallClock playing;
    {
      PhaseScope decoding(InstallPhase::decode);
      InstallClock::note_dictionary(64u << 20);
      nap(50);
    }
    playing_line = playing.summary("测试", "已装好");
  }
  set_running_game_probe(nullptr);
  if (playing_line.find("；字典 64 MB；装的时候主机上在运行 PPSA00001，占 ") == std::string::npos) {
    return fail("总账没写字典大小、装的时候在运行的游戏: " + playing_line);
  }
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
  request.download.max_connections = 1;
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
  // 按叠放顺序下载：主游戏、子目录里的补丁、游戏目录里的 backport。
  const std::vector<std::string> expected_order = {dir + "/game.zip", dir + "/补丁2 v1.05/patch.zip", dir + "/backport.zip"};
  if (shelf->stream_order() != expected_order) {
    std::string got;
    for (const auto& path : shelf->stream_order()) got += " " + path;
    return fail("下载顺序不对，应先主游戏、再补丁、最后 backport，实际:" + got);
  }
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

// 照盘架的编排装（ADR 0012）：只装编排里的压缩包组、按编排的顺序叠，单独的文件放进游戏目录覆盖压缩包里的。
int plan_install_test() {
  FakeDrive drive;
  const std::string dir = "/apps/panplay/Plan PPSA42000/@other";
  const auto param = pattern(300, 21);
  const auto old_eboot = pattern(5000, 22);
  const auto new_eboot = pattern(5200, 23);
  const auto old_lib = pattern(900, 24);
  const auto new_lib = pattern(1100, 25);
  drive.add(dir + "/main/game.zip", make_zip({{"PPSA42000-app/sce_sys/param.json", param, true},
                                              {"PPSA42000-app/eboot.bin", old_eboot, true},
                                              {"PPSA42000-app/fakelib/libSceAmpr.sprx", old_lib, false}}, {}).front());
  drive.add(dir + "/huge-stray.zip", make_zip({{"junk.bin", pattern(20000, 26), false}}, {}).front());
  drive.add(dir + "/patch/p1.zip", make_zip({{"PPSA42000-app/eboot.bin", new_eboot, true}}, {}).front());
  drive.add(dir + "/apr/libSceAmpr.sprx", new_lib);
  std::string error;
  const auto entry = [&](const std::string& folder, const std::string& name) {
    for (const auto& item : drive.list(folder, &error)) {
      if (item.name == name) return item;
    }
    return DriveEntry{};
  };
  InstallPlan plan;
  plan.layers.push_back(group_archives({entry(dir + "/main", "game.zip")}).front());
  plan.layers.push_back(group_archives({entry(dir + "/patch", "p1.zip")}).front());
  InstallPlan old_plan = plan;
  plan.copies.push_back({entry(dir + "/apr", "libSceAmpr.sprx"), "fakelib/libSceAmpr.sprx"});
  if (plan.copies.front().file.file_id.empty()) return fail("编排自检没找到要放的文件");

  const std::string root = "/data/homebrew/PPSA42000/";
  const auto check = [&](MemorySink& sink, const char* when) -> int {
    if (sink.files[root + "eboot.bin"].bytes != new_eboot) return fail(std::string(when) + "：补丁没按编排盖住主游戏");
    if (sink.files[root + "fakelib/libSceAmpr.sprx"].bytes != new_lib) return fail(std::string(when) + "：编排要放的文件没覆盖进去");
    if (sink.files[root + "sce_sys/param.json"].bytes != param) return fail(std::string(when) + "：主游戏的文件不对");
    for (const auto& file : sink.files) {
      if (file.first.find("junk.bin") != std::string::npos) return fail(std::string(when) + "：编排外的压缩包也装了");
      if (file.first.find(".panplay-staging") != std::string::npos) return fail(std::string(when) + "：暂存区有剩: " + file.first);
    }
    return 0;
  };

  MemorySink sink;
  InstallRequest request;
  request.source_dir = dir;
  request.plan = plan;
  const InstallResult installed = install_game(drive, sink, request);
  if (installed.status != "installed") return fail("照编排装失败: " + installed.message);
  if (const int code = check(sink, "照编排装")) return code;
  if (install_game(drive, sink, request).status != "already") return fail("照编排装好后再装应该是已安装");
  if (install_outdated(sink, request.dest_prefix, dir, plan.layers, {}, plan.copies)) return fail("编排要放的文件都在，不该算有更新");

  // 编排后来才多了要放的文件：压缩包没变也算有更新，再装只补上那个文件。
  MemorySink later;
  request.plan = old_plan;
  if (install_game(drive, later, request).status != "installed") return fail("没有要放的文件的编排装失败");
  if (later.files[root + "fakelib/libSceAmpr.sprx"].bytes != old_lib) return fail("没放文件时应该是压缩包里的那份");
  if (!install_outdated(later, request.dest_prefix, dir, plan.layers, {}, plan.copies)) return fail("缺了编排要放的文件应该算有更新");
  request.plan = plan;
  const InstallResult patched = install_game(drive, later, request);
  if (patched.status != "installed") return fail("补放编排的文件失败: " + patched.message);
  if (const int code = check(later, "补放文件")) return code;
  if (install_game(drive, later, request).status != "already") return fail("补放之后再装应该是已安装");
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
  request.download.max_connections = 1;
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

// store 时抛异常的存放位置，其余交给 MemorySink。
class ThrowingSink final : public InstallSink {
 public:
  MemorySink inner;
  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) override {
    return inner.open_writer(path, size, error);
  }
  std::string ensure_dir(const std::string& path) override { return inner.ensure_dir(path); }
  std::int64_t file_size(const std::string& path, std::string* error) override { return inner.file_size(path, error); }
  std::string store(const std::string&, const std::uint8_t*, std::size_t) override { throw std::runtime_error("坏扇区"); }
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    return inner.append(path, data, size);
  }
  std::string rename_path(const std::string& from, const std::string& to) override { return inner.rename_path(from, to); }
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    return inner.read_file(path, out, max_bytes);
  }
  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    return inner.list_children(path, error);
  }
  std::string remove_file(const std::string& path) override { return inner.remove_file(path); }
  std::string remove_dir(const std::string& path) override { return inner.remove_dir(path); }
  std::string remove_tree(const std::string& path) override { return inner.remove_tree(path); }
};

int write_behind_test() {
  const auto data = pattern(300000, 3);
  const std::uint8_t record[] = {'o', 'k', '\n'};
  {
    MemorySink memory;
    WriteBehindSink behind(memory, 64 * 1024);
    std::string error;
    auto writer = behind.open_writer("/g/a.bin", static_cast<std::int64_t>(data.size()), &error);
    if (!writer || !error.empty()) return fail("后台写盘开不了文件: " + error);
    for (std::size_t at = 0; at < data.size(); at += 10000) {
      if (!(error = writer->write(data.data() + at, std::min<std::size_t>(10000, data.size() - at))).empty()) {
        return fail("后台写盘写入出错: " + error);
      }
    }
    if (!(error = writer->finish()).empty()) return fail("后台写盘关文件出错: " + error);
    writer.reset();
    if (!(error = behind.append("/g/journal", record, sizeof(record))).empty()) return fail("后台写盘记进度出错: " + error);
    if (behind.file_size("/g/a.bin", &error) != static_cast<std::int64_t>(data.size())) return fail("后台写盘查大小前没有等写完");
    if (!(error = behind.drain()).empty()) return fail("后台写盘收尾出错: " + error);
    if (memory.files["/g/a.bin"].bytes != data) return fail("后台写盘写出的内容不对");
    if (memory.files["/g/journal"].size != 3) return fail("后台写盘的进度没有记上");
  }
  {
    MemorySink memory;
    memory.room = 50000;
    WriteBehindSink behind(memory, 64 * 1024);
    std::string error;
    auto writer = behind.open_writer("/g/a.bin", static_cast<std::int64_t>(data.size()), &error);
    std::string first;
    for (std::size_t at = 0; at < data.size() && first.empty(); at += 10000) {
      first = writer->write(data.data() + at, 10000);
    }
    writer.reset();
    const std::string drained = behind.drain();
    if (!is_disk_full_error(drained)) return fail("后台写盘写满时没有报空间不够: " + drained);
    if (behind.append("/g/journal", record, sizeof(record)) != drained) return fail("后台写盘出错后还在接着做");
    if (memory.files.count("/g/journal") != 0) return fail("后台写盘出错后还记了进度");
  }
  {
    ThrowingSink throwing;
    WriteBehindSink behind(throwing, 64 * 1024);
    behind.store("/g/done", record, sizeof(record));
    const std::string drained = behind.drain();
    if (drained.find("坏扇区") == std::string::npos) return fail("后台写盘抛异常时没有变成出错: " + drained);
    std::string error;
    if (behind.open_writer("/g/b.bin", 10, &error) && error.empty()) return fail("后台写盘抛异常后还能开文件");
  }
  return 0;
}

int disk_full_test() {
  FakeDrive drive;
  const auto small = pattern(5000, 5);
  const auto big = pattern(300000, 9);
  drive.add("/apps/panplay/Full PPSA41000/game.zip", make_zip({{"sce_sys/param.json", small, true}, {"eboot.bin", big, false}}, {}).front());
  MemorySink sink;
  sink.room = 100000;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Full PPSA41000";
  const InstallResult full = install_game(drive, sink, request);
  if (full.status != "error" || !is_disk_full_error(full.message)) return fail("存放位置写满时没有报空间不够: " + full.message);
  if (full.auto_resume) return fail("存放位置写满了不该自动再装");
  if (!full.retryable) return fail("腾出空间后应该能接着装");
  sink.room = -1;
  const InstallResult resumed = install_game(drive, sink, request);
  if (resumed.status != "installed") return fail("腾出空间后没有接着装完: " + resumed.message);
  if (sink.files["/data/homebrew/PPSA41000/eboot.bin"].bytes.size() != big.size()) return fail("腾出空间后装的大文件大小不对");
  if (!is_disk_full_error(disk_full_error("主机存储空间不足（552）"))) return fail("FTP 空间不足没有认成写满");
  return 0;
}

int variant_test() {
  if (variant_format("/apps/panplay/Var PPSA41000/@exfat") != "exfat" || !variant_format("/apps/panplay/Var PPSA41000").empty() ||
      !variant_format("/apps/panplay/@exfat/Var PPSA41000").empty()) {
    return fail("认不出格式子目录");
  }
  if (game_dir_of("/apps/panplay/Var PPSA41000/@folder") != "/apps/panplay/Var PPSA41000" ||
      game_dir_of("/apps/panplay/Var PPSA41000") != "/apps/panplay/Var PPSA41000") {
    return fail("格式子目录找不回游戏目录");
  }
  if (is_variant_dir("@") || is_variant_dir("@ExFAT") || !is_variant_dir("@other")) return fail("格式子目录的命名规则不对");
  if (install_folder_name("/apps/panplay/Var PPSA41000/@exfat") != "PPSA41000") return fail("格式子目录装到了别的文件夹");

  const auto image = pattern(5000, 21);
  const auto folder = pattern(3000, 22);
  FakeDrive drive;
  drive.add("/apps/panplay/Var PPSA41000/@folder/game.zip", make_zip({{"eboot.bin", folder, true}}, {}).front());
  drive.add("/apps/panplay/Var PPSA41000/@exfat/game.zip", make_zip({{"PPSA41000.exfat", image, false}}, {}).front());
  drive.add("/apps/panplay/Var PPSA41000/@other/game.zip", make_zip({{"x.bin", folder, false}}, {}).front());
  std::string error;
  const auto variants = list_variants(drive, "/apps/panplay/Var PPSA41000", &error);
  if (!error.empty() || variants.size() != 3 || variants[0].format != "exfat" || variants[1].format != "folder" ||
      variants[2].format != "other" || variants[0].path != "/apps/panplay/Var PPSA41000/@exfat") {
    return fail("格式子目录列得不对: " + error);
  }
  if (!collect_archives(drive, "/apps/panplay/Var PPSA41000", &error).empty()) return fail("游戏目录把格式子目录里的压缩包也算进来了");
  if (!list_variants(drive, "/apps/panplay/Var PPSA41000/@exfat", &error).empty()) return fail("格式子目录里又列出了格式");

  drive.add("/apps/panplay/Var PPSA41000/loose.zip", make_zip({{"y.bin", folder, false}}, {}).front());
  const auto with_loose = list_variants(drive, "/apps/panplay/Var PPSA41000", &error);
  if (with_loose.size() != 4 || !with_loose.back().format.empty() || with_loose.back().path != "/apps/panplay/Var PPSA41000") {
    return fail("游戏目录里直接放的压缩包没有算成一个变体");
  }
  {
    const auto bytes_in = [&](const std::string& dir) {
      std::int64_t total = 0;
      for (const auto& file : collect_archives(drive, dir, &error)) total += file.size;
      return total;
    };
    const auto sizes = measure_game(drive, "/apps/panplay/Var PPSA41000", &error);
    if (!error.empty() || sizes.size() != with_loose.size()) return fail("分了格式的游戏量出的变体数不对: " + error);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      if (sizes[i].format != with_loose[i].format || sizes[i].path != with_loose[i].path ||
          sizes[i].bytes <= 0 || sizes[i].bytes != bytes_in(sizes[i].path)) {
        return fail("分了格式的游戏量出的容量不对: " + sizes[i].path);
      }
    }
    const auto game_zip = make_zip({{"eboot.bin", folder, true}}, {}).front();
    const auto patch_zip = make_zip({{"patch.bin", image, false}}, {}).front();
    FakeDrive plain;
    plain.add("/apps/panplay/Plain PPSA41001/game.zip", game_zip);
    plain.add("/apps/panplay/Plain PPSA41001/补丁/patch.zip", patch_zip);
    plain.add("/apps/panplay/Plain PPSA41001/readme.txt", {1, 2, 3});
    const auto one = measure_game(plain, "/apps/panplay/Plain PPSA41001", &error);
    if (!error.empty() || one.size() != 1 || !one[0].format.empty() || one[0].path != "/apps/panplay/Plain PPSA41001" ||
        one[0].bytes != static_cast<std::int64_t>(game_zip.size() + patch_zip.size())) {
      return fail("没分格式的游戏量出的容量不对（要算上补丁子目录、不算别的文件）: " + error);
    }
  }

  MemorySink sink;
  InstallRequest request;
  request.source_dir = "/apps/panplay/Var PPSA41000/@exfat";
  const InstallResult installed = install_game(drive, sink, request);
  if (installed.status != "installed") return fail("exFAT 格式没有装上: " + installed.message);
  if (sink.files["/data/homebrew/PPSA41000/PPSA41000.exfat"].bytes != image) return fail("exFAT 镜像内容不对");
  const auto marker = sink.files["/data/homebrew/PPSA41000/.panplay-done.json"].bytes;
  if (std::string(marker.begin(), marker.end()).find("\"variant\":\"exfat\"") == std::string::npos) return fail("完成标记没记格式");
  if (installed_variant(sink, "/data/homebrew", "/apps/panplay/Var PPSA41000/@folder") != std::optional<std::string>("exfat")) {
    return fail("读不出装好的是哪种格式");
  }
  if (install_game(drive, sink, request).status != "already") return fail("exFAT 格式装完后没有显示已安装");

  request.source_dir = "/apps/panplay/Var PPSA41000/@folder";
  const InstallResult other = install_game(drive, sink, request);
  if (other.status != "error" || other.message.find("先删除") == std::string::npos || other.retryable) {
    return fail("装了 exFAT 后又装文件夹版没有被拦住: " + other.message);
  }
  if (sink.files.count("/data/homebrew/PPSA41000/eboot.bin") != 0) return fail("拦住另一种格式之前写了文件");
  std::string list_error;
  const auto folder_sets = group_archives(collect_archives(drive, request.source_dir, &list_error));
  if (install_outdated(sink, "/data/homebrew", request.source_dir, folder_sets)) return fail("另一种格式不该显示有更新");

  if (!uninstall_game(sink, "/data/homebrew", request.source_dir).empty()) return fail("从另一种格式删除已装的游戏失败");
  if (installed_variant(sink, "/data/homebrew", request.source_dir).has_value()) return fail("删除后还认为装着 exFAT");
  if (install_game(drive, sink, request).status != "installed") return fail("删掉 exFAT 后装不上文件夹版");
  if (sink.files["/data/homebrew/PPSA41000/eboot.bin"].bytes != folder) return fail("文件夹版内容不对");

  // 旧版本装的完成标记没有 variant，认作和哪种格式都对得上。
  MemorySink legacy;
  request.source_dir = "/apps/panplay/Var PPSA41000/@folder";
  if (install_game(drive, legacy, request).status != "installed") return fail("文件夹版没装上");
  auto& done = legacy.files["/data/homebrew/PPSA41000/.panplay-done.json"];
  std::string text(done.bytes.begin(), done.bytes.end());
  const auto at = text.find(",\"variant\":\"folder\"");
  if (at == std::string::npos) return fail("完成标记里找不到文件夹版的格式");
  text.erase(at, std::strlen(",\"variant\":\"folder\""));
  done.bytes.assign(text.begin(), text.end());
  done.size = static_cast<std::int64_t>(text.size());
  if (installed_variant(legacy, "/data/homebrew", request.source_dir).has_value()) return fail("没记格式的完成标记读出了格式");
  if (install_game(drive, legacy, request).status != "already") return fail("没记格式的旧安装没有显示已安装");

  // 从没分格式的目录装好，盘架再把压缩包迁进 @folder，不用重装。
  const auto loose_zip = make_zip({{"eboot.bin", folder, true}}, {}).front();
  FakeDrive before;
  before.add("/apps/panplay/Mig PPSA42000/game.zip", loose_zip);
  MemorySink migrated;
  request.source_dir = "/apps/panplay/Mig PPSA42000";
  if (install_game(before, migrated, request).status != "installed") return fail("没分格式的游戏没装上");
  FakeDrive after;
  after.add("/apps/panplay/Mig PPSA42000/@folder/game.zip", loose_zip);
  request.source_dir = "/apps/panplay/Mig PPSA42000/@folder";
  if (installed_variant(migrated, "/data/homebrew", request.source_dir).has_value()) return fail("没分格式的安装读出了格式");
  const InstallResult moved = install_game(after, migrated, request);
  if (moved.status != "already") return fail("迁进 @folder 后没有显示已安装: " + moved.status + " " + moved.message);
  return 0;
}

int sparse_write_test() {
  // 镜像里大段全零的 4 MB 块跳过不写，文件结尾是空洞时要把文件补到原来的大小。
  constexpr std::size_t kBlock = 4u << 20;
  const auto root = std::filesystem::temp_directory_path() / "panplay-sparse-test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  auto local = make_local_sink(root.u8string());
  const auto data = pattern(kBlock, 31);
  const std::vector<std::uint8_t> zeros(kBlock, 0);
  std::vector<std::uint8_t> almost(kBlock, 0);
  almost.back() = 7;
  struct Case {
    const char* name;
    std::vector<const std::vector<std::uint8_t>*> blocks;
  };
  const std::vector<Case> cases = {{"head", {&zeros, &data}}, {"tail", {&data, &zeros, &zeros}}, {"mid", {&data, &zeros, &almost}},
                                   {"all", {&zeros, &zeros}}};
  for (const auto& test : cases) {
    std::vector<std::uint8_t> expect;
    for (const auto* block : test.blocks) expect.insert(expect.end(), block->begin(), block->end());
    std::string error;
    const std::string path = std::string("/") + test.name + ".exfat";
    auto writer = local->open_writer(path, static_cast<std::int64_t>(expect.size()), &error);
    if (!writer) return fail("稀疏写开不了文件: " + error);
    for (const auto* block : test.blocks) {
      // 分两半写，全零块也可能被拆开送进来。
      const std::size_t half = block->size() / 2;
      if (!writer->write(block->data(), half).empty() || !writer->write(block->data() + half, block->size() - half).empty()) {
        return fail(std::string("稀疏写写不进去: ") + test.name);
      }
    }
    if (!writer->finish().empty()) return fail(std::string("稀疏写收尾失败: ") + test.name);
    writer.reset();
    std::ifstream in(root / (std::string(test.name) + ".exfat"), std::ios::binary);
    const std::vector<std::uint8_t> got(std::istreambuf_iterator<char>(in), {});
    if (got.size() != expect.size()) return fail(std::string("稀疏写后文件大小不对: ") + test.name);
    if (got != expect) return fail(std::string("稀疏写后文件内容不对: ") + test.name);
  }
  local.reset();
  std::filesystem::remove_all(root, ec);
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
  request.download.max_connections = 1;
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
  // 文件头写 64 MB 字典，文件本身只有一百多 KB：解压按文件大小要窗口，不按文件头。
  if (!run_rar("a -ep -idq -y -ma5 -m3 -md64m " + shell_quoted(packed_rar) + " " + shell_quoted(rar_dir / "packed.txt"))) {
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
    // 以前中断的安装（比如另一种格式）留在暂存区的文件，不在这次的清单里，装好后也要清掉，不然一直算没装完。
    const std::string staging = "/data/homebrew/.panplay-staging/CUSA00001";
    const std::uint8_t stale[] = {'o', 'l', 'd'};
    const auto leave_stale = [&] {
      if (auto dir_error = local->ensure_dir(staging + "/other"); !dir_error.empty()) return dir_error;
      return local->store(staging + "/other/stale.bin", stale, sizeof(stale));
    };
    const auto staging_left = [&] {
      std::string list_error;
      const auto names = local->list_children("/data/homebrew/.panplay-staging", &list_error);
      return std::find(names.begin(), names.end(), "CUSA00001") != names.end();
    };
    if (error = leave_stale(); !error.empty()) return fail(error);
    InstallResult installed = install_game(drive, *local, request);
    if (!saw_reading_before_total || !saw_total) return fail("安装过程没有先报告正在读取压缩包");
    if (installed.status != "installed") return fail(installed.message);
    if (staging_left()) return fail("装好后暂存区里以前留下的文件没清掉");
    std::vector<std::uint8_t> got;
    error = local->read_file("/data/homebrew/CUSA00001/hello.txt", &got, 100);
    if (!error.empty() || got != hello) return fail("单卷 ZIP 解压内容不对");
    std::string list_error;
    const auto sets = group_archives(collect_archives(drive, request.source_dir, &list_error));
    if (!install_current(*local, request.dest_prefix, request.source_dir, sets)) return fail("装好的游戏没认成和网盘一致");
    if (error = leave_stale(); !error.empty()) return fail(error);
    InstallResult again = install_game(drive, *local, request);
    if (again.status != "already") return fail("单卷安装后没有显示已安装");
    if (staging_left()) return fail("已安装时暂存区里留下的文件没清掉");
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
    // 只存不压的文件解压时不用字典，上限再小也照装。
    auto local = make_local_sink((root / "rar-dict-stored").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Rar CUSA77777";
    request.dest_prefix = "/data/homebrew";
    request.max_dictionary = 1;
    const InstallResult stored = install_game(drive, *local, request);
    if (stored.status != "installed") return fail("只存不压的 RAR 不该因为字典上限被拒: " + stored.status + " " + stored.message);
  }

  {
    // 文件头写着 64 MB 字典，按文件大小只要 256 KB 的窗口：上限 1 MB 能装，上限 1 字节要拒。
    auto local = make_local_sink((root / "rar-dict").u8string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Packed CUSA75555";
    request.dest_prefix = "/data/homebrew";
    request.max_dictionary = 1 << 20;
    const InstallResult small = install_game(drive, *local, request);
    if (small.status != "installed") return fail("小文件不该按文件头里的大字典被拒: " + small.status + " " + small.message);
    if (auto remove_error = uninstall_game(*local, request.dest_prefix, request.source_dir); !remove_error.empty()) {
      return fail("删除装好的游戏失败: " + remove_error);
    }
    request.max_dictionary = 1;
    const InstallResult refused = install_game(drive, *local, request);
    if (refused.status != "error" || refused.message.find("超过上限") == std::string::npos) {
      return fail("字典超过上限的 RAR 没有被拒绝: " + refused.status + " " + refused.message);
    }
    if (!refused.retryable) return fail("字典超过上限调高后可以重试，不该标成不能重试");
    error.clear();
    if (local->file_size("/data/homebrew/CUSA75555/packed.txt", &error) >= 0) return fail("字典超过上限还是装了文件");
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
    // Linux 上 rar 按目录遍历顺序打包，先装好的不一定是 d1/f0.bin。
    std::size_t first_at = std::string::npos;
    for (int i = 0; i < 6 && first_at == std::string::npos; ++i) first_at = journal_text.find(many_name(i) + "\"");
    const auto name_at = first_at == std::string::npos ? std::string::npos : journal_text.rfind("\"name\":\"", first_at);
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

  if (const int code = download_tests()) return code;
  if (const int code = download_sim_tests()) return code;
  if (const int code = speed_test_tests()) return code;
  if (const int code = big_memory_test()) return code;
  if (const int code = install_clock_test()) return code;
  if (const int code = game_root_test()) return code;
  if (const int code = package_and_duplicate_test()) return code;
  if (const int code = many_files_test()) return code;
  if (const int code = plan_install_test()) return code;
  if (const int code = zip64_split_test()) return code;
  if (const int code = write_behind_test()) return code;
  if (const int code = disk_full_test()) return code;
  if (const int code = variant_test()) return code;
  if (const int code = sparse_write_test()) return code;
  if (const int code = cancel_install_test()) return code;
  if (const int code = large_file_test()) return code;
  if (const int code = sevenzip_test()) return code;

  std::cout << "self-test ok\n";
  return 0;
}

int run_download_bench() {
  for (const SimScenario& scenario : download_sim_scenarios()) {
    std::cout << describe_sim(scenario, run_download_sim(scenario)) << std::flush;
  }
  return 0;
}

int run_unpack_bench(const std::string& archive_dir, const std::string& out_dir, int rounds) {
  const std::filesystem::path source = std::filesystem::u8path(archive_dir);
  const std::string game_dir = "/apps/panplay/" + source.filename().u8string();
  FakeDrive drive;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::filesystem::path path = entry.path();
    drive.add_generated(game_dir + "/" + path.filename().u8string(), static_cast<std::int64_t>(entry.file_size()),
                        [path](std::int64_t offset, std::uint8_t* dst, std::size_t size) {
                          std::ifstream in(path, std::ios::binary);
                          in.seekg(offset);
                          in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
                        });
  }
  if (ec) {
    std::cerr << "读不了 " << archive_dir << ": " << ec.message() << "\n";
    return 1;
  }
  const std::filesystem::path out = std::filesystem::u8path(out_dir);
  for (int round = 0; round < rounds; ++round) {
    for (const std::size_t queue : {std::size_t{0}, std::size_t{32u << 20}}) {
      std::filesystem::remove_all(out, ec);
      auto local = make_local_sink(out.u8string());
      InstallRequest request;
      request.source_dir = game_dir;
      request.write_behind_bytes = queue;
      const auto started = std::chrono::steady_clock::now();
      const InstallResult result = install_game(drive, *local, request);
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      std::cout << (queue == 0 ? "同步写盘" : "后台写盘") << " 第 " << round + 1 << " 轮: " << result.status << "，"
                << seconds << " 秒，" << result.message << "\n"
                << std::flush;
      if (result.status != "installed") return 1;
    }
  }
  std::filesystem::remove_all(out, ec);
  return 0;
}

}  // namespace panplay
