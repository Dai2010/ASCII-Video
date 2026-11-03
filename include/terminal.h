/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_TERMINAL_H
#define MIKU_TERMINAL_H

#include "types.h"

/*
 * miku_terminal_columns - 优先从标准输出读取终端列数，失败再尝试标准输入；
 * 两者都不可用时返回零，布局方自行选择默认值。
 *
 * 返回：正列数，无法探测返回 0。
 */
int miku_terminal_columns(void);
/*
 * miku_terminal_rows - 优先从标准输出读取终端行数，失败再尝试标准输入；返
 * 回零表示当前无法获得尺寸。
 *
 * 返回：正行数，无法探测返回 0。
 */
int miku_terminal_rows(void);

#endif /* MIKU_TERMINAL_H */
