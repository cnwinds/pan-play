#include "panplay/install.hpp"

#include "install_common.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "dll.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct VolumeState {
  std::string name;
  std::string file_id;
  std::int64_t size = 0;
  std::int64_t pos = 0;
  std::unique_ptr<panplay::PrefetchReader> reader;
  std::vector<std::uint8_t> window;
  std::int64_t window_start = -1;
};

struct RarSession {
  panplay::DrivePlugin* drive = nullptr;
  const panplay::CancelToken* cancel = nullptr;
  panplay::PrefetchOptions prefetch;
  // 列目录时只读文件头，在文件之间跳着读，每次取一小段，不开长连接。
  bool random_access = false;
  std::vector<VolumeState> volumes;
  int index = 0;
  std::vector<wchar_t> name_buf;
  std::string read_error;
  std::chrono::steady_clock::duration read_time{};
  std::chrono::steady_clock::duration write_time{};
  std::int64_t read_calls = 0;
};

// UnRAR 的读取回调不带上下文；解压在调用线程里同步进行，所以按线程保存。
thread_local RarSession* session = nullptr;

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
  if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
  return wide;
}

std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr || text[0] == L'\0') return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string utf8(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
  return utf8;
}

std::string lower_copy(std::string value) {
  for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

std::string base_name(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return wide_to_utf8(path.c_str());
  return wide_to_utf8(path.substr(slash + 1).c_str());
}

int claim_name(const wchar_t* name) {
  if (session == nullptr || name == nullptr) return 0;
  const std::string wanted = lower_copy(base_name(name));
  for (std::size_t i = 0; i < session->volumes.size(); ++i) {
    if (lower_copy(session->volumes[i].name) == wanted) {
      if (static_cast<std::size_t>(session->index) < session->volumes.size()) {
        session->volumes[static_cast<std::size_t>(session->index)].reader.reset();
      }
      session->index = static_cast<int>(i);
      session->volumes[i].pos = 0;
      return 1;
    }
  }
  return 0;
}

bool safe_name(const std::string& name, std::string* normalized, bool* directory) {
  std::string path = name;
  for (char& ch : path) {
    if (ch == '\\') ch = '/';
  }
  *directory = !path.empty() && path.back() == '/';
  if (path.empty() || path.front() == '/') return false;
  for (const char ch : path) {
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) return false;
  }
  std::string out;
  std::size_t index = 0;
  while (index < path.size()) {
    while (index < path.size() && path[index] == '/') ++index;
    if (index >= path.size()) break;
    const auto slash = path.find('/', index);
    const auto end = slash == std::string::npos ? path.size() : slash;
    const std::string segment = path.substr(index, end - index);
    index = end;
    if (segment.empty() || segment == ".") continue;
    if (segment == ".." || segment.find(':') != std::string::npos) return false;
    if (!out.empty()) out += "/";
    out += segment;
  }
  if (out.empty()) return false;
  if (*directory) out += "/";
  *normalized = out;
  return true;
}

std::string raw_header_name(const RARHeaderDataEx& header) {
  std::string raw = wide_to_utf8(header.FileNameW);
  if (raw.empty()) raw = header.FileName;
  return raw;
}

std::string header_name(const RARHeaderDataEx& header, std::string* error) {
  const std::string raw = raw_header_name(header);
  bool directory = false;
  std::string normalized;
  if (!safe_name(raw, &normalized, &directory)) {
    *error = "压缩包路径不安全";
    return {};
  }
  return normalized;
}

// NTFS 附加流（如 Zone.Identifier）在 RAR 里是 "文件名:流名"，UnRAR 把冒号换成 U+F03A。
bool is_stream_entry(const std::string& name) {
  const auto slash = name.find_last_of("/\\");
  const std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
  return leaf.find(':') != std::string::npos || leaf.find("\xEF\x80\xBA") != std::string::npos;
}

std::uint64_t unpacked_size(const RARHeaderDataEx& header) {
  return static_cast<std::uint64_t>(header.UnpSize) | (static_cast<std::uint64_t>(header.UnpSizeHigh) << 32);
}

struct RarWrite {
  std::unique_ptr<panplay::OutputFile> out;
  bool interrupted = false;
  std::string error;
};

thread_local RarWrite* active_write = nullptr;

