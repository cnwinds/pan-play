#pragma once

#include "panplay/drive.hpp"

#include <functional>
#include <memory>
#include <string>

// 给盘架的反馈（ADR 0012）：装的时候发现编排对不上、照编排装失败、标了有问题、用户要求重新分析，
// 写到网盘 /盘游/反馈/<设备 ID>.json，盘架读到后重新分析。资源库里这种格式的编排版本变高了（盘架已经重新
// 分析过）或者过了 30 天，就把那条去掉。
void start_shelf_feedback_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive);

// kind：mismatch / install_failed / bad_review / manual。path 是网盘里的变体目录。资源库里没有这种格式时不记，
// 返回 false。同一条链接、同一种问题、同一版编排只记一条，再报只更新说明和时间。
bool report_to_shelf(const std::string& path, const std::string& kind, const std::string& detail);

// 这种格式有没有还没被盘架处理的反馈（详情页显示「已通知盘架」）。
bool shelf_feedback_pending(const std::string& path);
