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

// 测硬盘读速度用：尽量绕过系统缓存直接从盘上读（刚写完的文件还在缓存里，普通读量到的是内存速度）。
// 绕过缓存要求每次读的大小、缓冲地址按扇区对齐，调用方按 4 KB 的整数倍读、缓冲按 4 KB 对齐。
class RawFileIn {
 public:
  RawFileIn() = default;
  ~RawFileIn() { close(); }
  RawFileIn(const RawFileIn&) = delete;
  RawFileIn& operator=(const RawFileIn&) = delete;

  // 系统不支持绕过缓存时退回普通读，uncached() 为 false。
  bool open(const std::filesystem::path& path);
  bool ok() const { return handle_ >= 0; }
  bool uncached() const { return uncached_; }
  // 返回读到的字节数，0 是读完了，负数是出错。
  long long read(void* data, std::size_t size);
  void close();

 private:
  long long handle_ = -1;
  bool uncached_ = false;
};

// 已经写完关掉的文件再打开刷一次盘：fsync 刷的是整个文件，不管是哪个句柄写的。
bool sync_file(const std::filesystem::path& path);

}  // namespace panplay
