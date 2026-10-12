#include "panplay/install.hpp"

#include "panplay/dummy_ftp.hpp"
#include "panplay/library.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace panplay {
namespace {

class MemoryDrive final : public DrivePlugin {
 public:
  void add(const std::string& path, std::vector<std::uint8_t> bytes) {
    files_[path] = std::move(bytes);
    std::string cursor = path;
    while (true) {
      const auto slash = cursor.rfind('/');
      if (slash == std::string::npos || slash == 0) break;
      cursor = cursor.substr(0, slash);
      dirs_.insert(cursor);
    }
  }

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
    for (const auto& [file_path, bytes] : files_) consider(file_path, false, static_cast<std::int64_t>(bytes.size()));
    std::vector<DriveEntry> out;
    for (const auto& [name, entry] : children) {
      (void)name;
      out.push_back(entry);
    }
    return out;
  }

  ReadResult read_at(const std::string& file_id, std::int64_t offset, std::size_t length) override {
    ReadResult result;
    const auto it = files_.find(file_id);
    if (it == files_.end()) {
      result.error = "没有这个文件";
      return result;
    }
    if (offset < 0 || length == 0 || static_cast<std::uint64_t>(offset) > it->second.size()) {
      result.error = "读取范围无效";
      return result;
    }
    const auto start = static_cast<std::size_t>(offset);
    const auto n = std::min(length, it->second.size() - start);
    result.bytes.assign(it->second.begin() + static_cast<std::ptrdiff_t>(start),
                        it->second.begin() + static_cast<std::ptrdiff_t>(start + n));
    result.status = 206;
    return result;
  }

 private:
  static std::string trim(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path.empty() ? "/" : path;
  }

  std::map<std::string, std::vector<std::uint8_t>> files_;
  std::set<std::string> dirs_;
};

class CountingSink final : public InstallSink {
 public:
  explicit CountingSink(InstallSink& inner) : inner_(inner) {}

  int stores_ending(const std::string& suffix) const {
    int count = 0;
    for (const auto& [path, hits] : stores_) {
      if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) count += hits;
    }
    return count;
  }

  void reset() { stores_.clear(); }

  std::string ensure_dir(const std::string& path) override { return inner_.ensure_dir(path); }
  std::int64_t file_size(const std::string& path, std::string* error) override { return inner_.file_size(path, error); }
  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    stores_[path] += 1;
    return inner_.store(path, data, size);
  }
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override {
    return inner_.append(path, data, size);
  }
  std::string rename_path(const std::string& from, const std::string& to) override { return inner_.rename_path(from, to); }
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override {
    return inner_.read_file(path, out, max_bytes);
  }
  std::vector<std::string> list_children(const std::string& path, std::string* error) override {
    return inner_.list_children(path, error);
  }
  std::string remove_file(const std::string& path) override { return inner_.remove_file(path); }
  std::string remove_dir(const std::string& path) override { return inner_.remove_dir(path); }

 private:
  InstallSink& inner_;
  std::map<std::string, int> stores_;
};

void append16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
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

