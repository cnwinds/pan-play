#pragma once

// 监视主机上游戏的启动和退出（见 docs/adr/0008）：盘游注册的游戏带 backport 的 fakelib 时，
// 游戏进程一执行就把它用 unionfs 叠到沙盒的系统库目录上，进程退出就卸掉；每次启动的经过记到 panplay::game_launches()。
// 盘游启动服务前、恢复挂载之后调用一次。找不到 SceSysCore 等情况下记一行警告，不监视。
void start_game_watch();
