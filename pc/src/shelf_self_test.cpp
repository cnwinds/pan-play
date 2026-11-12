#include "console_packages.hpp"
#include "shelf_library.hpp"
#include "shelf_plan.hpp"
#include "shelf_transfer.hpp"

#include "panplay/app_paths.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {

int fail(const std::string& message) {
  std::cerr << "资源库自检失败: " << message << "\n";
  return 1;
}

constexpr std::int64_t kGb = 1ll << 30;

// 内存里的网盘：路径到文件（目录 size 为 -1）。分享按 surl 存一组相对路径和大小。
class FakeDrive : public panplay::DrivePlugin {
 public:
  struct Node {
    std::int64_t size = -1;
    std::string text;
  };
  struct Share {
    std::vector<std::pair<std::string, std::int64_t>> files;
    bool dead = false;
  };

  std::map<std::string, Node> nodes;
  std::map<std::string, std::string> md5s;
  std::map<std::string, Share> shares;
  std::set<std::string> need_verify;
  int transfers = 0;

  void add_file(const std::string& path, std::int64_t size, const std::string& text = "") {
    add_dirs(parent(path));
    nodes[path] = {size, text};
  }
  bool has(const std::string& path) const { return nodes.count(path) != 0; }

  std::string id() const override { return "fake"; }
  std::string display_name() const override { return "fake"; }
  panplay::LoginChallenge start_login() override { return {}; }
  panplay::LoginPoll poll_login(const panplay::LoginChallenge&) override { return {}; }
  bool session_valid() override { return true; }
  void logout() override {}

  std::vector<panplay::DriveEntry> list(const std::string& path, std::string* error) override {
    const auto dir = nodes.find(path);
    if (path != "/" && (dir == nodes.end() || dir->second.size >= 0)) {
      if (error) *error = "百度列目录失败 errno=-9";
      return {};
    }
    std::vector<panplay::DriveEntry> out;
    for (const auto& [child, node] : nodes) {
      if (child == path || parent(child) != path) continue;
      panplay::DriveEntry entry;
      entry.file_id = child;
      entry.name = child.substr(child.find_last_of('/') + 1);
      entry.path = child;
      entry.is_dir = node.size < 0;
      entry.size = node.size < 0 ? 0 : (node.text.empty() ? node.size : static_cast<std::int64_t>(node.text.size()));
      if (const auto md5 = md5s.find(child); md5 != md5s.end()) entry.content_hash = md5->second;
      out.push_back(entry);
    }
    return out;
  }

  panplay::ReadResult read_at(const std::string& file_id, std::int64_t, std::size_t) override {
    panplay::ReadResult result;
    const auto node = nodes.find(file_id);
    if (node == nodes.end()) result.error = "没有这个文件";
    else result.bytes.assign(node->second.text.begin(), node->second.text.end());
    return result;
  }

  std::string remove_path(const std::string& path, std::string* moved_to) override {
    if (moved_to) moved_to->clear();
    if (!has(path)) return "没有 " + path;
    if (need_verify.count(path) != 0) {
      if (moved_to) *moved_to = "/盘游/待删除";
      return move_path(path, "/盘游/待删除/" + path.substr(path.find_last_of('/') + 1));
    }
    for (auto it = nodes.begin(); it != nodes.end();) {
      it = it->first == path || it->first.rfind(path + "/", 0) == 0 ? nodes.erase(it) : std::next(it);
    }
    return "";
  }

  std::string move_path(const std::string& from, const std::string& to) override {
    if (!has(from) || has(to)) return "移动失败";
    add_dirs(parent(to));
    std::map<std::string, Node> moved;
    for (auto it = nodes.begin(); it != nodes.end();) {
      if (it->first == from || it->first.rfind(from + "/", 0) == 0) {
        moved[to + it->first.substr(from.size())] = it->second;
        it = nodes.erase(it);
      } else {
        ++it;
      }
    }
    nodes.insert(moved.begin(), moved.end());
    return "";
  }

