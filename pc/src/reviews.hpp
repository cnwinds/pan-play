#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 装过的游戏能不能玩，记在数据目录的 reviews.json。
// 按主游戏那组压缩包的内容指纹（content_fingerprint）认：网盘目录改名、删了再转存回来，评价还在；
// 同一个游戏的不同来源、不同打包各记各的。第一次用时读进内存，改了才写回。

struct ReviewLayer {
  std::string fingerprint;
  std::string name;  // 第一个分卷的文件名，给人看
};

struct GameReview {
  std::string fingerprint;
  std::string title_id;
  std::string name;              // 评价时界面上的游戏名
  bool playable = true;
  std::string note;              // 有问题时的一句说明
  std::string firmware;          // 评价时主机的固件版本；在电脑上评价、读不到时为空
  std::int64_t at = 0;           // 评价时间（Unix 秒）
  std::vector<ReviewLayer> layers;  // 当时装着的各组压缩包（主游戏、补丁）
};

std::optional<GameReview> find_review(const std::string& fingerprint);
// 同一个 Title ID 的其他版本的评价，新的在前。
std::vector<GameReview> reviews_of_title(const std::string& title_id, const std::string& except_fingerprint);
std::string save_review(const GameReview& review);
std::string remove_review(const std::string& fingerprint);

std::string review_json(const GameReview& review);
