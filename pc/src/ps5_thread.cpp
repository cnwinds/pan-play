#if defined(PANPLAY_PS5)

#include "ps5_thread.hpp"

#include "panplay/trace.hpp"

#include <pthread.h>

// 链接时 --wrap=pthread_create 把所有建线程的调用（std::thread、unrar 的线程池）接到 __wrap_pthread_create。
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*routine)(void*), void* arg);

namespace {

struct ThreadStart {
  void* (*routine)(void*);
  void* arg;
};

void* run_thread(void* raw) {
  const ThreadStart start = *static_cast<ThreadStart*>(raw);
  delete static_cast<ThreadStart*>(raw);
  panplay::install_signal_stack();
  return start.routine(start.arg);
}

}  // namespace

std::size_t ps5_default_thread_stack() {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return 0;
  std::size_t size = 0;
  pthread_attr_getstacksize(&attr, &size);
  pthread_attr_destroy(&attr);
  return size;
}

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*routine)(void*), void* arg) {
  pthread_attr_t own;
  if (attr == nullptr) {
    pthread_attr_init(&own);
    pthread_attr_setstacksize(&own, kPs5ThreadStack);
  }
  auto* start = new ThreadStart{routine, arg};
  const int code = __real_pthread_create(thread, attr != nullptr ? attr : &own, run_thread, start);
  if (code != 0) delete start;
  if (attr == nullptr) pthread_attr_destroy(&own);
  return code;
}

#endif
