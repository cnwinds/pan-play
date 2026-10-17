#include "panplay/install.hpp"
#include "panplay/trace.hpp"

#include "install_common.hpp"

#include "7z.h"
#include "7zCrc.h"
#include "Bra.h"
#include "Delta.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace panplay {
namespace {

constexpr std::uint32_t kCopy = 0;
constexpr std::uint32_t kLzma = 0x30101;
constexpr std::uint32_t kLzma2 = 0x21;
constexpr std::uint32_t kPpmd = 0x30401;
constexpr std::uint32_t kBcj2 = 0x303011B;
constexpr std::uint32_t kAes = 0x6F10701;
constexpr std::uint32_t kDelta = 3;
constexpr std::uint32_t kBcj = 0x3030103;
constexpr std::uint32_t kPpc = 0x3030205;
constexpr std::uint32_t kIa64 = 0x3030401;
constexpr std::uint32_t kArm = 0x3030501;
constexpr std::uint32_t kArmt = 0x3030701;
constexpr std::uint32_t kSparc = 0x3030805;
constexpr std::uint32_t kArm64 = 0xA;
constexpr std::uint32_t kRiscv = 0xB;

constexpr const char kRepack[] = "；请用 7-Zip 默认设置重新压缩，或换成 ZIP / RAR";

void* sz_alloc(ISzAllocPtr, size_t size) { return size == 0 ? nullptr : std::malloc(size); }
void sz_free(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kAlloc = {sz_alloc, sz_free};

void init_crc_table() {
  static std::once_flag once;
  std::call_once(once, [] { CrcGenerateTable(); });
}

std::uint32_t read_le32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::string hex(std::uint32_t value) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  do {
    out.insert(out.begin(), digits[value & 15]);
    value >>= 4;
  } while (value != 0);
  return "0x" + out;
}

bool is_main_method(std::uint32_t id) { return id == kCopy || id == kLzma || id == kLzma2; }

bool is_filter(std::uint32_t id) {
  switch (id) {
    case kDelta:
    case kBcj:
    case kPpc:
    case kIa64:
    case kArm:
    case kArmt:
    case kSparc:
    case kArm64:
    case kRiscv:
      return true;
    default:
      return false;
  }
}

std::uint64_t lzma2_dictionary(unsigned prop) {
  if (prop >= 40) return 0xFFFFFFFFull;
  return static_cast<std::uint64_t>(2 | (prop & 1)) << (prop / 2 + 11);
}

// 解这个块真正要用的字典：不会比解出来的数据还大。
std::uint64_t needed_dictionary(const SevenZipFolder& folder) {
  std::uint64_t dictionary = 0;
  if (folder.method == kLzma) dictionary = read_le32(folder.props.data() + 1);
  if (folder.method == kLzma2) dictionary = lzma2_dictionary(folder.props[0]);
  return std::min<std::uint64_t>(dictionary, folder.unpack_size);
}

std::string unsupported_method(std::uint32_t id) {
  if (id == kAes) return "这个 7z 加了密码，暂不支持" + std::string(kRepack);
  if (id == kBcj2) return "这个 7z 用了 BCJ2 过滤器，暂不支持" + std::string(kRepack);
  if (id == kPpmd) return "这个 7z 用了 PPMd 压缩，暂不支持" + std::string(kRepack);
  return "这个 7z 用了暂不支持的压缩方法（" + hex(id) + "）" + kRepack;
}

std::string open_error(SRes res) {
  switch (res) {
    case SZ_ERROR_NO_ARCHIVE:
      return "不是 7z 文件，或者开头已经损坏";
    case SZ_ERROR_UNSUPPORTED:
      return "这个 7z 的目录加了密码或用了暂不支持的方法" + std::string(kRepack);
    case SZ_ERROR_MEM:
      return "7z 目录太大，内存不够";
    case SZ_ERROR_INPUT_EOF:
      return "7z 压缩包不完整，可能缺了分卷";
    default:
      return "7z 目录损坏（错误码 " + std::to_string(res) + "）";
  }
}

// 给 SDK 读目录用的输入流：按 256 KB 一段随机读网盘，读过的一段缓存起来。
struct HeaderStream {
  ISeekInStream vt;
  DrivePlugin* drive = nullptr;
  const std::vector<ArchiveVolume>* volumes = nullptr;
  std::int64_t total = 0;
  std::int64_t position = 0;
  std::int64_t cache_start = 0;
  std::vector<std::uint8_t> cache;
  std::string error;
};

constexpr std::size_t kHeaderChunk = 256u << 10;

HeaderStream* header_stream(ISeekInStreamPtr p) {
  return reinterpret_cast<HeaderStream*>(const_cast<ISeekInStream*>(p));
}

SRes header_read(ISeekInStreamPtr p, void* buf, size_t* size) {
  HeaderStream* self = header_stream(p);
  if (*size == 0) return SZ_OK;
  if (self->position >= self->total) {
    *size = 0;
    return SZ_OK;
  }
  const bool cached = self->position >= self->cache_start &&
                      self->position < self->cache_start + static_cast<std::int64_t>(self->cache.size());
  if (!cached) {
    const auto want = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(std::max(*size, kHeaderChunk)), self->total - self->position));
    if (auto error = read_span(*self->drive, *self->volumes, self->position, want, &self->cache); !error.empty()) {
      self->error = error;
      self->cache.clear();
      *size = 0;
      return SZ_ERROR_READ;
    }
    self->cache_start = self->position;
  }
  const auto offset = static_cast<std::size_t>(self->position - self->cache_start);
  const std::size_t n = std::min(*size, self->cache.size() - offset);
  std::memcpy(buf, self->cache.data() + offset, n);
  self->position += static_cast<std::int64_t>(n);
  *size = n;
  return SZ_OK;
}

