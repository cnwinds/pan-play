#include "panplay/install.hpp"

#include "panplay/json.hpp"
#include "panplay/library.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

namespace panplay {
namespace {

constexpr const char* kInterrupted = "interrupted";

struct Part {
  std::string file_id;
  std::int64_t size = 0;
  std::int64_t data_offset = 0;
};

class VolumeSource {
 public:
  VolumeSource(DrivePlugin* drive, std::vector<Part> parts) : drive_(drive), parts_(std::move(parts)) {
    for (const auto& part : parts_) logical_size_ += part.size - part.data_offset;
    seek(0);
  }

  std::int64_t tell() const { return logical_; }
  bool at_end() const { return logical_ >= logical_size_; }

  std::string seek(std::int64_t logical) {
    if (logical < 0 || logical > logical_size_) return "压缩包偏移越界";
    if (parts_.empty()) {
      logical_ = logical;
      index_ = 0;
      offset_ = 0;
      return logical == 0 ? "" : "压缩包是空的";
    }
    std::int64_t left = logical;
    for (std::size_t i = 0; i < parts_.size(); ++i) {
      const std::int64_t span = parts_[i].size - parts_[i].data_offset;
      if (left < span || i + 1 == parts_.size()) {
        index_ = i;
        offset_ = parts_[i].data_offset + left;
        logical_ = logical;
        return "";
      }
      left -= span;
    }
    return "压缩包偏移越界";
  }

  std::string read(std::uint8_t* dst, std::size_t size) {
    std::size_t filled = 0;
    while (filled < size) {
      while (index_ < parts_.size() && offset_ >= parts_[index_].size) {
        ++index_;
        if (index_ < parts_.size()) offset_ = parts_[index_].data_offset;
      }
      if (index_ >= parts_.size() || logical_ >= logical_size_) return "压缩包意外结束";
      const Part& part = parts_[index_];
      const auto want = static_cast<std::size_t>(std::min<std::int64_t>(size - filled, part.size - offset_));
      ReadResult got = drive_->read_at(part.file_id, offset_, want);
      if (!got.error.empty()) return got.error;
      if (got.bytes.empty() || got.bytes.size() > want) return "压缩包意外结束";
      std::memcpy(dst + filled, got.bytes.data(), got.bytes.size());
      offset_ += static_cast<std::int64_t>(got.bytes.size());
      logical_ += static_cast<std::int64_t>(got.bytes.size());
      filled += got.bytes.size();
    }
    return "";
  }

