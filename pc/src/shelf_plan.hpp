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

// 还没转存时按编排里的文件名、大小、md5 把每层认成一组压缩包（路径挂在 variant_dir 下），转存前就能列出要装哪些。
// archive_set_id 只看文件名和大小，和转存后一样。认不成一组的层跳过。
std::vector<panplay::ArchiveSet> plan_archive_sets(const std::string& variant_dir, const ShelfPlan& plan);

// 还没转存、也没有编排时，按分享里的文件认压缩包组（路径挂在 variant_dir 下）。分享列出来的是分享人网盘里的完整路径，
// 去掉所有文件共同的目录（外包目录一起去掉），再和网盘里的游戏一样只看游戏目录和下一层子目录。
std::vector<panplay::ArchiveSet> share_archive_sets(const std::string& variant_dir,
                                                    const std::vector<panplay::ShareFile>& files);
