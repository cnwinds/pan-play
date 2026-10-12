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

游戏资料分两层，都以 `conceptId` 串起来：

1. **Title ID → conceptId、英文名**：来自 [PlayStation-Titles](https://github.com/andshrew/PlayStation-Titles)（MIT，作者每天自动刷新）。程序自带一份 `data/panplay-catalog.json`，自动同步的新版写在数据目录，两份取较新的。
2. **中文名、封面、类型、发行商、发售日、简介**：按 `conceptId` 读取 PlayStation 商店概念页，依次试港服简体 `zh-hans-hk`、港服繁体 `zh-hant-hk`、美服 `en-us`，结果缓存在数据目录的 `store-meta.json`，封面存到数据目录的 `covers/`。

只有目录名里能抽出 `CUSA` / `PPSA` 时才对照资料；没有 Title ID 时保持网盘文件夹原名，不按目录名猜测。展示名优先级是：手动改名 > 商店名称 > 英文名 > 目录名。手动改名保存在数据目录的 `name-overrides.json`。

**数据目录**（设置、网盘会话、资料库同步副本、商店资料缓存、封面都放这里）：

| 平台 | 默认位置 |
| --- | --- |
| Windows | `%APPDATA%\panplay` |
| PS5 载荷（编译时定义 `__PROSPERO__` 或 `-DPANPLAY_PS5`） | `/data/panplay` |
| Linux / 其他 | `$XDG_DATA_HOME/panplay`，否则 `~/.local/share/panplay` |

任何平台都可以用环境变量 `PANPLAY_DATA_DIR` 指定别的目录。

商店资料只在后台按需获取：封面墙先显示英文名，补齐后自动换成中文名和封面。每秒最多一次请求；取到的资料 30 天后刷新，未收录的 7 天后再试，网络失败不写缓存。命令行可以用 `panplay store-meta <conceptId>` 直接看某个概念页的解析结果。

**PlayStation-Titles 同步方式（全自动，无需 API 密钥）**：`panplay serve` 启动后会在后台自动拉取最新 Title ID 数据，写入数据目录的 `panplay-catalog.json` 并热重载，**不会阻塞** `/api/games` 等 HTTP 请求。上游停更或访问不了时，可以用环境变量 `PANPLAY_CATALOG_URL` 换成镜像地址，格式与 `All_Titles.json` 相同。

| 时机 | 行为 |
|------|------|
| 启动时 | 若 catalog 不存在，或文件超过 **7 天**未更新 → 后台线程自动拉取并重建 |
| 运行中 | 每 **24 小时**检查一次；过期则后台同步，完成后热重载 |
| 失败时 | 保留现有 catalog，stderr 输出简短中文错误；登录与安装不受影响 |
| 首次无 catalog | 游戏列表仍可用（回退目录名），同步完成后下次请求自动 enriched |

设置页 **游戏资料库** 可查看「已同步 / 正在更新 / 上次更新时间」。`GET /api/catalog` 返回 `sync` 字段（phase、last_success 等）。

**手动脚本（可选）**：仍可用 `scripts/update-catalog.ps1` 或 `scripts/build-catalog.py` 离线生成资料库；生成后可在设置页点「重新加载资料库」或等下次自动周期。脚本的 `--fetch-covers` 写入的包装图标只在商店没有封面时兜底。

**数据来源与许可**：

| 数据 | 来源 | 说明 |
|------|------|------|
| Title ID、英文名、conceptId、区域 | [PlayStation-Titles](https://github.com/andshrew/PlayStation-Titles) | MIT，本地携带并定期同步 |
| 中文名、封面、简介、类型、发行商 | PlayStation 商店概念页 | 按用户库里的 `conceptId` 逐个读取，结果只存本机 |

`POST /api/catalog/reload` 可在不重启服务的情况下从磁盘重读 JSON（不联网）。

## 这一版能做的事

- `panplay serve`：在 `http://127.0.0.1:8734` 打开界面。设置页填写 AppKey、SecretKey、游戏库目录和存放位置，只保存在这台电脑。
- `panplay serve 8734 --lan`：监听所有网卡，局域网里的手机也能打开。第一次打开要输访问口令，口令在数据目录的 `access-code.txt`。
- `panplay http-get <地址>`：排查网络，打印状态码、长度和跳转后的地址。
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
& "D:\tools\msys64\mingw64\bin\g++.exe" -std=c++17 -O2 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -I core/include -I pc/src -I third_party/unrar @src -lwinhttp -lws2_32 -lz -lole32 -luuid -ladvapi32 -lpowrprof -lshell32 -o build/panplay.exe
.\build\panplay.exe self-test
```

装了 CMake 时也可以：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

### Linux 和 PS5

Windows 以外的平台用自带的 HTTPS 客户端（mbedTLS），mbedTLS、zlib 和根证书包（curl.se 的 `cacert.pem`）在 CMake 配置时下载。Linux 上自测需要 PATH 里有 `rar`，或者用 `PANPLAY_RAR` 指定。

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux
```

PS5 载荷用 [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)（要 clang/lld 18 以上），产物是 `panplay.elf`，界面文件打包在里面：

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
$PS5_PAYLOAD_SDK/bin/prospero-cmake -S . -B build-ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ps5
```

载荷不带参数启动时，直接以局域网模式在 8734 端口开界面，主机右上角通知里给出地址和访问口令。数据在 `/data/panplay`，游戏装到「主机目录」设置的本机路径（默认 `/mnt/usb0/homebrew`）。

推送后 GitHub Actions（`.github/workflows/build.yml`）会编出 Linux 版并跑自测，再编出 PS5 的 `panplay.elf`，都在运行记录的 Artifacts 里下载。

界面里保存密钥。开发时环境变量 `PANPLAY_BAIDU_APP_KEY` 和 `PANPLAY_BAIDU_SECRET_KEY` 可以覆盖本机设置。会话在数据目录的 `baidu-session.json`。
