# 0008 游戏启动时挂上 backport 的 fakelib

状态：已接受（2026-10-31）

## 背景

很多装好的游戏在主机上点开就报错。游戏要求的固件比主机新时，backport 里除了降级过的 `eboot.bin`、`sce_module`，常带一个 `fakelib`（或 `fakelib2`）目录，放的是能在这台主机上用的系统库。系统库是从游戏沙盒里的 `common/lib` 加载的，`fakelib` 光放在游戏目录里不起作用，要在游戏启动时叠到沙盒上。ShadowMountPlus 和独立的 BackPork 都做这件事，ADR 0003 当时把它列在「不做」里，盘游注册的游戏带了 `fakelib` 也用不上。

另外游戏在主机桌面上点图标启动，盘游看不到启动成没成功、`fakelib` 挂没挂上。

参考实现：

- BackPork（`main.c` 约 400 行）：kqueue 监视 `SceSysCore.elf`（`NOTE_FORK | NOTE_EXEC | NOTE_TRACK`），子进程执行时用 `sceKernelGetAppInfo` 取 Title ID，在 `/mnt/sandbox/<ID>_NNN/` 里找带 `common/lib` 的随机目录，把 `app0/fakelib` 用 unionfs 叠上去；游戏退出后卸掉。
- ShadowMountPlus（`src/sm_fakelib.c`、`src/sm_game_lifecycle.c`）：同样监视进程执行，另外注入 ShellCore 在沙盒刚建好、游戏进程还没起来时就挂上。后者要按固件维护 ShellCore 偏移表。它还做了几处细节：`fakelib2` 优先于 `fakelib`；挂载来源用真实游戏目录，不用沙盒里的 `app0` 别名；`libkernel.sprx` 不叠；游戏进程一退出就卸，不等沙盒消失，免得旧沙盒删不掉。

## 决定

### 监视游戏启动，挂上 fakelib

盘游启动时（恢复挂载之后）开一个常驻线程，照 BackPork 监视 `SceSysCore.elf` 的子进程，不注入 ShellCore。

1. 子进程执行时取 Title ID。只管盘游注册的游戏：`/user/app/<ID>/mount.lnk` 在、不是正式版。别的不碰。
2. 按 `mount.lnk` 找到真实游戏目录（镜像版是镜像挂载点里的游戏目录），依次找 `fakelib2`、`fakelib`，取第一个存在的。
3. `fakelib` 里有 `libkernel.sprx` 时改名为 `libkernel.sprx.panplay-skip`，不叠内核库（同 ShadowMountPlus），也不删用户的文件。
4. 在 `/mnt/sandbox/` 里找这个 Title ID 有 `app0` 的沙盒，不止一个时取编号最大的（同 BackPork），再找带 `common/lib` 的随机目录，用 unionfs 把 fakelib 叠上去，参数照 ShadowMountPlus（`copymode=transparent`、`notime`、`fnodup`），不接受时退回不带参数（同 BackPork）。同名库用 fakelib 的，其余照用系统的。
5. 用进程原来的身份挂（BackPork 就是这样），不去抢注册用的特权身份锁，免得碰上正在进行的注册而挂晚了；被拒绝时才在特权身份下重试。
6. 游戏进程退出就卸掉，卸不掉（忙）时隔半秒重试，30 秒后放弃并记日志。同一个游戏退出后马上以新进程重启时按新的执行事件再挂。
7. ShadowMountPlus 或 BackPork（按线程名 `backpork.elf`）在运行时盘游不挂，交给它们；详情页说明由谁挂。

已知限制：收到执行事件时游戏进程已经开始运行，理论上可能在叠上之前就加载了系统库。BackPork 和旧版 ShadowMountPlus 都这么做、实际能用。日志记下从收到事件到挂完的毫秒数，实机上出现输掉的游戏再考虑先暂停进程、挂完再继续。

### 启动经过

监视线程把盘游注册的游戏每一次启动的经过记在内存里：进程号、挂了哪个 fakelib、挂载错误和用时、进程多久后退出。同一游戏退出后 5 秒内又执行的算同一次（游戏自己换执行文件）。详情页据此显示一句话，比如「backport 库已挂上（用时 40 毫秒）」「启动后 4 秒就退出了，多半是崩溃」，日志里也记一行。

### 可测试的部分放在 core

选 fakelib 目录、找沙盒里的 `common/lib`、启动经过的记录规则不依赖主机，放在 `core/`（`game_launch`）里写自检；`pc/src/ps5_game_watch.cpp` 只剩调系统的部分（kqueue、`sceKernelGetAppInfo`、`nmount`）。

## 不做

- 注入 ShellCore 在沙盒建好时预先挂载。
- ShadowMountPlus 的全局 fakelib、往沙盒里挂 `/data` 和 U 盘、补系统库（emulators）。
- 读内核日志分析崩溃原因。
- 在盘游里启动游戏。试过照 websrv 调 `sceSystemServiceLaunchApp`，要先关掉正在运行的游戏和主机浏览器（浏览器是盖在全屏应用上面的小应用，不关掉游戏起来了还被挡着），代价和风险都不值得；游戏一律在主机桌面上启动。

## 影响

- 盘游退出时游戏还在运行的，叠上的 fakelib 留着，游戏退出后系统清理沙盒时一起没了。
- 不能和 BackPork、ShadowMountPlus 同时挂同一个游戏；它们在跑时盘游让给它们。
- unionfs 参数、挂载是否需要特权身份，要实机验证。
