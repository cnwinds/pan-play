#include "big_memory.hpp"

#include "panplay/trace.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace panplay {
namespace {

// 每块前面记下整块多大，释放时据此决定还给系统还是还给堆。64 字节让返回的地址保持对齐。
constexpr std::size_t kHeader = 64;
// 小块走堆，免得每次都向系统要整页。
constexpr std::size_t kSystemThreshold = 1u << 20;
// 前面的文件在字典里写过这么多才清：PS5 上盘游只能常驻约 1.5 GB，写得少的那段一直在内存里，清了反而要重新缺页。
constexpr std::size_t kWindowReuseThreshold = std::size_t(1) << 30;

std::atomic<std::uint64_t> in_use{0};
std::atomic<std::uint64_t> peak{0};
std::atomic<std::size_t> limit{0};
std::atomic<std::uint64_t> discarded{0};

void* system_alloc(std::size_t bytes) {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void* block = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return block == MAP_FAILED ? nullptr : block;
#endif
}

void system_free(void* block, std::size_t bytes) {
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(block, 0, MEM_RELEASE);
#else
  munmap(block, bytes);
#endif
}

std::size_t page_size() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  const long size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::size_t>(size) : 4096;
#endif
}

// 告诉系统这段页的内容不要了。已经换到硬盘的页直接作废，下次写时给新页，不再先读回来。
bool system_discard(void* start, std::size_t bytes) {
#if defined(_WIN32)
  return VirtualAlloc(start, bytes, MEM_RESET, PAGE_READWRITE) != nullptr;
#elif defined(__linux__) || !defined(MADV_FREE)
  return madvise(start, bytes, MADV_DONTNEED) == 0;
#else
  // FreeBSD（PS5）的 MADV_DONTNEED 只是提示、不丢内容；MADV_FREE 才会释放换出页占的交换区。
  return madvise(start, bytes, MADV_FREE) == 0;
#endif
}

}  // namespace

void* big_alloc(std::size_t size) {
  const std::size_t cap = limit.load();
  if (cap > 0 && size > cap) return nullptr;
  if (size > std::numeric_limits<std::size_t>::max() - kHeader) return nullptr;
  const std::size_t total = size + kHeader;
  void* block = total >= kSystemThreshold ? system_alloc(total) : std::malloc(total);
  if (block == nullptr) return nullptr;
  *static_cast<std::size_t*>(block) = total;
  const std::uint64_t now = in_use.fetch_add(total) + total;
  std::uint64_t seen = peak.load();
  while (now > seen && !peak.compare_exchange_weak(seen, now)) {
  }
  return static_cast<char*>(block) + kHeader;
}

void big_free(void* address) {
  if (address == nullptr) return;
  void* block = static_cast<char*>(address) - kHeader;
  const std::size_t total = *static_cast<std::size_t*>(block);
  in_use.fetch_sub(total);
  if (total >= kSystemThreshold) {
    system_free(block, total);
  } else {
    std::free(block);
  }
}

std::size_t big_discard(void* address, std::size_t bytes) {
  if (address == nullptr || bytes == 0) return 0;
  const std::size_t total = *static_cast<std::size_t*>(static_cast<void*>(static_cast<char*>(address) - kHeader));
  if (total < kSystemThreshold) return 0;
  if (bytes > total - kHeader) bytes = total - kHeader;
  const std::size_t page = page_size();
  const auto from = reinterpret_cast<std::uintptr_t>(address);
  const std::uintptr_t start = (from + page - 1) / page * page;
  const std::uintptr_t end = (from + bytes) / page * page;
  if (end <= start) return 0;
  if (!system_discard(reinterpret_cast<void*>(start), end - start)) return 0;
  discarded.fetch_add(end - start);
  return end - start;
}

std::uint64_t big_memory_in_use() { return in_use.load(); }
std::uint64_t big_memory_peak() { return peak.load(); }
std::uint64_t big_memory_discarded() { return discarded.load(); }
void set_big_memory_limit(std::size_t bytes) { limit.store(bytes); }

bool window_reuse(void* window, std::size_t used) {
  if (used < kWindowReuseThreshold) return false;
  const auto started = std::chrono::steady_clock::now();
  const std::size_t dropped = big_discard(window, used);
  if (dropped == 0) {
    PANPLAY_DEBUG("解压字典：前面的文件写过 " + std::to_string(used >> 20) + " MB，交给系统丢掉没成功");
    return false;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  char text[160];
  std::snprintf(text, sizeof(text), "解压字典：前面的文件写过 %llu MB，下一个文件从头写前交给系统丢掉，用了 %.1f ms",
                static_cast<unsigned long long>(dropped >> 20), ms);
  PANPLAY_DEBUG(std::string(text));
  return true;
}

}  // namespace panplay

extern "C" void* panplay_big_alloc(std::size_t size) { return panplay::big_alloc(size); }
extern "C" void panplay_big_free(void* address) { panplay::big_free(address); }
extern "C" bool panplay_window_reuse(void* window, std::size_t used) { return panplay::window_reuse(window, used); }