int CALLBACK rar_callback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2) {
  (void)user;
  if (msg == UCM_LARGEDICT) return 1;
  if (msg == UCM_NEEDPASSWORD || msg == UCM_NEEDPASSWORDW) return -1;
  if (msg == UCM_CHANGEVOLUMEW) {
    if (p2 != RAR_VOL_ASK) return 1;
    return claim_name(reinterpret_cast<const wchar_t*>(p1)) ? 1 : -1;
  }
  if (msg == UCM_CHANGEVOLUME) {
    if (p2 != RAR_VOL_ASK) return 1;
    std::wstring wide;
    const auto* text = reinterpret_cast<const char*>(p1);
    while (text != nullptr && *text != '\0') wide.push_back(static_cast<unsigned char>(*text++));
    return claim_name(wide.c_str()) ? 1 : -1;
  }
  if (msg == UCM_PROCESSDATA && active_write != nullptr) {
    if (!active_write->out) return 1;
    const auto started = std::chrono::steady_clock::now();
    const auto error = active_write->out->write(reinterpret_cast<const std::uint8_t*>(p1), static_cast<std::size_t>(p2));
    if (session != nullptr) session->write_time += std::chrono::steady_clock::now() - started;
    if (error == panplay::kInterrupted) {
      active_write->interrupted = true;
      return -1;
    }
    if (!error.empty()) {
      active_write->error = error;
      return -1;
    }
    return 1;
  }
  return 1;
}

struct SessionGuard {
  RarSession state;
  SessionGuard(panplay::DrivePlugin& drive, const std::vector<panplay::DriveEntry>& parts, const panplay::CancelToken* cancel,
               const panplay::PrefetchOptions& prefetch) {
    state.drive = &drive;
    state.cancel = cancel;
    state.prefetch = prefetch;
    for (const auto& part : parts) {
      VolumeState volume;
      volume.name = part.name;
      volume.file_id = part.file_id;
      volume.size = part.size;
      state.volumes.push_back(std::move(volume));
    }
    const std::wstring wide = utf8_to_wide(parts.empty() ? std::string() : parts.front().name);
    state.name_buf.assign(wide.begin(), wide.end());
    state.name_buf.push_back(L'\0');
    session = &state;
  }
  ~SessionGuard() { session = nullptr; }
};

std::string open_archive(SessionGuard& guard, unsigned int mode, HANDLE* handle) {
  if (guard.state.volumes.empty()) return "RAR 分卷是空的";
  RAROpenArchiveDataEx data{};
  data.ArcNameW = guard.state.name_buf.data();
  data.OpenMode = mode;
  data.Callback = rar_callback;
  HANDLE opened = RAROpenArchiveEx(&data);
  if (opened == nullptr || data.OpenResult != 0) {
    if (opened != nullptr) RARCloseArchive(opened);
    return guard.state.read_error.empty() ? "无法打开 RAR" : guard.state.read_error;
  }
  *handle = opened;
  return "";
}

int read_window(VolumeState& volume, std::uint8_t* out, std::size_t size) {
  constexpr std::size_t kWindow = 64u << 10;
  const auto want = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(size), volume.size - volume.pos));
  const std::int64_t window_end = volume.window_start + static_cast<std::int64_t>(volume.window.size());
  if (volume.window_start < 0 || volume.pos < volume.window_start || volume.pos + static_cast<std::int64_t>(want) > window_end) {
    if (session->cancel && session->cancel->cancelled()) {
      session->read_error = panplay::kInterrupted;
      return -1;
    }
    const auto length = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(std::max(want, kWindow)), volume.size - volume.pos));
    panplay::ReadResult result = session->drive->read_at(volume.file_id, volume.pos, length);
    if (!result.error.empty()) {
      session->read_error = result.error;
      return -1;
    }
    volume.window = std::move(result.bytes);
    volume.window_start = volume.pos;
  }
  const auto offset = static_cast<std::size_t>(volume.pos - volume.window_start);
  const std::size_t n = std::min(want, volume.window.size() - offset);
  std::memcpy(out, volume.window.data() + offset, n);
  volume.pos += static_cast<std::int64_t>(n);
  return static_cast<int>(n);
}

std::string process_failure(const std::string& fallback) {
  if (session != nullptr && !session->read_error.empty()) return session->read_error;
  return fallback;
}

}  // namespace

extern "C" int panplay_unrar_claim(const wchar_t* name) { return claim_name(name); }