 private:
  DrivePlugin* drive_ = nullptr;
  std::vector<Part> parts_;
  std::size_t index_ = 0;
  std::int64_t offset_ = 0;
  std::int64_t logical_ = 0;
  std::int64_t logical_size_ = 0;
};

struct Member {
  int set_index = 0;
  std::string name;
  bool directory = false;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint32_t comp_size = 0;
  std::uint64_t uncomp_size = 0;
  std::int64_t data_offset = 0;
};

struct DoneFile {
  std::string name;
  std::int64_t size = 0;
  std::uint32_t crc = 0;
};

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

std::string base_name(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

std::string parent_path(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

bool safe_zip_name(const std::string& name, std::string* normalized, bool* directory) {
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

std::string open_parts(DrivePlugin& drive, const std::vector<DriveEntry>& files, std::vector<Part>* parts) {
  for (const auto& file : files) {
    Part part;
    part.file_id = file.file_id;
    part.size = file.size;
    if (file.size >= 4) {
      ReadResult got = drive.read_at(file.file_id, 0, 4);
      if (!got.error.empty()) return got.error;
      if (got.bytes.size() == 4 && got.bytes[0] == 0x50 && got.bytes[1] == 0x4b && got.bytes[2] == 0x07 &&
          got.bytes[3] == 0x08) {
        part.data_offset = 4;
      }
    }
    if (part.data_offset > part.size) return "分卷头比文件还长";
    parts->push_back(std::move(part));
  }
  return "";
}

std::string scan_zip(VolumeSource& source, int set_index, std::vector<Member>* members) {
  if (auto error = source.seek(0); !error.empty()) return error;
  while (!source.at_end()) {
    std::uint8_t signature_bytes[4];
    if (auto error = source.read(signature_bytes, 4); !error.empty()) return error;
    const std::uint32_t signature = read_u32(signature_bytes);
    if (signature == 0x02014b50 || signature == 0x06054b50) return "";
    if (signature != 0x04034b50) return "无法识别的 ZIP 结构";
    std::uint8_t header[26];
    if (auto error = source.read(header, sizeof(header)); !error.empty()) return error;
    const std::uint16_t flags = read_u16(header + 2);
    const std::uint16_t method = read_u16(header + 4);
    const std::uint32_t crc = read_u32(header + 10);
    const std::uint32_t comp_size = read_u32(header + 14);
    const std::uint32_t uncomp_size = read_u32(header + 18);
    const std::uint16_t name_length = read_u16(header + 22);
    const std::uint16_t extra_length = read_u16(header + 24);
    if ((flags & 1) != 0) return "ZIP 有密码，第一版不能安装";
    if (comp_size == 0xffffffffu || uncomp_size == 0xffffffffu) return "ZIP64 第一版不能安装";
    if ((flags & 8) != 0 && comp_size == 0) return "ZIP 未在文件头写明大小，第一版不能跳着读";
    std::string raw_name(name_length, '\0');
    if (name_length > 0) {
      if (auto error = source.read(reinterpret_cast<std::uint8_t*>(&raw_name[0]), name_length); !error.empty()) {
        return error;
      }
    }
    if (extra_length > 0) {
      std::vector<std::uint8_t> extra(extra_length);
      if (auto error = source.read(extra.data(), extra.size()); !error.empty()) return error;
    }
    Member member;
    member.set_index = set_index;
    member.directory = false;
    if (!safe_zip_name(raw_name, &member.name, &member.directory)) return "压缩包路径不安全";
    if (!member.directory && method != 0 && method != 8) return "ZIP 使用了第一版不支持的压缩方法";
    member.method = method;
    member.crc = crc;
    member.comp_size = comp_size;
    member.uncomp_size = uncomp_size;
    member.data_offset = source.tell();
    if (auto error = source.seek(member.data_offset + static_cast<std::int64_t>(comp_size)); !error.empty()) return error;
    members->push_back(std::move(member));
  }
  return "";
}

std::string marker_json(const std::string& title_id, const std::string& plugin, std::vector<Member> members) {
  std::sort(members.begin(), members.end(), [](const Member& left, const Member& right) { return left.name < right.name; });
  std::string json = "{\"entries\":[";
  bool first = true;
  for (const auto& member : members) {
    if (member.directory) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"crc32\":" + std::to_string(member.crc) + ",\"name\":\"" + json_escape(member.name) +
            "\",\"size\":" + std::to_string(member.uncomp_size) + "}";
  }
  json += "],\"plugin\":\"" + json_escape(plugin) + "\",\"title_id\":\"" + json_escape(title_id) + "\"}";
  return json;
}

std::string journal_json(const std::vector<DoneFile>& done) {
  std::string json = "{\"done\":[";
  for (std::size_t i = 0; i < done.size(); ++i) {
    if (i != 0) json += ",";
    json += "{\"crc32\":" + std::to_string(done[i].crc) + ",\"name\":\"" + json_escape(done[i].name) +
            "\",\"size\":" + std::to_string(done[i].size) + "}";
  }
  json += "]}";
  return json;
}

std::string load_journal(InstallSink& sink, const std::string& path, std::vector<DoneFile>* done) {
  std::vector<std::uint8_t> bytes;
  const std::string error = sink.read_file(path, &bytes, 1 << 20);
  if (error == "missing") return "";
  if (!error.empty()) return error;
  std::string parse_error;
  const Json json = Json::parse(std::string(bytes.begin(), bytes.end()), &parse_error);
  if (!parse_error.empty()) return parse_error;
  const Json* list = json.get("done");
  if (!list || list->type != Json::Type::array) return "进度记录无法识别";
  for (const Json& item : list->array) {
    DoneFile file;
    file.name = item.str("name");
    const std::string size_text = item.str("size");
    const std::string crc_text = item.str("crc32");
    if (file.name.empty() || size_text.empty() || crc_text.empty()) return "进度记录无法识别";
    file.size = std::stoll(size_text);
    file.crc = static_cast<std::uint32_t>(std::stoul(crc_text));
    done->push_back(std::move(file));
  }
  return "";
}

std::string save_journal(InstallSink& sink, const std::string& path, const std::vector<DoneFile>& done) {
  if (auto error = sink.ensure_dir(parent_path(path)); !error.empty()) return error;
  const std::string json = journal_json(done);
  return sink.store(path, reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
}

bool already_done(const std::vector<DoneFile>& done, const Member& member) {
  for (const auto& file : done) {
    if (file.name == member.name && file.size == static_cast<std::int64_t>(member.uncomp_size) && file.crc == member.crc) {
      return true;
    }
  }
  return false;
}

struct Writer {
  InstallSink* sink = nullptr;
  std::string path;
  std::int64_t stop = -1;
  std::int64_t* written = nullptr;
  std::uint32_t crc = 0;
  std::int64_t committed = 0;
  bool started = false;

  std::string emit(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return "";
    if (stop >= 0 && *written >= stop) return kInterrupted;
    bool partial = false;
    if (stop >= 0 && *written + static_cast<std::int64_t>(size) > stop) {
      size = static_cast<std::size_t>(stop - *written);
      partial = true;
    }
    std::string error;
    if (!started) {
      error = sink->ensure_dir(parent_path(path));
      if (!error.empty()) return error;
      error = sink->store(path, data, size);
      if (error.empty()) started = true;
    } else {
      error = write_continuing(*sink, path, committed, data, size);
    }
    if (!error.empty()) return error;
    crc = static_cast<std::uint32_t>(::crc32(crc, data, static_cast<uInt>(size)));
    committed += static_cast<std::int64_t>(size);
    *written += static_cast<std::int64_t>(size);
    return partial ? kInterrupted : "";
  }
};

std::string extract_member(VolumeSource& source, InstallSink& sink, const Member& member, const std::string& staging_root,
                           std::vector<DoneFile>* done, const std::string& journal_path, std::int64_t* written,
                           std::int64_t stop) {
  if (member.directory) return sink.ensure_dir(join_storage_path(staging_root, member.name));
  if (already_done(*done, member)) return source.seek(member.data_offset + static_cast<std::int64_t>(member.comp_size));
  if (stop >= 0 && *written >= stop) return kInterrupted;
  if (auto error = source.seek(member.data_offset); !error.empty()) return error;

  const std::string output = join_storage_path(staging_root, member.name);
  Writer writer;
  writer.sink = &sink;
  writer.path = output;
  writer.stop = stop;
  writer.written = written;
  writer.crc = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));

  std::string error;
  if (member.uncomp_size == 0 && member.comp_size == 0) {
    const std::uint8_t empty = 0;
    error = sink.ensure_dir(parent_path(output));
    if (error.empty()) error = sink.store(output, &empty, 0);
  } else if (member.method == 0) {
    std::int64_t left = member.comp_size;
    std::uint8_t buffer[8192];
    while (left > 0 && error.empty()) {
      const auto n = static_cast<std::size_t>(std::min<std::int64_t>(sizeof(buffer), left));
      error = source.read(buffer, n);
      if (error.empty()) error = writer.emit(buffer, n);
      if (error.empty()) left -= static_cast<std::int64_t>(n);
    }
  } else {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return "无法开始解压";
    std::int64_t left = member.comp_size;
    std::uint8_t input[16384];
    int result = Z_OK;
    while (result != Z_STREAM_END && error.empty()) {
      if (stream.avail_in == 0) {
        if (left == 0) {
          error = "解压数据提前结束";
          break;
        }
        const auto n = static_cast<std::size_t>(std::min<std::int64_t>(sizeof(input), left));
        error = source.read(input, n);
        if (!error.empty()) break;
        left -= static_cast<std::int64_t>(n);
        stream.next_in = input;
        stream.avail_in = static_cast<uInt>(n);
      }
      std::uint8_t output_buffer[8192];
      stream.next_out = output_buffer;
      stream.avail_out = sizeof(output_buffer);
      result = inflate(&stream, Z_NO_FLUSH);
      const std::size_t produced = sizeof(output_buffer) - stream.avail_out;
      if (produced > 0 && error.empty()) error = writer.emit(output_buffer, produced);
      if (result == Z_STREAM_END) break;
      if (result != Z_OK && error.empty()) error = "解压失败";
    }
    inflateEnd(&stream);
    if (error.empty() && member.uncomp_size == 0 && !writer.started) {
      const std::uint8_t empty = 0;
      error = sink.ensure_dir(parent_path(output));
      if (error.empty()) error = sink.store(output, &empty, 0);
    }
  }
  if (error == kInterrupted) return error;
  if (!error.empty()) return error;
  if (writer.committed != static_cast<std::int64_t>(member.uncomp_size) || writer.crc != member.crc) {
    return "校验和不符: " + member.name;
  }
  done->push_back(DoneFile{member.name, static_cast<std::int64_t>(member.uncomp_size), member.crc});
  return save_journal(sink, journal_path, *done);
}

std::string finish_install(InstallSink& sink, const std::string& staging_root, const std::string& final_root,
                           const std::vector<Member>& members, const std::string& marker, const std::string& marker_path) {
  for (const auto& member : members) {
    if (member.directory) {
      if (auto error = sink.ensure_dir(join_storage_path(final_root, member.name)); !error.empty()) return error;
      continue;
    }
    const std::string from = join_storage_path(staging_root, member.name);
    const std::string to = join_storage_path(final_root, member.name);
    std::string error;
    const std::int64_t staged = sink.file_size(from, &error);
    if (!error.empty()) return error;
    const std::int64_t placed = sink.file_size(to, &error);
    if (!error.empty()) return error;
    // 上次收尾中断时，有的文件已经搬过去了。
    if (staged < 0 && placed == static_cast<std::int64_t>(member.uncomp_size)) continue;
    if (staged < 0) return "暂存文件丢失，请删除暂存目录后重装: " + member.name;
    if (placed >= 0) {
      if (error = sink.remove_file(to); !error.empty()) return error;
    }
    if (error = sink.ensure_dir(parent_path(to)); !error.empty()) return error;
    if (error = sink.rename_path(from, to); !error.empty()) return error;
  }
  if (auto error = sink.ensure_dir(final_root); !error.empty()) return error;
  return sink.store(marker_path, reinterpret_cast<const std::uint8_t*>(marker.data()), marker.size());
}

// 安装完成后暂存区只剩日志和空目录。清理失败不影响安装结果，下次安装会再清一次。
void clean_staging(InstallSink& sink, const std::string& staging_root, const std::string& journal_path,
                   const std::vector<Member>& members) {
  std::set<std::string> dirs;
  for (const auto& member : members) {
    std::string name = member.name;
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (!member.directory) {
      const auto slash = name.find_last_of('/');
      name = slash == std::string::npos ? "" : name.substr(0, slash);
    }
    while (!name.empty()) {
      dirs.insert(name);
      const auto slash = name.find_last_of('/');
      name = slash == std::string::npos ? "" : name.substr(0, slash);
    }
  }
  std::vector<std::string> ordered(dirs.begin(), dirs.end());
  std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
    return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/');
  });
  sink.remove_file(journal_path);
  for (const auto& dir : ordered) sink.remove_dir(join_storage_path(staging_root, dir));
  sink.remove_dir(staging_root);
  sink.remove_dir(parent_path(staging_root));
}

}  // namespace

InstallResult install_game(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request) {
  InstallResult result;
  const std::string folder_name = base_name(request.source_dir);
  const std::string title_id = find_title_id(folder_name);
  const std::string folder = title_id.empty() ? folder_name : title_id;
  if (folder.empty() || folder == "." || folder == ".." || folder.find(':') != std::string::npos) {
    result.status = "error";
    result.message = "游戏目录名不能作为存放目录";
    return result;
  }

  const auto report = [&](const std::string& message, int done, int total) {
    if (!request.on_progress) return;
    InstallResult snapshot;
    snapshot.message = message;
    snapshot.files_done = done;
    snapshot.files_total = total;
    request.on_progress(snapshot);
  };
  report("正在读取压缩包…", 0, 0);

  std::string error;
  const auto files = collect_archives(drive, request.source_dir, &error);
  if (!error.empty()) {
    result.status = "error";
    result.message = error;
    return result;
  }
  const auto sets = group_archives(files);
  std::vector<ArchiveSet> zips;
  std::vector<ArchiveSet> rars;
  bool other = false;
  for (const auto& set : sets) {
    if (set.format == "zip") zips.push_back(set);
    else if (set.format == "rar") rars.push_back(set);
    else other = true;
  }
  if (zips.empty() && rars.empty()) {
    result.status = "error";
    result.message = other ? "还不能安装 7z" : "这个目录里没有压缩包";
    return result;
  }

  std::vector<std::vector<Part>> opened(zips.size());
  std::vector<Member> members;
  std::set<std::string> names;
  for (std::size_t i = 0; i < zips.size(); ++i) {
    if (auto open_error = open_parts(drive, zips[i].parts, &opened[i]); !open_error.empty()) {
      result.status = "error";
      result.message = open_error;
      return result;
    }
    VolumeSource source(&drive, opened[i]);
    if (auto scan_error = scan_zip(source, static_cast<int>(i), &members); !scan_error.empty()) {
      result.status = "error";
      result.message = scan_error;
      return result;
    }
  }
  std::vector<std::vector<ListedRarEntry>> rar_entries(rars.size());
  for (std::size_t i = 0; i < rars.size(); ++i) {
    if (auto list_error = list_rar_entries(drive, rars[i].parts, &rar_entries[i]); !list_error.empty()) {
      result.status = "error";
      result.message = list_error;
      return result;
    }
    for (const auto& entry : rar_entries[i]) {
      if (entry.size > 0xffffffffu) {
        result.status = "error";
        result.message = "有文件超过 4GB，这一版还不能安装";
        return result;
      }
      Member member;
      member.set_index = -1;
      member.name = entry.name;
      member.directory = entry.directory;
      member.crc = entry.crc;
      member.uncomp_size = entry.size;
      members.push_back(std::move(member));
    }
  }
  for (const auto& member : members) {
    if (member.directory) continue;
    if (!names.insert(member.name).second) {
      result.status = "error";
      result.message = "压缩包里有重复文件";
      return result;
    }
    ++result.files_total;
  }

  const std::string staging_root = join_storage_path(request.dest_prefix, ".panplay-staging/" + folder);
  const std::string final_root = join_storage_path(request.dest_prefix, folder);
  const std::string marker_path = join_storage_path(final_root, ".panplay-done.json");
  const std::string journal_path = join_storage_path(staging_root, ".panplay-journal.json");
  const std::string expected = marker_json(title_id, drive.id(), members);

  std::vector<std::uint8_t> existing;
  error = sink.read_file(marker_path, &existing, 1 << 20);
  if (error.empty()) {
    if (std::string(existing.begin(), existing.end()) == expected) {
      clean_staging(sink, staging_root, journal_path, members);
      result.status = "already";
      result.message = "已安装 " + folder;
      result.files_done = result.files_total;
      return result;
    }
  } else if (error != "missing") {
    result.status = "error";
    result.message = error;
    return result;
  }

  std::vector<DoneFile> done;
  error = load_journal(sink, journal_path, &done);
  if (!error.empty()) {
    result.status = "error";
    result.message = error;
    return result;
  }

  // 日志里可能留着旧版本写入、现在已不属于文件清单的条目，只数当前清单里的文件。
  const auto count_done = [&](const auto& list) {
    int count = 0;
    for (const auto& file : list) {
      if (names.count(file.name) != 0) ++count;
    }
    return count;
  };
  result.files_done = count_done(done);
  report("正在安装", result.files_done, result.files_total);

  std::int64_t written = 0;
  for (std::size_t i = 0; i < zips.size(); ++i) {
    VolumeSource source(&drive, opened[i]);
    for (const auto& member : members) {
      if (member.set_index != static_cast<int>(i)) continue;
      error = extract_member(source, sink, member, staging_root, &done, journal_path, &written, request.stop_after_output_bytes);
      if (error == kInterrupted) {
        result.status = "interrupted";
        result.files_done = count_done(done);
        result.message = "已中断，已完成 " + std::to_string(result.files_done) + " 个文件，再次执行安装会继续";
        return result;
      }
      if (!error.empty()) {
        result.status = "error";
        result.files_done = count_done(done);
        result.message = error;
        return result;
      }
      result.files_done = count_done(done);
      report("正在安装", result.files_done, result.files_total);
    }
  }
  for (std::size_t i = 0; i < rars.size(); ++i) {
    std::vector<FinishedFile> finished;
    for (const auto& file : done) finished.push_back(FinishedFile{file.name, file.size, file.crc});
    error = extract_rar_entries(drive, sink, rars[i].parts, rar_entries[i], staging_root, &finished, journal_path,
                                &written, request.stop_after_output_bytes,
                                [&](const std::vector<FinishedFile>& now_done) {
                                  result.files_done = count_done(now_done);
                                  report("正在安装", result.files_done, result.files_total);
                                });
    if (error == kInterrupted) {
      result.status = "interrupted";
      result.files_done = count_done(finished);
      result.message = "已中断，已完成 " + std::to_string(result.files_done) + " 个文件，再次执行安装会继续";
      return result;
    }
    if (!error.empty()) {
      result.status = "error";
      result.message = error;
      return result;
    }
    done.clear();
    for (const auto& file : finished) done.push_back(DoneFile{file.name, file.size, file.crc});
  }

  error = finish_install(sink, staging_root, final_root, members, expected, marker_path);
  if (!error.empty()) {
    result.status = "error";
    result.message = error;
    return result;
  }
  clean_staging(sink, staging_root, journal_path, members);
  result.status = "installed";
  result.files_done = result.files_total;
  result.message = "已安装 " + folder + "，" + std::to_string(result.files_done) + " 个文件";
  return result;
}

bool parse_destination(const std::string& text, Destination* destination, std::string* error) {
  if (text.rfind("ftp://", 0) == 0) {
    destination->ftp = true;
    std::string rest = text.substr(6);
    const auto slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos || colon == 0) {
      *error = "FTP 地址需要写成 ftp://主机:端口";
      return false;
    }
    destination->endpoint.host = host_port.substr(0, colon);
    try {
      destination->endpoint.port = static_cast<std::uint16_t>(std::stoi(host_port.substr(colon + 1)));
    } catch (const std::exception&) {
      *error = "FTP 端口无效";
      return false;
    }
    return true;
  }
  destination->ftp = false;
  destination->local_root = text;
  return true;
}

