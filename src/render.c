/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * RGB24 渲染；输入输出内存借用，按格子边界写入调用方画布。
 */

#include "render.h"
#include "util.h"
#include "glyph.h"

/*
 * miku_cell_color - 将源 RGB24 像素提高饱和度后整体缩放，避免逐通道截断改
 * 变色相。
 * pixel 借用至少三个字节；返回颜色不依赖输入内存的生命周期。
 * 近黑像素直接返回黑色，避免放大压缩噪点。无共享可变状态。
 *
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 *
 */
struct rgb miku_cell_color(const uint8_t *pixel)
{
	double gray;
	double channels[3];
	double peak;
	double target;
	double gain;
	struct rgb color = {0};
	int i;

	gray = RED_WEIGHT * pixel[0] + GREEN_WEIGHT * pixel[1] +
	       BLUE_WEIGHT * pixel[2];
	for (i = 0; i < 3; i++)
		channels[i] = miku_dmax(0, gray + (pixel[i] - gray) *
							   SATURATION_BOOST);
	peak = miku_dmax(channels[0], miku_dmax(channels[1], channels[2]));
	if (peak < 1.0)
		return color;
	target = miku_dmax(peak * COLOR_GAIN,
			   miku_dmin(COLOR_MIN_VALUE, peak * COLOR_MAX_GAIN));
	gain = miku_dmin(target, 255.0) / peak;
	color.r = (int)miku_dmin((int)(channels[0] * gain + 0.5), 255);
	color.g = (int)miku_dmin((int)(channels[1] * gain + 0.5), 255);
	color.b = (int)miku_dmin((int)(channels[2] * gain + 0.5), 255);
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
 * miku_render_frame - 把每个采样点渲染为字符格。atlas 的尺寸必须与 grid 的
 * 格子尺寸一致。
 * output 指向画布内网格左上角，黑边由调用方预先清零；本函数不分配内存。
 * 输入、输出和图集由调用方持有，允许不同画布并发渲染。
 *
 * @atlas: 字形图集，创建成功后 masks 所有权交给调用方。
 * @grid: 借用的网格几何与 RGB24 输入输出。
 *
 */
void miku_render_frame(const struct glyph_atlas *atlas,
		       const struct render_grid *grid)
{
	const uint8_t *pixel;
	const uint8_t *mask;
	uint8_t *output;
	size_t glyph_size = (size_t)grid->cell_w * grid->cell_h;
	int level;
	int y;
	int x;

	for (y = 0; y < grid->rows; y++) {
		for (x = 0; x < grid->cols; x++) {
			pixel = grid->cells + ((size_t)y * grid->cols + x) * 3;
			level = miku_pixel_level(pixel);
			mask = atlas->masks +
			       atlas->lut[miku_gamma_lut[level]] * glyph_size;
			output = grid->output +
				 y * grid->cell_h * grid->stride +
				 x * grid->cell_w * 3;
			paint_glyph(grid, mask, output, miku_cell_color(pixel));
		}
	}
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
