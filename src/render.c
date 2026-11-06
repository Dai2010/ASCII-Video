/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * RGB24 渲染；输入输出内存借用，按格子边界写入调用方画布。
 */

#include "render.h"
#include "util.h"
#include "glyph.h"

#include <errno.h>
#include <stdlib.h>

/*
 * render_white_gain - 对颜色的白底分量（三通道最小值）做高光软膝衰减。
 * 暗部维持与色度相同的增益以保住黑底字符可见度；亮部与近白区域压低白底
 * 放大，防止浅粉、浅青、肤色和高光白区被推上 255 变成一片惨白。
 *
 * @low: 饱和度提升后的三通道最小值。
 * @gain: 色度主通道的目标增益。
 *
 * 返回：白底分量的实际缩放系数。
 * 线程约束：纯计算函数，无共享可变状态，可并发调用。
 */
static double render_white_gain(double low, double gain)
{
	const double span = WHITE_FLOOR_CEIL - WHITE_FLOOR_KNEE;

	if (low <= WHITE_FLOOR_KNEE)
		return gain;
	if (low <= WHITE_FLOOR_CEIL)
		return 1.0 + (gain - 1.0) * ((WHITE_FLOOR_CEIL - low) / span);
	return (WHITE_FLOOR_CEIL +
		(low - WHITE_FLOOR_CEIL) * WHITE_FLOOR_SLOPE) /
	       low;
}

/*
 * miku_cell_color - 将源 RGB24 像素提高饱和度后分离白底与色度缩放，避免高
 * 光区域过曝漂白或改变色相。
 * pixel 借用至少三个字节；返回颜色不依赖输入内存的生命周期。
 * 近黑像素直接返回黑色，避免放大压缩噪点。无共享可变状态。
 *
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 *
 * 返回：提亮并防漂白后的 RGB 颜色值。
 * 线程约束：纯计算函数，无共享可变状态，可并发调用。
 */
struct rgb miku_cell_color(const uint8_t *pixel)
{
	double gray;
	double channels[3];
	double peak;
	double low;
	double target;
	double gain;
	double wgain;
	struct rgb color = {0};
	int i;

	gray = RED_WEIGHT * pixel[0] + GREEN_WEIGHT * pixel[1] +
	       BLUE_WEIGHT * pixel[2];
	for (i = 0; i < 3; ++i)
		channels[i] = miku_dmax(0, gray + (pixel[i] - gray) *
							   SATURATION_BOOST);
	peak = miku_dmax(channels[0], miku_dmax(channels[1], channels[2]));
	if (peak < 1.0)
		return color;
	low = miku_dmin(channels[0], miku_dmin(channels[1], channels[2]));
	target = miku_dmax(peak * COLOR_GAIN,
			   miku_dmin(COLOR_MIN_VALUE, peak * COLOR_MAX_GAIN));
	gain = miku_dmin(target, 255.0) / peak;
	wgain = render_white_gain(low, gain);
	for (i = 0; i < 3; ++i)
		channels[i] = low * wgain + (channels[i] - low) * gain;
	color.r = (int)miku_dmin((int)(channels[0] + 0.5), 255);
	color.g = (int)miku_dmin((int)(channels[1] + 0.5), 255);
	color.b = (int)miku_dmin((int)(channels[2] + 0.5), 255);
	return color;
}

/*
 * miku_pixel_level - 源像素的感知亮度，范围固定在 0..255，供字形查表使用。
 *
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 *
 */
int miku_pixel_level(const uint8_t *pixel)
{
	double lum;

	lum = RED_WEIGHT * pixel[0] + GREEN_WEIGHT * pixel[1] +
	      BLUE_WEIGHT * pixel[2];
	return (int)miku_dmax(0, miku_dmin((int)(lum + 0.5), 255));
}

/*
 * blend_channel - 8 位通道和蒙版相乘后除以 255，使用精确的整数舍入公式。
 * 128 是半个量化区间，右移 8 位对应 256；结果与原实现逐字节一致。
 *
 * @color: 按值传入的 RGB 通道。
 * @alpha: 字形覆盖率，范围 0..255。
 *
 */
static uint8_t blend_channel(int color, unsigned int alpha)
{
	unsigned int product = (unsigned int)color * alpha + 128;

	return (uint8_t)((product + (product >> 8)) >> 8);
}

