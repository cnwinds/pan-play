#include "panplay/install.hpp"

#include "install_common.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/trace.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(_UNIX)
#define _UNIX
#endif

#include "dll.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
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
};

struct RarSession {
  panplay::DrivePlugin* drive = nullptr;
  const panplay::CancelToken* cancel = nullptr;
  panplay::PrefetchOptions prefetch;
  std::function<void(std::int64_t)> on_bytes;
  std::chrono::steady_clock::time_point bytes_reported{};
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

// wchar_t 在 Windows 上是 UTF-16，在其他平台上是 UTF-32。
std::wstring utf8_to_wide(const std::string& text) {
  std::wstring wide;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    const int extra = lead < 0x80 ? 0 : lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : -1;
    std::uint32_t point = extra == 0 ? lead : extra == 1 ? lead & 0x1F : extra == 2 ? lead & 0x0F : lead & 0x07;
    bool valid = extra >= 0 && i + static_cast<std::size_t>(extra) < text.size();
    for (int k = 1; valid && k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
      if ((next & 0xC0) != 0x80) valid = false;
      point = (point << 6) | (next & 0x3F);
    }
    if (!valid) {
      wide.push_back(static_cast<wchar_t>(0xFFFD));
      ++i;
      continue;
    }
    i += static_cast<std::size_t>(extra) + 1;
    if (sizeof(wchar_t) == 2 && point >= 0x10000) {
      point -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xD800 + (point >> 10)));
      wide.push_back(static_cast<wchar_t>(0xDC00 + (point & 0x3FF)));
    } else {
      wide.push_back(static_cast<wchar_t>(point));
    }
  }
  return wide;
}

