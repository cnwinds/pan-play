# 0002 PS5 改为单个 elf，自己装桌面图标

状态：已接受（2026-10-17）

## 背景

PS5 版原来发两个包：`panplay.elf` 给 payload 加载器直接发送；`panplay-homebrew.zip` 放进 `/data/homebrew/`，在 websrv 的 Homebrew Launcher 里点图标启动。后者依赖 websrv，zip 里还要再带一份 elf，两份容易对不上版本，安装也要用 FTP 或 U 盘拷文件夹。

只在 websrv 的启动器里放一个打开网页的 `homebrew.js` 做不到：启动器把它放在 `sandbox="allow-scripts allow-modals"` 的隐藏 iframe 里运行，不能跳转顶层页面也不能开新窗口，`onclick` 的返回值只认 `{ path, args, cwd, daemon }` 用来启动 elf。

websrv 桌面上的 Homebrew Launcher 本身就是一个只打开网页的假应用：`/user/app/FAKE00000/sce_sys/` 下只有 `param.json`（`deeplinkUri` 指向 `http://127.0.0.1:8080`）和 `icon0.png`，由 websrv 启动时写入并调用 `sceAppInstUtilAppInstallTitleDir` 注册。

## 决定

- 只发一个 `panplay.elf`，用 payload 加载器发送。不再发 Homebrew Launcher 的 zip，不依赖 websrv。
- 启动服务之前检查桌面图标：`/user/app/PANP00001/sce_sys/` 下写 `param.json`（`deeplinkUri` 为 `http://127.0.0.1:8734`）和打包在 elf 里的 `icon0.png`，再注册。已注册且两个文件内容没变就跳过；被删掉或内容变了就重写重注册。已有盘游在运行时也先做这一步，所以重新发送一次 elf 就能把图标装回来。
- 加载和注册照 websrv、ShadowMountPlus 的做法：先加载 `libSceSystemService`、`libSceUserService` 并 `sceUserServiceInitialize`，再加载 `libSceAppInstUtil` 并 `sceAppInstUtilInitialize`，这几步都用进程原来的身份；然后把身份临时换成 `0x4800000000000006`（同 ShadowMountPlus），写文件、注册，再换回来。固件 12 以下按 NID `Wudg3Xe3heE` 找 `sceAppInstUtilAppInstallTitleDir` 只注册这一个目录；固件 12 起改用 `sceAppInstUtilAppInstallAll`。返回 `0x80990002`（已存在、已恢复）也算成功。
- 这几个库都不在链接时依赖，装图标时才 `dlopen`。SDK 的启动代码在进入 `main` 前加载所有链接依赖的库，有一个加载失败载荷就直接退出，日志里一行都没有。
- 装图标前在数据目录写 `dashboard-icon.pending`，做完删掉。下次启动看到它还在，说明上次卡住了，跳过一次并通知，盘游照常启动。

### 实测记录

- v0.4.0：链接时依赖 `libSceAppInstUtil`，没先加载用户服务库。发送 elf 后没有任何反应，日志一行都没有，载荷没进 `main`。
- v0.4.1：先切换身份到 `0x4801000000000013`，再 `dlopen("libSceAppInstUtil.sprx")`，卡在 `dlopen` 里不返回。卡住的线程占着模块加载锁，服务每个请求开新线程也要这把锁，所以浏览器一直转圈。
- 主机上按 `127.0.0.1` 打开，开了局域网口令也不用输入。

## 影响

- 点图标不会启动盘游，只打开网页。主机重启后要重新发送 elf，没发送时点图标是浏览器的连不上页面。
- 主机自带浏览器用手柄操作，界面目前按鼠标和触屏设计，方向键导航要另外补。
- 面向固件 11 及以上，各固件上的注册结果要实机验证。