  panplay::ShareListing list_share(const std::string& surl, const std::string&) override {
    panplay::ShareListing listing;
    const auto share = shares.find(surl);
    if (share == shares.end() || share->second.dead) {
      listing.error = "链接已失效";
      listing.invalid = true;
      return listing;
    }
    for (const auto& [rel, size] : share->second.files) listing.files.push_back({rel, size, ""});
    return listing;
  }

  panplay::ShareTransfer transfer_share(const std::string& surl, const std::string& pwd, const std::string& dest) override {
    panplay::ShareTransfer transfer;
    const panplay::ShareListing listing = list_share(surl, pwd);
    if (!listing.error.empty()) {
      transfer.error = listing.error;
      transfer.invalid = true;
      return transfer;
    }
    ++transfers;
    add_dirs(dest);
    for (const auto& file : listing.files) {
      add_file(dest + file.path, file.size);
      const std::string top = file.path.substr(1, file.path.find('/', 1) - 1);
      if (std::find(transfer.names.begin(), transfer.names.end(), top) == transfer.names.end()) transfer.names.push_back(top);
    }
    return transfer;
  }

  panplay::DriveQuota quota() override {
    panplay::DriveQuota quota;
    quota.total = 100000 * kGb;
    for (const auto& [path, node] : nodes) quota.used += std::max<std::int64_t>(0, node.size);
    return quota;
  }

 private:
  static std::string parent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == 0 ? "/" : path.substr(0, slash);
  }
  void add_dirs(const std::string& path) {
    for (std::string dir = path; dir != "/" && !dir.empty(); dir = parent(dir)) {
      if (!has(dir)) nodes[dir] = {};
    }
  }
};

const char* kLibrary = R"({"version":1,"generated_at":1794393246,"library_dir":"/lib","games":[
 {"title_id":"PPSA00001","concept_id":11,"name":"目标","added_at":5,"variants":[
   {"format":"exfat","surl":"1s1","pwd":"abcd","version":"1.05","size":0,"checked_at":0},
   {"format":"unknown","surl":"1s1b","pwd":"","size":5,"alive":false,"checked_at":9,"review":{"playable":false,"note":"黑屏"}}]},
 {"title_id":"PPSA00002","name":"装在主机上","variants":[{"format":"folder","surl":"1s2","pwd":"x","alive":true}]},
 {"title_id":"PPSA00003","name":"很久没用","store_concept_id":33,"variants":[{"format":"exfat","surl":"1s3","pwd":"x"}]},
 {"title_id":"PPSA00004","name":"刚用过","info":{"cover":"https://img.test/c.jpg","description":"帖子里的介绍","publisher":"发行商","release_date":"2021-08-17"},
  "variants":[{"format":"exfat","surl":"1s4","pwd":"x"}]},
 {"title_id":"PPSA00010","name":"封面地址不对","info":{"cover":"javascript:void 0"},"variants":[{"format":"exfat","surl":"1s10","pwd":"x"}]},
 {"title_id":"PPSA00005","name":"别的设备在装","variants":[{"format":"exfat","surl":"1s5","pwd":"x"}]},
 {"title_id":"PPSA00006","name":"链接失效","variants":[{"format":"exfat","surl":"1s6","pwd":"x","alive":false}]},
 {"title_id":"PPSA00007","name":"分享打不开","variants":[{"format":"exfat","surl":"1s7","pwd":"x"}]},
 {"title_id":"","name":"没编号","variants":[{"format":"exfat","surl":"1s8"}]},
 {"title_id":"PPSA00009","name":"没有格式","variants":[]}]})";

