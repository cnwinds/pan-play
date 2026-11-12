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

// 和 with_privileged_auth 一样，换的是 ShellCore 的身份：从安装包安装、查安装进度要用它。
std::string with_shellcore_auth(const std::function<std::string()>& action);

// 从主机上的安装包文件安装（激活、解锁用的附加内容包），调用后在后台装。已经装过的也算成功。
// 先 load_app_inst_util，再在 with_shellcore_auth 里调用。content_id 写包的内容 ID，查进度用。返回错误，成功为空。
std::string install_package(const std::string& file, const std::string& name, std::string* content_id);

// 安装进度：status 是 transferring / promoting / playable / error / none，出错时 error_code 是原因。
// 先 load_app_inst_util，再在 with_shellcore_auth 里调用。返回错误，查到了为空。
std::string package_install_status(const std::string& content_id, std::string* status, int* error_code);

// 安装接口的错误码换成一句话。
std::string installer_error_text(int code);