/*
 * paint_glyph - 在一个字符格内混合蒙版；调用者保证画布和蒙版覆盖整个格子。
 *
 * @grid: 借用的网格几何与 RGB24 输入输出。
 * @mask: 字形灰度覆盖率数组，至少容纳一个完整格子。
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 * @color: 按值传入的 RGB 通道。
 *
 */
static void paint_glyph(const struct render_grid *grid, const uint8_t *mask,
			uint8_t *output, struct rgb color)
{
	uint8_t *dst;
	unsigned int alpha;
	int y;
	int x;

	for (y = 0; y < grid->cell_h; y++) {
		dst = output + y * grid->stride;
		for (x = 0; x < grid->cell_w; x++) {
			alpha = mask[y * grid->cell_w + x];
			dst[0] = blend_channel(color.r, alpha);
			dst[1] = blend_channel(color.g, alpha);
			dst[2] = blend_channel(color.b, alpha);
			dst += 3;
		}
	}
}

/*
 * miku_render_memory_init - 为 cells 个格子分配跨帧状态并清零。成功返回 0
 * 并把两个数组交给调用方；失败返回 -ENOMEM，字段保持未分配。
 *
 * @memory: 调用方持有的稳定状态，非 const 指针允许更新。
 * @cells: 总格数，等于 cols*rows。
 *
 */
int miku_render_memory_init(struct render_memory *memory, size_t cells)
{
	memory->glyph = calloc(cells, 1);
	memory->color = calloc(cells, 3);
	memory->cells = cells;
	memory->primed = false;
	if (!memory->glyph || !memory->color) {
		miku_render_memory_free(memory);
		return -ENOMEM;
	}
	return 0;
}

/*
 * miku_render_memory_free - 释放跨帧状态并把字段复位，可对已释放对象重复
 * 调用。不转移所有权。
 *
 * @memory: 调用方持有的稳定状态，允许为 NULL。
 *
 */
void miku_render_memory_free(struct render_memory *memory)
{
	if (!memory)
		return;
	free(memory->glyph);
	free(memory->color);
	memory->glyph = NULL;
	memory->color = NULL;
	memory->cells = 0;
	memory->primed = false;
}

/*
 * render_hold_glyph - 候选字形与上一帧只差几档墨量时继续用上一帧的字形，
 * 避免亮度在档位边界抖动时字符来回切换。
 *
 * @previous: 上一帧该格实际使用的字形索引。
 * @candidate: 本帧按亮度查表得到的字形索引。
 *
 */
static int render_hold_glyph(int previous, int candidate)
{
	int delta = candidate - previous;

	if (delta < 0)
		delta = -delta;
	return delta <= GLYPH_HOLD_TOLERANCE ? previous : candidate;
}

/*
 * render_stabilize_cell - 对单格执行字形与颜色跨帧稳定。若与上一帧保持相
 * 同字形且三通道色差均在容忍范围内，沿用上一帧颜色并返回 true 表示整格无需
 * 重绘；否则更新跨帧记录并返回 false。
 *
 * @memory: 可选的跨帧状态，NULL 时直接返回 false。
 * @slot: 当前格在网格中的线性下标。
 * @glyph: 输入输出字形索引。
 * @color: 输入输出格子颜色。
 *
 */
static bool render_stabilize_cell(struct render_memory *memory, size_t slot,
				  int *glyph, struct rgb *color)
{
	uint8_t *prev;
	int old_glyph;
	bool color_held = false;

	if (!memory)
		return false;
	prev = memory->color + slot * 3;
	old_glyph = memory->primed ? memory->glyph[slot] : -1;
	if (memory->primed)
		*glyph = render_hold_glyph(old_glyph, *glyph);
	memory->glyph[slot] = (uint8_t)*glyph;
	if (*glyph == 0 && old_glyph == 0)
		return true;
	if (memory->primed && abs(color->r - prev[0]) <= COLOR_HOLD_TOLERANCE &&
	    abs(color->g - prev[1]) <= COLOR_HOLD_TOLERANCE &&
	    abs(color->b - prev[2]) <= COLOR_HOLD_TOLERANCE) {
		color->r = prev[0];
		color->g = prev[1];
		color->b = prev[2];
		color_held = true;
	} else {
		prev[0] = (uint8_t)color->r;
		prev[1] = (uint8_t)color->g;
		prev[2] = (uint8_t)color->b;
	}
	return *glyph == old_glyph && color_held;
}