extern "C" int panplay_unrar_read(void* data, size_t size) {
  if (session == nullptr || session->index < 0 || static_cast<std::size_t>(session->index) >= session->volumes.size()) {
    return -1;
  }
  VolumeState& volume = session->volumes[static_cast<std::size_t>(session->index)];
  if (volume.pos >= volume.size || size == 0) return 0;
  if (session->random_access) return read_window(volume, static_cast<std::uint8_t*>(data), size);
  if (!volume.reader) {
    volume.reader = std::make_unique<panplay::PrefetchReader>(*session->drive, volume.file_id, volume.size, session->cancel,
                                                              session->prefetch);
  }
  if (volume.reader->position() != volume.pos) {
    if (auto error = volume.reader->seek(volume.pos); !error.empty()) {
      session->read_error = error;
      return -1;
    }
  }
  const auto want = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(size), volume.size - volume.pos));
  auto* out = static_cast<std::uint8_t*>(data);
  std::size_t filled = 0;
  const auto started = std::chrono::steady_clock::now();
  ++session->read_calls;
  struct Timer {
    std::chrono::steady_clock::time_point started;
    ~Timer() { session->read_time += std::chrono::steady_clock::now() - started; }
  } timer{started};
  while (filled < want) {
    std::string error;
    const std::size_t got = volume.reader->read(out + filled, want - filled, &error);
    if (!error.empty()) {
      session->read_error = error;
      return -1;
    }
    if (got == 0) break;
    filled += got;
  }
  volume.pos += static_cast<std::int64_t>(filled);
  return static_cast<int>(filled);
}

extern "C" int panplay_unrar_seek(long long offset, int method) {
  if (session == nullptr || session->index < 0 || static_cast<std::size_t>(session->index) >= session->volumes.size()) {
    return 0;
  }
  VolumeState& volume = session->volumes[static_cast<std::size_t>(session->index)];
  long long next = volume.pos;
  if (method == SEEK_SET) next = offset;
  else if (method == SEEK_CUR) next = volume.pos + offset;
  else if (method == SEEK_END) next = volume.size + offset;
  else return 0;
  if (next < 0 || next > volume.size) return 0;
  volume.pos = next;
  return 1;
}

extern "C" long long panplay_unrar_tell(void) {
  if (session == nullptr || session->index < 0 || static_cast<std::size_t>(session->index) >= session->volumes.size()) {
    return -1;
  }
  return session->volumes[static_cast<std::size_t>(session->index)].pos;
}

