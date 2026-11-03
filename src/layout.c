/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 画布布局计算；不执行 I/O，不取得输入对象所有权。
 */

#include "layout.h"
#include "util.h"

#include <math.h>

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
			 int video_h, struct layout *layout)
{
	int cell_h;
	int max_cols;
	int max_rows;
	int cols;
	int rows;

	/* 字符宽度 = 画布宽度 / 列数，取偶数；高度是宽度的2倍（字符格子是 1:2
	 * 的） */
	int cell_w = (p->width / req_cols) & ~1;
	cell_w = miku_clamp_int(cell_w, MIN_CELL_WIDTH, MAX_CELL_WIDTH);
	cell_h = cell_w * 2;

	max_cols = p->width / cell_w;
	max_rows = p->height / cell_h;

	cols = req_cols > max_cols ? max_cols : req_cols;
	/* 行数 = 列数 * 视频高宽比 * (cell_w / cell_h) */
	rows = (int)lround((double)cols * video_h / video_w * cell_w / cell_h);

	if (rows > max_rows) { /* 视频比 16:9
				  更"方"或更"高"：按高度铺满，列数相应减少 */
		rows = max_rows;
		cols = (int)lround((double)rows * video_w / video_h * cell_h /
				   cell_w);
		cols = cols > max_cols ? max_cols : cols;
	}
	cols = cols < 1 ? 1 : cols;
	rows = rows < 1 ? 1 : rows;

	layout->cols = cols;
	layout->rows = rows;
	layout->cell_w = cell_w;
	layout->cell_h = cell_h;
	layout->off_x = ((p->width - cols * cell_w) / 2) & ~1;
	layout->off_y = ((p->height - rows * cell_h) / 2) & ~1;
}
