#pragma once

#include <filesystem>
#include <string>

namespace panplay {

// 把 FTP 上的绝对路径映射到本机根目录。拒绝 `..` 和越出根目录的路径。
bool storage_path(const std::filesystem::path& root, const std::string& logical, std::filesystem::path* out,
                  std::string* error);

}  // namespace panplay