int check_parse() {
  std::string error;
  const auto library = parse_shelf_library(kLibrary, &error);
  if (!library) return fail("解析失败：" + error);
  if (library->games.size() != 8) return fail("没编号、没格式的游戏应该跳过，实际 " + std::to_string(library->games.size()));
  if (library->find("PPSA00003")->store_concept_id != 33 || library->find("PPSA00001")->store_concept_id != 0) {
    return fail("借来的商店 conceptId 读错了");
  }
  const ShelfGameInfo& info = library->find("PPSA00004")->info;
  if (info.cover != "https://img.test/c.jpg" || info.description != "帖子里的介绍" || info.publisher != "发行商" ||
      info.release_date != "2021-08-17" || !library->find("PPSA00001")->info.empty()) {
    return fail("帖子资料读错了");
  }
  if (!library->find("PPSA00010")->info.cover.empty()) return fail("不是网址的封面应该丢掉");
  const ShelfGame* target = library->find("PPSA00001");
  if (!target || target->name != "目标" || target->concept_id != 11 || target->variants.size() != 2) return fail("目标游戏读错了");
  const ShelfVariant& dead = target->variants[1];
  if (dead.alive != 0 || !dead.reviewed || dead.playable || dead.review_note != "黑屏") return fail("失效、评价读错了");
  if (target->variants[0].alive != -1 || library->find("PPSA00002")->variants[0].alive != 1) return fail("alive 读错了");
  if (shelf_variant_dir("unknown") != "@other" || shelf_variant_dir("exfat") != "@exfat" || shelf_format_of("other") != "unknown") {
    return fail("格式和目录名对不上");
  }
  if (parse_shelf_library("{}", &error) || error.empty()) return fail("没有 games 应该报错");
  return 0;
}

AppSettings test_settings(std::uint64_t cap_gb) {
  AppSettings settings;
  settings.drive = "fake";
  settings.drives["fake"].library_path = "/lib";
  settings.drive_cap_gb = cap_gb;
  settings.drive_reserve_gb = 0;
  return settings;
}

void fill(FakeDrive& drive) {
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  drive.add_file("/lib/PPSA00002/@folder/a.rar", 20 * kGb);
  drive.add_file("/lib/PPSA00003/@exfat/a.rar", 25 * kGb);
  drive.add_file("/lib/PPSA00004/@exfat/a.rar", 25 * kGb);
  drive.add_file("/lib/PPSA00005/@exfat/a.rar", 25 * kGb);
  drive.add_file("/lib/PPSA00006/@exfat/a.rar", 25 * kGb);
  drive.add_file("/lib/PPSA00007/@exfat/a.rar", 25 * kGb);
  drive.add_file("/lib/MyGame/a.rar", 10 * kGb);
  drive.add_file("/盘游/在用/other.json", 0,
                 "{\"installing\":[{\"path\":\"/lib/PPSA00005/@exfat\",\"heartbeat\":" + std::to_string(now) +
                     "}],\"installed\":[\"/lib/PPSA00002\"],\"used\":{\"/lib/PPSA00003/@exfat\":100,"
                     "\"/lib/PPSA00004/@exfat\":" + std::to_string(now) + ",\"/lib/PPSA00007/@exfat\":50}}");
  for (const char* surl : {"1s2", "1s3", "1s4", "1s5"}) drive.shares[surl].files = {{"/a.rar", 1}};
  drive.shares["1s7"].dead = true;
  drive.shares["1s1"].files = {{"/对马岛/game.part1.rar", 20 * kGb}, {"/对马岛/game.part2.rar", 10 * kGb},
                               {"/对马岛/补丁01/patch.rar", 1}};
}

