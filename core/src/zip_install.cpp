#include "panplay/install.hpp"

#include "install_clock.hpp"
#include "install_common.hpp"
#include "panplay/json.hpp"
#include "panplay/library.hpp"
#include "panplay/prefetch.hpp"
#include "panplay/trace.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace panplay {
namespace {

struct Member {
  int set_index = 0;
  std::string name;
  // 在暂存区里的路径：压缩包内原路径，多个压缩包时前面加 "<序号>/"。
  std::string staged;
  bool directory = false;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint64_t comp_size = 0;
  std::uint64_t uncomp_size = 0;
  // 本地文件头在整组拼接后的位置。
  std::int64_t local_offset = 0;
  // 在 7z 文件列表里的序号。
  std::size_t seven_index = 0;
};

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t* data) {
  return static_cast<std::uint64_t>(read_u32(data)) | (static_cast<std::uint64_t>(read_u32(data + 4)) << 32);
}

std::string base_name(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndRecord = 0x06054b50;
constexpr std::uint32_t kZip64EndRecord = 0x06064b50;
constexpr std::uint32_t kZip64Locator = 0x07064b50;

struct CentralDirectory {
  std::uint64_t disk = 0;
  std::uint64_t cd_disk = 0;
  std::uint64_t entries = 0;
  std::uint64_t size = 0;
  std::uint64_t offset = 0;
};

// 分卷 ZIP 的偏移按卷号分别算；单个 ZIP 和 .zip.001 这种直接切开的，卷号都是 0，偏移就是拼接后的位置。
std::string to_logical(const std::vector<ArchiveVolume>& volumes, std::uint64_t disk, std::uint64_t offset, std::int64_t* out) {
  if (disk >= volumes.size()) return "ZIP 记录的分卷号超出了分卷数";
  *out = volumes[static_cast<std::size_t>(disk)].start + static_cast<std::int64_t>(offset);
  return "";
}

std::string find_central_directory(DrivePlugin& drive, const std::vector<ArchiveVolume>& volumes, CentralDirectory* cd) {
  const ArchiveVolume& last = volumes.back();
  const std::size_t tail_size = static_cast<std::size_t>(std::min<std::int64_t>(last.size, 22 + 65535 + 20));
  const std::int64_t tail_start = last.start + last.size - static_cast<std::int64_t>(tail_size);
  std::vector<std::uint8_t> tail;
  if (auto error = read_span(drive, volumes, tail_start, tail_size, &tail); !error.empty()) return error;
  std::size_t at = std::string::npos;
  for (std::size_t i = tail.size() >= 22 ? tail.size() - 22 + 1 : 0; i-- > 0;) {
    if (read_u32(tail.data() + i) != kEndRecord) continue;
    if (i + 22 + read_u16(tail.data() + i + 20) != tail.size()) continue;
    at = i;
    break;
  }
  if (at == std::string::npos) return "找不到 ZIP 目录，压缩包可能不完整";
  const std::uint8_t* end = tail.data() + at;
  cd->disk = read_u16(end + 4);
  cd->cd_disk = read_u16(end + 6);
  cd->entries = read_u16(end + 10);
  cd->size = read_u32(end + 12);
  cd->offset = read_u32(end + 16);

  if (at >= 20 && read_u32(tail.data() + at - 20) == kZip64Locator) {
    const std::uint8_t* locator = tail.data() + at - 20;
    std::int64_t position = 0;
    if (auto error = to_logical(volumes, read_u32(locator + 4), read_u64(locator + 8), &position); !error.empty()) {
      return error;
    }
    std::vector<std::uint8_t> record;
    if (auto error = read_span(drive, volumes, position, 56, &record); !error.empty()) return error;
    if (read_u32(record.data()) != kZip64EndRecord) return "ZIP64 目录记录无法识别";
    cd->disk = read_u32(record.data() + 16);
    cd->cd_disk = read_u32(record.data() + 20);
    cd->entries = read_u64(record.data() + 32);
    cd->size = read_u64(record.data() + 40);
    cd->offset = read_u64(record.data() + 48);
  }
  if (cd->disk != 0 && cd->disk + 1 != volumes.size()) return "分卷数量和压缩包记录的不一致，可能缺了分卷";
  if (cd->size > (512u << 20)) return "ZIP 目录大得不正常";
  return "";
}

std::string read_zip_members(DrivePlugin& drive, const std::vector<ArchiveVolume>& volumes, int set_index,
                             std::vector<Member>* members) {
  CentralDirectory cd;
  if (auto error = find_central_directory(drive, volumes, &cd); !error.empty()) return error;
  std::int64_t position = 0;
  if (auto error = to_logical(volumes, cd.cd_disk, cd.offset, &position); !error.empty()) return error;
  std::vector<std::uint8_t> bytes;
  if (auto error = read_span(drive, volumes, position, static_cast<std::size_t>(cd.size), &bytes); !error.empty()) {
    return error;
  }
  std::size_t at = 0;
  for (std::uint64_t i = 0; i < cd.entries; ++i) {
    if (at + 46 > bytes.size() || read_u32(bytes.data() + at) != kCentralHeader) return "ZIP 目录无法识别";
    const std::uint8_t* header = bytes.data() + at;
    const std::uint16_t flags = read_u16(header + 8);
    const std::uint16_t method = read_u16(header + 10);
    Member member;
    member.set_index = set_index;
    member.method = method;
    member.crc = read_u32(header + 16);
    member.comp_size = read_u32(header + 20);
    member.uncomp_size = read_u32(header + 24);
    const std::uint16_t name_length = read_u16(header + 28);
    const std::uint16_t extra_length = read_u16(header + 30);
    const std::uint16_t comment_length = read_u16(header + 32);
    std::uint64_t disk = read_u16(header + 34);
    std::uint64_t local = read_u32(header + 42);
    const std::size_t record = 46u + name_length + extra_length + comment_length;
    if (at + record > bytes.size()) return "ZIP 目录无法识别";
    const std::string raw_name(reinterpret_cast<const char*>(header + 46), name_length);

    const std::uint8_t* extra = header + 46 + name_length;
    std::size_t e = 0;
    while (e + 4 <= extra_length) {
      const std::uint16_t id = read_u16(extra + e);
      const std::uint16_t length = read_u16(extra + e + 2);
      if (e + 4 + length > extra_length) break;
      if (id == 0x0001) {
        const std::uint8_t* field = extra + e + 4;
        std::size_t used = 0;
        auto take = [&](std::uint64_t* value) {
          if (used + 8 > length) return false;
          *value = read_u64(field + used);
          used += 8;
          return true;
        };
        if (member.uncomp_size == 0xffffffffu && !take(&member.uncomp_size)) return "ZIP64 字段不完整";
        if (member.comp_size == 0xffffffffu && !take(&member.comp_size)) return "ZIP64 字段不完整";
        if (local == 0xffffffffu && !take(&local)) return "ZIP64 字段不完整";
        if (disk == 0xffff) {
          if (used + 4 > length) return "ZIP64 字段不完整";
          disk = read_u32(field + used);
        }
      }
      e += 4u + length;
    }
    at += record;

    if ((flags & 1) != 0) return "ZIP 有密码，这一版不能安装";
    if (!safe_archive_name(raw_name, &member.name, &member.directory)) return "压缩包路径不安全";
    if (!member.directory && method != 0 && method != 8) return "ZIP 使用了不支持的压缩方法";
    if (auto error = to_logical(volumes, disk, local, &member.local_offset); !error.empty()) return error;
    members->push_back(std::move(member));
  }
  return "";
}

struct LayerRecord {
  std::string source;
  bool param = false;
};

// 安装标记里的 source 是网盘上各个压缩包文件的名字和大小。再次安装时只比对它，不用为了比对去读压缩包。
// layers 按叠放顺序记下每一层，出了新补丁时据此只叠新的部分。
std::string marker_json(const std::string& title_id, const std::string& plugin, const std::string& source,
                        const std::string& variant, const std::vector<LayerRecord>& layers, std::vector<Member> members) {
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
  json += "],\"layers\":[";
  for (std::size_t i = 0; i < layers.size(); ++i) {
    if (i > 0) json += ",";
    json += "{\"param\":" + std::string(layers[i].param ? "true" : "false") + ",\"source\":\"" +
            json_escape(layers[i].source) + "\"}";
  }
  json += "],\"plugin\":\"" + json_escape(plugin) + "\",\"source\":\"" + json_escape(source) + "\",\"title_id\":\"" +
          json_escape(title_id) + "\",\"variant\":\"" + json_escape(variant) + "\"}";
  return json;
}

std::string source_signature(const std::vector<ArchiveSet>& sets) { return archive_sets_signature(sets); }

// PS5 只认 homebrew/<游戏>/sce_sys 这一层。压缩包里游戏外面常包着一两层目录（如 PPSA05106-app0/），
// 找出 sce_sys 所在的那层作为游戏根目录，安装时去掉外层；没有 sce_sys 时去掉所有文件共有的外层目录。
// 返回的根目录带结尾斜杠，为空表示压缩包本身就是一层。has_param 表示根目录下有 param.json/param.sfo，即完整游戏而不是补丁。
std::string find_game_root(const std::vector<Member>& members, std::string* root, bool* has_param) {
  root->clear();
  *has_param = false;
  std::set<std::string> with_param;
  std::set<std::string> with_sys;
  for (const auto& member : members) {
    if (member.directory) continue;
    std::string lower = member.name;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (std::size_t at = 0; (at = lower.find("sce_sys/", at)) != std::string::npos; ++at) {
      if (at != 0 && lower[at - 1] != '/') continue;
      const std::string prefix = member.name.substr(0, at);
      with_sys.insert(prefix);
      const std::string rest = lower.substr(at + 8);
      if (rest == "param.json" || rest == "param.sfo") with_param.insert(prefix);
      break;
    }
  }
  const std::set<std::string>& candidates = with_param.empty() ? with_sys : with_param;
  if (!candidates.empty()) {
    const auto depth = [](const std::string& path) { return std::count(path.begin(), path.end(), '/'); };
    std::vector<std::string> shallowest;
    for (const auto& candidate : candidates) {
      if (!shallowest.empty() && depth(candidate) > depth(shallowest.front())) continue;
      if (!shallowest.empty() && depth(candidate) < depth(shallowest.front())) shallowest.clear();
      shallowest.push_back(candidate);
    }
    if (shallowest.size() > 1) {
      return "压缩包里有多个游戏目录（" + shallowest[0] + "、" + shallowest[1] + "），不知道该装哪个";
    }
    *root = shallowest.front();
    *has_param = !with_param.empty();
    return "";
  }
  while (true) {
    std::string top;
    bool shared = false;
    for (const auto& member : members) {
      if (member.directory) continue;
      const auto slash = member.name.find('/', root->size());
      if (slash == std::string::npos) return "";
      const std::string head = member.name.substr(0, slash + 1);
      if (!shared) {
        top = head;
        shared = true;
      } else if (head != top) {
        return "";
      }
    }
    if (!shared) return "";
    *root = top;
  }
}

// 换了目录结构重装时，删掉上次装进去、这次不再有的文件，以及因此变空的目录。
void remove_stale(InstallSink& sink, const std::string& final_root, const std::vector<std::uint8_t>& old_marker,
                  const std::vector<Member>& members) {
  std::string parse_error;
  const Json old = Json::parse(std::string(old_marker.begin(), old_marker.end()), &parse_error);
  const Json* entries = old.get("entries");
  if (!parse_error.empty() || entries == nullptr) return;
  std::set<std::string> current;
  for (const auto& member : members) current.insert(member.name);
  std::set<std::string> dirs;
  for (const auto& entry : entries->array) {
    const std::string name = entry.str("name");
    std::string normalized;
    bool directory = false;
    if (!safe_archive_name(name, &normalized, &directory) || directory || current.count(normalized) != 0) continue;
    sink.remove_file(join_storage_path(final_root, normalized));
    for (auto slash = normalized.rfind('/'); slash != std::string::npos; slash = normalized.rfind('/', slash - 1)) {
      dirs.insert(normalized.substr(0, slash));
      if (slash == 0) break;
    }
  }
  std::vector<std::string> ordered(dirs.begin(), dirs.end());
  std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
    return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/');
  });
  for (const auto& dir : ordered) sink.remove_dir(join_storage_path(final_root, dir));
}

