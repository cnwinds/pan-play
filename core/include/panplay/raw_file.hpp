#pragma once

#include <cstddef>
#include <filesystem>

namespace panplay {

// 直接用系统调用写文件，不经过 stdio：libc++ 的 ofstream 底下是 FILE*，大块数据会被 stdio 按自己的小块拆开，
// PS5 上每次 64 KB 只能写 28 MB/s，每次 4 MB 能到近 300 MB/s。每次写多大由调用方决定。
class RawFileOut {
 public:
  RawFileOut() = default;
  explicit RawFileOut(const std::filesystem::path& path) { open(path); }
  ~RawFileOut() { close(); }
  RawFileOut(const RawFileOut&) = delete;
  RawFileOut& operator=(const RawFileOut&) = delete;

  bool open(const std::filesystem::path& path);  // 新建或清空
  bool ok() const { return fd_ >= 0; }
  bool write_all(const void* data, std::size_t size);
  bool sync();
  bool close();

 private:
  int fd_ = -1;
};

// 已经写完关掉的文件再打开刷一次盘：fsync 刷的是整个文件，不管是哪个句柄写的。
bool sync_file(const std::filesystem::path& path);

}  // namespace panplay