int check_transfer_and_room() {
  FakeDrive drive;
  fill(drive);
  panplay::CancelToken cancel;
  std::vector<std::string> said;
  const auto say = [&](const std::string& message) { said.push_back(message); };
  // 游戏区 155 GB，要装 30 GB，上限 150 GB：要腾出 35 GB。
  PrepareResult result = prepare_on_drive(drive, test_settings(150), "shelf-self-test", "/lib/PPSA00001/@exfat", {}, cancel, say);
  if (!result.error.empty() || !result.transferred) return fail("转存应该成功：" + result.error);
  // 先退装在主机上的（20 GB），再退最久没用的：分享打不开的跳过，接着是很久没用的（25 GB），够了就停。
  if (drive.has("/lib/PPSA00002")) return fail("装在主机上的应该先退");
  if (drive.has("/lib/PPSA00003")) return fail("很久没用的应该退掉，游戏目录一起删");
  if (!drive.has("/lib/PPSA00007/@exfat")) return fail("分享打不开的不能退");
  if (!drive.has("/lib/PPSA00004/@exfat")) return fail("腾够了就不该再退");
  if (!drive.has("/lib/PPSA00005/@exfat")) return fail("别的设备正在装的不能退");
  if (!drive.has("/lib/PPSA00006/@exfat")) return fail("链接失效的不能退");
  if (!drive.has("/lib/MyGame")) return fail("资源库里没有的不能退");
  if (!drive.has("/lib/PPSA00001/@exfat/game.part1.rar") || !drive.has("/lib/PPSA00001/@exfat/补丁01/patch.rar") ||
      drive.has("/lib/PPSA00001/@exfat/对马岛")) {
    return fail("外包目录应该去掉");
  }
  if (said.empty()) return fail("应该报告进度");

  // 已经在网盘里的不再转存。
  const int before = drive.transfers;
  result = prepare_on_drive(drive, test_settings(150), "shelf-self-test", "/lib/PPSA00001/@exfat", {}, cancel, say);
  if (!result.error.empty() || result.transferred || drive.transfers != before) return fail("网盘里有了不该再转存");

  // 链接失效的格式：不重试。
  result = prepare_on_drive(drive, test_settings(150), "shelf-self-test", "/lib/PPSA00001/@other", {}, cancel, say);
  if (result.error.empty() || result.retryable) return fail("链接失效应该报错且不重试");
  return 0;
}

int check_not_enough() {
  FakeDrive drive;
  fill(drive);
  drive.need_verify.insert("/lib/PPSA00002/@folder");
  panplay::CancelToken cancel;
  // 上限 50 GB：能退的全退了也不够；要安全验证的移进待删除，不算腾出空间。
  const PrepareResult result =
      prepare_on_drive(drive, test_settings(50), "shelf-self-test", "/lib/PPSA00001/@exfat", {"/lib/PPSA00004/@exfat"}, cancel,
                       [](const std::string&) {});
  if (result.error.find("网盘空间不够") == std::string::npos || result.error.find("待删除") == std::string::npos) {
    return fail("空间不够时应该说明原因：" + result.error);
  }
  if (drive.transfers != 0) return fail("空间不够不该转存");
  if (!drive.has("/盘游/待删除/@folder")) return fail("要安全验证的应该移进待删除");
  if (!drive.has("/lib/PPSA00004/@exfat")) return fail("本机队列里的不能退");
  return 0;
}