struct ExtractContext {
  InstallSink& sink;
  std::string staging_root;
  JournalWriter& journal;
  std::int64_t* written;
  std::int64_t stop;
  const CancelToken* cancel;
  // 上次已校验过的暂存文件：暂存路径 -> 大小、CRC。
  std::map<std::string, std::pair<std::int64_t, std::uint32_t>> finished_before;
};

bool already_done(const ExtractContext& context, const Member& member) {
  const auto found = context.finished_before.find(member.staged);
  return found != context.finished_before.end() && found->second.first == static_cast<std::int64_t>(member.uncomp_size) &&
         found->second.second == member.crc;
}

bool cancelled(const CancelToken* cancel) { return cancel != nullptr && cancel->cancelled(); }

std::string copy_member(VolumeStream& stream, const Member& member, OutputFile& out, const CancelToken* cancel) {
  constexpr std::size_t kChunk = 64u << 10;
  std::vector<std::uint8_t> input(kChunk);
  std::uint64_t left = member.comp_size;
  if (member.method == 0) {
    while (left > 0) {
      if (cancelled(cancel)) return kInterrupted;
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, left));
      if (auto error = stream.read(input.data(), n); !error.empty()) return error;
      if (auto error = out.write(input.data(), n); !error.empty()) return error;
      left -= n;
    }
    return "";
  }
  if (left == 0) return "";
  z_stream inflater{};
  if (inflateInit2(&inflater, -MAX_WBITS) != Z_OK) return "无法开始解压";
  std::vector<std::uint8_t> output(kChunk);
  std::string error;
  int result = Z_OK;
  while (result != Z_STREAM_END && error.empty()) {
    if (cancelled(cancel)) {
      error = kInterrupted;
      break;
    }
    if (inflater.avail_in == 0) {
      if (left == 0) {
        error = "解压数据提前结束";
        break;
      }
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, left));
      error = stream.read(input.data(), n);
      if (!error.empty()) break;
      left -= n;
      inflater.next_in = input.data();
      inflater.avail_in = static_cast<uInt>(n);
    }
    inflater.next_out = output.data();
    inflater.avail_out = static_cast<uInt>(output.size());
    result = inflate(&inflater, Z_NO_FLUSH);
    const std::size_t produced = output.size() - inflater.avail_out;
    if (produced > 0) error = out.write(output.data(), produced);
    if (result != Z_OK && result != Z_STREAM_END && error.empty()) error = "解压失败";
  }
  inflateEnd(&inflater);
  return error;
}

