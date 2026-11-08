#pragma once

#include "panplay/sink.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panplay {

// 写盘放到另一个线程：解压线程把写文件、建目录、记进度这些操作连同数据拷一份排进队列就接着解压，
// 写盘线程按顺序做。查大小、读文件、列目录、改名、删除前先等队列做完，再同步做，所以结果和同步写时一样；
// 记进度（追加 journal）排在文件 finish 之后，续装时记下的文件一定已经写完关好。
// 某个操作出错后，后面排队的操作不再做（关文件除外），之后每个调用都返回这个错误。
class WriteBehindSink final : public InstallSink {
 public:
  WriteBehindSink(InstallSink& inner, std::size_t queue_bytes);
  ~WriteBehindSink() override;
  WriteBehindSink(const WriteBehindSink&) = delete;
  WriteBehindSink& operator=(const WriteBehindSink&) = delete;

  // 等排着的操作都做完，返回第一个出错的原因。
  std::string drain();
  // 写盘线程花在做事上的毫秒数。
  double busy_ms() const;

  std::unique_ptr<FileWriter> open_writer(const std::string& path, std::int64_t size, std::string* error) override;
  std::string ensure_dir(const std::string& path) override;
  std::int64_t file_size(const std::string& path, std::string* error) override;
  std::string store(const std::string& path, const std::uint8_t* data, std::size_t size) override;
  std::string append(const std::string& path, const std::uint8_t* data, std::size_t size) override;
  std::string rename_path(const std::string& from, const std::string& to) override;
  std::string read_file(const std::string& path, std::vector<std::uint8_t>* out, std::size_t max_bytes) override;
  std::vector<std::string> list_children(const std::string& path, std::string* error) override;
  std::string remove_file(const std::string& path) override;
  std::string remove_dir(const std::string& path) override;
  std::string remove_tree(const std::string& path) override;

 private:
  class Writer;
  struct Op {
    std::function<std::string()> run;
    std::size_t bytes = 0;
    // 出错以后也要做：关掉文件、放掉句柄。
    bool cleanup = false;
  };

  // 排进队列；队列里的数据超过上限时等写盘线程腾出地方。返回之前出过的错。
  std::string push(std::function<std::string()> run, std::size_t bytes = 0, bool cleanup = false);
  void loop();

  InstallSink& inner_;
  const std::size_t queue_bytes_;
  mutable std::mutex mu_;
  std::condition_variable changed_;
  std::deque<Op> ops_;
  std::size_t queued_bytes_ = 0;
  // 排着的加上正在做的。
  int pending_ = 0;
  bool stopping_ = false;
  std::string error_;
  double busy_ms_ = 0;
  std::thread thread_;
};

}  // namespace panplay
