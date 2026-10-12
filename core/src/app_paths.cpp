#include "panplay/app_paths.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace panplay {
namespace {

std::filesystem::path env_path(const char* name) {
#if defined(_WIN32)
  // 窄字符 getenv 按本地代码页返回，用户名里有中文时路径会坏掉。
  const std::wstring wide(name, name + std::strlen(name));
  const wchar_t* value = _wgetenv(wide.c_str());
#else
  const char* value = std::getenv(name);
#endif
  return value != nullptr && *value != 0 ? std::filesystem::path(value) : std::filesystem::path();
}

std::filesystem::path platform_data_dir() {
#if defined(_WIN32)
  const auto appdata = env_path("APPDATA");
  if (!appdata.empty()) return appdata / "panplay";
#elif defined(__PROSPERO__) || defined(PANPLAY_PS5)
  return "/data/panplay";
#else
  const auto xdg = env_path("XDG_DATA_HOME");
  if (!xdg.empty()) return xdg / "panplay";
  const auto home = env_path("HOME");
  if (!home.empty()) return home / ".local" / "share" / "panplay";
#endif
  std::error_code ec;
  const auto exe = executable_dir();
  return (exe.empty() ? std::filesystem::current_path(ec) : exe) / "panplay-data";
}

}  // namespace

std::filesystem::path executable_dir() {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::filesystem::path(buffer).parent_path();
#else
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : self.parent_path();
#endif
}

std::filesystem::path app_data_dir() {
  static const std::filesystem::path dir = [] {
    const auto custom = env_path("PANPLAY_DATA_DIR");
    const auto chosen = custom.empty() ? platform_data_dir() : custom;
    std::error_code ec;
    std::filesystem::create_directories(chosen, ec);
    return chosen;
  }();
  return dir;
}

}  // namespace panplay