std::string extract_member(VolumeStream& stream, ExtractContext& context, const Member& member, std::vector<FinishedFile>* done) {
  if (member.directory) return "";
  if (already_done(context, member)) return "";
  if (context.stop >= 0 && *context.written >= context.stop) return kInterrupted;
  if (cancelled(context.cancel)) return kInterrupted;

  if (auto error = stream.seek(member.local_offset); !error.empty()) return error;
  std::uint8_t header[30];
  if (auto error = stream.read(header, sizeof(header)); !error.empty()) return error;
  if (read_u32(header) != kLocalHeader) return "ZIP 文件头和目录对不上: " + member.name;
  if (auto error = stream.skip(read_u16(header + 26) + read_u16(header + 28)); !error.empty()) return error;

  OutputFile out(context.sink, join_storage_path(context.staging_root, member.staged),
                 static_cast<std::int64_t>(member.uncomp_size), context.stop, context.written);
  if (auto error = copy_member(stream, member, out, context.cancel); !error.empty()) return error;
  if (auto error = out.finish(); !error.empty()) return error;
  if (out.size() != static_cast<std::int64_t>(member.uncomp_size) || out.crc() != member.crc) {
    return "校验和不符: " + member.name;
  }
  const FinishedFile finished{member.staged, static_cast<std::int64_t>(member.uncomp_size), member.crc};
  done->push_back(finished);
  return context.journal.add(finished);
}

