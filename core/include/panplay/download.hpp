#pragma once

#include "panplay/drive.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace panplay {

// 下载用几条连接的结论（ADR 0009）。同一次安装的各个会话共用一份，服务端按设备存盘、下次安装开头直接用。
struct DownloadMemory {
  std::mutex mu;
  // 确认过（或拿不准而留下）的条数，和那时每条连接的速度（字节/秒，0 是不知道）。
  int lanes = 1;
  double lane_rate = 0;
  // 往上加被判成分流、退回了：从几条往上试的、当时的总速度、第几次、什么时候能再试（本进程单调时钟上的秒数，不存盘）。
  int failed_from = 0;
  double failed_total = 0;
  int failures = 0;
  double retry_at = 0;
};

struct DownloadOptions {
  // adaptive 时是条数上限：从记下的条数开始，按实测加减；否则固定用这么多条。
  int max_connections = 8;
  bool adaptive = true;
  // 缓冲上限（字节），0 按平台：PS5 128 MB，知道字典不超过 256 MB 后放到 256 MB；电脑上 512 MB。
  std::int64_t window_max = 0;
  // 同一条连接连续出错几次算下载失败，间隔从 retry_base_ms 开始翻倍。
  int max_retries = 5;
  int retry_base_ms = 1000;
  // 默认每份选项一个；复制选项时一起复制，所以同一次安装的各个会话共用。
  std::shared_ptr<DownloadMemory> memory = std::make_shared<DownloadMemory>();
};

// 要下载的一段：网盘文件 file_id 里从 offset 起 length 字节。file_size 是整个文件多大，段读到文件末尾时请求不设终点。
struct DownloadExtent {
  std::string file_id;
  std::int64_t offset = 0;
  std::int64_t length = 0;
  std::int64_t file_size = 0;
};

// 把若干段（通常是一组分卷）首尾相接当成一条流顺序读。后台连接按 ADR 0009 分段下载、调条数，读取方从内存取数据。
// 第一次 read 时才开始下载。只能由一个线程使用 read/seek。
class DownloadSession {
 public:
  DownloadSession(DrivePlugin& drive, std::vector<DownloadExtent> extents, const CancelToken* cancel,
                  DownloadOptions options = {});
  ~DownloadSession();
  DownloadSession(const DownloadSession&) = delete;
  DownloadSession& operator=(const DownloadSession&) = delete;

  // 返回读到的字节数，0 表示读完或出错（看 error）。取消时 error 为 "interrupted"。
  std::size_t read(std::uint8_t* dst, std::size_t size, std::string* error);
  std::string seek(std::int64_t position);
  std::int64_t position() const;
  std::int64_t size() const;
  // 解压器报出字典大小后调用：PS5 上字典小就把缓冲放大。
  void set_dictionary(std::uint64_t bytes);
  // 日志里的编号（下载 #N）。
  int id() const;
  // 现在用几条连接。
  int connections() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 记下的结论和设备上 download.json 的内容互转。只存条数和每条速度，退避是本次运行的事。
std::string download_memory_json(DownloadMemory& memory);
void load_download_memory(const std::string& json, DownloadMemory* memory);

}  // namespace panplay
