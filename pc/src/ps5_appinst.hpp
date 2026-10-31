#pragma once

#include <chrono>
#include <functional>
#include <string>

// 主机上注册、注销应用用的系统接口（libSceAppInstUtil），桌面图标和游戏注册共用。

// 固件主版本号，12.02 返回 12；以及 "12.02" 这样的文字。
unsigned firmware_major();
std::string firmware_text();

std::string hex_code(int code);

// 按 websrv、ShadowMountPlus 的顺序加载用户服务和安装库并初始化，成功后复用，失败下次再试。返回错误，成功为空。
// 必须在 with_privileged_auth 之外调用：顶着特权身份加载系统库会卡死，卡住时还占着模块加载锁，连新线程都开不了。
// 它和 with_privileged_auth 拿同一把锁，所以别的线程换着身份时这里会等着，不会撞上。
std::string load_app_inst_util();

// 正在运行的游戏（全屏应用）的 Title ID；没有游戏在运行、或者这个固件上查不了时为空。
// 和 load_app_inst_util 一样不能在 with_privileged_auth 里调用。
std::string running_app_title_id();

// 别的游戏在运行、又没让关掉它时 launch_app 返回的就是这句。
extern const char* const kOtherAppRunning;

// 在主机上启动应用（同 websrv）：取前台用户，有别的全屏应用在运行时 close_running 为 false 就不动它、
// 把它的 Title ID 写进 *running_title 并返回 kOtherAppRunning，为 true 时先关掉它、等它退出再启动。
// 盘游自己的页面（全屏应用或盖在上面的小应用）直接关掉；from_console 表示是在主机浏览器里点的，这时开着的小应用就是那个浏览器，也关掉。
// 和 load_app_inst_util 一样不能在 with_privileged_auth 里调用。返回错误，成功为空。
std::string launch_app(const std::string& title_id, bool close_running, bool from_console, std::string* running_title);

// 把进程身份临时换成能注册应用、挂载目录的特权身份，执行完换回。同一时间只有一个在执行。
std::string with_privileged_auth(const std::function<std::string()>& action);

// 退出前调用：等别的线程做完正在进行的注册、挂载、加载系统库，然后一直占着锁，之后谁也开始不了新的。
// 等了 wait 还没做完返回 false。
bool hold_privileged_ops(std::chrono::milliseconds wait);

// 注册 /user/app/<title_id> 下准备好的应用。固件 12 以下只注册这一个，12 起整体重新扫描。
// 先 load_app_inst_util，再在 with_privileged_auth 里调用。返回错误，成功（含已存在、已恢复）为空。
std::string register_app(const std::string& title_id);

// 注销应用。先 load_app_inst_util，再在 with_privileged_auth 里调用。返回错误，成功为空。
std::string uninstall_app(const std::string& title_id);
