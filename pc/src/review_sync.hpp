#pragma once

#include "panplay/drive.hpp"

#include <functional>
#include <memory>

// 评价经网盘在各设备之间同步，盘架也从这里读：本机的写到网盘 /盘游/评价/<设备>.json，别的设备的读回来。
// 只用当前网盘插件的登录态，不用配地址；没登录、网盘不支持写文件时什么都不做。见 ADR 0004。
void start_review_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive);
// 本机评价改了：过几秒传上去，连着改几次只传一回。
void review_changed();
// 打开游戏库时调用：几分钟没读过别的设备的评价，就在后台读一次。
void refresh_reviews_if_stale();
