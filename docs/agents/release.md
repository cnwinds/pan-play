# 发布流程

每次发布都按这里的步骤走，不跳步。发布由 CI 完成：在 `main` 上推送 `v` 开头的标签后，`.github/workflows/build.yml` 编译 Linux、PS5、Windows 三个平台，Linux 上跑完整自测，全部通过后 `release` 任务建 GitHub Release，挂上 `panplay-<版本>-windows-x64.exe`、`panplay-<版本>-ps5.elf`、`panplay-<版本>-ps5-homebrew.zip` 和 `SHA256SUMS.txt`，说明文字由提交记录自动生成。

## 1. 定版本号

- 格式 `vX.Y.Z`，从上一个标签往上加：`git tag --sort=-creatordate | Select-Object -First 1`。
- 修问题、小改动加 Z；新增用户能感知的功能加 Y（Z 归零）；配置或数据目录不兼容的改动加 X。
- 版本号只体现在标签上，代码里没有要改的版本常量。

## 2. 整理工作区

- `git status` 必须能解释每一处改动。发布带上的是 `main` 上已提交的内容，未提交的不会进 Release。
- 不同来源的改动分开提交，一个提交讲一件事。同一个文件里混着几件事时，按改动块拆开暂存（`git apply --cached` 只放其中一部分），不要一把 `git add -A`。
- 提交说明沿用仓库习惯：`feat:` / `fix:` / `ci:` / `docs:` 加中文，写清用户能看到的变化，因为 Release 说明直接由它生成。
- 改了行为的，`README.md` 和 `docs/PRD.md` 要同步。

## 3. 本地验证

按 `docs/build.md` 在本机编出 `build/panplay.exe`，然后：

- `.\build\panplay.exe self-test` 通过。缺 WinRAR 或 7-Zip 跑不了全量时，至少把改到的模块单独编译并跑它的自检，并在发布记录里注明全量自测交给 CI。
- 改了界面或接口的，重启本机服务（`panplay.exe serve`，默认 http://127.0.0.1:8734 ），实际点一遍改到的地方。
- 运行中的 `build\panplay.exe` 会锁住文件：先编到 `build\panplay-new.exe`，停掉服务后再替换。别的进程（比如测试用的 `dummy-ftp`）还占着时，可以把旧文件改名为 `panplay-old.exe` 再放新文件进来。

## 4. 推送 main 并等 CI 变绿

```powershell
git push origin main
```

等 `main` 这次提交的 build 工作流成功再打标签。查看方式二选一：

- 装了 `gh`：`gh run list --branch main --limit 3`，`gh run watch <id>`。
- 没装 `gh`（公开仓库可匿名查）：

```powershell
(Invoke-RestMethod "https://api.github.com/repos/cnwinds/pan-play/actions/runs?per_page=4").workflow_runs |
  Select-Object id, head_branch, status, conclusion, html_url
```

失败就修好、重新提交推送，直到通过。

## 5. 打标签发布

标签打在刚通过 CI 的 `main` 提交上：

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

推送后会出现一个 `head_branch` 为 `vX.Y.Z` 的工作流运行，四个任务 `linux`、`ps5`、`windows`、`release` 都要成功。

## 6. 核对 Release

```powershell
$r = Invoke-RestMethod "https://api.github.com/repos/cnwinds/pan-play/releases/tags/vX.Y.Z"
$r.html_url; $r.assets | Select-Object name, size
```

必须有四个附件：`panplay-vX.Y.Z-windows-x64.exe`、`panplay-vX.Y.Z-ps5.elf`、`panplay-vX.Y.Z-ps5-homebrew.zip`、`SHA256SUMS.txt`。把 Release 链接、包含的提交和验证情况（本地自测范围、CI 结果）告诉用户。

## 出了问题

- 标签的工作流失败、Release 还没建出来：修好后推到 `main`，删掉标签重打同一个版本：`git tag -d vX.Y.Z`、`git push origin :refs/tags/vX.Y.Z`，再回到第 4 步。
- Release 已经建出来、有人可能下载过：不要改动或重打这个版本，修好后发下一个补丁版本。
