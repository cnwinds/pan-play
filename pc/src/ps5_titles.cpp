#if defined(PANPLAY_PS5)

#include "ps5_titles.hpp"

#include "panplay/game_launch.hpp"
#include "panplay/json.hpp"
#include "panplay/trace.hpp"
#include "ps5_appinst.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppBase = "/user/app";
constexpr const char* kAppMetaBase = "/user/appmeta";
constexpr const char* kTrophyMetaBase = "/system_data/priv/appmeta";
constexpr const char* kSystemExBase = "/system_ex/app";
// /system_ex/app/<ID> 上最多卸几层 nullfs。正常只有一层，叠多了说明有别的工具在用，宁可报错也不硬来。
constexpr int kMaxNullfsLayers = 4;
// PS5 基于 FreeBSD 11，statfs 里记挂载来源的 f_mntfromname 只有 88 字节，长路径会被截断。
constexpr std::size_t kMountNameMax = 88;

// PS5 上 kinfo_proc 里进程号、线程名的位置（同 ShadowMountPlus、BackPork）。
constexpr std::size_t kKinfoPidOffset = 72;
constexpr std::size_t kKinfoThreadNameOffset = 447;

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

std::string errno_text() { return std::strerror(errno); }

bool valid_title_id(const std::string& id) {
  if (id.size() != 9) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    if (!std::isupper(static_cast<unsigned char>(id[i]))) return false;
  }
  for (std::size_t i = 4; i < 9; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(id[i]))) return false;
  }
  return true;
}

// 返回 Title ID，不能注册时把原因写进 *problem。
std::string read_title_id(const std::string& game_dir, std::string* problem) {
  const fs::path sce_sys = fs::u8path(game_dir) / "sce_sys";
  std::error_code ec;
  if (!fs::exists(sce_sys / "param.json", ec)) {
    *problem = fs::exists(sce_sys / "param.sfo", ec) ? "PS4 游戏暂时不能注册到主机桌面" : "游戏目录里没有 sce_sys/param.json";
    return "";
  }
  std::string error;
  const panplay::Json json = panplay::Json::parse(read_text(sce_sys / "param.json"), &error);
  const std::string id = json.str("titleId");
  if (!error.empty() || !valid_title_id(id)) {
    *problem = "sce_sys/param.json 里没有有效的 titleId";
    return "";
  }
  if (!fs::exists(fs::u8path(game_dir) / "eboot.bin", ec)) {
    *problem = "游戏目录里没有 eboot.bin";
    return "";
  }
  return id;
}

std::string app_dir(const std::string& id) { return std::string(kAppBase) + "/" + id; }
std::string link_path(const std::string& id) { return app_dir(id) + "/mount.lnk"; }
// 调注册接口前写下，成功后删掉。还在就说明上次注册没成功：appmeta 是盘游自己先复制过去的，不能拿它当注册成功的证据。
std::string pending_path(const std::string& id) { return app_dir(id) + "/panplay.pending"; }
std::string system_ex_path(const std::string& id) { return std::string(kSystemExBase) + "/" + id; }

std::string read_link(const std::string& id) {
  std::string text = read_text(link_path(id));
  const auto end = text.find_first_of("\r\n");
  if (end != std::string::npos) text.resize(end);
  return text;
}

// 同一个目录：字面一样，或者指向同一个文件（写法不同、经过符号链接）。
bool same_place(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  if (fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal()) return true;
  std::error_code ec;
  return fs::equivalent(fs::u8path(a), fs::u8path(b), ec) && !ec;
}

bool same_text_path(const std::string& a, const std::string& b) {
  return fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal();
}

// path 本身是挂载点时返回最上面那层的文件系统类型和来源，不是挂载点时 type 为空。
struct MountTop {
  std::string type;
  std::string from;
};

MountTop mount_top(const std::string& path) {
  struct statfs st {};
  if (statfs(path.c_str(), &st) != 0 || !same_text_path(st.f_mntonname, path)) return {};
  return {st.f_fstypename, st.f_mntfromname};
}

// 游戏目录正以 nullfs 挂在 /system_ex/app/<ID>。来源被截断时按前缀比。
bool mounted_from(const std::string& id, const std::string& game_dir) {
  const MountTop top = mount_top(system_ex_path(id));
  if (top.type != "nullfs") return false;
  if (top.from.size() + 1 >= kMountNameMax) return game_dir.compare(0, top.from.size(), top.from) == 0;
  return same_place(top.from, game_dir);
}

// 主机上装着这个游戏的正式版（PKG）：内置或扩展存储的 user/app/<ID>/app.pkg 在，或者 /system_ex/app/<ID> 挂的不是 nullfs。
bool official_install(const std::string& id) {
  std::error_code ec;
  for (const char* root : {"", "/mnt/ext0", "/mnt/ext1"}) {
    if (fs::exists(std::string(root) + "/user/app/" + id + "/app.pkg", ec)) return true;
  }
  const MountTop top = mount_top(system_ex_path(id));
  return !top.type.empty() && top.type != "nullfs";
}

