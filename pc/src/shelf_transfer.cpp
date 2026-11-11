#include "shelf_transfer.hpp"

#include "detail_cache.hpp"
#include "drive_usage.hpp"
#include "game_sizes.hpp"
#include "shelf_library.hpp"

#include "panplay/library.hpp"
#include "panplay/trace.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

constexpr std::int64_t kGb = 1ll << 30;

std::string gb_text(std::int64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / kGb);
  return text;
}

std::string folder_name(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

const ShelfVariant* shelf_variant(const ShelfGame& game, const std::string& variant_format) {
  const std::string format = shelf_format_of(variant_format);
  for (const auto& variant : game.variants) {
    if (variant.format == format) return &variant;
  }
  return nullptr;
}

// 游戏目录里各个变体多大：容量缓存里齐全就用缓存，否则当场量。量不出来时为空。
std::vector<panplay::VariantSize> sizes_of(panplay::DrivePlugin& drive, const std::string& key, const std::string& dir) {
  if (const auto size = cached_game_size(key, dir)) {
    if (size->variants.empty()) return {{"", dir, size->bytes}};
    std::vector<panplay::VariantSize> out;
    for (const auto& variant : size->variants) {
      const auto own = variant.path == dir ? size : cached_game_size(key, variant.path);
      if (!own) {
        out.clear();
        break;
      }
      out.push_back({variant.format, variant.path, own->bytes});
    }
    if (!out.empty()) return out;
  }
  std::string error;
  auto measured = panplay::measure_game(drive, dir, &error);
  return error.empty() ? measured : std::vector<panplay::VariantSize>{};
}

// 分享里只有一个外包目录时，把它里面的东西挪到变体目录这一层，外包目录删掉，和盘架转存时一样：
// 不然主游戏的压缩包会被当成补丁子目录里的。
void flatten_wrapper(panplay::DrivePlugin& drive, const std::string& dir, const std::vector<std::string>& names) {
  if (names.size() != 1) return;
  std::string error;
  const auto entries = drive.list(dir, &error);
  if (!error.empty() || entries.size() != 1 || !entries.front().is_dir || entries.front().name != names.front()) return;
  const std::string wrapper = panplay::join_storage_path(dir, names.front());
  const auto kids = drive.list(wrapper, &error);
  if (!error.empty() || kids.empty()) return;
  int moved = 0;
  for (const auto& kid : kids) {
    if (drive.move_path(panplay::join_storage_path(wrapper, kid.name), panplay::join_storage_path(dir, kid.name)).empty()) {
      ++moved;
    }
  }
  const auto left = drive.list(wrapper, &error);
  if (error.empty() && left.empty()) {
    std::string moved_to;
    drive.remove_path(wrapper, &moved_to);
  }
  panplay::log_info("去掉外包目录「" + names.front() + "」：移出 " + std::to_string(moved) + "/" +
                    std::to_string(kids.size()) + " 项");
}

struct Candidate {
  std::string path;
  std::string game_dir;
  std::string title;
  std::int64_t bytes = 0;
  const ShelfVariant* variant = nullptr;
  bool installed = false;
  std::int64_t used = 0;
};

// 网盘空间不够装 need 这么大的游戏时，按 ADR 0011 挑游戏移出网盘。返回出错原因。
std::string make_room(panplay::DrivePlugin& drive, const AppSettings& settings, const std::string& key,
                      const ShelfLibrary& library, const std::string& target_dir, std::int64_t need,
                      const std::set<std::string>& busy, panplay::CancelToken& cancel,
                      const std::function<void(const std::string&)>& say) {
  const std::int64_t cap = static_cast<std::int64_t>(settings.drive_cap_gb) * kGb;
  const std::int64_t reserve = static_cast<std::int64_t>(settings.drive_reserve_gb) * kGb;
  std::string error;
  const auto folders = drive.list(settings.library_path(), &error);
  if (!error.empty()) return "列游戏库目录失败：" + error;

  std::string usage_error;
  const DriveUsage usage = read_drive_usage(drive, &usage_error);
  if (!usage_error.empty()) panplay::log_warn("没能读全别的设备的在用记录：" + usage_error);

  std::int64_t area = 0;
  std::vector<Candidate> candidates;
  for (const auto& folder : folders) {
    if (!folder.is_dir) continue;
    const std::string dir = folder.path.empty() ? panplay::join_storage_path(settings.library_path(), folder.name) : folder.path;
    const auto sizes = sizes_of(drive, key, dir);
    const ShelfGame* game = library.find(panplay::find_title_id(folder.name));
    for (const auto& size : sizes) {
      area += size.bytes;
      // 只退资源库里有这种格式、能再转回来的；没分格式的旧目录、自己放的、正在装的、要装的这款都不动。
      if (!game || size.format.empty() || dir == target_dir) continue;
      const ShelfVariant* variant = shelf_variant(*game, size.format);
      if (!variant || variant->alive == 0) continue;
      if (busy.count(size.path) != 0 || busy.count(dir) != 0 || usage.installing.count(size.path) != 0) continue;
      Candidate candidate{size.path, dir, game->name.empty() ? folder.name : game->name, size.bytes, variant};
      candidate.installed = usage.installed.count(dir) != 0;
      if (const auto at = usage.used.find(size.path); at != usage.used.end()) candidate.used = at->second;
      candidates.push_back(std::move(candidate));
    }
  }

  std::int64_t to_free = area + need - cap;
  const panplay::DriveQuota quota = drive.quota();
  if (quota.error.empty() && quota.total > 0) {
    to_free = std::max(to_free, reserve + need - (quota.total - quota.used));
  } else {
    panplay::log_warn("查不到网盘空间，只按游戏区上限算：" + quota.error);
  }
  if (to_free <= 0) return "";

  // 先退已经装在主机上的，再按最久没用过的；同样时先退大的，少删几个。
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    if (left.installed != right.installed) return left.installed;
    if (left.used != right.used) return left.used < right.used;
    return left.bytes > right.bytes;
  });
  panplay::log_info("网盘要腾出 " + gb_text(to_free) + " 才能装（游戏区 " + gb_text(area) + "，上限 " + gb_text(cap) +
                    "），能退的有 " + std::to_string(candidates.size()) + " 个");
  std::int64_t freed = 0;
  std::vector<std::string> removed;
  bool trashed = false;
  for (const auto& candidate : candidates) {
    if (freed >= to_free) break;
    if (cancel.cancelled()) return "已暂停";
    say("腾网盘空间：核对「" + candidate.title + "」的分享还能不能转回来…");
    // 退之前当场核对：退掉之后要能再转回来。
    const panplay::ShareListing share = drive.list_share(candidate.variant->surl, candidate.variant->pwd);
    if (!share.error.empty()) {
      panplay::log_info("不退「" + candidate.title + "」：分享链接" + (share.invalid ? "已经不能用了：" : "暂时打不开：") + share.error);
      continue;
    }
    say("腾网盘空间：移出「" + candidate.title + "」…");
    std::string moved_to;
    if (const std::string remove_error = drive.remove_path(candidate.path, &moved_to); !remove_error.empty()) {
      panplay::log_warn("移出「" + candidate.title + "」失败：" + remove_error);
      continue;
    }
    forget_game_detail(candidate.path);
    // 这款游戏的格式都退光了，游戏目录也删掉。
    std::string list_error;
    if (candidate.path != candidate.game_dir && drive.list(candidate.game_dir, &list_error).empty() && list_error.empty()) {
      std::string ignored;
      drive.remove_path(candidate.game_dir, &ignored);
    }
    if (!moved_to.empty()) {
      // 移进待删除目录不腾空间，接着挑下一个。
      trashed = true;
      panplay::log_warn("「" + candidate.title + "」百度要求安全验证、只能移到 " + moved_to + "，没腾出空间");
      continue;
    }
    freed += candidate.bytes;
    removed.push_back(candidate.title);
  }
  if (!removed.empty()) {
    std::string list;
    for (const auto& title : removed) list += (list.empty() ? "" : "、") + title;
    panplay::log_info("为了装 " + folder_name(target_dir) + "，移出网盘：" + list + "，腾出约 " + gb_text(freed));
  }
  if (freed >= to_free) return "";
  std::string message = "网盘空间不够：还差约 " + gb_text(to_free - freed) +
                        "。能再转回来的游戏都退出网盘了，自己放的、链接失效的、正在装的不会动。";
  if (trashed) message += "有的游戏百度要求安全验证、只移到了 /盘游待删除，在百度网盘 App 里删掉这个文件夹才会腾出空间。";
  message += "可以在设置里调高游戏占用上限，或者自己清理网盘。";
  return message;
}

}  // namespace

