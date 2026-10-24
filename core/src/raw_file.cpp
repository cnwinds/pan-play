#include "panplay/raw_file.hpp"

#include <algorithm>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
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

bool sync_file(const std::filesystem::path& path) {
  const int fd = open_for_write(path, false);
  if (fd < 0) return false;
  const bool ok = sync_fd(fd) == 0;
  close_fd(fd);
  return ok;
}

}  // namespace panplay