// 主机上已经有这个应用，但不是按 mount.lnk 注册的（正式版、别的工具装的）。不注册、不注销，免得盖掉或删掉它。
// 带着盘游的 pending 标记的算自己的（注册到一半断掉）。
bool foreign_title(const std::string& id) {
  if (official_install(id)) return true;
  std::error_code ec;
  return fs::exists(std::string(kAppMetaBase) + "/" + id + "/param.json", ec) && read_link(id).empty() &&
         !fs::exists(pending_path(id), ec);
}

// 把 /system_ex/app/<ID> 上的 nullfs 一层层卸掉，直到不再是挂载点。碰到不是 nullfs 的挂载就停下报错，不动系统自己的。
// force 为 false 时遇到占用（游戏正在运行）直接报错。
std::string unmount_nullfs(const std::string& id, bool force) {
  const std::string target = system_ex_path(id);
  for (int layer = 0; layer < kMaxNullfsLayers; ++layer) {
    const MountTop top = mount_top(target);
    if (top.type.empty()) return "";
    if (top.type != "nullfs") return target + " 上挂的是 " + top.type + "，不是盘游挂的，先不动";
    if (unmount(target.c_str(), 0) == 0) continue;
    if (errno == EBUSY && !force) return kTitleBusyError;
    if (!force || unmount(target.c_str(), MNT_FORCE) != 0) return "卸不掉 " + target + " 上的挂载：" + errno_text();
  }
  return mount_top(target).type.empty() ? "" : target + " 上叠了太多层挂载，先不动";
}

// force：挂着别的来源时强卸。只有开机恢复时用，那时不会有游戏在跑；注册时游戏正在运行就报错。
std::string mount_game(const std::string& id, const std::string& game_dir, bool force) {
  if (mounted_from(id, game_dir)) return "";
  const std::string target = system_ex_path(id);
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec) return "建不了 " + target + "：" + ec.message();
  // 挂着别的来源（旧位置、ShadowMountPlus 挂的 U 盘副本）时换成这个目录。
  if (auto error = unmount_nullfs(id, force); !error.empty()) return error;
  const auto entry = [](const std::string& text) {
    return iovec{const_cast<char*>(text.c_str()), text.size() + 1};
  };
  const std::string fstype = "fstype", nullfs = "nullfs", from = "from", fspath = "fspath";
  iovec items[] = {entry(fstype), entry(nullfs), entry(from), entry(game_dir), entry(fspath), entry(target)};
  if (nmount(items, sizeof(items) / sizeof(items[0]), 0) != 0) {
    return "把 " + game_dir + " 挂到 " + target + " 失败：" + errno_text();
  }
  if (!fs::exists(target + "/eboot.bin", ec)) {
    unmount_nullfs(id, true);
    return "挂载后 " + target + " 里看不到 eboot.bin";
  }
  PANPLAY_DEBUG("已挂载 " + game_dir + " -> " + target);
  return "";
}

// 逐块读写：用 out << in.rdbuf() 时空文件会把输出流标成失败。
std::string copy_one_file(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  if (!in) return "读不了 " + from.u8string();
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) return "写不进 " + to.u8string();
  std::vector<char> buffer(64 << 10);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0 && !out.write(buffer.data(), got)) return "写不进 " + to.u8string();
  }
  if (in.bad()) return "读不了 " + from.u8string();
  out.close();
  if (!out) return "写不进 " + to.u8string();
  return "";
}

// 不用 fs::copy：主机上的 libc++ 复制文件可能用到这里没有的系统调用。
std::string copy_tree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return "建不了 " + to.u8string() + "：" + ec.message();
  for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path target = to / it->path().filename();
    std::error_code entry_ec;
    if (it->is_symlink(entry_ec)) continue;
    if (it->is_directory(entry_ec)) {
      if (auto error = copy_tree(it->path(), target); !error.empty()) return error;
    } else if (it->is_regular_file(entry_ec)) {
      if (auto error = copy_one_file(it->path(), target); !error.empty()) return error;
    }
  }
  if (ec) return "列不出 " + from.u8string() + "：" + ec.message();
  return "";
}

// ---- 镜像版游戏（exFAT、ffpkg）----
// 游戏文件夹里放的是一个镜像文件（见 docs/adr/0007）：用 LVD 把镜像挂成 /dev/lvdN，再按文件系统挂到 /mnt/panplay/<文件夹>，
// 镜像里的游戏目录就当文件夹版用（nullfs 挂到 /system_ex/app/<ID>、mount.lnk 指向它）。镜像外面的文件（backport、补丁）
// 挂好后写进镜像，所以镜像按读写挂。参数照 ShadowMountPlus（include/sm_mount_defs.h、src/sm_image.c）。

constexpr const char* kImageMountBase = "/mnt/panplay";
constexpr const char* kLvdControl = "/dev/lvdctl";
constexpr unsigned long kLvdAttach = 0xC0286D00;
constexpr unsigned long kLvdDetach = 0xC0286D01;
// 镜像外的文件写进镜像后记下当时的完成标记，标记没变就不再写。
constexpr const char* kOverlayStamp = ".panplay-overlay";
constexpr const char* kDoneMarker = ".panplay-done.json";

