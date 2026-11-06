/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 整屏帧的字节流构建：把渲染好的字符格或色块格按行拼成终端输出。只做序列化，不做渲染计算；cells 与 atlas 为借用。
 */

#include "terminal_frame.h"
#include "render.h"
#include "strbuf.h"
#include "util.h"
#include "glyph.h"

#include <errno.h>
#include <string.h>

/*
 * append_color - 只在颜色改变时追加 ANSI 前景或背景序列，减少终端输出量。
 * previous 初始值应为 {-1,-1,-1}，仅在追加成功时更新。
 * buffer 的内存仍归调用方；失败返回 -ENOMEM，已追加内容可以直接丢弃。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @color: 按值传入的 RGB 通道。
 * @previous: 上一次成功输出的颜色缓存地址。
 * @background: 非零表示背景色，零表示前景色。
 *
 */
static int append_color(struct strbuf *buffer, struct rgb color,
			struct rgb *previous, int background)
{
	int ok;

	if (color.r == previous->r && color.g == previous->g &&
	    color.b == previous->b)
		return 0;
	ok = (miku_sb_addf(buffer, "\x1b[%d;2;%d;%d;%dm", background ? 48 : 38,
			   color.r, color.g, color.b) == 0);
	if (!ok)
		return -ENOMEM;
	*previous = color;
	return 0;
}

/*
 * append_cell - 追加一个字符格。色块使用 UTF-8 的 ▀，上采样为前色、下采样
 * 为背景色。
 * 单字符风格只使用前色和亮度图集；三个 UTF-8 字节必须整体写入。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @frame: 借用的终端采样帧和网格尺寸，不转移图集所有权。
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 * @colors: 当前行的前景和背景颜色缓存，调用方持有。
 *
 */
static int append_cell(struct strbuf *buffer,
		       const struct terminal_frame *frame, const uint8_t *pixel,
		       size_t slot, struct rgb colors[2])
{
	struct render_memory *memory = frame->memory;
	int level;
	int glyph;
	int delta;
	int ret;

	ret = append_color(buffer, miku_cell_color(pixel), &colors[0], 0);
	if (ret)
		return ret;
	if (frame->style == STYLE_BLOCK) {
		ret = append_color(buffer,
				   miku_cell_color(pixel + frame->cols * 3),
				   &colors[1], 1);
		if (ret)
			return ret;
		if (!(miku_sb_reserve(buffer, 3) == 0))
			return -ENOMEM;
		memcpy(buffer->buf + buffer->len, "\xe2\x96\x80", 3);
		buffer->len += 3;
		return 0;
	}
	if (!(miku_sb_reserve(buffer, 1) == 0))
		return -ENOMEM;
	level = miku_pixel_level(pixel);
	glyph = frame->atlas->lut[miku_gamma_lut[level]];
	if (memory && memory->primed) {
		delta = glyph - memory->glyph[slot];
		if (delta < 0)
			delta = -delta;
		if (delta <= GLYPH_HOLD_TOLERANCE)
			glyph = memory->glyph[slot];
	}
	if (memory)
		memory->glyph[slot] = (uint8_t)glyph;
	buffer->buf[buffer->len++] = frame->atlas->chars[glyph];
	return 0;
}

/*
 * append_row - 每行末复位颜色，因此每行的颜色缓存必须重新初始化。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @frame: 借用的终端采样帧和网格尺寸，不转移图集所有权。
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 */
static int append_row(struct strbuf *buffer, const struct terminal_frame *frame,
		      int row)
{
	struct rgb colors[2] = {{-1, -1, -1}, {-1, -1, -1}};
	const uint8_t *pixel;
	int sample_row = frame->style == STYLE_BLOCK ? row * 2 : row;
	size_t slot;
	int x;
	int ret;

	for (x = 0; x < frame->cols; x++) {
		slot = (size_t)row * frame->cols + x;
		pixel = frame->cells +
			((size_t)sample_row * frame->cols + x) * 3;
		ret = append_cell(buffer, frame, pixel, slot, colors);
		if (ret)
			return ret;
	}
	if (row == frame->rows - 1)
		return (miku_sb_addf(buffer, "\x1b[0m") == 0) ? 0 : -ENOMEM;
	return (miku_sb_addf(buffer, "\x1b[0m\r\n") == 0) ? 0 : -ENOMEM;
}

/*
 * miku_terminal_frame_build - 重建一整帧终端输出，成功返回 0，扩容失败返回
 * -ENOMEM。
 * 原 buffer 可复用：只重置 len，保留容量。函数不执行终端 I/O，
 * 使播放时钟与昂贵的字符拼接分离；失败时调用方不得输出半帧。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @frame: 借用的终端采样帧和网格尺寸，不转移图集所有权。
 *
 */
int miku_terminal_frame_build(struct strbuf *buffer,
			      const struct terminal_frame *frame)
{
	int y;
	int ret;

	buffer->len = 0;
	if (!(miku_sb_addf(buffer, "\x1b[H") == 0))
		return -ENOMEM;
	for (y = 0; y < frame->rows; y++) {
		ret = append_row(buffer, frame, y);
		if (ret)
			return ret;
	}
	if (frame->memory)
		frame->memory->primed = true;
	return 0;
}