// 盘架的编排（ADR 0012）：PPSA03016 那样外包目录没挪干净、分卷散在两处，按 md5 照样对得上。
int check_plan() {
  const char* text = R"({"games":[{"title_id":"PPSA03016","name":"x","variants":[{"format":"unknown","surl":"1s","pwd":"p",
    "plan":{"rev":3,"layers":[{"role":"main","name":"主游戏","files":[
        {"path":"PPSA03016-app.part1.rar","size":100,"md5":"AA01"},
        {"path":"PPSA03016-app.part2.rar","size":100,"md5":"aa02"},
        {"path":"PPSA03016-app.part3.rar","size":7,"md5":""}]}],
      "copies":[{"from":{"path":"apr0.4.2.1/libSceAmpr.sprx","size":5,"md5":"bb01"},"to":"fakelib/libSceAmpr.sprx"}],
      "notes":["要用 ShadowMountPlus 1.7alpha12 或以上挂载"]}}]}]})";
  std::string error;
  const auto library = parse_shelf_library(text, &error);
  if (!library) return fail("带编排的资源库解析失败：" + error);
  const ShelfVariant* variant = library->variant("PPSA03016", "other");
  if (variant == nullptr || !variant->plan) return fail("@other 应该对上 unknown 格式和它的编排");
  const ShelfPlan& plan = *variant->plan;
  if (plan.rev != 3 || plan.layers.size() != 1 || plan.layers[0].files.size() != 3 || plan.copies.size() != 1 ||
      plan.notes.size() != 1) {
    return fail("编排读错了");
  }

  FakeDrive drive;
  const std::string dir = "/lib/PPSA03016/@other";
  drive.add_file(dir + "/PPSA03016-app.part1.rar", 100);
  drive.add_file(dir + "/PPSA03016-app.part2.rar", 100);
  drive.add_file(dir + "/PPSA03016 LZ4/PPSA03016-app.part3.rar", 7);
  drive.add_file(dir + "/PPSA03016 LZ4/使用说明.txt", 3);
  drive.add_file(dir + "/apr0.4.2.1/libSceAmpr.sprx", 5);
  drive.md5s[dir + "/PPSA03016-app.part1.rar"] = "aa01";
  drive.md5s[dir + "/PPSA03016-app.part2.rar"] = "AA02";
  drive.md5s[dir + "/apr0.4.2.1/libSceAmpr.sprx"] = "BB01";
  // 第 3 卷编排里没写 md5、路径也不同：只按路径对不上，要报缺。
  ResolvedPlan resolved = resolve_plan(drive, dir, plan, &error);
  if (!error.empty()) return fail("核对编排时列目录出错：" + error);
  if (resolved.missing.size() != 1 || resolved.missing[0] != "PPSA03016-app.part3.rar") return fail("没 md5、路径又不同的分卷应该报缺");

  // 给第 3 卷配上 md5：散在子目录里也认得出，一层三卷，放的文件对到网盘里那个。
  ShelfPlan with_md5 = plan;
  with_md5.layers[0].files[2].md5 = "cc03";
  drive.md5s[dir + "/PPSA03016 LZ4/PPSA03016-app.part3.rar"] = "CC03";
  resolved = resolve_plan(drive, dir, with_md5, &error);
  if (!resolved.missing.empty()) return fail("按 md5 应该都对得上：" + resolved.missing.front());
  if (resolved.plan.layers.size() != 1 || resolved.plan.layers[0].parts.size() != 3 ||
      resolved.plan.layers[0].parts[2].name != "PPSA03016-app.part3.rar" || resolved.plan.layers[0].format != "rar") {
    return fail("主游戏三卷没按卷序认成一组");
  }
  if (resolved.plan.copies.size() != 1 || resolved.plan.copies[0].target != "fakelib/libSceAmpr.sprx" ||
      resolved.plan.copies[0].file.path != dir + "/apr0.4.2.1/libSceAmpr.sprx") {
    return fail("要放的文件没对上");
  }

  // 没 md5 时按相对路径和大小认。
  ShelfPlan by_path = plan;
  by_path.layers[0].files[2].path = "PPSA03016 LZ4/PPSA03016-app.part3.rar";
  resolved = resolve_plan(drive, dir, by_path, &error);
  if (!resolved.missing.empty()) return fail("没 md5 时按路径应该对得上");
  return 0;
}