struct LvdLayer {
  std::uint16_t source_type;  // 1：普通文件
  std::uint16_t flags;        // 1：没有位图文件
  std::uint32_t reserved0;
  const char* path;
  std::uint64_t offset;
  std::uint64_t size;
  const char* bitmap_path;
  std::uint64_t bitmap_offset;
  std::uint64_t bitmap_size;
};

struct LvdAttachRequest {
  std::uint32_t io_version;
  std::int32_t device_id;  // 传 -1，返回分到的 /dev/lvdN 的 N
  std::uint32_t sector_size;
  std::uint32_t secondary_unit;
  std::uint16_t flags;
  std::uint16_t image_type;
  std::uint32_t layer_count;
  std::uint64_t device_size;
  LvdLayer* layers;
};
static_assert(sizeof(LvdAttachRequest) == 0x28, "LVD attach ioctl 的参数是 0x28 字节");

struct LvdDetachRequest {
  std::uint32_t reserved0;
  std::int32_t device_id;
  std::uint8_t reserved[0x20];
};
static_assert(sizeof(LvdDetachRequest) == 0x28, "LVD detach ioctl 的参数是 0x28 字节");

enum class ImageKind { none, exfat, ufs };

std::string lower_text(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

ImageKind image_kind(const std::string& name) {
  const std::string lower = lower_text(name);
  const auto ends = [&](const char* ext) {
    const std::size_t n = std::strlen(ext);
    return lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0;
  };
  if (ends(".exfat")) return ImageKind::exfat;
  if (ends(".ffpkg")) return ImageKind::ufs;
  return ImageKind::none;
}

// 游戏文件夹第一层里唯一的镜像文件，没有（或不止一个）时为空。
std::string find_image(const std::string& game_dir) {
  std::string found;
  std::error_code ec;
  for (fs::directory_iterator it(fs::u8path(game_dir), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || image_kind(it->path().filename().u8string()) == ImageKind::none) continue;
    if (!found.empty()) return "";
    found = it->path().u8string();
  }
  return found;
}

std::string image_mount_point(const std::string& game_dir) {
  return std::string(kImageMountBase) + "/" + fs::u8path(game_dir).filename().u8string();
}

// 镜像文件是哪一个。重装时新镜像先写进暂存区再改名替换，旧镜像还被 LVD 挂着，路径一样、文件不一样。
std::string image_identity(const std::string& image) {
  struct stat st {};
  if (::stat(image.c_str(), &st) != 0) return "";
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" + std::to_string(st.st_size);
}

// 挂上时记下挂的是哪个镜像文件。
std::string mounted_image_path(const std::string& mount_point) { return mount_point + ".image"; }

// 挂着的 /dev/lvdN 的 N，不是 LVD 挂的返回 -1。
int lvd_unit(const std::string& mount_point) {
  const MountTop top = mount_top(mount_point);
  if (top.from.rfind("/dev/lvd", 0) != 0) return -1;
  char* end = nullptr;
  const long unit = std::strtol(top.from.c_str() + 8, &end, 10);
  return end != top.from.c_str() + 8 && *end == '\0' ? static_cast<int>(unit) : -1;
}

bool wait_device(const std::string& device, bool present) {
  for (int i = 0; i < 100; ++i) {
    std::error_code ec;
    if (fs::exists(device, ec) == present) return true;
    usleep(100000);
  }
  return false;
}

void detach_lvd(int unit) {
  if (unit < 0) return;
  const int fd = open(kLvdControl, O_RDWR);
  if (fd < 0) return;
  LvdDetachRequest request{};
  request.device_id = unit;
  if (ioctl(fd, kLvdDetach, &request) != 0) panplay::log_warn("卸不掉 /dev/lvd" + std::to_string(unit) + "：" + errno_text());
  close(fd);
  wait_device("/dev/lvd" + std::to_string(unit), false);
}

std::string mount_image(const std::string& image, const std::string& mount_point) {
  const ImageKind kind = image_kind(image);
  struct stat st {};
  if (::stat(image.c_str(), &st) != 0 || st.st_size <= 0) return "读不了镜像 " + image + "：" + errno_text();
  std::error_code ec;
  fs::create_directories(mount_point, ec);
  if (ec) return "建不了 " + mount_point + "：" + ec.message();

  LvdLayer layer{};
  layer.source_type = 1;
  layer.flags = 1;
  layer.path = image.c_str();
  layer.size = static_cast<std::uint64_t>(st.st_size);
  LvdAttachRequest request{};
  request.device_id = -1;
  request.layer_count = 1;
  request.device_size = layer.size;
  request.layers = &layer;
  if (kind == ImageKind::exfat) {
    request.sector_size = 512;
    request.secondary_unit = 0x10000;
    request.flags = 0x14;  // 单个镜像、读写（ShadowMountPlus 的原始值 0x8 换算后）
    request.image_type = 0;
  } else {
    request.sector_size = 4096;
    request.secondary_unit = 4096;
    request.flags = 0x16;  // download-data、读写（原始值 0xC）
    request.image_type = 7;
  }
  const int fd = open(kLvdControl, O_RDWR);
  if (fd < 0) return "打不开 " + std::string(kLvdControl) + "：" + errno_text();
  const int attached = ioctl(fd, kLvdAttach, &request);
  const std::string attach_error = attached != 0 ? errno_text() : "";
  close(fd);
  if (attached != 0) return "镜像挂不上（LVD）：" + attach_error;
  const std::string device = "/dev/lvd" + std::to_string(request.device_id);
  if (request.device_id < 0 || !wait_device(device, true)) {
    detach_lvd(request.device_id);
    return "镜像挂上后没出现 " + device;
  }

  char message[256] = {};
  const auto entry = [](const char* text) { return iovec{const_cast<char*>(text), std::strlen(text) + 1}; };
  // 只有名字、没有值的选项，值那一项为空。
  const iovec none{nullptr, 0};
  std::vector<iovec> items = {entry("from"), entry(device.c_str()), entry("fspath"), entry(mount_point.c_str()),
                              entry("budgetid"), entry("game"), entry("async"), none, entry("noatime"), none};
  int flags = 0;
  if (kind == ImageKind::exfat) {
    items.insert(items.end(), {entry("fstype"), entry("exfatfs"), entry("large"), entry("yes"), entry("timezone"),
                               entry("static"), entry("ignoreacl"), none});
  } else {
    items.insert(items.end(), {entry("fstype"), entry("ufs")});
    flags = 0x10000000;
  }
  items.push_back(entry("errmsg"));
  items.push_back(iovec{message, sizeof(message)});
  if (nmount(items.data(), static_cast<unsigned>(items.size()), flags) != 0) {
    const std::string error = errno_text();
    detach_lvd(request.device_id);
    return "镜像挂不上：" + (message[0] != '\0' ? std::string(message) : error);
  }
  write_text(mounted_image_path(mount_point), image_identity(image));
  PANPLAY_DEBUG("已挂载镜像 " + image + " -> " + mount_point);
  return "";
}

std::string unmount_image(const std::string& mount_point) {
  if (mount_top(mount_point).type.empty()) return "";
  const int unit = lvd_unit(mount_point);
  if (unmount(mount_point.c_str(), 0) != 0) {
    if (errno == EBUSY) return kTitleBusyError;
    if (unmount(mount_point.c_str(), MNT_FORCE) != 0) return "卸不掉镜像 " + mount_point + "：" + errno_text();
  }
  detach_lvd(unit);
  std::error_code ec;
  fs::remove(mount_point, ec);
  fs::remove(mounted_image_path(mount_point), ec);
  return "";
}

// 镜像里的游戏目录：镜像根目录，或者根目录下唯一带 sce_sys/param.json 的那个子目录。
std::string image_game_root(const std::string& mount_point) {
  std::error_code ec;
  if (fs::exists(fs::u8path(mount_point) / "sce_sys" / "param.json", ec)) return mount_point;
  std::string found;
  for (fs::directory_iterator it(fs::u8path(mount_point), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_directory(entry_ec) || !fs::exists(it->path() / "sce_sys" / "param.json", entry_ec)) continue;
    if (!found.empty()) return mount_point;
    found = it->path().u8string();
  }
  return found.empty() ? mount_point : found;
}

// 把游戏文件夹里镜像以外的文件（backport、补丁，安装时已去掉外层目录）按原路径写进镜像里的游戏目录。
std::string copy_overlay(const fs::path& from, const fs::path& to, bool top, const std::string& image) {
  std::error_code ec;
  for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (top && (name.rfind(".panplay", 0) == 0 || same_text_path(it->path().u8string(), image))) continue;
    std::error_code entry_ec;
    if (it->is_symlink(entry_ec)) continue;
    if (it->is_directory(entry_ec)) {
      fs::create_directories(to / it->path().filename(), entry_ec);
      if (entry_ec) return "建不了 " + (to / it->path().filename()).u8string() + "：" + entry_ec.message();
      if (auto error = copy_overlay(it->path(), to / it->path().filename(), false, image); !error.empty()) return error;
    } else if (it->is_regular_file(entry_ec)) {
      if (auto error = copy_one_file(it->path(), to / it->path().filename()); !error.empty()) return error;
    }
  }
  if (ec) return "列不出 " + from.u8string() + "：" + ec.message();
  return "";
}