SRes header_seek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
  HeaderStream* self = header_stream(p);
  std::int64_t base = 0;
  if (origin == SZ_SEEK_CUR) base = self->position;
  if (origin == SZ_SEEK_END) base = self->total;
  const std::int64_t next = base + *pos;
  if (next < 0) return SZ_ERROR_PARAM;
  self->position = next;
  *pos = next;
  return SZ_OK;
}

void append_utf8(std::string* out, std::uint32_t code) {
  if (code < 0x80) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

std::string file_name(const CSzArEx& db, std::size_t index) {
  const std::size_t length = SzArEx_GetFileNameUtf16(&db, index, nullptr);
  std::vector<UInt16> text(length + 1, 0);
  SzArEx_GetFileNameUtf16(&db, index, text.data());
  std::string out;
  for (std::size_t i = 0; i < length && text[i] != 0; ++i) {
    std::uint32_t code = text[i];
    if (code >= 0xD800 && code < 0xDC00 && i + 1 < length && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
      code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      ++i;
    } else if (code >= 0xD800 && code < 0xE000) {
      code = 0xFFFD;
    }
    append_utf8(&out, code);
  }
  return out;
}

bool is_link(const CSzArEx& db, std::size_t index) {
  if (!SzBitWithVals_Check(&db.Attribs, index)) return false;
  const std::uint32_t attrib = db.Attribs.Vals[index];
  constexpr std::uint32_t kReparsePoint = 0x400;
  constexpr std::uint32_t kUnixExtension = 0x8000;
  if (attrib & kReparsePoint) return true;
  return (attrib & kUnixExtension) != 0 && ((attrib >> 16) & 0170000) == 0120000;
}

// 只认一个主方法，或者主方法后面接一个过滤器。
std::string read_folder(const CSzArEx& db, std::uint32_t index, SevenZipFolder* out) {
  const CSzAr& ar = db.db;
  // SzGetNextFolderItem 会把 data 往后推，PropsOffset 是相对块描述开头算的。
  const Byte* start = ar.CodersData + ar.FoCodersOffsets[index];
  CSzData data;
  data.Data = start;
  data.Size = ar.FoCodersOffsets[index + 1] - ar.FoCodersOffsets[index];
  CSzFolder folder;
  if (SzGetNextFolderItem(&folder, &data) != SZ_OK) return "7z 目录损坏";
  for (std::uint32_t i = 0; i < folder.NumCoders; ++i) {
    const std::uint32_t id = folder.Coders[i].MethodID;
    if (id == kAes || id == kBcj2 || id == kPpmd) return unsupported_method(id);
  }
  for (std::uint32_t i = 0; i < folder.NumCoders; ++i) {
    const std::uint32_t id = folder.Coders[i].MethodID;
    if (!is_main_method(id) && !is_filter(id)) return unsupported_method(id);
  }
  if (folder.NumCoders < 1 || folder.NumCoders > 2 || folder.NumPackStreams != 1) {
    return "这个 7z 的压缩方法组合暂不支持" + std::string(kRepack);
  }
  for (std::uint32_t i = 0; i < folder.NumCoders; ++i) {
    if (folder.Coders[i].NumStreams != 1) return "这个 7z 的压缩方法组合暂不支持" + std::string(kRepack);
  }
  // 每个编码器只有一个输入一个输出，第 k 个编码器的输入、输出流编号都是 k。
  const std::uint32_t main = folder.PackStreams[0];
  if (main >= folder.NumCoders || !is_main_method(folder.Coders[main].MethodID)) {
    return "这个 7z 的压缩方法组合暂不支持" + std::string(kRepack);
  }
  const CSzCoderInfo& main_coder = folder.Coders[main];
  out->method = main_coder.MethodID;
  out->props.assign(start + main_coder.PropsOffset, start + main_coder.PropsOffset + main_coder.PropsSize);
  const std::uint32_t sizes = ar.FoToCoderUnpackSizes[index];
  const std::uint64_t main_size = ar.CoderUnpackSizes[sizes + main];
  out->unpack_size = SzAr_GetFolderUnpackSize(&ar, index);
  if (folder.NumCoders == 2) {
    const std::uint32_t filter = 1 - main;
    if (folder.NumBonds != 1 || folder.Bonds[0].InIndex != filter || folder.Bonds[0].OutIndex != main ||
        !is_filter(folder.Coders[filter].MethodID)) {
      return "这个 7z 的压缩方法组合暂不支持" + std::string(kRepack);
    }
    const CSzCoderInfo& filter_coder = folder.Coders[filter];
    out->filter = filter_coder.MethodID;
    out->filter_props.assign(start + filter_coder.PropsOffset,
                             start + filter_coder.PropsOffset + filter_coder.PropsSize);
    if (main_size != out->unpack_size) return "7z 目录损坏";
    bool props_ok = out->filter_props.empty();
    if (out->filter == kDelta) props_ok = out->filter_props.size() == 1;
    if (out->filter == kArm64 || out->filter == kRiscv) {
      props_ok = out->filter_props.empty() || out->filter_props.size() == 4;
      const std::uint32_t align = out->filter == kArm64 ? 3 : 1;
      if (out->filter_props.size() == 4 && (read_le32(out->filter_props.data()) & align) != 0) props_ok = false;
    }
    if (!props_ok) return "这个 7z 的过滤器参数暂不支持" + std::string(kRepack);
  } else if (folder.NumBonds != 0) {
    return "7z 目录损坏";
  }

  const std::uint32_t pack = ar.FoStartPackStreamIndex[index];
  out->pack_offset = static_cast<std::int64_t>(db.dataPos + ar.PackPositions[pack]);
  out->pack_size = ar.PackPositions[pack + 1] - ar.PackPositions[pack];
  if (out->method == kCopy && out->pack_size != main_size) return "7z 目录损坏";
  if (out->method == kLzma && out->props.size() != LZMA_PROPS_SIZE) return "7z 目录损坏";
  if (out->method == kLzma2 && (out->props.size() != 1 || out->props[0] > 40)) return "7z 目录损坏";
  out->has_crc = SzBitWithVals_Check(&ar.FolderCRCs, index);
  if (out->has_crc) out->crc = ar.FolderCRCs.Vals[index];
  return "";
}

std::string copy_archive(const CSzArEx& db, SevenZipArchive* archive) {
  const CSzAr& ar = db.db;
  archive->folders.resize(ar.NumFolders);
  for (std::uint32_t i = 0; i < ar.NumFolders; ++i) {
    if (auto error = read_folder(db, i, &archive->folders[i]); !error.empty()) return error;
  }
  archive->files.resize(db.NumFiles);
  for (std::uint32_t i = 0; i < db.NumFiles; ++i) {
    SevenZipFile& file = archive->files[i];
    file.directory = SzArEx_IsDir(&db, i);
    file.link = is_link(db, i);
    file.size = SzArEx_GetFileSize(&db, i);
    file.has_crc = SzBitWithVals_Check(&db.CRCs, i);
    if (file.has_crc) file.crc = db.CRCs.Vals[i];
    const std::uint32_t folder = db.FileToFolder[i];
    if (folder != static_cast<std::uint32_t>(-1)) {
      if (folder >= ar.NumFolders) return "7z 目录损坏";
      file.folder = static_cast<int>(folder);
      archive->folders[folder].files.push_back(i);
    }
    const std::string raw = file_name(db, i);
    bool directory = false;
    if (!safe_archive_name(file.directory ? raw + "/" : raw, &file.name, &directory)) {
      return "7z 里有不安全的路径: " + raw;
    }
  }
  return "";
}

// 把一个块的压缩数据边读边解：先过主方法，再过过滤器。
class FolderDecoder {
 public:
  FolderDecoder(VolumeStream& stream, const SevenZipFolder& folder)
      : stream_(stream), folder_(folder), pack_left_(folder.pack_size), main_left_(folder.unpack_size) {
    LzmaDec_CONSTRUCT(&lzma_);
    Lzma2Dec_CONSTRUCT(&lzma2_);
  }

  ~FolderDecoder() {
    LzmaDec_Free(&lzma_, &kAlloc);
    Lzma2Dec_Free(&lzma2_, &kAlloc);
  }

  FolderDecoder(const FolderDecoder&) = delete;
  FolderDecoder& operator=(const FolderDecoder&) = delete;

  std::string open() {
    if (folder_.method != kCopy) input_.resize(kInputChunk);
    if (folder_.method == kLzma) {
      std::uint8_t props[LZMA_PROPS_SIZE];
      std::memcpy(props, folder_.props.data(), sizeof(props));
      const std::uint64_t dictionary = std::max<std::uint64_t>(needed_dictionary(folder_), 1u << 12);
      props[1] = static_cast<std::uint8_t>(dictionary);
      props[2] = static_cast<std::uint8_t>(dictionary >> 8);
      props[3] = static_cast<std::uint8_t>(dictionary >> 16);
      props[4] = static_cast<std::uint8_t>(dictionary >> 24);
      if (LzmaDec_Allocate(&lzma_, props, LZMA_PROPS_SIZE, &kAlloc) != SZ_OK) return memory_error();
      LzmaDec_Init(&lzma_);
    } else if (folder_.method == kLzma2) {
      const std::uint64_t needed = needed_dictionary(folder_);
      unsigned prop = 0;
      while (prop < folder_.props[0] && lzma2_dictionary(prop) < needed) ++prop;
      if (Lzma2Dec_Allocate(&lzma2_, static_cast<Byte>(prop), &kAlloc) != SZ_OK) return memory_error();
      Lzma2Dec_Init(&lzma2_);
    }
    if (folder_.filter != 0) {
      filtered_.resize(kFilterChunk);
      if (folder_.filter == kDelta) {
        Delta_Init(delta_state_);
        delta_ = folder_.filter_props[0] + 1u;
      }
      if (folder_.filter_props.size() == 4) pc_ = read_le32(folder_.filter_props.data());
    }
    if (folder_.has_crc) crc_ = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
    return "";
  }

  // 已经读掉的压缩数据字节数。
  std::uint64_t consumed() const { return folder_.pack_size - pack_left_ - (input_len_ - input_pos_); }

  // 解出正好 size 字节。
  std::string read(std::uint8_t* dst, std::size_t size) {
    while (size > 0) {
      std::size_t got = 0;
      if (auto error = pull(dst, size, &got); !error.empty()) return error;
      if (got == 0) return "7z 数据提前结束";
      if (folder_.has_crc) crc_ = static_cast<std::uint32_t>(::crc32(crc_, dst, static_cast<uInt>(got)));
      dst += got;
      size -= got;
      produced_ += got;
    }
    return "";
  }

  // 块已经全部读出：确认压缩数据正好用完，再核对块的校验和。
  std::string finish() {
    if (produced_ != folder_.unpack_size) return "7z 数据长度和目录不符";
    if (auto error = finish_main(); !error.empty()) return error;
    if (folder_.has_crc && crc_ != folder_.crc) return "7z 数据校验和不符";
    return "";
  }

 private:
  static constexpr std::size_t kInputChunk = 256u << 10;
  static constexpr std::size_t kFilterChunk = 256u << 10;

  std::string memory_error() const {
    return "解压这个 7z 需要约 " + std::to_string((needed_dictionary(folder_) >> 20) + 1) + " MB 内存，分配失败";
  }

  std::string refill() {
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(input_.size(), pack_left_));
    if (auto error = stream_.read(input_.data(), n); !error.empty()) return error;
    pack_left_ -= n;
    input_pos_ = 0;
    input_len_ = n;
    return "";
  }

  SRes decode(std::uint8_t* dst, SizeT* dst_len, SizeT* src_len, ELzmaFinishMode mode) {
    const Byte* src = input_.data() + input_pos_;
    if (folder_.method == kLzma2) return Lzma2Dec_DecodeToBuf(&lzma2_, dst, dst_len, src, src_len, mode, &status_);
    return LzmaDec_DecodeToBuf(&lzma_, dst, dst_len, src, src_len, mode, &status_);
  }

  // 主方法解出至少一个字节；整块解完时 got 为 0。
  std::string main_pull(std::uint8_t* dst, std::size_t size, std::size_t* got) {
    *got = 0;
    if (main_left_ == 0) return "";
    size = static_cast<std::size_t>(std::min<std::uint64_t>(size, main_left_));
    if (folder_.method == kCopy) {
      if (auto error = stream_.read(dst, size); !error.empty()) return error;
      pack_left_ -= size;
      main_left_ -= size;
      *got = size;
      return "";
    }
    for (;;) {
      if (input_pos_ == input_len_ && pack_left_ > 0) {
        if (auto error = refill(); !error.empty()) return error;
      }
      SizeT dst_len = size;
      SizeT src_len = input_len_ - input_pos_;
      const ELzmaFinishMode mode = size == main_left_ ? LZMA_FINISH_END : LZMA_FINISH_ANY;
      const SRes res = decode(dst, &dst_len, &src_len, mode);
      input_pos_ += src_len;
      if (res != SZ_OK) return "7z 数据损坏，解压失败";
      if (dst_len > 0) {
        main_left_ -= dst_len;
        *got = dst_len;
        return "";
      }
      if (status_ == LZMA_STATUS_FINISHED_WITH_MARK) return "7z 数据提前结束";
      if (src_len == 0 && input_pos_ == input_len_ && pack_left_ == 0) return "7z 数据提前结束";
      if (src_len == 0 && input_pos_ < input_len_) return "7z 数据损坏，解压失败";
    }
  }

  std::string finish_main() {
    if (main_left_ != 0) return "7z 数据长度和目录不符";
    if (folder_.method == kCopy) return pack_left_ == 0 ? "" : "7z 压缩数据末尾有多余内容";
    for (;;) {
      if (input_pos_ == input_len_ && pack_left_ > 0) {
        if (auto error = refill(); !error.empty()) return error;
      }
      const bool ended = status_ == LZMA_STATUS_FINISHED_WITH_MARK || status_ == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK;
      if (input_pos_ == input_len_) return ended ? "" : "7z 数据提前结束";
      if (status_ == LZMA_STATUS_FINISHED_WITH_MARK) return "7z 压缩数据末尾有多余内容";
      // LZMA2 的结束标记可能还没读到。
      std::uint8_t unused = 0;
      SizeT dst_len = 0;
      SizeT src_len = input_len_ - input_pos_;
      const SRes res = decode(&unused, &dst_len, &src_len, LZMA_FINISH_END);
      input_pos_ += src_len;
      if (res != SZ_OK) return "7z 数据损坏，解压失败";
      if (src_len == 0) return "7z 压缩数据末尾有多余内容";
    }
  }

  std::size_t convert(std::uint8_t* data, std::size_t size) {
    Byte* end = data;
    switch (folder_.filter) {
      case kDelta:
        Delta_Decode(delta_state_, delta_, data, size);
        return size;
      case kBcj:
        end = z7_BranchConvSt_X86_Dec(data, size, pc_, &x86_state_);
        break;
      case kPpc:
        end = z7_BranchConv_PPC_Dec(data, size, pc_);
        break;
      case kIa64:
        end = z7_BranchConv_IA64_Dec(data, size, pc_);
        break;
      case kArm:
        end = z7_BranchConv_ARM_Dec(data, size, pc_);
        break;
      case kArmt:
        end = z7_BranchConv_ARMT_Dec(data, size, pc_);
        break;
      case kSparc:
        end = z7_BranchConv_SPARC_Dec(data, size, pc_);
        break;
      case kArm64:
        end = z7_BranchConv_ARM64_Dec(data, size, pc_);
        break;
      case kRiscv:
        end = z7_BranchConv_RISCV_Dec(data, size, pc_);
        break;
      default:
        return size;
    }
    const auto processed = static_cast<std::size_t>(end - data);
    pc_ += static_cast<std::uint32_t>(processed);
    return processed;
  }

  // 过滤器每次只能转换到指令边界，转换不了的尾巴留着等后面的数据；整块结束时尾巴原样输出。
  std::string pull(std::uint8_t* dst, std::size_t size, std::size_t* got) {
    if (folder_.filter == 0) return main_pull(dst, size, got);
    for (;;) {
      if (ready_ > 0) {
        const std::size_t n = std::min(size, ready_);
        std::memcpy(dst, filtered_.data() + head_, n);
        head_ += n;
        ready_ -= n;
        *got = n;
        return "";
      }
      if (head_ > 0) {
        std::memmove(filtered_.data(), filtered_.data() + head_, filtered_len_ - head_);
        filtered_len_ -= head_;
        head_ = 0;
      }
      if (main_left_ == 0) {
        *got = 0;
        if (filtered_len_ == 0) return "";
        ready_ = filtered_len_;
        continue;
      }
      std::size_t more = 0;
      if (auto error = main_pull(filtered_.data() + filtered_len_, filtered_.size() - filtered_len_, &more);
          !error.empty()) {
        return error;
      }
      filtered_len_ += more;
      ready_ = convert(filtered_.data(), filtered_len_);
    }
  }

  VolumeStream& stream_;
  const SevenZipFolder& folder_;
  std::uint64_t pack_left_;
  std::uint64_t main_left_;
  std::uint64_t produced_ = 0;
  CLzmaDec lzma_;
  CLzma2Dec lzma2_;
  ELzmaStatus status_ = LZMA_STATUS_NOT_SPECIFIED;
  std::vector<std::uint8_t> input_;
  std::size_t input_pos_ = 0;
  std::size_t input_len_ = 0;
  std::vector<std::uint8_t> filtered_;
  std::size_t head_ = 0;
  std::size_t ready_ = 0;
  std::size_t filtered_len_ = 0;
  std::uint32_t pc_ = 0;
  std::uint32_t x86_state_ = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
  Byte delta_state_[DELTA_STATE_SIZE] = {};
  unsigned delta_ = 1;
  std::uint32_t crc_ = 0;
};

