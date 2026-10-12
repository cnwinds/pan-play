#include "panplay/install.hpp"

#include "panplay/json.hpp"
#include "panplay/library.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "dll.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct VolumeState {
  std::string name;
  std::string file_id;
  std::int64_t size = 0;
  std::int64_t pos = 0;
};

struct RarSession {
  panplay::DrivePlugin* drive = nullptr;
  std::vector<VolumeState> volumes;
  int index = 0;
  std::vector<wchar_t> name_buf;
};

RarSession* session = nullptr;

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
      session->index = static_cast<int>(i);
      session->volumes[i].pos = 0;
      return 1;
    }
  }
  return 0;
}

std::string parent_path(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
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

std::string journal_json(const std::vector<panplay::FinishedFile>& done) {
  std::string json = "{\"done\":[";
  for (std::size_t i = 0; i < done.size(); ++i) {
    if (i != 0) json += ",";
    json += "{\"crc32\":" + std::to_string(done[i].crc) + ",\"name\":\"" + panplay::json_escape(done[i].name) +
            "\",\"size\":" + std::to_string(done[i].size) + "}";
  }
  json += "]}";
  return json;
}

std::string save_journal(panplay::InstallSink& sink, const std::string& path, const std::vector<panplay::FinishedFile>& done) {
  if (auto error = sink.ensure_dir(parent_path(path)); !error.empty()) return error;
  const std::string json = journal_json(done);
  return sink.store(path, reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
}

struct RarWrite {
  panplay::InstallSink* sink = nullptr;
  std::string path;
  std::int64_t stop = -1;
  std::int64_t* written = nullptr;
  std::uint32_t crc = 0;
  std::int64_t committed = 0;
  bool started = false;
  bool discard = false;
  bool interrupted = false;
  std::string error;

  std::string emit(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || discard) return "";
    if (stop >= 0 && *written >= stop) return "interrupted";
    bool partial = false;
    if (stop >= 0 && *written + static_cast<std::int64_t>(size) > stop) {
      size = static_cast<std::size_t>(stop - *written);
      partial = true;
    }
    std::string write_error;
    if (!started) {
      write_error = sink->ensure_dir(parent_path(path));
      if (write_error.empty()) write_error = sink->store(path, data, size);
      if (write_error.empty()) started = true;
    } else {
      write_error = panplay::write_continuing(*sink, path, committed, data, size);
    }
    if (!write_error.empty()) return write_error;
    crc = static_cast<std::uint32_t>(::crc32(crc, data, static_cast<uInt>(size)));
    committed += static_cast<std::int64_t>(size);
    *written += static_cast<std::int64_t>(size);
    return partial ? "interrupted" : "";
  }
};

RarWrite* active_write = nullptr;

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
    const auto error = active_write->emit(reinterpret_cast<const std::uint8_t*>(p1), static_cast<std::size_t>(p2));
    if (error == "interrupted") {
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
  explicit SessionGuard(panplay::DrivePlugin& drive, const std::vector<panplay::DriveEntry>& parts) {
    state.drive = &drive;
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
    return "无法打开 RAR";
  }
  *handle = opened;
  return "";
}

}  // namespace

extern "C" int panplay_unrar_claim(const wchar_t* name) { return claim_name(name); }

