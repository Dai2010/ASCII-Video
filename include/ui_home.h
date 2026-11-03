/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_HOME_H
#define MIKU_UI_HOME_H

#include "types.h"

/*
 * miku_ui_home - 维护欢迎菜单焦点，支持方向键、滚轮、数字键和点击。返回转
 * 换、播放或退出菜单索引，歌词每次进入时选择一次。
 *
 * 返回：0 转换，1 播放，2 退出。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_home(void);

#endif /* MIKU_UI_HOME_H */