std::vector<std::uint8_t> make_zip(const std::vector<ZipInput>& members) {
  struct Meta {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t comp_size = 0;
    std::uint32_t uncomp_size = 0;
    std::uint32_t local_offset = 0;
    std::vector<std::uint8_t> payload;
  };
  std::vector<Meta> metas;
  std::vector<std::uint8_t> out;
  for (const auto& member : members) {
    Meta meta;
    meta.name = member.name;
    meta.uncomp_size = static_cast<std::uint32_t>(member.data.size());
    meta.crc = static_cast<std::uint32_t>(::crc32(::crc32(0L, Z_NULL, 0), member.data.empty() ? Z_NULL : member.data.data(),
                                                 static_cast<uInt>(member.data.size())));
    if (member.deflate) {
      meta.method = 8;
      meta.payload = raw_deflate(member.data);
    } else {
      meta.method = 0;
      meta.payload = member.data;
    }
    meta.comp_size = static_cast<std::uint32_t>(meta.payload.size());
    meta.local_offset = static_cast<std::uint32_t>(out.size());
    append32(out, 0x04034b50);
    append16(out, 20);
    append16(out, 0);
    append16(out, meta.method);
    append16(out, 0);
    append16(out, 0);
    append32(out, meta.crc);
    append32(out, meta.comp_size);
    append32(out, meta.uncomp_size);
    append16(out, static_cast<std::uint16_t>(meta.name.size()));
    append16(out, 0);
    out.insert(out.end(), meta.name.begin(), meta.name.end());
    out.insert(out.end(), meta.payload.begin(), meta.payload.end());
    metas.push_back(std::move(meta));
  }
  const auto central = static_cast<std::uint32_t>(out.size());
  for (const auto& meta : metas) {
    append32(out, 0x02014b50);
    append16(out, 20);
    append16(out, 20);
    append16(out, 0);
    append16(out, meta.method);
    append16(out, 0);
    append16(out, 0);
    append32(out, meta.crc);
    append32(out, meta.comp_size);
    append32(out, meta.uncomp_size);
    append16(out, static_cast<std::uint16_t>(meta.name.size()));
    append16(out, 0);
    append16(out, 0);
    append16(out, 0);
    append16(out, 0);
    append32(out, 0);
    append32(out, meta.local_offset);
    out.insert(out.end(), meta.name.begin(), meta.name.end());
  }
  const auto central_size = static_cast<std::uint32_t>(out.size() - central);
  append32(out, 0x06054b50);
  append16(out, 0);
  append16(out, 0);
  append16(out, static_cast<std::uint16_t>(metas.size()));
  append16(out, static_cast<std::uint16_t>(metas.size()));
  append32(out, central_size);
  append32(out, central);
  append16(out, 0);
  return out;
}

std::vector<std::vector<std::uint8_t>> split_marked(const std::vector<std::uint8_t>& zip, std::size_t chunk) {
  std::vector<std::vector<std::uint8_t>> parts;
  for (std::size_t offset = 0; offset < zip.size(); offset += chunk) {
    std::vector<std::uint8_t> part{0x50, 0x4b, 0x07, 0x08};
    const std::size_t n = std::min(chunk, zip.size() - offset);
    part.insert(part.end(), zip.begin() + static_cast<std::ptrdiff_t>(offset),
                zip.begin() + static_cast<std::ptrdiff_t>(offset + n));
    parts.push_back(std::move(part));
  }
  return parts;
}

int fail(const std::string& message) {
  std::cerr << "install: " << message << "\n";
  return 1;
}

bool same_bytes(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right) { return left == right; }