extern "C" int panplay_unrar_read(void* data, size_t size) {
  if (session == nullptr || session->index < 0 || static_cast<std::size_t>(session->index) >= session->volumes.size()) {
    return -1;
  }
  VolumeState& volume = session->volumes[static_cast<std::size_t>(session->index)];
  if (volume.pos >= volume.size || size == 0) return 0;
  const auto want = static_cast<std::size_t>(std::min<std::int64_t>(size, volume.size - volume.pos));
  panplay::ReadResult got = session->drive->read_at(volume.file_id, volume.pos, want);
  if (!got.error.empty() || got.bytes.empty() || got.bytes.size() > want) return -1;
  std::memcpy(data, got.bytes.data(), got.bytes.size());
  volume.pos += static_cast<std::int64_t>(got.bytes.size());
  return static_cast<int>(got.bytes.size());
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

std::string list_rar_entries(DrivePlugin& drive, const std::vector<DriveEntry>& parts, std::vector<ListedRarEntry>* entries) {
  SessionGuard guard(drive, parts);
  HANDLE handle = nullptr;
  if (auto error = open_archive(guard, RAR_OM_LIST, &handle); !error.empty()) return error;
  while (true) {
    RARHeaderDataEx header{};
    const int code = RARReadHeaderEx(handle, &header);
    if (code == ERAR_END_ARCHIVE) break;
    if (code != ERAR_SUCCESS) {
      RARCloseArchive(handle);
      return "读取 RAR 文件头失败";
    }
    if ((header.Flags & RHDF_ENCRYPTED) != 0) {
      RARCloseArchive(handle);
      return "RAR 有密码，这一版不能安装";
    }
    if (is_stream_entry(raw_header_name(header))) {
      if (RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
        RARCloseArchive(handle);
        return "读取 RAR 目录失败";
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
    entry.size = static_cast<std::uint64_t>(header.UnpSize) | (static_cast<std::uint64_t>(header.UnpSizeHigh) << 32);
    entries->push_back(std::move(entry));
    if (RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
      RARCloseArchive(handle);
      return "读取 RAR 目录失败";
    }
  }
  RARCloseArchive(handle);
  return "";
}

std::string extract_rar_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                                const std::vector<ListedRarEntry>& entries, const std::string& staging_root,
                                std::vector<FinishedFile>* done, const std::string& journal_path, std::int64_t* written,
                                std::int64_t stop_after_output_bytes,
                                const std::function<void(const std::vector<FinishedFile>& done)>& on_file_done) {
  (void)entries;
  SessionGuard guard(drive, parts);
  HANDLE handle = nullptr;
  RarWrite writer;
  if (auto error = open_archive(guard, RAR_OM_EXTRACT, &handle); !error.empty()) return error;
  writer.sink = &sink;
  writer.stop = stop_after_output_bytes;
  writer.written = written;
  active_write = &writer;
  std::string failure;
  while (failure.empty()) {
    RARHeaderDataEx header{};
    const int code = RARReadHeaderEx(handle, &header);
    if (code == ERAR_END_ARCHIVE) break;
    if (code != ERAR_SUCCESS) {
      failure = "读取 RAR 文件头失败";
      break;
    }
    if ((header.Flags & RHDF_ENCRYPTED) != 0) {
      failure = "RAR 有密码，这一版不能安装";
      break;
    }
    if (is_stream_entry(raw_header_name(header))) {
      writer.discard = true;
      writer.interrupted = false;
      writer.error.clear();
      const bool solid = (header.Flags & RHDF_SOLID) != 0;
      if (RARProcessFileW(handle, solid ? RAR_TEST : RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) {
        failure = "解压 RAR 失败";
        break;
      }
      continue;
    }
    std::string name_error;
    const std::string name = header_name(header, &name_error);
    if (!name_error.empty()) {
      failure = name_error;
      break;
    }
    const bool directory = (header.Flags & RHDF_DIRECTORY) != 0 || (!name.empty() && name.back() == '/');
    const bool solid = (header.Flags & RHDF_SOLID) != 0;
    const auto size = static_cast<std::uint64_t>(header.UnpSize) | (static_cast<std::uint64_t>(header.UnpSizeHigh) << 32);
    bool already = false;
    for (const auto& file : *done) {
      if (file.name == name && file.size == static_cast<std::int64_t>(size) && file.crc == header.FileCRC) already = true;
    }
    writer.discard = already;
    writer.path = join_storage_path(staging_root, name);
    writer.started = false;
    writer.committed = 0;
    writer.crc = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
    writer.interrupted = false;
    writer.error.clear();
    const int operation = directory || (already && !solid) ? RAR_SKIP : RAR_TEST;
    const int processed = RARProcessFileW(handle, operation, nullptr, nullptr);
    if (writer.interrupted) {
      failure = "interrupted";
      break;
    }
    if (!writer.error.empty()) {
      failure = writer.error;
      break;
    }
    if (processed != ERAR_SUCCESS) {
      failure = "解压 RAR 失败: " + name;
      break;
    }
    if (directory || already) continue;
    if (size == 0 && !writer.started) {
      failure = sink.ensure_dir(parent_path(writer.path));
      if (failure.empty()) {
        const std::uint8_t empty = 0;
        failure = sink.store(writer.path, &empty, 0);
      }
    }
    if (!failure.empty()) break;
    if (writer.committed != static_cast<std::int64_t>(size) || writer.crc != header.FileCRC) {
      failure = "校验和不符: " + name;
      break;
    }
    done->push_back(FinishedFile{name, static_cast<std::int64_t>(size), header.FileCRC});
    failure = save_journal(sink, journal_path, *done);
    if (failure.empty() && on_file_done) on_file_done(*done);
  }
  active_write = nullptr;
  RARCloseArchive(handle);
  return failure;
}

}  // namespace panplay