std::string apply_overlay(const std::string& game_dir, const std::string& root, const std::string& image) {
  const fs::path dir = fs::u8path(game_dir);
  const std::string marker = read_text(dir / kDoneMarker);
  // 换了镜像文件（整个重装）也要重新写。
  const std::string stamp = marker.empty() ? "" : image_identity(image) + "\n" + marker;
  if (!stamp.empty() && read_text(dir / kOverlayStamp) == stamp) return "";
  if (auto error = copy_overlay(dir, fs::u8path(root), true, image); !error.empty()) return "补丁写不进镜像：" + error;
  if (!stamp.empty()) write_text(dir / kOverlayStamp, stamp);
  panplay::log_info("已把 " + game_dir + " 里镜像外的文件写进镜像");
  return "";
}

// 主机要的游戏目录。游戏文件夹里是镜像时在镜像里：mount 为 true 时还没挂就挂上、把镜像外的文件写进去
// （要在 with_privileged_auth 里），为 false 时只算出挂上后在哪（没挂时就是挂载点）。
std::string console_source(const std::string& game_dir, bool mount, std::string* error) {
  const std::string image = find_image(game_dir);
  if (image.empty()) return game_dir;
  const std::string mount_point = image_mount_point(game_dir);
  // 挂着的是重装前的旧镜像：先卸掉它（连同 /system_ex/app/<ID> 上的 nullfs），再挂新的。旧镜像卸掉才腾得出空间。
  if (mount && !mount_top(mount_point).type.empty() && read_text(mounted_image_path(mount_point)) != image_identity(image)) {
    const std::string old_root = image_game_root(mount_point);
    std::string problem;
    const std::string id = read_title_id(old_root, &problem);
    panplay::log_info("镜像换过了，重新挂载：" + image);
    std::string failed = !id.empty() && mounted_from(id, old_root) ? unmount_nullfs(id, false) : "";
    if (failed.empty()) failed = unmount_image(mount_point);
    if (!failed.empty()) {
      *error = failed;
      return "";
    }
  }
  if (mount_top(mount_point).type.empty()) {
    if (!mount) return mount_point;
    if (auto failed = mount_image(image, mount_point); !failed.empty()) {
      *error = failed;
      return "";
    }
  }
  const std::string root = image_game_root(mount_point);
  if (mount) {
    if (auto failed = apply_overlay(game_dir, root, image); !failed.empty()) {
      *error = failed;
      return "";
    }
  }
  return root;
}

