#pragma once

#include <cstddef>

// 编译时打包进程序的界面文件，由 cmake/embed_files.cmake 生成。
namespace panplay_embedded {

struct File {
  const char* name;
  const unsigned char* data;
  std::size_t size;
};

extern const File files[];
extern const std::size_t file_count;

}  // namespace panplay_embedded
