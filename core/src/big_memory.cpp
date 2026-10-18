#include "big_memory.hpp"

#include <atomic>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace panplay {
namespace {

// 每块前面记下整块多大，释放时据此决定还给系统还是还给堆。64 字节让返回的地址保持对齐。
constexpr std::size_t kHeader = 64;
// 小块走堆，免得每次都向系统要整页。
constexpr std::size_t kSystemThreshold = 1u << 20;

std::atomic<std::uint64_t> in_use{0};
std::atomic<std::uint64_t> peak{0};
std::atomic<std::size_t> limit{0};

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

std::uint64_t big_memory_in_use() { return in_use.load(); }
std::uint64_t big_memory_peak() { return peak.load(); }
void set_big_memory_limit(std::size_t bytes) { limit.store(bytes); }

}  // namespace panplay

extern "C" void* panplay_big_alloc(std::size_t size) { return panplay::big_alloc(size); }
extern "C" void panplay_big_free(void* address) { panplay::big_free(address); }
