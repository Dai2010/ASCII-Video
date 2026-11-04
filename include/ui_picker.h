/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_PICKER_H
#define MIKU_UI_PICKER_H

#include "types.h"

/*
 * miku_ui_pick_file - 默认使用内置选择器（常驻输入栏：可过滤、可输路径、
 * 行可直接点击）。设置 MIKU_PICKER=fzf 或 MIKU_PICKER=dialog 才改用外部
 * 工具，外部工具启动失败时仍退回内置列表。用户取消不会再次弹出另一种
 * 选择器。
 * out 由调用方提供并容纳 capacity 字节；成功路径会再次验证普通文件可读。
 * 返回 0 成功，取消或文件不可读时返回负 errno；不转移字符串所有权。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：0 选定，-ECANCELED 用户取消，-ENOENT 所选文件不可读。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_file(char *out, size_t capacity, const char *start);

#endif /* MIKU_UI_PICKER_H */
