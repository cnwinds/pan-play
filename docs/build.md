# 从源码构建

日常使用下载 [Releases](https://github.com/cnwinds/pan-play/releases) 即可，这里是自己编译时用的。

## Windows

装了 CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\panplay.exe self-test
```

没装 CMake、用 MinGW 时：

```powershell
New-Item -ItemType Directory -Force -Path build | Out-Null
$unrar = @(
  "rar","strlist","strfn","pathfn","smallfn","global","file","filefn","filcreat",
  "archive","arcread","unicode","system","crypt","crc","rawread","encname","resource",
  "match","timefn","rdwrfn","consio","options","errhnd","rarvm","secpassword","rijndael",
  "getbits","sha1","sha256","blake2s","hash","extinfo","extract","volume","list","find",
  "unpack","headers","threadpool","rs16","cmddata","ui","largepage","filestr","scantree","dll","qopen","isnt","motw"
) | ForEach-Object { "third_party/unrar/$_.cpp" }
New-Item -ItemType Directory -Force -Path build\lzma | Out-Null
$lzma = @(
  "7zArcIn","7zBuf","7zCrc","7zCrcOpt","7zDec","7zStream","Bcj2","Bra","Bra86","BraIA64","CpuArch","Delta","LzmaDec","Lzma2Dec"
) | ForEach-Object {
  & "D:\tools\msys64\mingw64\bin\gcc.exe" -c -O2 "third_party/lzma/$_.c" -o "build/lzma/$_.o"
  "build/lzma/$_.o"
}
$src = @(Get-ChildItem core\src\*.cpp, pc\src\*.cpp | ForEach-Object { $_.FullName }) + $unrar + $lzma
& "D:\tools\msys64\mingw64\bin\g++.exe" -static -std=c++17 -O2 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -I core/include -I pc/src -I third_party/unrar -I third_party/lzma @src -lwinhttp -lws2_32 -lz -lole32 -luuid -ladvapi32 -lpowrprof -lshell32 -o build/panplay.exe
.\build\panplay.exe self-test
```

## Linux

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux
```

## PS5

需要 [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)（clang/lld 18 以上），产物是 `panplay.elf`，界面文件打包在里面：

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
$PS5_PAYLOAD_SDK/bin/prospero-cmake -S . -B build-ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ps5
```

改了 `ps5/icon.svg` 后重新生成桌面图标用的 `icon0.png`（512×512，编译时打包进 elf），比如用 Chrome：`chrome --headless=new --window-size=512,512 --screenshot=ps5/icon0.png file:///<仓库路径>/ps5/icon.svg`。

## 说明

- 自测需要 PATH 里有 `rar` 和 `7z`，或用 `PANPLAY_RAR`、`PANPLAY_7Z` 指定。Windows 上默认用 `C:\Program Files\WinRAR\Rar.exe` 和 `C:\Program Files\7-Zip\7z.exe`。
- Windows 和 Linux 以外的平台在 CMake 配置时下载 mbedTLS、zlib 和根证书包。
- 推送后 GitHub Actions 会编出三个平台的包；在 `main` 上推送 `v` 开头的标签会自动建 Release，流程见 [发布流程](agents/release.md)。
