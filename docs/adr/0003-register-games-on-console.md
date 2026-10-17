# 0003 PS5 上装完的游戏由盘游注册到主机，不再依赖 ShadowMountPlus

状态：已接受（2026-10-17）

## 背景

盘游把游戏解压成文件夹放进 `/data/homebrew/<游戏>`，之前要靠 ShadowMountPlus 每 15 秒扫描一次，再挂载、注册到主机桌面。实际用下来不可靠：扫描时文件可能还没写完，或时机不对就漏掉、注册失败；盘游删掉游戏后，桌面图标还留着。

ShadowMountPlus 的大部分代码在镜像挂载（`.ffpkg` / `.exfat` / PFS）、backport 的 fakelib 覆盖、kstuff 自动暂停、扫描循环上，这些盘游用不上。它对文件夹游戏的处理只有几步（见 `src/sm_install.c`、`src/sm_filesystem.c`）。

## 决定

PS5 上存放方式是本机文件夹时，盘游自己做 ShadowMountPlus 对文件夹游戏做的事，约定和它一致，两边可以互相接管：

1. `sce_sys` 复制到 `/user/app/<ID>/sce_sys`，`icon0.png` 再放一份在 `/user/app/<ID>/`。
2. `sce_sys` 里的 `param.json`、`param.sfo` 和 `.png`、`.dds`、`.at9` 复制到 `/user/appmeta/<ID>/`。
3. 奖杯资料（`trophy2/npbind.dat`、`uds/npbind.dat`、`param.json`）复制到 `/system_data/priv/appmeta/<ID>/`，已有的不覆盖。
4. 游戏目录用 nullfs 挂到 `/system_ex/app/<ID>`（`nmount` 的 `fstype=nullfs`、`from`、`fspath`），挂完确认能看到 `eboot.bin`。
5. `/user/app/<ID>/mount.lnk` 写源目录。
6. 注册：固件 12 以下用 `sceAppInstUtilAppInstallTitleDir`（按 NID 找），12 起用 `sceAppInstUtilAppInstallAll`。`0x80990002`（已存在、已恢复）也算成功。

`<ID>` 取游戏 `sce_sys/param.json` 的 `titleId`。只认 PS5 游戏；只有 `param.sfo` 的 PS4 游戏不注册，详情页说明原因。

- **装完自动注册**：安装结果是装好或已装时注册；已注册且挂载还在就不重做。失败原因记下来，详情页显示，可以点「重新注册」。
- **删除即注销**：先卸掉 `/system_ex/app/<ID>` 的挂载，再删 `mount.lnk`、调 `sceAppInstUtilAppUnInstall`，然后才删游戏文件。先卸挂载是为了防止系统注销时顺着挂载删到游戏文件。卸挂载失败就不删文件，免得留下打不开的图标。
- **开机恢复挂载**：nullfs 挂载重启就没了。盘游启动服务前，按 `/user/app/*/mount.lnk` 把源目录还在、还没挂上的游戏挂回去。ShadowMountPlus 注册过的游戏也一样恢复，所以从它切过来不用重装。
- **和 ShadowMountPlus 不同时动手**：按线程名 `shadowmountplus.elf`（或 `shadowmount.elf`）检测到它在运行时，盘游不注册、不挂载、不注销，只在详情页提示先停掉它。两边同时扫 `/data/homebrew` 会抢着挂载同一个游戏。
- 注册接口、身份切换和桌面图标共用 `ps5_appinst`：安装库在切换身份之前加载（顶着特权身份加载会卡死，见 ADR 0002 的实测记录），注册、挂载期间进程身份临时换成 `0x4800000000000006`，同一时间只有一个操作在换身份。

## 不做

- 改 `app.db` 补背景音乐信息（要引入 sqlite）。
- backport 的 fakelib 覆盖、kstuff 自动暂停、镜像格式。
- 固件 12 起逐个注册（ShadowMountPlus 借 SceShellCore 做的 TitleDir bridge）。

## 影响

- 盘游要像 ShadowMountPlus 一样每次开机加载（比如放进 plk-autoloader 的 `autoload.txt`），不然重启后点游戏图标进不去。kstuff 仍然需要。
- 固件 12 起每次注册都会让系统整体重新扫描 `/user/app`，比逐个注册慢。
- 注销接口会不会碰存档、各固件上注册是否都成功，要实机验证。
