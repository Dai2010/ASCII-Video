/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_RENDER_H
#define MIKU_RENDER_H

#include "types.h"

/* 颜色通道取值 0..255；按值返回，不持有像素内存。 */
struct rgb {
	int r;
	int g;
	int b;
};

/* 借用输入采样和输出画布；stride 是整张画布每行的字节数。 */
struct render_grid {
	const uint8_t *cells;
	uint8_t *output;
	size_t stride;
	int cols;
	int rows;
	int cell_w;
	int cell_h;
};

/*
 * miku_cell_color - 将源 RGB24 像素提高饱和度后整体缩放，避免逐通道截断改
 * 变色相。
 * pixel 借用至少三个字节；返回颜色不依赖输入内存的生命周期。
 * 近黑像素直接返回黑色，避免放大压缩噪点。无共享可变状态。
 *
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 *
 */
struct rgb miku_cell_color(const uint8_t *pixel);
/*
 * miku_pixel_level - 源像素的感知亮度，范围固定在 0..255，供字形查表使用。
 *
 * @pixel: 至少三个字节的 RGB24 采样点，仅借用。
 *
 */
int miku_pixel_level(const uint8_t *pixel);
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
		       const struct render_grid *grid);
/*
 * miku_render_block_cells - 色块渲染要求 cells 有 rows*2 行 RGB24
 * 采样，output 覆盖完整网格。
 * 每个格子只在本格范围内写入；不拥有、不释放输入输出内存。
 *
 * @grid: 借用的网格几何与 RGB24 输入输出。
 *
 */
void miku_render_block_cells(const struct render_grid *grid);

#endif /* MIKU_RENDER_H */
