#pragma once

namespace panplay {

// 程序版本：发布版是标签名（如 v0.19.1），自己编的是 git 提交号，取不到时为「开发版」。编译时由 CMake 写入。
const char* app_version();

}  // namespace panplay
