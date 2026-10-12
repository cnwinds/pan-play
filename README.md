# 盘游 Panplay

把百度网盘里的游戏分卷流式安装到主机存放目录。本期只有百度开放平台插件，网盘能力走统一接口。

需求见 [docs/PRD.md](docs/PRD.md)。

## 目录

- `core/`：网盘接口、扫描、ZIP/RAR 安装和 dummy FTP
- `pc/`：命令行和本机界面服务
- `ui/`：封面墙、详情和设置页
- `data/`：本地游戏资料库 `panplay-catalog.json`（Title ID → 名称、封面等）
- `scripts/build-catalog.py`：从 PlayStation-Titles 生成或更新资料库
- `third_party/unrar/`：随仓库带上的 UnRAR，用来解 RAR5

## 游戏资料库

名称与 Title ID 来自 [PlayStation-Titles](https://github.com/andshrew/PlayStation-Titles)（MIT，可离线携带）。只有目录名里能抽出 `CUSA` / `PPSA` 时才对照资料库；没有 Title ID 时保持网盘文件夹原名，不按目录名猜测。

**同步方式（全自动，无需 API 密钥）**：`panplay serve` 启动后会在后台自动从 PlayStation-Titles 拉取最新 Title ID 数据，写入 `data/panplay-catalog.json` 并热重载，**不会阻塞** `/api/games` 等 HTTP 请求。

| 时机 | 行为 |
|------|------|
| 启动时 | 若 catalog 不存在，或文件超过 **7 天**未更新 → 后台线程自动拉取并重建 |
| 运行中 | 每 **24 小时**检查一次；过期则后台同步，完成后热重载 |
| 失败时 | 保留现有 catalog，stderr 输出简短中文错误；登录与安装不受影响 |
| 首次无 catalog | 游戏列表仍可用（回退目录名），同步完成后下次请求自动 enriched |

设置页 **游戏资料库** 可查看「已同步 / 正在更新 / 上次更新时间」。`GET /api/catalog` 返回 `sync` 字段（phase、last_success 等）。

**手动脚本（可选）**：仍可用 `scripts/update-catalog.ps1` 或 `scripts/build-catalog.py` 离线生成或附带封面；生成后可在设置页点「重新加载资料库」或等下次自动周期。

**数据来源与许可**：

| 数据 | 来源 | 许可 / 说明 |
|------|------|-------------|
| Title ID、名称、conceptId、区域 | [PlayStation-Titles](https://github.com/andshrew/PlayStation-Titles) | MIT，随软件离线携带 |
| 封面 URL（可选） | [PROSPEROPatches](https://prosperopatches.com) / [ORBISPatches](https://orbispatches.com) | 公开页面抓取，非官方 API |

封面 URL 可选：加 `--fetch-covers` 时会从上述站点抓取 `icon0.webp` 地址写入 catalog（耗时较长）。未命中或未抓封面时，界面回退到纯色块占位。

```powershell
# 附带封面（可用 --cover-limit 限制条数）
.\scripts\update-catalog.ps1 --fetch-covers --platform ps5 --cover-limit 500
```

`POST /api/catalog/reload` 可在不重启服务的情况下从磁盘重读 JSON（不联网）。

## 这一版能做的事

- `panplay serve`：在 `http://127.0.0.1:8734` 打开界面。设置页填写 AppKey、SecretKey、游戏库目录和存放位置，只保存在这台电脑。
- `panplay login` / `ls` / `scan` / `install` / `read`：同一套安装流程的命令行
- `panplay dummy-ftp <端口> <根目录>`：在 `127.0.0.1` 上模拟主机 FTP 目录
- `panplay self-test`：检查 JSON、FTP 续写、ZIP 分卷续传，以及单卷 RAR 安装

开放平台授权后，游戏库一般在 `/apps/应用名`。能安装单卷和带分卷头的 ZIP（存储和 Deflate），以及 RAR。7z 会明确拒绝。写入规则是：远端文件大小等于已提交字节才 `APPE`；对不上就重新 `STOR` 覆盖。已校验通过的文件继续时不再重写。

## 构建

这台机器上用 MinGW：

```powershell
New-Item -ItemType Directory -Force -Path build | Out-Null
$unrar = @(
  "rar","strlist","strfn","pathfn","smallfn","global","file","filefn","filcreat",
  "archive","arcread","unicode","system","crypt","crc","rawread","encname","resource",
  "match","timefn","rdwrfn","consio","options","errhnd","rarvm","secpassword","rijndael",
  "getbits","sha1","sha256","blake2s","hash","extinfo","extract","volume","list","find",
  "unpack","headers","threadpool","rs16","cmddata","ui","largepage","filestr","scantree","dll","qopen","isnt","motw"
) | ForEach-Object { "third_party/unrar/$_.cpp" }
$src = @(Get-ChildItem core\src\*.cpp, pc\src\*.cpp | ForEach-Object { $_.FullName }) + $unrar
& "D:\tools\msys64\mingw64\bin\g++.exe" -std=c++17 -O2 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -I core/include -I pc/src -I third_party/unrar @src -lwinhttp -lws2_32 -lz -lole32 -luuid -ladvapi32 -lpowrprof -o build/panplay.exe
.\build\panplay.exe self-test
```

装了 CMake 时也可以：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

界面里保存密钥。开发时环境变量 `PANPLAY_BAIDU_APP_KEY` 和 `PANPLAY_BAIDU_SECRET_KEY` 可以覆盖本机设置。会话在 `%APPDATA%\panplay\baidu-session.json`。
