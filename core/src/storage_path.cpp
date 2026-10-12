#include "panplay/storage_path.hpp"

namespace panplay {

bool storage_path(const std::filesystem::path& root, const std::string& logical, std::filesystem::path* out,
                  std::string* error) {
  std::string path = logical;
  for (char& ch : path) {
    if (ch == '\\') ch = '/';
  }
  if (path.empty() || path.front() != '/') {
    *error = "存放路径需要是绝对路径";
    return false;
  }

  std::filesystem::path relative;
  std::size_t index = 0;
  while (index < path.size()) {
    while (index < path.size() && path[index] == '/') ++index;
    if (index >= path.size()) break;
    const std::size_t slash = path.find('/', index);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string segment = path.substr(index, end - index);
    index = end;
    if (segment.empty() || segment == ".") continue;
    if (segment == ".." || segment.find(':') != std::string::npos) {
      *error = "路径不能包含 ..";
      return false;
    }
    relative /= std::filesystem::u8path(segment);
  }

  const std::filesystem::path base = root.lexically_normal();
  const std::filesystem::path full = (base / relative).lexically_normal();
  const std::string back = full.lexically_relative(base).generic_string();
  if (back == ".." || back.rfind("../", 0) == 0 || back.find("/../") != std::string::npos) {
    *error = "路径越出存放目录";
    return false;
  }
  *out = full;
  return true;
}

}  // namespace panplay