bool cancelled(const CancelToken* cancel) { return cancel != nullptr && cancel->cancelled(); }

}  // namespace

bool is_unsupported_7z_error(const std::string& message) { return message.rfind("这个 7z ", 0) == 0; }

std::string read_7z_archive(DrivePlugin& drive, const std::vector<DriveEntry>& parts, SevenZipArchive* archive,
                            std::uint64_t max_dictionary) {
  init_crc_table();
  *archive = SevenZipArchive{};
  const std::vector<ArchiveVolume> volumes = archive_volumes(parts);
  if (volumes.empty()) return "7z 压缩包是空的";

  HeaderStream stream;
  stream.vt.Read = header_read;
  stream.vt.Seek = header_seek;
  stream.drive = &drive;
  stream.volumes = &volumes;
  stream.total = volumes.back().start + volumes.back().size;

  std::vector<Byte> look(kHeaderChunk);
  CLookToRead2 look_stream;
  LookToRead2_CreateVTable(&look_stream, False);
  look_stream.buf = look.data();
  look_stream.bufSize = look.size();
  look_stream.realStream = &stream.vt;
  LookToRead2_INIT(&look_stream);

  CSzArEx db;
  SzArEx_Init(&db);
  const SRes res = SzArEx_Open(&db, &look_stream.vt, &kAlloc, &kAlloc);
  std::string error;
  if (res != SZ_OK) {
    error = !stream.error.empty() ? stream.error : open_error(res);
  } else {
    error = copy_archive(db, archive);
  }
  SzArEx_Free(&db, &kAlloc);
  if (error.empty()) {
    std::uint64_t largest = 0;
    for (const auto& folder : archive->folders) largest = std::max(largest, needed_dictionary(folder));
    trace("7z 字典 " + std::to_string(largest >> 20) + " MB，解压需要这么多内存");
    if (max_dictionary > 0 && largest > max_dictionary) error = dictionary_refusal("7z", largest, max_dictionary);
  }
  if (!error.empty()) *archive = SevenZipArchive{};
  return error;
}

