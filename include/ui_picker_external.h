/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_PICKER_EXTERNAL_H
#define MIKU_UI_PICKER_EXTERNAL_H

#include <stddef.h>

/*
 * miku_ui_pick_with_fzf - 选择目录时重新扫描，选择文件时再次核对可读性，防
 * 止扫描后文件消失。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_with_fzf(char *out, size_t cap, const char *start);
/*
 * miku_ui_pick_with_dialog - 调用外部 dialog 接管终端并读取结果。工具返回
 * 后重新进入备用屏，结果必须再次验证为可读普通文件。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_with_dialog(char *out, size_t cap, const char *start);

#endif /* MIKU_UI_PICKER_EXTERNAL_H */
