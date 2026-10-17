# 0002 PS5 改为单个 elf，自己装桌面图标

状态：已接受（2026-10-17）

## 背景

PS5 版原来发两个包：`panplay.elf` 给 payload 加载器直接发送；`panplay-homebrew.zip` 放进 `/data/homebrew/`，在 websrv 的 Homebrew Launcher 里点图标启动。后者依赖 websrv，zip 里还要再带一份 elf，两份容易对不上版本，安装也要用 FTP 或 U 盘拷文件夹。

只在 websrv 的启动器里放一个打开网页的 `homebrew.js` 做不到：启动器把它放在 `sandbox="allow-scripts allow-modals"` 的隐藏 iframe 里运行，不能跳转顶层页面也不能开新窗口，`onclick` 的返回值只认 `{ path, args, cwd, daemon }` 用来启动 elf。

websrv 桌面上的 Homebrew Launcher 本身就是一个只打开网页的假应用：`/user/app/FAKE00000/sce_sys/` 下只有 `param.json`（`deeplinkUri` 指向 `http://127.0.0.1:8080`）和 `icon0.png`，由 websrv 启动时写入并调用 `sceAppInstUtilAppInstallTitleDir` 注册。

## 决定

- 只发一个 `panplay.elf`，用 payload 加载器发送。不再发 Homebrew Launcher 的 zip，不依赖 websrv。
- elf 每次启动时检查桌面图标：`/user/app/PANP00001/sce_sys/` 下写 `param.json`（`deeplinkUri` 为 `http://127.0.0.1:8734`）和打包在 elf 里的 `icon0.png`，再注册。已注册且两个文件内容没变就跳过；被删掉或内容变了就重写重注册。已有盘游在运行时也先做这一步，所以重新发送一次 elf 就能把图标装回来。
- 注册前把进程身份临时换成系统界面的 `0x4801000000000013`，注册完换回原来的。注册单个目录的 `sceAppInstUtilAppInstallTitleDir` 没有导出名，按 NID `Wudg3Xe3heE` 查找，找不到时退回 `sceAppInstUtilAppInstallAll`，做法同 websrv。
- 主机上按 `127.0.0.1` 打开，开了局域网口令也不用输入。

## 影响

- 点图标不会启动盘游，只打开网页。主机重启后要重新发送 elf，没发送时点图标是浏览器的连不上页面。
- 主机自带浏览器用手柄操作，界面目前按鼠标和触屏设计，方向键导航要另外补。
- 面向固件 11 及以上，各固件上的注册结果要实机验证。
