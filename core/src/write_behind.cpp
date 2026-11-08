#include "write_behind.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace panplay {

using Slot = std::shared_ptr<std::unique_ptr<FileWriter>>;

class WriteBehindSink::Writer final : public FileWriter {
 public:
  Writer(WriteBehindSink& owner, Slot slot) : owner_(owner), slot_(std::move(slot)) {}
  ~Writer() override {
    owner_.push([slot = slot_] {
      slot->reset();
      return std::string();
    }, 0, true);
  }

  std::string write(const std::uint8_t* data, std::size_t size) override {
    auto copy = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
    return owner_.push([slot = slot_, copy] { return (*slot)->write(copy->data(), copy->size()); }, size);
  }

  std::string finish() override {
    return owner_.push([slot = slot_] { return (*slot)->finish(); });
  }

 private:
  WriteBehindSink& owner_;
  Slot slot_;
};

WriteBehindSink::WriteBehindSink(InstallSink& inner, std::size_t queue_bytes)
    : inner_(inner), queue_bytes_(queue_bytes), thread_([this] { loop(); }) {}

WriteBehindSink::~WriteBehindSink() {
  drain();
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  changed_.notify_all();
  thread_.join();
}

std::string WriteBehindSink::push(std::function<std::string()> run, std::size_t bytes, bool cleanup) {
  std::unique_lock<std::mutex> lock(mu_);
  changed_.wait(lock, [&] { return ops_.empty() || queued_bytes_ + bytes <= queue_bytes_ || (!error_.empty() && !cleanup); });
  if (!error_.empty() && !cleanup) return error_;
  ops_.push_back(Op{std::move(run), bytes, cleanup});
  queued_bytes_ += bytes;
  ++pending_;
  changed_.notify_all();
  return "";
}

void WriteBehindSink::loop() {
  for (;;) {
    Op op;
    bool skip = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      changed_.wait(lock, [&] { return stopping_ || !ops_.empty(); });
      if (ops_.empty()) return;
      op = std::move(ops_.front());
      ops_.pop_front();
      skip = !error_.empty() && !op.cleanup;
    }
    std::string error;
    double ms = 0;
    if (!skip) {
      const auto started = std::chrono::steady_clock::now();
      // 同步写时异常会沿安装线程往上抛，这里没人接，不拦住整个进程会退出。
      try {
        error = op.run();
      } catch (const std::exception& ex) {
        error = std::string("写盘出错：") + ex.what();
      } catch (...) {
        error = "写盘出错";
      }
      ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    op.run = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!error.empty() && error_.empty()) error_ = error;
      queued_bytes_ -= op.bytes;
      --pending_;
      busy_ms_ += ms;
    }
    changed_.notify_all();
  }
}

std::string WriteBehindSink::drain() {
  std::unique_lock<std::mutex> lock(mu_);
  changed_.wait(lock, [&] { return pending_ == 0; });
  return error_;
}

double WriteBehindSink::busy_ms() const {
  std::lock_guard<std::mutex> lock(mu_);
  return busy_ms_;
}

std::unique_ptr<FileWriter> WriteBehindSink::open_writer(const std::string& path, std::int64_t size, std::string* error) {
  auto slot = std::make_shared<std::unique_ptr<FileWriter>>();
  const std::string queued = push([this, slot, path, size] {
    std::string failure;
    *slot = inner_.open_writer(path, size, &failure);
    if (*slot) return std::string();
    return failure.empty() ? "无法创建文件 " + path : failure;
  });
  if (!queued.empty()) {
    *error = queued;
    return nullptr;
  }
  return std::make_unique<Writer>(*this, std::move(slot));
}

std::string WriteBehindSink::ensure_dir(const std::string& path) {
  return push([this, path] { return inner_.ensure_dir(path); });
}

std::int64_t WriteBehindSink::file_size(const std::string& path, std::string* error) {
  if (std::string failure = drain(); !failure.empty()) {
    *error = failure;
    return -1;
  }
  return inner_.file_size(path, error);
}

std::string WriteBehindSink::store(const std::string& path, const std::uint8_t* data, std::size_t size) {
  auto copy = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
  return push([this, path, copy] { return inner_.store(path, copy->data(), copy->size()); }, size);
}

std::string WriteBehindSink::append(const std::string& path, const std::uint8_t* data, std::size_t size) {
  auto copy = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
  return push([this, path, copy] { return inner_.append(path, copy->data(), copy->size()); }, size);
}

std::string WriteBehindSink::rename_path(const std::string& from, const std::string& to) {
  if (std::string failure = drain(); !failure.empty()) return failure;
  return inner_.rename_path(from, to);
}

std::string WriteBehindSink::read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) {
  if (std::string failure = drain(); !failure.empty()) return failure;
  return inner_.read_file(path, out, max_bytes);
}

std::vector<std::string> WriteBehindSink::list_children(const std::string& path, std::string* error) {
  if (std::string failure = drain(); !failure.empty()) {
    *error = failure;
    return {};
  }
  return inner_.list_children(path, error);
}

// 改名、删除不多，调用方有的不管删没删掉，排队出错会连累后面的操作，所以同步做。
std::string WriteBehindSink::remove_file(const std::string& path) {
  if (std::string failure = drain(); !failure.empty()) return failure;
  return inner_.remove_file(path);
}

std::string WriteBehindSink::remove_dir(const std::string& path) {
  if (std::string failure = drain(); !failure.empty()) return failure;
  return inner_.remove_dir(path);
}

std::string WriteBehindSink::remove_tree(const std::string& path) {
  if (std::string failure = drain(); !failure.empty()) return failure;
  return inner_.remove_tree(path);
}

}  // namespace panplay