// 挂镜像要特权身份；不是镜像版时不切身份。
std::string mounted_source(const std::string& game_dir, std::string* error) {
  if (find_image(game_dir).empty()) return game_dir;
  // 两边同时读写挂同一个镜像会把镜像写坏。
  if (shadowmount_running()) {
    *error = "ShadowMountPlus 在运行，镜像版游戏的挂载、注册交给它；想让盘游来管，先停掉它";
    return "";
  }
  std::string source;
  *error = with_privileged_auth([&]() -> std::string {
    std::string failed;
    source = console_source(game_dir, true, &failed);
    return failed;
  });
  return error->empty() ? source : "";
}

bool appmeta_file(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  if (lower == "param.json" || lower == "param.sfo") return true;
  const auto dot = lower.rfind('.');
  if (dot == std::string::npos) return false;
  const std::string ext = lower.substr(dot);
  return ext == ".png" || ext == ".dds" || ext == ".at9";
}

// 照 ShadowMountPlus：sce_sys 整个放到 /user/app/<ID>，icon0.png 再放一份在外层；
// 主机列表显示用的几种文件放到 /user/appmeta/<ID>；奖杯资料放到 /system_data/priv/appmeta/<ID>（已有的不覆盖）。
std::string stage_metadata(const std::string& id, const std::string& game_dir) {
  const fs::path sce_sys = fs::u8path(game_dir) / "sce_sys";
  const fs::path app = fs::u8path(app_dir(id));
  if (auto error = copy_tree(sce_sys, app / "sce_sys"); !error.empty()) return error;
  std::error_code ec;
  if (fs::exists(sce_sys / "icon0.png", ec)) {
    if (auto error = copy_one_file(sce_sys / "icon0.png", app / "icon0.png"); !error.empty()) return error;
  }

  const fs::path appmeta = fs::path(kAppMetaBase) / id;
  fs::create_directories(appmeta, ec);
  if (ec) return "建不了 " + appmeta.u8string() + "：" + ec.message();
  for (fs::directory_iterator it(sce_sys, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || !appmeta_file(it->path().filename().u8string())) continue;
    if (auto error = copy_one_file(it->path(), appmeta / it->path().filename()); !error.empty()) return error;
  }

  const fs::path trophy = fs::path(kTrophyMetaBase) / id;
  for (const char* sub : {"trophy2", "uds"}) fs::create_directories(trophy / sub, ec);
  for (const char* file : {"trophy2/npbind.dat", "uds/npbind.dat", "param.json"}) {
    std::error_code file_ec;
    if (!fs::exists(sce_sys / file, file_ec) || fs::exists(trophy / file, file_ec)) continue;
    if (auto error = copy_one_file(sce_sys / file, trophy / file); !error.empty()) {
      panplay::log_warn("奖杯资料没复制过去：" + error);
    }
  }
  return "";
}

