// ps5-payload-dev/websrv 的自制程序扩展：在 Homebrew Launcher 里点盘游的图标时调用。
// 盘游是常驻的网页服务，必须用 daemon 方式启动。默认方式会把它当前台应用运行，
// 打开游戏时它会被结束，正在进行的安装也跟着中断。
async function main() {
  return {
    mainText: "盘游",
    secondaryText: "网盘游戏流式安装",
    onclick: async () => ({
      path: window.workingDir + "/eboot.elf",
      cwd: window.workingDir,
      daemon: true,
    }),
  };
}
