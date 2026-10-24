#include "panplay/raw_file.hpp"

#include <algorithm>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace panplay {

namespace {

#if defined(_WIN32)
int open_for_write(const std::filesystem::path& path, bool truncate) {
  const int flags = _O_WRONLY | _O_BINARY | (truncate ? _O_CREAT | _O_TRUNC : 0);
  return _wopen(path.c_str(), flags, _S_IREAD | _S_IWRITE);
}
long long write_some(int fd, const void* data, std::size_t size) {
  return _write(fd, data, static_cast<unsigned>(std::min<std::size_t>(size, 1u << 30)));
}
int sync_fd(int fd) { return _commit(fd); }
int close_fd(int fd) { return _close(fd); }
#else
int open_for_write(const std::filesystem::path& path, bool truncate) {
  return ::open(path.c_str(), O_WRONLY | (truncate ? O_CREAT | O_TRUNC : 0), 0666);
}
long long write_some(int fd, const void* data, std::size_t size) { return ::write(fd, data, size); }
int sync_fd(int fd) { return ::fsync(fd); }
int close_fd(int fd) { return ::close(fd); }
#endif

}  // namespace

bool RawFileOut::open(const std::filesystem::path& path) {
  close();
  fd_ = open_for_write(path, true);
  return ok();
}

bool RawFileOut::write_all(const void* data, std::size_t size) {
  if (fd_ < 0) return false;
  const char* at = static_cast<const char*>(data);
  while (size > 0) {
    const long long written = write_some(fd_, at, size);
    if (written <= 0) return false;
    at += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

bool RawFileOut::sync() { return fd_ >= 0 && sync_fd(fd_) == 0; }

bool RawFileOut::close() {
  if (fd_ < 0) return true;
  const bool closed = close_fd(fd_) == 0;
  fd_ = -1;
  return closed;
}

#if defined(_WIN32)
bool RawFileIn::open(const std::filesystem::path& path) {
  close();
  for (const bool uncached : {true, false}) {
    const DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN | (uncached ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      handle_ = reinterpret_cast<long long>(file);
      uncached_ = uncached;
      return true;
    }
  }
  return false;
}

long long RawFileIn::read(void* data, std::size_t size) {
  if (handle_ < 0) return -1;
  DWORD got = 0;
  const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
  if (!ReadFile(reinterpret_cast<HANDLE>(handle_), data, want, &got, nullptr)) return -1;
  return got;
}

void RawFileIn::close() {
  if (handle_ >= 0) CloseHandle(reinterpret_cast<HANDLE>(handle_));
  handle_ = -1;
}
#else
bool RawFileIn::open(const std::filesystem::path& path) {
  close();
#if defined(O_DIRECT)
  handle_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
  uncached_ = handle_ >= 0;
#endif
  if (handle_ < 0) handle_ = ::open(path.c_str(), O_RDONLY);
  return ok();
}

long long RawFileIn::read(void* data, std::size_t size) {
  if (handle_ < 0) return -1;
  for (;;) {
    const auto n = ::read(static_cast<int>(handle_), data, size);
    if (n < 0 && errno == EINTR) continue;
    return static_cast<long long>(n);
  }
}

void RawFileIn::close() {
  if (handle_ >= 0) ::close(static_cast<int>(handle_));
  handle_ = -1;
}
#endif

bool sync_file(const std::filesystem::path& path) {
  const int fd = open_for_write(path, false);
  if (fd < 0) return false;
  const bool ok = sync_fd(fd) == 0;
  close_fd(fd);
  return ok;
}

}  // namespace panplay
