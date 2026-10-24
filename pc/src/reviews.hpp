#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 装过的游戏能不能玩，记在数据目录的 reviews.json。
// 按主游戏那组压缩包的内容指纹（content_fingerprint）认：网盘目录改名、删了再转存回来，评价还在；
// 同一个游戏的不同来源、不同打包各记各的。第一次用时读进内存，改了才写回。
// 本机的评价同步到网盘（review_sync），别的设备的也读回来；同一个指纹以最后评的为准，清除也算一次评价。

struct ReviewLayer {
  std::string fingerprint;
  std::string name;  // 第一个分卷的文件名，给人看
};

// 主游戏那组的一个分卷。md5 是网盘给的，和分享页里看到的一样，盘架转存前就能拿它认出同一份资源。
struct ReviewPart {
  std::string md5;
  std::int64_t size = 0;
  std::string name;
};

struct GameReview {
  std::string fingerprint;
  std::string title_id;
  std::string name;              // 评价时界面上的游戏名
  bool playable = true;
  std::string note;              // 有问题时的一句说明
  std::string firmware;          // 评价时主机的固件版本；在电脑上评价、读不到时为空
  std::string format;            // 评的是哪种格式（exfat、folder、unknown，同盘架）；游戏目录没按格式分时为空
  std::int64_t at = 0;           // 评价时间（Unix 秒）
  std::vector<ReviewLayer> layers;  // 当时装着的各组压缩包（主游戏、补丁）
  std::vector<ReviewPart> parts;    // 主游戏那组的各分卷
  bool removed = false;          // 清除评价：留一条记录，别的设备上更早的评价也跟着作废
  std::string device;            // 别的设备评的时是那台设备的名字，本机评的为空
};

std::optional<GameReview> find_review(const std::string& fingerprint);
// 同一个 Title ID 的其他版本的评价，新的在前。
std::vector<GameReview> reviews_of_title(const std::string& title_id, const std::string& except_fingerprint);
std::string save_review(const GameReview& review);
std::string remove_review(const std::string& fingerprint);
// 旧评价没记分卷的，打开详情读到后补上。返回有没有补。
bool fill_review_parts(const std::string& fingerprint, const std::vector<ReviewPart>& parts);

std::string review_json(const GameReview& review);

// 给同步用：本机全部评价（含清除记录）的文件内容，和改动的序号；传上去之后用这个序号标记已同步。
struct ReviewUpload {
  std::string document;
  std::uint64_t revision = 0;
};
std::optional<ReviewUpload> pending_review_upload(const std::string& device_id, const std::string& device_name);
void mark_reviews_uploaded(std::uint64_t revision);
// 换上从网盘读到的一台设备的评价。
void set_device_reviews(const std::string& device_id, const std::string& device_name,
                        const std::vector<GameReview>& reviews);
// 网盘上已经没有的设备，去掉它的评价。
void keep_devices(const std::vector<std::string>& device_ids);
std::vector<GameReview> parse_reviews(const std::string& document, std::string* device_id, std::string* device_name,
                                      std::string* error);
