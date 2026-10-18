#pragma once

#include <cstddef>
#include <cstdint>

namespace panplay {

// 解压字典这种动辄几百 MB 到几 GB 的内存直接向系统要，释放时马上还给系统。
// PS5 的 libc 堆释放后不把内存还给系统：4 GB 字典用过一次就一直占着，主机上别的程序跟着缺内存；
// 下次再要 4 GB 也只能在堆的碎片里找，常常要不到。
// 要不到时返回 nullptr。
void* big_alloc(std::size_t size);
void big_free(void* address);

// 现在和最多同时占着多少字节，记日志用。
std::uint64_t big_memory_in_use();
std::uint64_t big_memory_peak();

// 自测用：单次要的超过这么多字节就当作要不到，0 是不限制。
void set_big_memory_limit(std::size_t bytes);

}  // namespace panplay
