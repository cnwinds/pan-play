#pragma once

#include "panplay/drive.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 盘架编排里要在主机上安装的小 PKG（ADR 0012 的 packages）：激活完整版、解锁 DLC 的授权包。游戏装好、注册到主机后，
// 盘游把它下载到数据目录，核对是这款游戏的 PS5 附加内容包，再交给主机的安装接口装上。
// 装的结果按包的 md5 记在数据目录 console-packages.json，装好的不再装；每款游戏要装哪些也记着，详情页显示。

// 只收这么大以内的包，整款游戏的 PKG 不碰。盘架那边的上限一样。
constexpr std::int64_t kMaxConsolePackageBytes = 32ll << 20;

// 核对下载下来的包：PS5 的 PKG（\x7FFIH 开头）、类型是附加内容（PS5AC）、内容 ID 里有这款游戏的编号。
// 对得上返回空、content_id 写内容 ID；对不上返回原因。
std::string check_unlock_package(const std::string& file, const std::string& title_id, std::string* content_id);

// 读到编排时记下这款游戏（网盘里的变体目录）要装哪些包。
void note_console_packages(const std::string& path, const std::vector<panplay::DriveEntry>& packages);

// 记下的包里还有几个没在主机上装好。
int pending_console_packages(const std::string& path);

// 详情页用：[{"name","size","state":"installed|failed|pending|manual","error"}]，没有要装的包时是 []。
// manual 是电脑上的盘游：装不了主机安装包。
std::string console_packages_json(const std::string& path);

// 游戏目录里往下三层（第三层是文件）、32 MB 以内、核对得上的 .pkg，按路径排好。
std::vector<std::string> find_unlock_packages(const std::string& game_dir, const std::string& title_id);

// 压缩包里带着激活包的（文件夹格式常见：游戏目录/DLC/xxx.pkg）：装好、注册后在主机上的游戏目录里往下三层找
// 32 MB 以内、核对得上的 .pkg，装上并记下。核对不上的当游戏自己的数据文件，不碰。电脑上什么都不做。
// 返回失败的汇总，没有要装的或全部装好为空。
std::string install_game_dir_packages(const std::string& path, const std::string& game_dir, const std::string& title_id,
                                      const std::function<void(const std::string&)>& progress);

// 游戏从主机上删掉后调用：这款游戏（各种格式）装过的包都当成没装，再装游戏时重新装。
// 包本身不卸：注销游戏时系统可能已经一起删了；还在的话再装时系统回「已经装过」，照样算装好。
void forget_console_packages(const std::string& path);

// 把还没装好的包装到主机上，progress 报进度文字。只有主机上的盘游能装，电脑上返回原因。
// 每个包的结果都记下；返回失败的汇总，全部装好为空。
std::string install_console_packages(panplay::DrivePlugin& drive, const std::string& path, const std::string& title_id,
                                     const std::vector<panplay::DriveEntry>& packages,
                                     const std::function<void(const std::string&)>& progress);