// 资源库第 2 版只放编排摘要，完整的按需到网盘 /盘游/编排/<编号>/<surl>.json 取，按版本号缓存在本机。
int check_plan_on_demand() {
  const char* text = R"({"version":2,"games":[{"title_id":"PPSA03016","name":"x","variants":[{"format":"unknown","surl":"1sumtest","pwd":"p",
    "plan":{"rev":4,"size":207,"layers":["主游戏"],"copies":1,"notes":["要用 ShadowMountPlus 挂载"]}}]}]})";
  std::string error;
  const auto library = parse_shelf_library(text, &error);
  const ShelfVariant* variant = library ? library->variant("PPSA03016", "other") : nullptr;
  if (variant == nullptr || !variant->plan || variant->plan->rev != 4 || variant->plan->complete() || variant->plan->notes.size() != 1) {
    return fail("编排摘要读错了");
  }
  const auto cache = panplay::app_data_dir() / "shelf-plans" / "1sumtest.json";
  std::error_code ec;
  std::filesystem::remove(cache, ec);
  const auto full_plan = [](int rev) {
    return std::string(R"({"rev":)") + std::to_string(rev) +
           R"(,"layers":[{"role":"main","name":"主游戏","files":[{"path":"a.part1.rar","size":200,"md5":"m1"}]}],)"
           R"("copies":[{"from":{"path":"x.sprx","size":7,"md5":"m2"},"to":"fakelib/x.sprx"}],"notes":["要用 ShadowMountPlus 挂载"]})";
  };

  FakeDrive drive;
  auto loaded = load_shelf_plan(drive, "PPSA03016", *variant, &error);
  if (loaded || error.empty()) return fail("网盘上没有编排时应该报错");
  drive.add_file("/盘游/编排/PPSA03016/1sumtest.json", 0, full_plan(3));
  error.clear();
  loaded = load_shelf_plan(drive, "PPSA03016", *variant, &error);
  if (loaded || error.find("版本") == std::string::npos) return fail("网盘上是旧版编排时不该拿来用");
  drive.add_file("/盘游/编排/PPSA03016/1sumtest.json", 0, full_plan(4));
  error.clear();
  loaded = load_shelf_plan(drive, "PPSA03016", *variant, &error);
  if (!loaded || !loaded->complete() || loaded->copies.size() != 1 || loaded->layers[0].files[0].md5 != "m1") {
    return fail("没取到完整编排：" + error);
  }
  // 取过一次后本机有缓存，网盘上的没了也照样用。
  drive.remove_path("/盘游/编排/PPSA03016/1sumtest.json", nullptr);
  loaded = load_shelf_plan(drive, "PPSA03016", *variant, &error);
  std::filesystem::remove(cache, ec);
  if (!loaded || loaded->rev != 4) return fail("同一版编排应该用本机缓存");
  return 0;
}

