#pragma once

#include "shelf_library.hpp"

#include "panplay/drive.hpp"
#include "panplay/install.hpp"

#include <string>
#include <vector>

// 把盘架的安装编排（ADR 0012）对到网盘里这个格式目录下的文件。
struct ResolvedPlan {
  panplay::InstallPlan plan;
  // 对不上的文件（编排里的路径），空的时候才能照编排装。
  std::vector<std::string> missing;
};

// 在 variant_dir 往下三层里按 md5 和大小找编排里的每个文件，md5 没有时按相对路径和大小。
// 文件放在哪个子目录不要紧：转存时外包目录没挪干净的也认得出。列目录出错时 error 写原因。
ResolvedPlan resolve_plan(panplay::DrivePlugin& drive, const std::string& variant_dir, const ShelfPlan& plan,
                          std::string* error);