/*
 * miku_render_frame - 把每个采样点渲染为字符格。atlas 的尺寸必须与 grid 的
 * 格子尺寸一致。
 * output 指向画布内网格左上角，黑边由调用方预先清零；本函数不分配内存。
 * memory 非 NULL 时启用跨帧稳定：候选字形与上一帧只差几档墨量就沿用上一
 * 帧该格的字形，颜色也在容忍范围内时直接跳过重绘，不改变静止画面的结果。
 * 输入、输出和图集由调用方持有，允许不同画布并发渲染。
 *
 * @atlas: 字形图集，创建成功后 masks 所有权交给调用方。
 * @grid: 借用的网格几何与 RGB24 输入输出。
 * @memory: 可选的跨帧状态，NULL 表示每帧独立渲染。
 *
 */
void miku_render_frame(const struct glyph_atlas *atlas,
		       const struct render_grid *grid,
		       struct render_memory *memory)
{
	const uint8_t *pixel;
	const uint8_t *mask;
	uint8_t *output;
	size_t glyph_size = (size_t)grid->cell_w * grid->cell_h;
	size_t slot;
	struct rgb color;
	int glyph;
	int y;
	int x;

	for (y = 0; y < grid->rows; y++) {
		for (x = 0; x < grid->cols; x++) {
			slot = (size_t)y * grid->cols + x;
			pixel = grid->cells + slot * 3;
			glyph = atlas->lut[miku_gamma_lut[miku_pixel_level(
				pixel)]];
			if (memory && memory->primed &&
			    memory->glyph[slot] == 0 &&
			    glyph <= GLYPH_HOLD_TOLERANCE)
				continue;
			color = miku_cell_color(pixel);
			if (render_stabilize_cell(memory, slot, &glyph, &color))
				continue;
			mask = atlas->masks + (size_t)glyph * glyph_size;
			output = grid->output +
				 y * grid->cell_h * grid->stride +
				 x * grid->cell_w * 3;
			paint_glyph(grid, mask, output, color);
		}
	}
	if (memory)
		memory->primed = true;
}

/*
 * paint_block - 一个色块格的上、下半部来自两个连续采样行，奇数高度沿用原分
 * 界。
 *
 * @grid: 借用的网格几何与 RGB24 输入输出。
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 * @top: 图标首行的一基终端行号。
 * @bottom: 按值传入的格子下半部颜色。
 *
 */
static void paint_block(const struct render_grid *grid, uint8_t *output,
			struct rgb top, struct rgb bottom)
{
	struct rgb color;
	uint8_t *dst;
	int y;
	int x;

	for (y = 0; y < grid->cell_h; y++) {
		color = y < grid->cell_h / 2 ? top : bottom;
		dst = output + y * grid->stride;
		for (x = 0; x < grid->cell_w; x++) {
			dst[x * 3] = (uint8_t)color.r;
			dst[x * 3 + 1] = (uint8_t)color.g;
			dst[x * 3 + 2] = (uint8_t)color.b;
		}
	}
}

/*
 * miku_render_block_cells - 色块渲染要求 cells 有 rows*2 行 RGB24
 * 采样，output 覆盖完整网格。
 * 每个格子只在本格范围内写入；不拥有、不释放输入输出内存。
 *
 * @grid: 借用的网格几何与 RGB24 输入输出。
 *
 */
void miku_render_block_cells(const struct render_grid *grid)
{
	const uint8_t *top;
	const uint8_t *bottom;
	uint8_t *output;
	int y;
	int x;

	for (y = 0; y < grid->rows; y++) {
		for (x = 0; x < grid->cols; x++) {
			top = grid->cells +
			      ((size_t)y * 2 * grid->cols + x) * 3;
			bottom = top + grid->cols * 3;
			output = grid->output +
				 y * grid->cell_h * grid->stride +
				 x * grid->cell_w * 3;
			paint_block(grid, output, miku_cell_color(top),
				    miku_cell_color(bottom));
		}
	}
}
