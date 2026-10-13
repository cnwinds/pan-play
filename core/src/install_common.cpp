#include "install_common.hpp"

#include "panplay/json.hpp"

#include <cctype>

#include <zlib.h>

namespace panplay {

std::string parent_path(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

namespace {

std::string lower_copy(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

}  // namespace

bool is_install_package(const std::string& name, std::int64_t size) {
  const std::string lower = lower_copy(name);
  return size >= kInstallPackageMinBytes && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".pkg") == 0;
}

bool in_sce_sys(const std::string& name) {
  const std::string lower = lower_copy(name);
  return lower.rfind("sce_sys/", 0) == 0 || lower.find("/sce_sys/") != std::string::npos;
}

namespace {

constexpr char kInstallPackagePrefix[] = "这是 PKG 安装包（";

}  // namespace

std::string install_package_error(const std::string& name) {
  const auto slash = name.find_last_of('/');
  return kInstallPackagePrefix + (slash == std::string::npos ? name : name.substr(slash + 1)) +
         "），要在主机上用安装包工具安装；盘游和 ShadowMountPlus 只能用解压好的游戏文件夹";
}

bool is_install_package_error(const std::string& message) {
  return message.rfind(kInstallPackagePrefix, 0) == 0;
}

OutputFile::OutputFile(InstallSink& sink, std::string path, std::int64_t expected, std::int64_t stop,
                       std::int64_t* written)
    : sink_(sink), path_(std::move(path)), expected_(expected), stop_(stop), written_(written),
      crc_(static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0))) {}

std::string OutputFile::open() {
  if (writer_) return "";
  if (auto error = sink_.ensure_dir(parent_path(path_)); !error.empty()) return error;
  std::string error;
  writer_ = sink_.open_writer(path_, expected_, &error);
  return writer_ ? "" : (error.empty() ? "无法写入" : error);
}

std::string OutputFile::write(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return "";
  if (stop_ >= 0 && *written_ >= stop_) return kInterrupted;
  bool partial = false;
  if (stop_ >= 0 && *written_ + static_cast<std::int64_t>(size) > stop_) {
    size = static_cast<std::size_t>(stop_ - *written_);
    partial = true;
  }
  if (auto error = open(); !error.empty()) return error;
  if (auto error = writer_->write(data, size); !error.empty()) return error;
  crc_ = static_cast<std::uint32_t>(::crc32(crc_, data, static_cast<uInt>(size)));
  committed_ += static_cast<std::int64_t>(size);
  *written_ += static_cast<std::int64_t>(size);
  return partial ? kInterrupted : "";
}

std::string OutputFile::finish() {
  if (auto error = open(); !error.empty()) return error;
  std::string error = writer_->finish();
  writer_.reset();
  return error;
}

std::vector<FinishedFile> load_journal(InstallSink& sink, const std::string& path) {
  std::vector<FinishedFile> done;
  std::vector<std::uint8_t> bytes;
  if (!sink.read_file(path, &bytes, 256u << 20).empty()) return done;
  const std::string text(bytes.begin(), bytes.end());
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    const bool last = end == std::string::npos;
    if (last) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty()) continue;
    std::string parse_error;
    const Json json = Json::parse(line, &parse_error);
    FinishedFile file;
    file.name = json.str("name");
    const std::string size_text = json.str("size");
    const std::string crc_text = json.str("crc32");
    bool ok = parse_error.empty() && !file.name.empty() && !size_text.empty() && !crc_text.empty();
    if (ok) {
      try {
        file.size = std::stoll(size_text);
        file.crc = static_cast<std::uint32_t>(std::stoul(crc_text));
      } catch (const std::exception&) {
        ok = false;
      }
    }
    if (!ok) {
      if (last) break;
      return {};
    }
    done.push_back(std::move(file));
  }
  return done;
}

namespace {

std::string journal_line(const FinishedFile& file) {
  return "{\"crc32\":" + std::to_string(file.crc) + ",\"name\":\"" + json_escape(file.name) +
         "\",\"size\":" + std::to_string(file.size) + "}\n";
}

}  // namespace

std::string write_journal(InstallSink& sink, const std::string& path, const std::vector<FinishedFile>& done) {
  if (auto error = sink.ensure_dir(parent_path(path)); !error.empty()) return error;
  std::string text;
  for (const auto& file : done) text += journal_line(file);
  return sink.store(path, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

JournalWriter::JournalWriter(InstallSink& sink, std::string path) : sink_(sink), path_(std::move(path)) {}

JournalWriter::~JournalWriter() { flush(); }

std::string JournalWriter::add(const FinishedFile& file) {
  pending_ += journal_line(file);
  ++pending_count_;
  const auto now = std::chrono::steady_clock::now();
  if (pending_count_ < kBatchFiles && now - last_flush_ < std::chrono::seconds(1)) return "";
  return flush();
}

std::string JournalWriter::flush() {
  last_flush_ = std::chrono::steady_clock::now();
  if (pending_.empty()) return "";
  if (!dir_ready_) {
    if (auto error = sink_.ensure_dir(parent_path(path_)); !error.empty()) return error;
    dir_ready_ = true;
  }
  const std::string text = std::move(pending_);
  pending_.clear();
  pending_count_ = 0;
  return sink_.append(path_, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

}  // namespace panplay
