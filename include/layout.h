/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_LAYOUT_H
#define MIKU_LAYOUT_H

#include "types.h"

/*
 * miku_compute_layout - 计算画布内居中的字符网格。字符高度为宽度两倍，网格
 * 超出高度时反推列数，黑边偏移按偶数对齐。
 *
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 * @req_cols: 请求的字符列数，必须为正。
 * @video_w: 源视频宽度，必须为正。
 * @video_h: 源视频高度，必须为正。
 * @layout: 调用方持有的布局输出对象。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 */
void miku_compute_layout(const struct profile *p, int req_cols, int video_w,
			 int video_h, struct layout *layout);

#endif /* MIKU_LAYOUT_H */