PrepareResult prepare_on_drive(panplay::DrivePlugin& drive, const AppSettings& settings, const std::string& sizes_key,
                               const std::string& path, const std::set<std::string>& busy, panplay::CancelToken& cancel,
                               const std::function<void(const std::string&)>& say) {
  PrepareResult out;
  std::string error;
  const auto entries = drive.list(path, &error);
  if (error.empty() && !entries.empty()) return out;
  if (!error.empty() && error.find("errno=-9") == std::string::npos) {
    out.error = error;
    // 没登录的等也没用；网络断了这类过一会儿再试。
    out.auto_resume = error.find("未登录") == std::string::npos;
    return out;
  }
  const std::string game_dir = panplay::game_dir_of(path);
  const auto library = shelf_library();
  const ShelfGame* game = library ? library->find(panplay::find_title_id(folder_name(game_dir))) : nullptr;
  const ShelfVariant* variant = game && path != game_dir ? shelf_variant(*game, panplay::variant_format(path)) : nullptr;
  if (!variant) {
    out.error = game ? "先选一种格式再装" : "网盘里没有这个游戏，盘架资源库里也没有";
    out.retryable = false;
    return out;
  }
  if (variant->alive == 0) {
    out.error = "盘架检查时这个分享链接已经失效，换别的格式试试";
    out.retryable = false;
    return out;
  }

  say("正在核对分享链接…");
  const panplay::ShareListing share = drive.list_share(variant->surl, variant->pwd);
  if (!share.error.empty()) {
    out.error = std::string("分享链接") + (share.invalid ? "已经不能用了：" : "暂时打不开：") + share.error;
    out.retryable = !share.invalid;
    out.auto_resume = !share.invalid;
    return out;
  }
  std::int64_t need = 0;
  for (const auto& file : share.files) need += file.size;
  if (cancel.cancelled()) return out;

  if (std::string room = make_room(drive, settings, sizes_key, *library, game_dir, need, busy, cancel, say); !room.empty()) {
    out.error = room;
    return out;
  }
  if (cancel.cancelled()) return out;

  say("正在转存到网盘（" + gb_text(need) + "）…");
  const panplay::ShareTransfer transfer = drive.transfer_share(variant->surl, variant->pwd, path);
  if (!transfer.error.empty()) {
    out.error = transfer.quota ? "网盘空间不够，转存失败：" + transfer.error : "转存失败：" + transfer.error;
    out.retryable = !transfer.invalid;
    out.auto_resume = !transfer.invalid && !transfer.quota;
    return out;
  }
  out.transferred = true;
  flatten_wrapper(drive, path, transfer.names);
  forget_game_detail(path);
  panplay::log_info("已从盘架资源库转存 " + path + "（" + gb_text(need) + "）");
  return out;
}