std::string finish_install(InstallSink& sink, const std::string& staging_root, const std::string& final_root,
                           const std::vector<Member>& members, const std::string& marker, const std::string& marker_path) {
  for (const auto& member : members) {
    if (member.directory) {
      if (auto error = sink.ensure_dir(join_storage_path(final_root, member.name)); !error.empty()) return error;
      continue;
    }
    const std::string from = join_storage_path(staging_root, member.staged);
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

// 搬完后暂存区只剩日志、没搬走的文件（游戏目录外的、被补丁覆盖的）和空目录。
// staged 是解压过的全部暂存路径，leftovers 是其中没搬走的文件。清理失败不影响安装结果，下次安装会再清一次。
void clean_staging(InstallSink& sink, const std::string& staging_root, const std::string& journal_path,
                   const std::vector<std::string>& staged, const std::vector<std::string>& leftovers) {
  for (const auto& file : leftovers) sink.remove_file(join_storage_path(staging_root, file));
  std::set<std::string> dirs;
  for (const auto& path : staged) {
    std::string name = path;
    while (!name.empty() && name.back() == '/') name.pop_back();
    {
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
  sink.remove_file(join_storage_path(staging_root, ".panplay-journal.json"));
  for (const auto& dir : ordered) sink.remove_dir(join_storage_path(staging_root, dir));
  sink.remove_dir(staging_root);
  sink.remove_dir(parent_path(staging_root));
}

std::string variant_label(const std::string& variant) {
  if (variant == "exfat") return " exFAT 镜像版";
  if (variant == "folder") return "文件夹版";
  if (variant == "other") return "其他格式的版本";
  if (variant.empty()) return "没分格式的版本";
  return "「" + variant + "」版";
}

// 游戏在主机上的目录名：有 Title ID 用它，没有用网盘目录名。不能用时返回空。
// 同一款游戏的几种格式装到同一个文件夹，同时只能装一种。
std::string install_folder(const std::string& source_dir) {
  const std::string folder_name = base_name(game_dir_of(source_dir));
  const std::string title_id = find_title_id(folder_name);
  const std::string folder = title_id.empty() ? folder_name : title_id;
  if (folder.empty() || folder == "." || folder == ".." || folder.find(':') != std::string::npos) return "";
  return folder;
}

}  // namespace

namespace {

bool read_marker(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir, Json* marker) {
  const std::string folder = install_folder(source_dir);
  if (folder.empty()) return false;
  std::vector<std::uint8_t> bytes;
  if (!sink.read_file(join_storage_path(join_storage_path(dest_prefix, folder), ".panplay-done.json"), &bytes, 256u << 20)
           .empty()) {
    return false;
  }
  std::string parse_error;
  *marker = Json::parse(std::string(bytes.begin(), bytes.end()), &parse_error);
  return parse_error.empty();
}

// 完成标记里记的格式。旧版本没记、或者装的是没分格式的游戏目录时为空 optional，和哪种格式都对得上：
// 盘架把旧目录迁进 @格式 子目录后，装好的游戏不用重装。
std::optional<std::string> marker_variant(const Json& marker) {
  const std::string variant = marker.str("variant");
  if (variant.empty()) return std::nullopt;
  return variant;
}

}  // namespace

bool install_outdated(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir,
                      const std::vector<ArchiveSet>& sets, const std::set<std::string>& skip_sets) {
  Json marker;
  if (!read_marker(sink, dest_prefix, source_dir, &marker)) return false;
  if (const auto variant = marker_variant(marker); variant && *variant != variant_format(source_dir)) return false;
  std::vector<ArchiveSet> installable;
  for (const auto& set : sets) {
    if ((set.format == "zip" || set.format == "rar" || set.format == "7z") && skip_sets.count(archive_set_id(set)) == 0) {
      installable.push_back(set);
    }
  }
  return !installable.empty() && marker.str("source") != source_signature(installable);
}

std::vector<std::string> installed_sets(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir) {
  std::vector<std::string> ids;
  Json marker;
  if (!read_marker(sink, dest_prefix, source_dir, &marker)) return ids;
  const Json* layers = marker.get("layers");
  if (layers == nullptr) return ids;
  for (const auto& layer : layers->array) ids.push_back(layer.str("source"));
  return ids;
}

std::optional<std::string> installed_variant(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir) {
  Json marker;
  if (!read_marker(sink, dest_prefix, source_dir, &marker)) return std::nullopt;
  return marker_variant(marker);
}

std::string discard_install(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir) {
  const std::string folder = install_folder(source_dir);
  if (folder.empty()) return "游戏目录名不能作为存放目录";
  const std::string staging_parent = join_storage_path(dest_prefix, ".panplay-staging");
  if (auto error = sink.remove_tree(join_storage_path(staging_parent, folder)); !error.empty()) return error;
  sink.remove_dir(staging_parent);
  return "";
}

std::string uninstall_game(InstallSink& sink, const std::string& dest_prefix, const std::string& source_dir) {
  const std::string folder = install_folder(source_dir);
  if (folder.empty() || folder.rfind(".panplay", 0) == 0) return "游戏目录名不能作为存放目录";
  if (auto error = discard_install(sink, dest_prefix, source_dir); !error.empty()) return error;
  return sink.remove_tree(join_storage_path(dest_prefix, folder));
}

std::string install_folder_name(const std::string& source_dir) { return install_folder(source_dir); }

namespace {

InstallResult install_layers(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request) {
  InstallResult result;
  const std::string folder = install_folder(request.source_dir);
  const std::string title_id = find_title_id(base_name(game_dir_of(request.source_dir)));
  const std::string variant = variant_format(request.source_dir);
  if (folder.empty()) {
    result.status = "error";
    result.message = "游戏目录名不能作为存放目录";
    return result;
  }

  const auto report = [&](const std::string& message) {
    if (!request.on_progress) return;
    InstallResult snapshot;
    snapshot.message = message;
    snapshot.files_done = result.files_done;
    snapshot.files_total = result.files_total;
    snapshot.bytes_done = result.bytes_done;
    snapshot.bytes_total = result.bytes_total;
    snapshot.files_checked = result.files_checked;
    snapshot.files_to_check = result.files_to_check;
    request.on_progress(snapshot);
  };
  const auto fail = [&](const std::string& message) {
    result.status = message == kInterrupted ? "interrupted" : "error";
    result.message = message == kInterrupted ? "已中断，再次执行安装会继续" : message;
    result.retryable = !is_install_package_error(message) && !is_unsupported_7z_error(message);
    result.auto_resume = result.retryable && message != kInterrupted && !is_dictionary_refusal(message) &&
                         !is_out_of_memory_error(message) && !is_disk_full_error(message) &&
                         message.find("未登录") == std::string::npos;
    return result;
  };
  const auto interrupted = [&]() {
    result.status = "interrupted";
    result.message = "已中断，已完成 " + std::to_string(result.files_done) + " 个文件，再次执行安装会继续";
    return result;
  };
  report("正在读取压缩包…");

  std::string error;
  const auto files = collect_archives(drive, request.source_dir, &error);
  if (!error.empty()) return fail(error);
  const auto sets = group_archives(files);
  std::vector<ArchiveSet> zips;
  std::vector<ArchiveSet> rars;
  std::vector<ArchiveSet> sevens;
  for (const auto& set : sets) {
    if (set.format == "zip") zips.push_back(set);
    else if (set.format == "rar") rars.push_back(set);
    else if (set.format == "7z") sevens.push_back(set);
  }
  if (zips.empty() && rars.empty() && sevens.empty()) return fail("这个目录里没有压缩包");

  // 每个压缩包（可能分卷）各自原样解压到暂存区，最后按各自的游戏根目录合并。
  // 多个压缩包时各占暂存区里的 "<序号>/" 子目录，免得同名文件互相覆盖。
  struct Source {
    bool rar = false;
    const ArchiveSet* set = nullptr;
    std::vector<ArchiveVolume> volumes;
    std::vector<Member> members;
    std::string prefix;
    std::int64_t start = 0;
    std::string root;
    bool has_param = false;
    // 压缩包在游戏目录下的哪个子目录里，直接放在游戏目录里时为空。
    std::string folder;
    // 这一层的身份：各分卷的文件名和大小。完成标记按它记下装过哪些层。
    std::string id;
    bool extracted = false;
    bool sevenzip = false;
    SevenZipArchive seven;
  };
  std::vector<Source> sources;
  for (const auto& set : zips) sources.push_back(Source{false, &set});
  for (const auto& set : rars) sources.push_back(Source{true, &set});
  for (const auto& set : sevens) {
    sources.push_back(Source{false, &set});
    sources.back().sevenzip = true;
  }
  // 暂存区序号按全部压缩包编，改了要装哪些之后，没跳过的仍落在原来的子目录里。
  for (std::size_t i = 0; i < sources.size(); ++i) {
    sources[i].folder = archive_folder(request.source_dir, sources[i].set->parts.front().path);
    sources[i].id = archive_set_id(*sources[i].set);
    sources[i].prefix = sources.size() > 1 ? std::to_string(i + 1) + "/" : "";
  }
  sources.erase(std::remove_if(sources.begin(), sources.end(),
                               [&](const Source& source) { return request.skip_sets.count(source.id) > 0; }),
                sources.end());
  if (sources.empty()) return fail("没有选中要安装的压缩包");
  for (std::size_t i = 0; i < sources.size(); ++i) {
    Source& source = sources[i];
    if (source.rar) continue;
    source.volumes = archive_volumes(source.set->parts);
    if (source.sevenzip) {
      if (auto seven_error = read_7z_archive(drive, source.set->parts, &source.seven, request.max_dictionary);
          !seven_error.empty()) {
        return fail(seven_error);
      }
      for (std::size_t k = 0; k < source.seven.files.size(); ++k) {
        const SevenZipFile& file = source.seven.files[k];
        if (file.link) continue;
        Member member;
        member.set_index = static_cast<int>(i);
        member.seven_index = k;
        member.name = file.name;
        member.directory = file.directory;
        member.crc = file.crc;
        member.uncomp_size = file.size;
        source.members.push_back(std::move(member));
      }
    } else if (auto zip_error = read_zip_members(drive, source.volumes, static_cast<int>(i), &source.members);
               !zip_error.empty()) {
      return fail(zip_error);
    }
    for (auto& member : source.members) member.staged = source.prefix + member.name;
    const bool any_sys = std::any_of(source.members.begin(), source.members.end(),
                                     [](const Member& member) { return in_sce_sys(member.name); });
    for (const auto& member : source.members) {
      if (!any_sys && !member.directory && is_install_package(member.name, static_cast<std::int64_t>(member.uncomp_size))) {
        result.install_package_set = source.id;
        return fail(install_package_error(member.name));
      }
    }
    if (auto root_error = find_game_root(source.members, &source.root, &source.has_param); !root_error.empty()) {
      return fail(root_error);
    }
  }
  std::vector<ArchiveSet> archives;
  for (const auto& source : sources) archives.push_back(*source.set);
  const std::string signature = source_signature(archives);

  const std::string staging_root = join_storage_path(request.dest_prefix, ".panplay-staging/" + folder);
  const std::string final_root = join_storage_path(request.dest_prefix, folder);
  const std::string marker_path = join_storage_path(final_root, ".panplay-done.json");
  const std::string journal_path = join_storage_path(staging_root, ".panplay-journal.jsonl");

  std::vector<std::uint8_t> existing;
  // 已装的各层（按叠放顺序）和它们带不带 param。旧版本写的标记没有这项。
  std::vector<std::string> old_layers;
  std::map<std::string, bool> old_param;
  std::vector<Member> old_entries;
  error = sink.read_file(marker_path, &existing, 256u << 20);
  if (error.empty()) {
    std::string parse_error;
    const Json old = Json::parse(std::string(existing.begin(), existing.end()), &parse_error);
    const Json* entries = old.get("entries");
    const Json* layers = old.get("layers");
    const auto old_variant = parse_error.empty() ? marker_variant(old) : std::nullopt;
    if (old_variant && *old_variant != variant) {
      result.retryable = false;
      result.status = "error";
      result.message = "这款游戏已经装了" + variant_label(*old_variant) + "，要换成" + variant_label(variant) +
                       "得先删除已装的";
      return result;
    }
    if (parse_error.empty() && entries != nullptr && layers != nullptr && old.str("plugin") == drive.id()) {
      for (const auto& layer : layers->array) {
        old_layers.push_back(layer.str("source"));
        old_param[layer.str("source")] = layer.boolean_or("param", false);
      }
      for (const auto& entry : entries->array) {
        Member member;
        bool directory = false;
        if (!safe_archive_name(entry.str("name"), &member.name, &directory) || directory) continue;
        try {
          member.crc = static_cast<std::uint32_t>(std::stoul(entry.str("crc32")));
          member.uncomp_size = std::stoull(entry.str("size"));
        } catch (const std::exception&) {
          old_layers.clear();
          break;
        }
        old_entries.push_back(std::move(member));
      }
    }
    if (parse_error.empty() && entries != nullptr && old.str("source") == signature && old.str("plugin") == drive.id()) {
      sink.remove_file(journal_path);
      sink.remove_dir(staging_root);
      sink.remove_dir(parent_path(staging_root));
      result.status = "already";
      result.message = "已安装 " + folder;
      result.files_total = static_cast<int>(entries->array.size());
      result.files_done = result.files_total;
      result.bytes_done = result.bytes_total;
      return result;
    }
  } else if (error != "missing") {
    return fail(error);
  }

  std::vector<std::string> done_dirs;
  std::vector<FinishedFile> done = load_journal(sink, journal_path, &done_dirs);
  error.clear();
  if (sink.file_size(journal_path, &error) >= 0) {
    if (auto write_error = write_journal(sink, journal_path, done, done_dirs); !write_error.empty()) return fail(write_error);
  }
  result.files_done = static_cast<int>(done.size());
  result.files_to_check = static_cast<int>(done.size());
  // 续装时先顺着压缩包把上次装好的文件逐个核对、跳过，这段时间显示“正在核对”。
  const char* phase = done.empty() ? "正在安装" : "正在核对";
  const auto enter_entry = [&](bool already) {
    if (already) ++result.files_checked;
    phase = already ? "正在核对" : "正在安装";
    report(phase);
  };
  report(phase);

  std::int64_t written = 0;
  JournalWriter journal(sink, journal_path);
  ExtractContext context{sink, staging_root, journal, &written, request.stop_after_output_bytes, request.cancel, {}};
  for (const auto& file : done) context.finished_before[file.name] = {file.size, file.crc};
  // 解压一层到暂存区。进度按要解压的压缩包字节算，所以开始解压时才把这层的大小加进总数。
  const auto extract = [&](Source& source) -> std::string {
    PhaseScope decoding(InstallPhase::decode);
    InstallClock::note_dictionary(0);
    source.extracted = true;
    source.start = result.bytes_total;
    for (const auto& part : source.set->parts) result.bytes_total += part.size;
    log_info(std::string("解压 ") + (source.rar ? "RAR" : source.sevenzip ? "7z" : "ZIP") + " " + source.set->parts.front().name +
          "，" + std::to_string(source.set->parts.size()) + " 卷共 " +
          std::to_string((result.bytes_total - source.start) >> 20) + " MB，预读 " +
          std::to_string(request.prefetch.connections) + " 条连接 × " + std::to_string(request.prefetch.block_size >> 20) +
          " MB，" + memory_summary());
    if (source.rar) {
      RarExtract options;
      options.staging_root = staging_root;
      options.journal = &journal;
      options.written = &written;
      options.stop_after_output_bytes = request.stop_after_output_bytes;
      options.cancel = request.cancel;
      options.prefetch = request.prefetch;
      options.staging_prefix = source.prefix;
      options.max_dictionary = request.max_dictionary;
      options.finished_dirs = &done_dirs;
      options.on_file_done = [&](const std::vector<FinishedFile>& now_done) {
        result.files_done = static_cast<int>(now_done.size());
        report(phase);
      };
      options.on_bytes = [&](std::int64_t bytes) {
        result.bytes_done = source.start + bytes;
        report(phase);
      };
      options.on_entry = enter_entry;
      std::vector<ListedRarEntry> listed;
      const std::string rar_error = extract_rar_entries(drive, sink, source.set->parts, options, &done, &listed);
      result.files_done = static_cast<int>(done.size());
      if (is_install_package_error(rar_error)) result.install_package_set = source.id;
      if (!rar_error.empty()) return rar_error;
      for (const auto& entry : listed) {
        Member member;
        member.set_index = -1;
        member.name = entry.name;
        member.staged = source.prefix + entry.name;
        member.directory = entry.directory;
        member.crc = entry.crc;
        member.uncomp_size = entry.size;
        source.members.push_back(std::move(member));
      }
      return find_game_root(source.members, &source.root, &source.has_param);
    }
    if (source.sevenzip) {
      SevenZipExtract options;
      options.staging_root = staging_root;
      options.journal = &journal;
      options.written = &written;
      options.stop_after_output_bytes = request.stop_after_output_bytes;
      options.cancel = request.cancel;
      options.prefetch = request.prefetch;
      options.targets.resize(source.seven.files.size());
      for (const auto& member : source.members) {
        if (member.directory || member.name.compare(0, source.root.size(), source.root) != 0) continue;
        options.targets[member.seven_index] = member.staged;
      }
      options.finished_before = &context.finished_before;
      options.on_file_done = [&](const std::vector<FinishedFile>& now_done) {
        result.files_done = static_cast<int>(now_done.size());
        report(phase);
      };
      options.on_bytes = [&](std::int64_t bytes) {
        result.bytes_done = source.start + bytes;
        report(phase);
      };
      options.on_entry = enter_entry;
      const std::string seven_error = extract_7z_entries(drive, sink, source.set->parts, source.seven, options, &done);
      result.files_done = static_cast<int>(done.size());
      return seven_error;
    }
    std::vector<const Member*> ordered;
    for (const auto& member : source.members) {
      if (member.name.compare(0, source.root.size(), source.root) == 0) ordered.push_back(&member);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Member* left, const Member* right) { return left->local_offset < right->local_offset; });
    VolumeStream stream(drive, source.volumes, request.cancel, request.prefetch);
    for (const Member* member : ordered) {
      if (!member->directory) enter_entry(already_done(context, *member));
      const std::string zip_error = extract_member(stream, context, *member, &done);
      result.files_done = static_cast<int>(done.size());
      if (!zip_error.empty()) return zip_error;
      result.bytes_done = source.start + member->local_offset + static_cast<std::int64_t>(member->comp_size);
      report(phase);
    }
    return "";
  };
  const auto extract_or_stop = [&](Source& source) -> std::optional<InstallResult> {
    const std::string extract_error = extract(source);
    if (extract_error == kInterrupted) return interrupted();
    if (!extract_error.empty()) return fail(extract_error);
    return std::nullopt;
  };

  // 已装过的层带不带 param 记在完成标记里，不用为了排顺序再解压一遍（RAR 要解压完才知道）。
  // 第一轮只解压新的层。
  for (auto& source : sources) {
    const auto known = old_param.find(source.id);
    if (!old_layers.empty() && known != old_param.end()) {
      if (source.rar) source.has_param = known->second;
      continue;
    }
    if (auto stop = extract_or_stop(source)) return *stop;
  }

  // 叠放顺序（规则见 docs/game-folder-guide.md）：带 param 的（游戏本体、正式补丁）在前，不带的（固件补丁）在后；
  // 同类里游戏目录里直接放的在前，子目录里的按子目录名排；同一处的按文件名排。名字里的数字按大小比。
  std::vector<Source*> layers;
  for (auto& source : sources) layers.push_back(&source);
  std::stable_sort(layers.begin(), layers.end(), [](const Source* left, const Source* right) {
    if (left->has_param != right->has_param) return left->has_param;
    if (left->folder.empty() != right->folder.empty()) return left->folder.empty();
    if (const int order = natural_compare(left->folder, right->folder); order != 0) return order < 0;
    return natural_compare(left->set->parts.front().name, right->set->parts.front().name) < 0;
  });

  // 已装好的游戏只多了补丁：已装的各层在新顺序里原样都在，就保留前面没变的那几层，从第一个新层起往后重新叠。
  // 后面原有的层（如固件补丁）也要重叠，因为新补丁可能盖掉了它改过的文件。有层被删或换了就整个重装。
  std::size_t keep = 0;
  if (!old_layers.empty()) {
    std::size_t matched = 0;
    for (const Source* layer : layers) {
      if (matched < old_layers.size() && layer->id == old_layers[matched]) ++matched;
    }
    if (matched == old_layers.size()) {
      while (keep < old_layers.size() && layers[keep]->id == old_layers[keep]) ++keep;
    }
  }
  const bool patch_only = keep > 0;
  for (std::size_t i = keep; i < layers.size(); ++i) {
    if (layers[i]->extracted) continue;
    if (auto stop = extract_or_stop(*layers[i])) return *stop;
  }

  // 收尾会删掉日志，所以先把攒着的写完，免得析构时又把日志写回暂存区。
  if (auto flush_error = journal.flush(); !flush_error.empty()) return fail(flush_error);

  std::vector<Member> members;
  std::map<std::string, std::pair<std::size_t, const Source*>> placed;
  std::vector<std::string> staged;
  std::vector<std::string> leftovers;
  int outside = 0;
  for (std::size_t i = keep; i < layers.size(); ++i) {
    const Source* source = layers[i];
    for (const auto& member : source->members) {
      if (!member.directory) staged.push_back(member.staged);
      if (member.name.compare(0, source->root.size(), source->root) != 0) {
        if (member.directory) continue;
        ++outside;
        if (source->rar) leftovers.push_back(member.staged);
        continue;
      }
      Member final_member = member;
      final_member.name.erase(0, source->root.size());
      if (final_member.name.empty()) continue;
      const auto found = placed.find(final_member.name);
      if (found == placed.end()) {
        placed[final_member.name] = {members.size(), source};
        members.push_back(std::move(final_member));
        continue;
      }
      Member& earlier = members[found->second.first];
      if (earlier.directory && final_member.directory) continue;
      // 资源站往每个压缩包里塞的同一份说明文件之类，内容一样，留一份就行。
      if (!earlier.directory && !final_member.directory && earlier.uncomp_size == final_member.uncomp_size &&
          earlier.crc == final_member.crc) {
        leftovers.push_back(final_member.staged);
        continue;
      }
      // 后叠的覆盖先叠的。只有同一处的两个固件补丁之间分不出先后，这时报错。
      const Source* below = found->second.second;
      const bool ordered = below != source && (below->has_param || below->folder != source->folder);
      if (earlier.directory != final_member.directory || !ordered) {
        return fail("压缩包里有重复文件（" + final_member.name + "），分不出哪个该覆盖哪个");
      }
      leftovers.push_back(earlier.staged);
      earlier = std::move(final_member);
      found->second.second = source;
    }
  }
  result.files_total = 0;
  for (const auto& member : members) {
    if (!member.directory) ++result.files_total;
  }
  result.bytes_done = result.bytes_total;
  report("正在收尾");

  // 只装补丁时，完成标记里的文件清单是原有的加上这次叠上去的。
  std::vector<Member> recorded = members;
  if (patch_only) {
    for (const auto& entry : old_entries) {
      if (placed.count(entry.name) == 0) recorded.push_back(entry);
    }
  }
  std::vector<LayerRecord> layer_records;
  for (const Source* layer : layers) layer_records.push_back({layer->id, layer->has_param});
  const std::string expected = marker_json(title_id, drive.id(), signature, variant, layer_records, recorded);
  error = finish_install(sink, staging_root, final_root, members, expected, marker_path);
  if (!error.empty()) return fail(error);
  if (!existing.empty() && !patch_only) remove_stale(sink, final_root, existing, members);
  clean_staging(sink, staging_root, journal_path, staged, leftovers);
  result.status = "installed";
  result.files_done = result.files_total;
  result.message = (patch_only ? "已更新 " : "已安装 ") + folder + "，" +
                   (patch_only ? "叠上 " + std::to_string(layers.size() - keep) + " 层、写入 " : std::string()) +
                   std::to_string(result.files_done) + " 个文件";
  if (outside > 0) result.message += "（游戏目录外的 " + std::to_string(outside) + " 个文件没有装）";
  return result;
}

}  // namespace

// 三种格式、本机和 FTP 都在这里统一记安装时间花在哪，结束时写一行总账。
InstallResult install_game(DrivePlugin& drive, InstallSink& sink, const InstallRequest& request) {
  InstallClock clock;
  const auto timed = timed_sink(sink);
  InstallRequest paced = request;
  if (request.on_progress) {
    paced.on_progress = [&](const InstallResult& snapshot) {
      InstallResult with_pace = snapshot;
      with_pace.dictionary = clock.dictionary();
      with_pace.slow = clock.slow();
      request.on_progress(with_pace);
    };
  }
  const InstallResult result = install_layers(drive, *timed, paced);
  const std::string summary = clock.summary(base_name(request.source_dir), result.status == "installed"     ? "已装好"
                                                                         : result.status == "interrupted" ? "中断"
                                                                                                          : "出错");
  if (!summary.empty()) log_info(summary);
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
  const std::filesystem::path root = std::filesystem::u8path(destination.local_root);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (!std::filesystem::is_directory(root, ec)) {
    *error = "存放目录 " + destination.local_root + " 不存在且无法创建，请在设置里换一个目录";
    return nullptr;
  }
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
    std::cerr << "用法: panplay install <网盘游戏目录> --to <本机目录或 ftp://主机:端口> [--prefix /data/homebrew]"
                 " [--connections N] [--block-mb M] [--blocks K]\n";
    return 1;
  }
  std::string to;
  std::string prefix = "/data/homebrew";
  PrefetchOptions prefetch;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--to" && i + 1 < argc) {
      to = argv[++i];
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else if (arg == "--connections" && i + 1 < argc) {
      prefetch.connections = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--block-mb" && i + 1 < argc) {
      prefetch.block_size = static_cast<std::size_t>(std::max(1, std::atoi(argv[++i]))) << 20;
    } else if (arg == "--blocks" && i + 1 < argc) {
      prefetch.max_blocks = static_cast<std::size_t>(std::max(1, std::atoi(argv[++i])));
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
  if (prefetch.max_blocks < static_cast<std::size_t>(prefetch.connections) * 2) {
    prefetch.max_blocks = static_cast<std::size_t>(prefetch.connections) * 2;
  }
  request.prefetch = prefetch;
  const InstallResult result = install_game(drive, *sink, request);
  if (result.status == "error") {
    std::cerr << result.message << "\n";
    return 1;
  }
  std::cout << result.message << "\n";
  return result.status == "interrupted" ? 2 : 0;
}

}  // namespace panplay