std::unique_ptr<InstallSink> open_destination(const Destination& destination, std::string* error) {
  if (destination.ftp) return make_ftp_sink(destination.endpoint, error);
  return make_local_sink(destination.local_root);
}

int run_scan_cli(DrivePlugin& drive, const std::string& dir) {
  std::string error;
  const auto games = scan_library(drive, dir, &error);
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  if (games.empty()) {
    std::cout << "没有游戏目录\n";
    return 0;
  }
  for (const auto& game : games) {
    std::string formats;
    for (const auto& set : game.sets) {
      if (formats.find(set.format) == std::string::npos) {
        if (!formats.empty()) formats += "+";
        formats += set.format;
      }
    }
    if (formats.empty()) formats = "-";
    const std::string title = game.title_id.empty() ? "-" : game.title_id;
    std::cout << title << "\t" << game.name << "\t" << game.archive_bytes << "\t" << formats << "\n";
  }
  return 0;
}

int run_install_cli(DrivePlugin& drive, int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: panplay install <网盘游戏目录> --to <本机目录或 ftp://主机:端口> [--prefix /mnt/usb0/homebrew]\n";
    return 1;
  }
  std::string to;
  std::string prefix = "/mnt/usb0/homebrew";
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--to" && i + 1 < argc) {
      to = argv[++i];
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else {
      std::cerr << "无法识别的参数\n";
      return 1;
    }
  }
  if (to.empty()) {
    std::cerr << "需要 --to\n";
    return 1;
  }
  Destination destination;
  std::string error;
  if (!parse_destination(to, &destination, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  destination.prefix = prefix;
  auto sink = open_destination(destination, &error);
  if (!sink) {
    std::cerr << error << "\n";
    return 1;
  }
  InstallRequest request;
  request.source_dir = argv[2];
  request.dest_prefix = prefix;
  const InstallResult result = install_game(drive, *sink, request);
  if (result.status == "error") {
    std::cerr << result.message << "\n";
    return 1;
  }
  std::cout << result.message << "\n";
  return result.status == "interrupted" ? 2 : 0;
}

}  // namespace panplay