// 删掉盘游在 /user/app/<ID>、/user/appmeta/<ID> 里放的东西。固件 12 起每次整体扫描都会把 /user/app 下留着的目录注册回去。
// 不用 fs::remove_all，理由同 install_sink 的 remove_recursive。
std::string remove_tree(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (status.type() == fs::file_type::not_found) return "";
  if (ec) return "读不了 " + path.u8string() + "：" + ec.message();
  if (fs::is_directory(status)) {
    std::vector<fs::path> children;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
    if (ec) return "列不出 " + path.u8string() + "：" + ec.message();
    for (const auto& child : children) {
      if (auto error = remove_tree(child); !error.empty()) return error;
    }
  }
  if (!fs::remove(path, ec) && ec) return "删不掉 " + path.u8string() + "：" + ec.message();
  return "";
}

void remove_staged(const std::string& id) {
  if (!valid_title_id(id)) return;
  for (const std::string& dir : {app_dir(id), std::string(kAppMetaBase) + "/" + id}) {
    if (auto error = remove_tree(fs::u8path(dir)); !error.empty()) panplay::log_warn("清理 " + dir + "：" + error);
  }
}

// 旧版盘游在主机上装的游戏文件权限是 0644 之类，没有执行位，主机启动不了（新装的见 install_sink 的 open_up）。
// 注册时把整个游戏目录补成 0777，和 FTP 传上来的一样。返回补了多少个。
int open_up_tree(const std::string& game_dir) {
  int changed = 0;
  const auto fix = [&](const fs::path& path) {
    struct stat st {};
    if (::stat(path.string().c_str(), &st) == 0 && (st.st_mode & 0777) != 0777 && ::chmod(path.string().c_str(), 0777) == 0) ++changed;
  };
  const fs::path root = fs::u8path(game_dir);
  fix(root);
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (it->is_symlink(entry_ec)) {
      it.disable_recursion_pending();
      continue;
    }
    fix(it->path());
  }
  return changed;
}

bool eboot_runnable(const std::string& game_dir) {
  struct stat st {};
  const std::string eboot = game_dir + "/eboot.bin";
  return ::stat(eboot.c_str(), &st) == 0 && (st.st_mode & 0005) == 0005;
}

// mount.lnk 指向这个目录。镜像没挂上时 game_dir 是挂载点，游戏目录可能在镜像的子目录里，所以也认它下面的目录。
bool linked_to(const std::string& id, const std::string& game_dir) {
  const std::string link = read_link(id);
  if (same_place(link, game_dir)) return true;
  const std::string prefix = fs::u8path(game_dir).lexically_normal().u8string() + "/";
  return fs::u8path(link).lexically_normal().u8string().rfind(prefix, 0) == 0;
}

// 读不到游戏自己的 Title ID 时（param.json 坏了、被删了、镜像没挂上），按 mount.lnk 找注册到这个目录的应用。
std::string title_linked_to(const std::string& game_dir) {
  std::error_code ec;
  for (fs::directory_iterator it(kAppBase, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string id = it->path().filename().u8string();
    if (valid_title_id(id) && linked_to(id, game_dir)) return id;
  }
  return "";
}

// 镜像版游戏注册时记下游戏文件夹，开机恢复时先挂镜像。
std::string image_record_path(const std::string& id) { return app_dir(id) + "/panplay.image"; }

}  // namespace

const char* const kTitleBusyError = "游戏正在运行，先关掉游戏再试";

bool console_title_running(const std::string& game_dir) {
  std::string problem;
  const std::string source = console_source(game_dir, false, &problem);
  std::string id = read_title_id(source, &problem);
  if (id.empty()) id = title_linked_to(source);
  if (id.empty() || official_install(id)) return false;
  return running_app_title_id() == id;
}

int find_process(std::initializer_list<const char*> thread_names) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  std::vector<std::uint8_t> buffer;
  std::size_t size = 0;
  // 两次调用之间进程变多时第二次会报 ENOMEM，重试几次。
  bool listed = false;
  for (int attempt = 0; attempt < 4 && !listed; ++attempt) {
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) return -1;
    buffer.resize(size + size / 4);
    size = buffer.size();
    listed = sysctl(mib, 4, buffer.data(), &size, nullptr, 0) == 0;
    if (!listed && errno != ENOMEM) return -1;
  }
  if (!listed) return -1;
  for (std::size_t at = 0; at + kKinfoThreadNameOffset < size;) {
    int struct_size = 0;
    std::memcpy(&struct_size, buffer.data() + at, sizeof(struct_size));
    if (struct_size <= static_cast<int>(kKinfoThreadNameOffset) || at + static_cast<std::size_t>(struct_size) > size) break;
    const char* name = reinterpret_cast<const char*>(buffer.data() + at + kKinfoThreadNameOffset);
    const std::string thread(name, strnlen(name, static_cast<std::size_t>(struct_size) - kKinfoThreadNameOffset));
    for (const char* wanted : thread_names) {
      if (thread == wanted) {
        int pid = -1;
        std::memcpy(&pid, buffer.data() + at + kKinfoPidOffset, sizeof(pid));
        return pid;
      }
    }
    at += static_cast<std::size_t>(struct_size);
  }
  return -1;
}