namespace panplay {

std::string list_rar_entries(DrivePlugin& drive, const std::vector<DriveEntry>& parts, const CancelToken* cancel,
                             std::vector<ListedRarEntry>* entries) {
  SessionGuard guard(drive, parts, cancel, PrefetchOptions{});
  guard.state.random_access = true;
  const auto listed_at = std::chrono::steady_clock::now();
  struct Trace {
    std::chrono::steady_clock::time_point started;
    ~Trace() {
      if (std::getenv("PANPLAY_TRACE") == nullptr) return;
      std::cerr << "rar 列目录 " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " 秒\n";
    }
  } trace{listed_at};
  HANDLE handle = nullptr;
  if (auto error = open_archive(guard, RAR_OM_LIST, &handle); !error.empty()) return error;
  while (true) {
    RARHeaderDataEx header{};
    const int code = RARReadHeaderEx(handle, &header);
    if (code == ERAR_END_ARCHIVE) break;
    if (code != ERAR_SUCCESS) {
      RARCloseArchive(handle);
      return process_failure("读取 RAR 文件头失败");
    }
    if ((header.Flags & RHDF_ENCRYPTED) != 0) {
      RARCloseArchive(handle);
      return "RAR 有密码，这一版不能安装";
    }
    if (is_stream_entry(raw_header_name(header))) {
      if (RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
        RARCloseArchive(handle);
        return process_failure("读取 RAR 目录失败");
      }
      continue;
    }
    std::string name_error;
    ListedRarEntry entry;
    entry.name = header_name(header, &name_error);
    if (!name_error.empty()) {
      RARCloseArchive(handle);
      return name_error;
    }
    entry.directory = (header.Flags & RHDF_DIRECTORY) != 0 || (!entry.name.empty() && entry.name.back() == '/');
    entry.solid = (header.Flags & RHDF_SOLID) != 0;
    entry.crc = header.FileCRC;
    entry.size = unpacked_size(header);
    entries->push_back(std::move(entry));
    if (RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
      RARCloseArchive(handle);
      return process_failure("读取 RAR 目录失败");
    }
  }
  RARCloseArchive(handle);
  return "";
}

std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const RarExtract& options, std::vector<FinishedFile>* done) {
  SessionGuard guard(drive, parts, options.cancel, options.prefetch);
  const auto started = std::chrono::steady_clock::now();
  struct Trace {
    RarSession& state;
    std::chrono::steady_clock::time_point started;
    ~Trace() {
      if (std::getenv("PANPLAY_TRACE") == nullptr) return;
      const auto seconds = [](std::chrono::steady_clock::duration value) { return std::chrono::duration<double>(value).count(); };
      const auto total = std::chrono::steady_clock::now() - started;
      std::cerr << "rar 解压共 " << seconds(total) << " 秒：等数据 " << seconds(state.read_time) << " 秒（" << state.read_calls
                << " 次读），写出 " << seconds(state.write_time) << " 秒，其余（解压+校验）"
                << seconds(total - state.read_time - state.write_time) << " 秒\n";
    }
  } trace{guard.state, started};
  HANDLE handle = nullptr;
  if (auto error = open_archive(guard, RAR_OM_EXTRACT, &handle); !error.empty()) return error;
  RarWrite writer;
  active_write = &writer;
  std::string failure;
  while (failure.empty()) {
    RARHeaderDataEx header{};
    const int code = RARReadHeaderEx(handle, &header);
    if (code == ERAR_END_ARCHIVE) break;
    if (code != ERAR_SUCCESS) {
      failure = process_failure("读取 RAR 文件头失败");
      break;
    }
    if ((header.Flags & RHDF_ENCRYPTED) != 0) {
      failure = "RAR 有密码，这一版不能安装";
      break;
    }
    const bool solid = (header.Flags & RHDF_SOLID) != 0;
    writer.out.reset();
    writer.interrupted = false;
    writer.error.clear();
    if (is_stream_entry(raw_header_name(header))) {
      if (RARProcessFileW(handle, solid ? RAR_TEST : RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
        failure = process_failure("解压 RAR 失败");
        break;
      }
      continue;
    }
    std::string name_error;
    std::string name = header_name(header, &name_error);
    if (!name_error.empty()) {
      failure = name_error;
      break;
    }
    const std::string& root = options.game_root;
    if (!root.empty()) {
      const bool inside = name.compare(0, root.size(), root) == 0 && name.size() > root.size();
      if (!inside) {
        if (RARProcessFileW(handle, solid ? RAR_TEST : RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
          failure = process_failure("解压 RAR 失败");
          break;
        }
        continue;
      }
      name.erase(0, root.size());
    }
    const bool directory = (header.Flags & RHDF_DIRECTORY) != 0 || (!name.empty() && name.back() == '/');
    const std::uint64_t size = unpacked_size(header);
    bool already = false;
    for (const auto& file : *done) {
      if (file.name == name && file.size == static_cast<std::int64_t>(size) && file.crc == header.FileCRC) already = true;
    }
    if (!directory && !already) {
      writer.out = std::make_unique<OutputFile>(sink, join_storage_path(options.staging_root, name),
                                                options.stop_after_output_bytes, options.written);
    }
    const int operation = directory || (already && !solid) ? RAR_SKIP : RAR_TEST;
    const int processed = RARProcessFileW(handle, operation, nullptr, nullptr);
    if (writer.interrupted) {
      failure = kInterrupted;
      break;
    }
    if (!writer.error.empty()) {
      failure = writer.error;
      break;
    }
    if (processed != ERAR_SUCCESS) {
      failure = process_failure("解压 RAR 失败: " + name);
      break;
    }
    if (!writer.out) continue;
    failure = writer.out->finish();
    if (!failure.empty()) break;
    if (writer.out->size() != static_cast<std::int64_t>(size) || writer.out->crc() != header.FileCRC) {
      failure = "校验和不符: " + name;
      break;
    }
    const FinishedFile finished{name, static_cast<std::int64_t>(size), header.FileCRC};
    done->push_back(finished);
    failure = append_journal(sink, options.journal_path, finished);
    if (failure.empty() && options.on_file_done) options.on_file_done(*done);
  }
  writer.out.reset();
  active_write = nullptr;
  RARCloseArchive(handle);
  return failure;
}

}  // namespace panplay
