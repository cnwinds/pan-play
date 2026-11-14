#pragma once

#include "panplay/drive.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 盘架发布的资源库（网盘 /盘游/资源库.json）：盘架采集到的全部游戏和它们的分享链接。游戏库按它把网盘里还没有的
// 游戏也列出来，装的时候盘游自己转存。见 ADR 0011。

// 盘架用大模型排的安装编排（ADR 0012）。路径相对去掉外包目录后的分享根。
struct ShelfPlanFile {
  std::string path;
  std::int64_t size = 0;
  std::string md5;
};

struct ShelfPlanLayer {
  std::string role;
  std::string name;
  std::vector<ShelfPlanFile> files;
  // 只适用于这些主机固件主版本的层（backport），0 是这一头不限；两头都是 0 时哪种固件都装。
  int firmware_min = 0;
  int firmware_max = 0;
};

struct ShelfPlanCopy {
  ShelfPlanFile from;
  std::string to;
};

struct ShelfPlan {
  int rev = 0;
  // 按叠放顺序，第一层是主游戏。资源库里只有摘要时为空，要用 load_shelf_plan 取完整的。
  std::vector<ShelfPlanLayer> layers;
  std::vector<ShelfPlanCopy> copies;
  // 装好、注册到主机后要在主机上安装的小 PKG（激活完整版、解锁 DLC 的授权包）。
  std::vector<ShelfPlanFile> packages;
  // 要装几个包；资源库摘要里只有这个数，packages 要取完整编排才有。
  int package_count = 0;
  // 盘游做不了、要提醒用户的步骤。
  std::vector<std::string> notes;

  bool complete() const { return !layers.empty(); }
};

struct ShelfVariant {
  // exfat、folder、unknown，网盘里分别放在 @exfat、@folder、@other。
  std::string format;
  std::string surl;
  std::string pwd;
  std::string version;
  std::string firmware;
  // 分享里文件的总大小，盘架还没列过分享时为 0。
  std::int64_t size = 0;
  // 链接还能不能用：1 能用，0 失效，-1 盘架还没查过。
  int alive = -1;
  std::int64_t checked_at = 0;
  // 盘架把分享里的文件和盘游的评价对上了：就是评过的那份。
  bool reviewed = false;
  bool playable = true;
  std::string review_note;
  std::optional<ShelfPlan> plan;
};

// 商店里已经没有这款游戏的页面（下架了）时，盘架从资源站帖子里取的资料。cover 是图片地址。
struct ShelfGameInfo {
  std::string cover;
  std::string description;
  std::string publisher;
  std::string release_date;
  // 盘架传到网盘的那份封面（/盘游/封面/…）。主机常常连不上外面的图床，先读这份；盘架还没传时为空。
  std::string cover_file;

  bool empty() const { return cover.empty() && description.empty() && publisher.empty() && release_date.empty(); }
};

struct ShelfGame {
  std::string title_id;
  std::int64_t concept_id = 0;
  // 这款的商店页是空的、盘架借了同名游戏那页的资料时，那页的 conceptId；查商店用它。没借时为 0。
  std::int64_t store_concept_id = 0;
  std::string name;
  std::int64_t added_at = 0;
  ShelfGameInfo info;
  std::vector<ShelfVariant> variants;
};

struct ShelfLibrary {
  std::int64_t generated_at = 0;
  // 盘架的转存目录；和盘游的游戏库目录不一样时设置页提示。
  std::string library_dir;
  std::vector<ShelfGame> games;

  const ShelfGame* find(const std::string& title_id) const;
  // 网盘里变体目录对应的那种格式（variant_format 给 exfat、folder、other），没有时为空指针。
  const ShelfVariant* variant(const std::string& title_id, const std::string& variant_format) const;
};

std::shared_ptr<ShelfLibrary> parse_shelf_library(const std::string& text, std::string* error);

// 这种格式的完整编排：资源库里是旧格式、本来就带着时直接用；只有摘要时到网盘 /盘游/编排/<编号>/<surl>.json 取，
// 按版本号缓存在本机，同一版不再下载。没有编排时为空，取不到时 error 写原因。见 ADR 0012。
std::optional<ShelfPlan> load_shelf_plan(panplay::DrivePlugin& drive, const std::string& title_id,
                                         const ShelfVariant& variant, std::string* error);

// 去掉不适用于这台主机固件主版本的层（主游戏不动），返回去掉的层名。firmware_major 为 0（不知道主机固件）时什么都不去。
std::vector<std::string> drop_layers_for_firmware(ShelfPlan& plan, unsigned firmware_major);

// 资源库格式（unknown）和网盘里变体目录名（@other）互换。
std::string shelf_variant_dir(const std::string& format);
std::string shelf_format_of(const std::string& variant_format);

// 启动时先用上次存在本机的那份，之后在后台每 30 分钟看一次网盘上的文件有没有变。on_change 在读到新内容时调用。
void start_shelf_library_sync(std::function<std::shared_ptr<panplay::DrivePlugin>()> open_drive,
                              std::function<void()> on_change);
// 打开游戏库时调用：几分钟没看过就在后台看一次。
void refresh_shelf_library_if_stale();
// 当前的资源库，从没读到过时为空指针。
std::shared_ptr<const ShelfLibrary> shelf_library();
// 自测用：直接换上一份资源库。
void use_shelf_library(std::shared_ptr<const ShelfLibrary> library);
// 设置页显示的状态：读到了几款游戏、什么时候发布的、上次出了什么错。
std::string shelf_library_status_json(const std::string& library_path);