bool shadowmount_running() { return find_process({"shadowmountplus.elf", "shadowmount.elf"}) > 0; }

bool backpork_running() { return find_process({"backpork.elf"}) > 0; }

std::string fakelib_mounter() {
  if (shadowmount_running()) return "ShadowMountPlus";
  if (backpork_running()) return "BackPork";
  return "panplay";
}

std::string managed_game_source(const std::string& title_id) {
  if (!valid_title_id(title_id) || official_install(title_id)) return "";
  return read_link(title_id);
}

ConsoleTitle inspect_console_title(const std::string& install_dir) {
  ConsoleTitle title;
  // 镜像版要挂上镜像才读得到游戏信息；挂过的不再挂。
  const std::string game_dir = mounted_source(install_dir, &title.problem);
  if (!title.problem.empty()) return title;
  title.title_id = read_title_id(game_dir, &title.problem);
  if (title.title_id.empty()) return title;
  const std::string& id = title.title_id;
  std::error_code ec;
  const bool known = fs::exists(std::string(kAppMetaBase) + "/" + id + "/param.json", ec);
  const bool pending = fs::exists(pending_path(id), ec);
  title.registered = known && !pending && same_place(read_link(id), game_dir);
  title.mounted = mounted_from(id, game_dir);
  // 镜像里文件的权限由挂载决定，补不了，不看。
  title.files_ok = game_dir != install_dir || eboot_runnable(game_dir);
  title.fakelib = panplay::pick_fakelib(fs::u8path(game_dir));
  if (!title.fakelib.empty()) title.fakelib_by = fakelib_mounter();
  if (foreign_title(id)) title.problem = "主机上已经有这个游戏（正式版或别的工具装的），不能再注册这个目录";
  else if (shadowmount_running()) title.problem = "ShadowMountPlus 在运行，注册交给它；想让盘游来管，先停掉它";
  return title;
}

std::string register_console_title(const std::string& install_dir) {
  std::string problem;
  const std::string game_dir = mounted_source(install_dir, &problem);
  if (!problem.empty()) return problem;
  const bool image = game_dir != install_dir;
  const std::string id = read_title_id(game_dir, &problem);
  if (id.empty()) return problem;
  if (foreign_title(id)) return "主机上已经有这个游戏（正式版或别的工具装的），不能再注册这个目录";
  if (shadowmount_running()) return "ShadowMountPlus 在运行，先停掉它再让盘游注册";
  const std::string previous = read_link(id);
  std::error_code check_ec;
  // 原来已经确认注册好的（重新注册、换目录）：还没走到注册接口就失败时要恢复原样，不能把它降级成没注册。
  const bool previously_confirmed = !previous.empty() && !fs::exists(pending_path(id), check_ec) &&
                                    fs::exists(std::string(kAppMetaBase) + "/" + id + "/param.json", check_ec);
  panplay::log_info("注册到主机：" + id + "，" + game_dir +
                    (!previous.empty() && !same_place(previous, game_dir) ? "（原来注册在 " + previous + "）" : ""));
  if (auto error = load_app_inst_util(); !error.empty()) return error;
  const std::string error = with_privileged_auth([&]() -> std::string {
    std::error_code ec;
    fs::create_directories(app_dir(id), ec);
    if (ec) return "建不了 " + app_dir(id) + "：" + ec.message();
    if (const int fixed = image ? 0 : open_up_tree(game_dir); fixed > 0) {
      panplay::log_info("补上了 " + std::to_string(fixed) + " 个文件、目录的权限：" + game_dir);
    }
    // mount.lnk 和标记最先写：中途出错、断电时也认得出是盘游的、算没注册上，删除时能清理干净。
    if (!write_text(pending_path(id), "1")) return "写不进 " + pending_path(id);
    if (!write_text(link_path(id), game_dir)) return "写不进 " + link_path(id);
    if (image && !write_text(image_record_path(id), install_dir)) return "写不进 " + image_record_path(id);
    if (!image) fs::remove(image_record_path(id), ec);
    const auto undo = [&](const std::string& failure) {
      if (!previous.empty()) write_text(link_path(id), previous);
      if (previously_confirmed) fs::remove(pending_path(id), ec);
      return failure;
    };
    if (auto staged = stage_metadata(id, game_dir); !staged.empty()) return undo(staged);
    if (auto mounted = mount_game(id, game_dir, false); !mounted.empty()) return undo(mounted);
    if (auto registered = register_app(id); !registered.empty()) return registered;
    fs::remove(pending_path(id), ec);
    return "";
  });
  if (error.empty()) panplay::log_info("已注册到主机：" + id);
  else panplay::log_warn("注册到主机失败 " + id + "：" + error);
  return error;
}

