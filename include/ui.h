/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_H
#define MIKU_UI_H

#include "types.h"

/*
 * miku_tui_main - 交互入口分别保存转换和播放配置。终端进入和离开成对管理，
 * 信号或退出菜单都经同一正常收尾路径。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_tui_main(void);

#endif /* MIKU_UI_H */