std::string extract_7z_entries(DrivePlugin& drive, InstallSink& sink, const std::vector<DriveEntry>& parts,
                               const SevenZipArchive& archive, const SevenZipExtract& options,
                               std::vector<FinishedFile>* done) {
  std::int64_t local_written = 0;
  std::int64_t* written = options.written != nullptr ? options.written : &local_written;
  const std::int64_t stop = options.stop_after_output_bytes;
  const auto started = std::chrono::steady_clock::now();
  auto last_report = started;
  const auto report = [&](std::int64_t position, bool force) {
    if (!options.on_bytes) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_report < std::chrono::milliseconds(500)) return;
    last_report = now;
    options.on_bytes(position);
  };
  const auto target = [&](std::size_t index) -> const std::string* {
    if (index >= options.targets.size() || options.targets[index].empty()) return nullptr;
    const SevenZipFile& file = archive.files[index];
    if (file.directory || file.link) return nullptr;
    return &options.targets[index];
  };
  const auto already = [&](std::size_t index) {
    if (options.finished_before == nullptr) return false;
    const auto found = options.finished_before->find(options.targets[index]);
    const SevenZipFile& file = archive.files[index];
    return found != options.finished_before->end() && found->second.first == static_cast<std::int64_t>(file.size) &&
           found->second.second == file.crc;
  };
  const auto record = [&](std::size_t index, OutputFile& out) -> std::string {
    const SevenZipFile& file = archive.files[index];
    if (auto error = out.finish(); !error.empty()) return error;
    if (out.size() != static_cast<std::int64_t>(file.size) || (file.has_crc && out.crc() != file.crc)) {
      return "校验和不符: " + file.name;
    }
    const FinishedFile finished{options.targets[index], static_cast<std::int64_t>(file.size), file.crc};
    done->push_back(finished);
    if (options.journal != nullptr) {
      if (auto error = options.journal->add(finished); !error.empty()) return error;
    }
    if (options.on_file_done) options.on_file_done(*done);
    return "";
  };
  const auto may_start = [&]() -> std::string {
    if (stop >= 0 && *written >= stop) return kInterrupted;
    if (cancelled(options.cancel)) return kInterrupted;
    return "";
  };

  for (std::size_t i = 0; i < archive.files.size(); ++i) {
    if (archive.files[i].folder >= 0 || target(i) == nullptr) continue;
    const bool skip = already(i);
    if (options.on_entry) options.on_entry(skip);
    if (skip) continue;
    if (auto error = may_start(); !error.empty()) return error;
    OutputFile out(sink, join_storage_path(options.staging_root, *target(i)), 0, stop, written);
    if (auto error = record(i, out); !error.empty()) return error;
  }

  std::vector<std::size_t> order(archive.folders.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return archive.folders[a].pack_offset < archive.folders[b].pack_offset;
  });

  VolumeStream stream(drive, archive_volumes(parts), options.cancel, options.prefetch);
  constexpr std::size_t kChunk = 256u << 10;
  std::vector<std::uint8_t> buffer;
  double wait_seconds = 0;
  std::uint64_t decoded_bytes = 0;
  for (const std::size_t index : order) {
    const SevenZipFolder& folder = archive.folders[index];
    std::size_t last_needed = folder.files.size();
    for (std::size_t k = 0; k < folder.files.size(); ++k) {
      if (target(folder.files[k]) != nullptr && !already(folder.files[k])) last_needed = k;
    }
    if (last_needed == folder.files.size()) {
      for (const std::size_t file : folder.files) {
        if (target(file) != nullptr && options.on_entry) options.on_entry(true);
      }
      report(folder.pack_offset + static_cast<std::int64_t>(folder.pack_size), true);
      continue;
    }

    if (buffer.empty()) buffer.resize(kChunk);
    if (auto error = may_start(); !error.empty()) return error;
    if (auto error = stream.seek(folder.pack_offset); !error.empty()) return error;
    FolderDecoder decoder(stream, folder);
    if (auto error = decoder.open(); !error.empty()) return error;
    for (std::size_t k = 0; k < folder.files.size(); ++k) {
      const std::size_t file_index = folder.files[k];
      const SevenZipFile& file = archive.files[file_index];
      const std::string* path = target(file_index);
      const bool skip = path == nullptr || already(file_index);
      if (path != nullptr && options.on_entry) options.on_entry(skip);
      if (k > last_needed) continue;
      std::unique_ptr<OutputFile> out;
      if (!skip) {
        if (auto error = may_start(); !error.empty()) return error;
        out = std::make_unique<OutputFile>(sink, join_storage_path(options.staging_root, *path),
                                           static_cast<std::int64_t>(file.size), stop, written);
      }
      std::uint64_t left = file.size;
      while (left > 0) {
        if (cancelled(options.cancel)) return kInterrupted;
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, left));
        const auto before = std::chrono::steady_clock::now();
        if (auto error = decoder.read(buffer.data(), n); !error.empty()) return error + ": " + file.name;
        wait_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
        decoded_bytes += n;
        if (out) {
          if (auto error = out->write(buffer.data(), n); !error.empty()) return error;
        }
        left -= n;
        report(folder.pack_offset + static_cast<std::int64_t>(decoder.consumed()), false);
      }
      if (out) {
        if (auto error = record(file_index, *out); !error.empty()) return error;
      }
    }
    if (last_needed + 1 == folder.files.size()) {
      if (auto error = decoder.finish(); !error.empty()) return error;
    }
    report(folder.pack_offset + static_cast<std::int64_t>(folder.pack_size), true);
  }

  if (std::getenv("PANPLAY_TRACE") != nullptr) {
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[trace] 7z: %.1fs total, %.1fs reading+decoding, %.1f MB decoded\n", total, wait_seconds,
                 static_cast<double>(decoded_bytes) / (1 << 20));
  }
  return "";
}

}  // namespace panplay