// 编排里要在主机上安装的包（packages）：解析、对到网盘里、下载后核对是这款游戏的 PS5 附加内容包、记下装没装好。
int check_packages() {
  const char* text = R"({"games":[{"title_id":"PPSA08560","name":"x","variants":[{"format":"exfat","surl":"1pk","pwd":"p",
    "plan":{"rev":2,"layers":[{"role":"main","name":"主游戏","files":[{"path":"g.part01.rar","size":100,"md5":"m1"}]}],
      "copies":[],"packages":[{"path":"DLC/Crack.pkg","size":9,"md5":"PK01"}],"notes":[]}}]}]})";
  std::string error;
  const auto library = parse_shelf_library(text, &error);
  const ShelfVariant* variant = library ? library->variant("PPSA08560", "exfat") : nullptr;
  if (variant == nullptr || !variant->plan || variant->plan->packages.size() != 1 || variant->plan->package_count != 1 ||
      variant->plan->packages[0].path != "DLC/Crack.pkg") {
    return fail("编排里的 packages 读错了");
  }
  const auto summary = parse_shelf_library(
      R"({"version":2,"games":[{"title_id":"PPSA08560","name":"x","variants":[{"format":"exfat","surl":"1pk","pwd":"p",
        "plan":{"rev":2,"layers":["主游戏"],"copies":0,"packages":1,"notes":[]}}]}]})",
      &error);
  const ShelfVariant* brief = summary ? summary->variant("PPSA08560", "exfat") : nullptr;
  if (brief == nullptr || !brief->plan || brief->plan->package_count != 1 || !brief->plan->packages.empty()) {
    return fail("编排摘要里包的个数读错了");
  }

  FakeDrive drive;
  const std::string dir = "/lib/PPSA08560/@exfat";
  drive.add_file(dir + "/g.part01.rar", 100);
  drive.add_file(dir + "/DLC/改了名的激活包.pkg", 9);
  drive.md5s[dir + "/g.part01.rar"] = "m1";
  drive.md5s[dir + "/DLC/改了名的激活包.pkg"] = "pk01";
  ResolvedPlan resolved = resolve_plan(drive, dir, *variant->plan, &error);
  if (!resolved.missing.empty() || resolved.plan.packages.size() != 1 ||
      resolved.plan.packages[0].path != dir + "/DLC/改了名的激活包.pkg") {
    return fail("要装的包没按 md5 对到网盘里");
  }

  // 包配置是明文 XML，放在 1 MB 分块的交界上也要找得到。
  const auto dir_on_disk = std::filesystem::temp_directory_path() / ("panplay-pkg-test-" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir_on_disk);
  const auto write_pkg = [&](const std::string& name, const std::string& magic, const std::string& config) {
    std::string bytes = magic + std::string((1 << 20) - 20, '\0') + config + std::string(64, '\0');
    const auto file = dir_on_disk / name;
    std::ofstream(file, std::ios::binary) << bytes;
    return file.u8string();
  };
  const std::string config = "<content-id>UP0006-PPSA08560_00-FULLGAMEUNLOCK00</content-id><content-type>PS5AC</content-type>";
  std::string content_id;
  const std::string good = check_unlock_package(write_pkg("good.pkg", "\x7F" "FIH", config), "PPSA08560", &content_id);
  const std::string other_game = check_unlock_package(write_pkg("other.pkg", "\x7F" "FIH", config), "PPSA00001", &content_id);
  std::string game_pkg_config = config;
  game_pkg_config.replace(game_pkg_config.find("PS5AC"), 5, "PS5GD");
  const std::string game_pkg = check_unlock_package(write_pkg("game.pkg", "\x7F" "FIH", game_pkg_config), "PPSA08560", &content_id);
  const std::string ps4 = check_unlock_package(write_pkg("ps4.pkg", "\x7F" "CNT", config), "PPSA08560", &content_id);
  std::error_code ec;
  std::filesystem::remove_all(dir_on_disk, ec);
  if (!good.empty()) return fail("PS5 附加内容包应该过：" + good);
  if (other_game.empty() || game_pkg.empty() || ps4.empty()) return fail("别的游戏的、整款游戏的、PS4 的包都不该过");

  // 记下要装的包：没装好时算待装，删掉游戏后清掉装过的记录。
  const std::string path = dir + "-selftest";
  note_console_packages(path, resolved.plan.packages);
  if (pending_console_packages(path) != 1) return fail("没装过的包应该算待装");
  if (console_packages_json(path).find("改了名的激活包.pkg") == std::string::npos) return fail("详情里应该列出要装的包");
  forget_console_packages(path);
  note_console_packages(path, {});
  if (pending_console_packages(path) != 0 || console_packages_json(path) != "[]") return fail("编排里没包了应该清掉记录");
  return 0;
}

}  // namespace

int run_shelf_self_test() {
  const auto saved = shelf_library();
  if (const int parse = check_parse()) return parse;
  if (const int planned = check_plan()) return planned;
  if (const int on_demand = check_plan_on_demand()) return on_demand;
  if (const int packages = check_packages()) return packages;
  use_shelf_library(parse_shelf_library(kLibrary, nullptr));
  int result = check_transfer_and_room();
  if (result == 0) result = check_not_enough();
  use_shelf_library(saved);
  return result;
}
