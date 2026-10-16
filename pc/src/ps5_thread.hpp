#pragma once

#include <cstddef>

// 主机上新线程一律用这么大的栈，不依赖系统默认值：栈不够时撞到栈底，进程直接被杀，连崩溃信号都来不及记。
constexpr std::size_t kPs5ThreadStack = 2u << 20;

// 系统给新线程的默认栈大小（字节），写进日志对照用。
std::size_t ps5_default_thread_stack();
