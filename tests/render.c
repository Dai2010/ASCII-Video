/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */
/* 五种字形尺寸与字符/色块渲染的基线校验，期望值来自重构前实现。 */
#include "glyph.h"
#include "render.h"
#include "util.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t bytes)
{
	const uint8_t *p = data;
	size_t i;

	for (i = 0; i < bytes; i++) {
		hash ^= p[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

int main(void)
{
	struct glyph_atlas atlas = {0};
	uint8_t cells[8 * 8 * 3];
	uint8_t *canvas;
	uint64_t hash;
	size_t bytes;
	size_t i;
	int widths[] = {4, 8, 16, 24, 32};
	int width;
	int height;
	int n;
	struct render_grid grid = {0};
	const uint64_t expected[] = {
		UINT64_C(6887403445357186851), UINT64_C(4341286001493870571),
		UINT64_C(6448623448790527072), UINT64_C(10292222191830578188),
		UINT64_C(17857590708387110276)};

	miku_build_gamma_lut();
	for (i = 0; i < sizeof(cells); i++)
		cells[i] = (uint8_t)(i * 37 + 11);
	for (n = 0; n < 5; n++) {
		width = widths[n];
		height = width * 2;
		if (miku_atlas_build(&atlas, width, height) != 0)
			return 1;
		bytes = (size_t)8 * width * 4 * height * 3;
		canvas = calloc(bytes, 1);
		if (!canvas)
			return 1;
		hash = hash_bytes(UINT64_C(14695981039346656037), atlas.masks,
				  width * height * FONT_CHAR_COUNT);
		hash = hash_bytes(hash, atlas.chars, sizeof(atlas.chars));
		grid = (struct render_grid){
			.cells = cells,
			.output = canvas,
			.stride = 8 * width * 3,
			.cols = 8,
			.rows = 4,
			.cell_w = width,
			.cell_h = height,
		};
		miku_render_frame(&atlas, &grid);
		hash = hash_bytes(hash, canvas, bytes);
		miku_render_block_cells(&grid);
		hash = hash_bytes(hash, canvas, bytes);
		assert(hash == expected[n]);
		free(canvas);
		free(atlas.masks);
	}
	puts("render equivalence: PASS");
	return 0;
}