namespace {

std::string unregister_dir(const std::string& game_dir) {
  std::string problem;
  std::string id = read_title_id(game_dir, &problem);
  if (id.empty()) id = title_linked_to(game_dir);
  if (id.empty()) return "";
  const bool linked = linked_to(id, game_dir);
  const bool mounted = mounted_from(id, game_dir);
  if (!linked && !mounted) return "";
  if (official_install(id)) {
    panplay::log_info(id + " 在主机上已经是正式版，不动主机那边，只删游戏目录");
    return "";
  }
  if (shadowmount_running()) {
    panplay::log_info("ShadowMountPlus 在运行，" + id + " 的注销交给它");
    return "";
  }
  std::error_code pending_ec;
  // 标记还在说明上次注册没确认成功：系统里可能有也可能没有这个应用。照样试着注销，失败也不拦着删除。
  const bool confirmed = !fs::exists(pending_path(id), pending_ec);
  bool can_uninstall = false;
  if (linked) {
    const std::string load_error = load_app_inst_util();
    if (!load_error.empty() && confirmed) return load_error;
    can_uninstall = load_error.empty();
  }
  panplay::log_info("从主机注销：" + id + "，" + game_dir);
  const std::string error = with_privileged_auth([&]() -> std::string {
    // 先卸掉挂载，免得系统注销时顺着挂载删到游戏文件；卸不干净（游戏在运行、有别的挂载）就不往下走。
    if (auto unmounted = unmount_nullfs(id, false); !unmounted.empty()) return unmounted;
    if (!linked) return "";
    if (can_uninstall) {
      if (auto uninstalled = uninstall_app(id); !uninstalled.empty()) {
        // 挂载已经卸下，这时在主机上删图标不会删到游戏文件。mount.lnk 留着，重启或重新注册能恢复。
        // 写回标记：系统里其实没有这个应用时注销会一直失败，再删一次就跳过注销，不会永远删不掉。
        if (confirmed) {
          write_text(pending_path(id), "1");
          return uninstalled + "。挂载已卸下，游戏暂时启动不了。主机上还有这个游戏的图标的话，先在主机上删掉它；"
                               "没有的话再点一次删除就会跳过注销";
        }
        panplay::log_info("注销 " + id + "：" + uninstalled + "（上次注册没确认成功，忽略）");
      }
    }
    remove_staged(id);
    return "";
  });
  if (error.empty()) panplay::log_info("已从主机注销：" + id);
  else panplay::log_warn("从主机注销失败 " + id + "：" + error);
  return error;
}

}  // namespace

// 镜像版注销完还要卸掉镜像，之后才能删镜像文件。
std::string unregister_console_title(const std::string& install_dir) {
  std::string unused;
  const std::string game_dir = console_source(install_dir, false, &unused);
  if (auto error = unregister_dir(game_dir); !error.empty()) return error;
  if (game_dir == install_dir) return "";
  const std::string mount_point = image_mount_point(install_dir);
  if (mount_top(mount_point).type.empty()) return "";
  const std::string error = with_privileged_auth([&]() { return unmount_image(mount_point); });
  if (!error.empty()) panplay::log_warn("卸镜像失败 " + mount_point + "：" + error);
  return error;
}

void restore_console_mounts() {
  if (shadowmount_running()) {
    panplay::log_info("ShadowMountPlus 在运行，游戏挂载交给它");
    return;
  }
  struct Pending {
    std::string id;
    std::string source;
    std::string install_dir;  // 镜像版：先挂镜像
  };
  std::vector<Pending> pending;
  std::error_code ec;
  for (fs::directory_iterator it(kAppBase, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string id = it->path().filename().u8string();
    if (!valid_title_id(id)) continue;
    const std::string source = read_link(id);
    if (source.empty() || official_install(id)) continue;
    std::error_code entry_ec;
    if (fs::exists(fs::u8path(source) / "eboot.bin", entry_ec)) {
      if (!mounted_from(id, source)) pending.push_back({id, source, ""});
      continue;
    }
    std::string install_dir = read_text(image_record_path(id));
    if (const auto end_of_line = install_dir.find_first_of("\r\n"); end_of_line != std::string::npos) {
      install_dir.resize(end_of_line);
    }
    if (!install_dir.empty() && !find_image(install_dir).empty()) {
      pending.push_back({id, source, install_dir});
      continue;
    }
    panplay::log_info("恢复挂载：" + id + " 的游戏目录不在（" + source + "），跳过");
  }
  if (pending.empty()) return;
  int restored = 0;
  with_privileged_auth([&]() -> std::string {
    for (const auto& [id, linked, install_dir] : pending) {
      std::string source = linked;
      if (!install_dir.empty()) {
        std::string error;
        source = console_source(install_dir, true, &error);
        if (!error.empty()) {
          panplay::log_warn("恢复挂载 " + id + " 时镜像挂不上：" + error);
          continue;
        }
      }
      if (auto error = mount_game(id, source, true); !error.empty()) {
        panplay::log_warn("恢复挂载 " + id + " 失败：" + error);
      } else {
        ++restored;
      }
    }
    return "";
  });
  panplay::log_info("恢复挂载 " + std::to_string(restored) + " / " + std::to_string(pending.size()) + " 个游戏");
}

#endif