std::string wide_to_utf8(const wchar_t* text) {
  std::string utf8;
  if (text == nullptr) return utf8;
  for (; *text != L'\0'; ++text) {
    std::uint32_t point = static_cast<std::uint32_t>(*text);
    if (sizeof(wchar_t) == 2 && point >= 0xD800 && point < 0xDC00 && text[1] >= 0xDC00 && text[1] < 0xE000) {
      point = 0x10000 + ((point - 0xD800) << 10) + (static_cast<std::uint32_t>(text[1]) - 0xDC00);
      ++text;
    }
    if (point < 0x80) {
      utf8.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      utf8.push_back(static_cast<char>(0xC0 | (point >> 6)));
      utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
      utf8.push_back(static_cast<char>(0xE0 | (point >> 12)));
      utf8.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
      utf8.push_back(static_cast<char>(0xF0 | (point >> 18)));
      utf8.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
  }
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

// 已读到整组分卷的哪个位置，算进度用。
std::int64_t archive_position() {
  std::int64_t position = 0;
  for (int i = 0; i < session->index && static_cast<std::size_t>(i) < session->volumes.size(); ++i) {
    position += session->volumes[static_cast<std::size_t>(i)].size;
  }
  if (static_cast<std::size_t>(session->index) < session->volumes.size()) {
    position += session->volumes[static_cast<std::size_t>(session->index)].pos;
  }
  return position;
}

void report_bytes(bool force) {
  if (!session->on_bytes) return;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - session->bytes_reported < std::chrono::milliseconds(500)) return;
  session->bytes_reported = now;
  session->on_bytes(archive_position());
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
  report_bytes(false);
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

std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const RarExtract& options, std::vector<FinishedFile>* done,
                                std::vector<ListedRarEntry>* listed) {
  SessionGuard guard(drive, parts, options.cancel, options.prefetch);
  guard.state.on_bytes = options.on_bytes;
  std::map<std::string, std::pair<std::int64_t, std::uint32_t>> finished_before;
  for (const auto& file : *done) finished_before[file.name] = {file.size, file.crc};
  const auto started = std::chrono::steady_clock::now();
  struct Trace {
    RarSession& state;
    std::chrono::steady_clock::time_point started;
    ~Trace() {
      if (!trace_enabled()) return;
      const auto seconds = [](std::chrono::steady_clock::duration value) {
        return std::to_string(static_cast<long long>(std::chrono::duration<double>(value).count()));
      };
      const auto total = std::chrono::steady_clock::now() - started;
      panplay::trace("RAR 解压结束，共 " + seconds(total) + " 秒：等数据 " + seconds(state.read_time) + " 秒（" +
                     std::to_string(state.read_calls) + " 次读），写出 " + seconds(state.write_time) + " 秒，其余（解压+校验）" +
                     seconds(total - state.read_time - state.write_time) + " 秒，" + memory_summary());
    }
  } trace_end{guard.state, started};
  std::uint64_t largest_dictionary = 0;
  HANDLE handle = nullptr;
  if (auto error = open_archive(guard, RAR_OM_EXTRACT, &handle); !error.empty()) return error;
  RarWrite writer;
  active_write = &writer;
  std::string failure;
  bool seen_sys = false;
  while (failure.empty()) {
    RARHeaderDataEx header{};
    const int code = RARReadHeaderEx(handle, &header);
    // 网盘读出错、安装被取消、接不上下一卷时，UnRAR 也常常只报压缩包结束；这些都不能当作读完了。
    if (code == ERAR_END_ARCHIVE) {
      const int last = static_cast<int>(guard.state.volumes.size()) - 1;
      if (!guard.state.read_error.empty()) {
        failure = guard.state.read_error;
      } else if (options.cancel != nullptr && options.cancel->cancelled()) {
        failure = kInterrupted;
      } else if (guard.state.index < last) {
        failure = "RAR 没有读完：停在第 " + std::to_string(guard.state.index + 1) + " 卷，共 " + std::to_string(last + 1) + " 卷";
      }
      break;
    }
    if (code != ERAR_SUCCESS) {
      failure = process_failure("读取 RAR 文件头失败");
      break;
    }
    if ((header.Flags & RHDF_ENCRYPTED) != 0) {
      failure = "RAR 有密码，这一版不能安装";
      break;
    }
    const bool solid = (header.Flags & RHDF_SOLID) != 0;
    // 解压要把整个字典放在内存里，这是装 RAR 时最大的一块内存。
    const std::uint64_t dictionary = static_cast<std::uint64_t>(header.DictSize) << 10;
    if (dictionary > largest_dictionary) {
      largest_dictionary = dictionary;
      trace("RAR 字典 " + std::to_string(dictionary >> 20) + " MB" + (solid ? "（固实）" : "") + "，解压需要这么多内存");
    }
    if (options.max_dictionary > 0 && dictionary > options.max_dictionary) {
      failure = dictionary_refusal("RAR", dictionary, options.max_dictionary);
      break;
    }
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
    // 跳过跨卷文件时 UnRAR 不会接着读后面的卷，而是在后面每一卷再给出这个文件的续段；续段跟着跳过。
    // 解压时 UnRAR 自己接卷，不会出现续段。
    if ((header.Flags & RHDF_SPLITBEFORE) != 0) {
      if (RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
        failure = process_failure("跳过跨卷文件失败: " + name);
        break;
      }
      report_bytes(false);
      continue;
    }
    const bool directory = (header.Flags & RHDF_DIRECTORY) != 0 || (!name.empty() && name.back() == '/');
    const std::uint64_t size = unpacked_size(header);
    seen_sys = seen_sys || in_sce_sys(name);
    if (!directory && !seen_sys && is_install_package(name, static_cast<std::int64_t>(size))) {
      failure = install_package_error(name);
      break;
    }
    listed->push_back(ListedRarEntry{name, directory, solid, header.FileCRC, size});
    const std::string staged = options.staging_prefix + name;
    const auto before = finished_before.find(staged);
    const bool already = before != finished_before.end() && before->second.first == static_cast<std::int64_t>(size) &&
                         before->second.second == header.FileCRC;
    if (!directory && options.on_entry) options.on_entry(already);
    if (!directory && !already) {
      writer.out = std::make_unique<OutputFile>(sink, join_storage_path(options.staging_root, staged),
                                                static_cast<std::int64_t>(size), options.stop_after_output_bytes,
                                                options.written);
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
      failure = process_failure("解压 RAR 失败（错误码 " + std::to_string(processed) + "）: " + name);
      break;
    }
    report_bytes(false);
    if (!writer.out) continue;
    failure = writer.out->finish();
    if (!failure.empty()) break;
    // 跨卷文件的文件头里是本卷那段数据的 CRC，不是整个文件的；整个文件的 CRC 已由 UnRAR 在最后一卷校验过。
    const bool split = (header.Flags & RHDF_SPLITAFTER) != 0;
    if (writer.out->size() != static_cast<std::int64_t>(size) || (!split && writer.out->crc() != header.FileCRC)) {
      failure = "校验和不符: " + name;
      break;
    }
    const FinishedFile finished{staged, static_cast<std::int64_t>(size), header.FileCRC};
    done->push_back(finished);
    failure = options.journal->add(finished);
    if (failure.empty() && options.on_file_done) options.on_file_done(*done);
  }
  if (failure.empty()) report_bytes(true);
  writer.out.reset();
  active_write = nullptr;
  RARCloseArchive(handle);
  return failure;
}

}  // namespace panplay