int resume_on(InstallSink& raw, MemoryDrive& drive, const std::vector<std::uint8_t>& param, const std::vector<std::uint8_t>& eboot,
              const std::filesystem::path& disk_root) {
  CountingSink sink(raw);
  InstallRequest request;
  request.source_dir = "/apps/panplay/Demo CUSA12345";
  request.dest_prefix = "/mnt/usb0/homebrew";
  request.stop_after_output_bytes = 150;
  InstallResult first = install_game(drive, sink, request);
  if (first.status != "interrupted" || first.files_done != 1) return fail("中断后状态不对: " + first.status + " " + first.message);
  std::string error;
  const std::string partial = "/mnt/usb0/homebrew/.panplay-staging/CUSA12345/eboot.bin";
  if (sink.file_size(partial, &error) != 50 || !error.empty()) return fail("中断时第二个文件不是 50 字节");
  std::vector<std::uint8_t> journal;
  error = sink.read_file("/mnt/usb0/homebrew/.panplay-staging/CUSA12345/.panplay-journal.json", &journal, 1 << 20);
  if (!error.empty()) return fail(error);
  const std::string journal_text(journal.begin(), journal.end());
  if (journal_text.find("param.sfo") == std::string::npos || journal_text.find("eboot.bin") != std::string::npos) {
    return fail("进度记录没有只留下已校验文件");
  }

  sink.reset();
  request.stop_after_output_bytes = -1;
  InstallResult second = install_game(drive, sink, request);
  if (second.status != "installed" || second.files_done != 2) return fail("继续后没有装完: " + second.message);
  if (sink.stores_ending("/param.sfo") != 0) return fail("已校验文件被重写了");
  if (sink.stores_ending("/eboot.bin") < 1) return fail("未完成文件没有继续写");

  std::vector<std::uint8_t> got_param;
  std::vector<std::uint8_t> got_eboot;
  error = sink.read_file("/mnt/usb0/homebrew/CUSA12345/param.sfo", &got_param, 1 << 20);
  if (!error.empty() || !same_bytes(got_param, param)) return fail("param.sfo 内容不对");
  error = sink.read_file("/mnt/usb0/homebrew/CUSA12345/eboot.bin", &got_eboot, 1 << 20);
  if (!error.empty() || !same_bytes(got_eboot, eboot)) return fail("eboot.bin 内容不对");

  sink.reset();
  InstallResult third = install_game(drive, sink, request);
  if (third.status != "already") return fail("完成后再次安装没有显示已安装");
  if (sink.stores_ending("/param.sfo") != 0 || sink.stores_ending("/eboot.bin") != 0) return fail("已安装后又写了游戏文件");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(disk_root)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() >= 4 && (name.compare(name.size() - 4, 4, ".zip") == 0 || name.compare(name.size() - 4, 4, ".z01") == 0)) {
      return fail("压缩包被写进了存放目录");
    }
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
  const auto split_zip = make_zip({{"param.sfo", param, false}, {"eboot.bin", eboot, true}});
  if (split_zip.empty()) return fail("没有生成测试压缩包");
  const auto parts = split_marked(split_zip, 500);
  if (parts.size() < 2) return fail("分卷没有切开");
  const auto single_zip = make_zip({{"hello.txt", hello, false}});

  MemoryDrive drive;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    char name[32];
    if (i + 1 == parts.size()) {
      std::snprintf(name, sizeof(name), "disc.zip");
    } else {
      std::snprintf(name, sizeof(name), "disc.z%02d", static_cast<int>(i + 1));
    }
    drive.add(std::string("/apps/panplay/Demo CUSA12345/") + name, parts[i]);
  }
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
  const std::string rar_cmd = "\"\"C:\\Program Files\\WinRAR\\Rar.exe\" a -ep -idq -y -ma5 \"" + rar_file.string() +
                              "\" \"" + (rar_dir / "note.txt").string() + "\"\"";
  if (std::system(rar_cmd.c_str()) != 0) return fail("WinRAR 没能生成测试 RAR");
  std::vector<std::uint8_t> rar_bytes;
  {
    std::ifstream rar_in(rar_file, std::ios::binary);
    rar_bytes.assign(std::istreambuf_iterator<char>(rar_in), {});
  }
  std::filesystem::remove(rar_file);
  if (rar_bytes.size() < 7 || std::string(rar_bytes.begin(), rar_bytes.begin() + 4) != "Rar!") return fail("测试 RAR 是空的");
  drive.add("/apps/panplay/Rar CUSA77777/game.rar", std::move(rar_bytes));

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
  auto local_probe = make_local_sink(std::filesystem::temp_directory_path().string());
  InstallResult only_7z = install_game(drive, *local_probe, rejected);
  if (only_7z.status != "error" || only_7z.message.find("7z") == std::string::npos) return fail("7z 没有被拒绝");

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
    auto local = make_local_sink(local_root.string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Single CUSA00001";
    request.dest_prefix = "/mnt/usb0/homebrew";
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
    error = local->read_file("/mnt/usb0/homebrew/CUSA00001/hello.txt", &got, 100);
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
    auto local = make_local_sink((root / "rar").string());
    InstallRequest request;
    request.source_dir = "/apps/panplay/Rar CUSA77777";
    request.dest_prefix = "/mnt/usb0/homebrew";
    const InstallResult installed = install_game(drive, *local, request);
    if (installed.status != "installed") return fail(installed.message);
    std::vector<std::uint8_t> got;
    error = local->read_file("/mnt/usb0/homebrew/CUSA77777/note.txt", &got, 100);
    const std::vector<std::uint8_t> expect{'r', 'a', 'r', '-', 'o', 'k'};
    if (!error.empty() || got != expect) return fail("RAR 解压内容不对");
    const InstallResult again = install_game(drive, *local, request);
    if (again.status != "already") return fail("RAR 安装后没有显示已安装");
  }

  std::cout << "self-test ok\n";
  return 0;
}

}  // namespace panplay
