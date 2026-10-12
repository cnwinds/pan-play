#pragma once

#include <functional>
#include <mutex>

namespace panplay {

// 让另一个线程中止一个正在阻塞的请求，包括还在建立连接、等响应头的阶段。
class AbortSignal {
 public:
  // 登记中止动作。已经中止过时不登记，返回 false。
  bool arm(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(mu_);
    if (aborted_) return false;
    action_ = std::move(action);
    return true;
  }

  // 撤销登记。返回 true 表示期间已被中止，登记的动作已经执行过。
  bool disarm() {
    std::lock_guard<std::mutex> lock(mu_);
    action_ = nullptr;
    return aborted_;
  }

  void abort() {
    std::lock_guard<std::mutex> lock(mu_);
    aborted_ = true;
    if (action_) {
      action_();
      action_ = nullptr;
    }
  }

  bool aborted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return aborted_;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mu_);
    aborted_ = false;
    action_ = nullptr;
  }

 private:
  mutable std::mutex mu_;
  bool aborted_ = false;
  std::function<void()> action_;
};

}  // namespace panplay
