#pragma once

#include <string>

// 把装好的游戏文件夹注册到主机桌面，和 ShadowMountPlus 对文件夹游戏的做法一致，两边可以互相接管：
//   sce_sys 复制到 /user/app/<ID>、显示用的文件复制到 /user/appmeta/<ID>、奖杯资料复制到 /system_data/priv/appmeta/<ID>，
//   游戏目录用 nullfs 挂到 /system_ex/app/<ID>（主机启动游戏时读这里），/user/app/<ID>/mount.lnk 记下源目录，再调注册接口。
// nullfs 挂载重启就没了，盘游每次启动时按 mount.lnk 恢复。只认 PS5 游戏（sce_sys/param.json）。

struct ConsoleTitle {
  std::string title_id;     // 从游戏的 sce_sys/param.json 读，读不到时为空
  bool registered = false;  // 主机上有这个应用，且 mount.lnk 指向这个游戏目录
  bool mounted = false;     // 游戏目录正挂在 /system_ex/app/<ID>
  std::string problem;      // 不能注册的原因，能注册时为空
};

ConsoleTitle inspect_console_title(const std::string& game_dir);

// 注册（已注册时重做一遍，用来修复、刷新版本信息）。主机上装着同一游戏的正式版时拒绝。返回错误，成功为空。
std::string register_console_title(const std::string& game_dir);

// 删除游戏前注销：先把 nullfs 一层层卸干净（游戏在运行就报错，不强卸），再注销，最后清掉盘游放在 /user/app、/user/appmeta 的东西。
// 确认注册过的注销失败时报错，调用方据此不删文件，挂载保持卸下、mount.lnk 留着以便恢复。
// 没注册到这个目录、主机上是正式版、ShadowMountPlus 在运行时不动主机那边。返回错误，成功为空。
std::string unregister_console_title(const std::string& game_dir);

// 盘游启动时按 /user/app/*/mount.lnk 把还没挂上的游戏目录挂回去。
void restore_console_mounts();

// ShadowMountPlus 在运行时注册、挂载都交给它，盘游不动手，免得两边抢同一个游戏。
bool shadowmount_running();
