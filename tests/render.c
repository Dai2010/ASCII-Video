/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */
/* 五种字形尺寸与字符/色块渲染的基线校验，期望值来自重构前实现。 */
#include "glyph.h"
#include "constants.h"
#include "render.h"
#include "terminal_frame.h"
#include "ui_settings.h"
#include "ui_text.h"
#include "util.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

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

/*
 * check_hold - 跨帧稳定的语义测试：亮度只变两档时沿用上一帧字形，亮度大变
 * 时照常换字。
 * 样本亮度在表里搜索得到，不假设字体把某个值映射到哪一档，换字体也不会假
 * 通过。断言失败直接中止进程。
 *
 */
static void check_hold(void)
{
	struct glyph_atlas atlas = {0};
	struct render_memory memory = {0};
	struct render_grid grid;
	uint8_t cell[3];
	uint8_t canvas[8 * 16 * 3];
	int first = -1;
	int candidate;
	int level;

	assert(miku_atlas_build(&atlas, 8, 16) == 0);
	assert(miku_render_memory_init(&memory, 1) == 0);
	/* 找一处「再亮两档就会换字、但只换相邻几档」的亮度作为样本。 */
	for (level = 0; level < 128; level++) {
		first = atlas.lut[miku_gamma_lut[level]];
		candidate = atlas.lut[miku_gamma_lut[level + 2]];
		if (first != candidate &&
		    abs(first - candidate) <= GLYPH_HOLD_TOLERANCE)
			break;
	}
	assert(level < 128);

	grid = (struct render_grid){
		.cells = cell,
		.output = canvas,
		.stride = 8 * 3,
		.cols = 1,
		.rows = 1,
		.cell_w = 8,
		.cell_h = 16,
	};
	cell[0] = cell[1] = cell[2] = (uint8_t)level;
	miku_render_frame(&atlas, &grid, &memory);
	assert(memory.glyph[0] == first);

	/* 亮度只动两档：候选字形确实变了，但沿用上一帧的字形。 */
	cell[0] = cell[1] = cell[2] = (uint8_t)(level + 2);
	miku_render_frame(&atlas, &grid, &memory);
	assert(memory.glyph[0] == first);

	/* 亮度大变：必须接受新字形，画面照常跟随。 */
	cell[0] = cell[1] = cell[2] = 255;
	miku_render_frame(&atlas, &grid, &memory);
	assert(memory.glyph[0] == atlas.lut[miku_gamma_lut[255]]);

	miku_render_memory_free(&memory);
	free(atlas.masks);
}

/*
 * check_terminal_and_utf8 - 校验终端帧字形稳定与截断 UTF-8 边界安全。
 */
static void check_terminal_and_utf8(void)
{
	struct glyph_atlas atlas = {0};
	struct render_memory memory = {0};
	struct terminal_frame frame = {0};
	struct strbuf sb = {0};
	uint8_t cell[3];
	char *trunc;
	int first = -1;
	int level;

	assert(miku_atlas_build(&atlas, 8, 16) == 0);
	assert(miku_render_memory_init(&memory, 1) == 0);
	for (level = 0; level < 128; level++) {
		first = atlas.lut[miku_gamma_lut[level]];
		if (first != atlas.lut[miku_gamma_lut[level + 2]] &&
		    abs(first - atlas.lut[miku_gamma_lut[level + 2]]) <=
			    GLYPH_HOLD_TOLERANCE)
			break;
	}
	assert(level < 128);
	frame = (struct terminal_frame){
		.cells = cell,
		.atlas = &atlas,
		.memory = &memory,
		.cols = 1,
		.rows = 1,
		.style = STYLE_CHAR,
	};
	cell[0] = cell[1] = cell[2] = (uint8_t)level;
	assert(miku_terminal_frame_build(&sb, &frame) == 0);
	assert(memory.glyph[0] == first);
	cell[0] = cell[1] = cell[2] = (uint8_t)(level + 2);
	assert(miku_terminal_frame_build(&sb, &frame) == 0);
	assert(memory.glyph[0] == first);

	trunc = malloc(2);
	assert(trunc != NULL);
	trunc[0] = (char)0xE4;
	trunc[1] = '\0';
	assert(miku_ui_text_width(trunc) == 1);
	sb.len = 0;
	miku_ui_add_text_tail(&sb, "一二三四五", 5);
	assert(sb.len == strlen("…四五"));
	assert(memcmp(sb.buf, "…四五", sb.len) == 0);

	cell[0] = 180;
	cell[1] = 180;
	cell[2] = 180;
	assert(miku_cell_color(cell).r < 200);
	cell[0] = 175;
	cell[1] = 195;
	cell[2] = 196;
	assert(miku_cell_color(cell).b - miku_cell_color(cell).r >= 30);

	{
		char dir[UI_PATH_MAX];
		char file[UI_PATH_MAX];

		/*
		 * 设置写入配置目录：测试改到临时目录，绝不碰用户的真实设置
		 * 文件，结束后连同目录一起删除。
		 */
		snprintf(dir, sizeof dir, "%s/miku-test-XXXXXX",
			 miku_temp_dir());
		assert(mkdtemp(dir) != NULL);
		setenv("MIKU_CONFIG_DIR", dir, 1);

		assert(miku_settings_get_font_size() >= 4.0);
		assert(miku_settings_get_font_size() <= 12.5);
		miku_settings_set_font_size(3.0);
		assert(miku_settings_get_font_size() == 4.0);
		miku_settings_set_font_size(15.0);
		assert(miku_settings_get_font_size() == 12.5);
		miku_settings_set_font_size(7.2);
		assert(miku_settings_get_font_size() == 7.0);
		miku_settings_set_font_size(6.0);
		assert(miku_settings_get_font_size() == 6.0);

		snprintf(file, sizeof file, "%s/%s", dir, UI_SETTINGS_FILE);
		remove(file);
		rmdir(dir);
	}

	free(trunc);
	free(sb.buf);
	miku_render_memory_free(&memory);
	free(atlas.masks);
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
		UINT64_C(803704655914170639), UINT64_C(4298418238200983221),
		UINT64_C(4092220845012865912), UINT64_C(4821808165070991088),
		UINT64_C(7031257936295492962)};

	miku_build_gamma_lut();
	check_hold();
	check_terminal_and_utf8();
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
		miku_render_frame(&atlas, &grid, NULL);
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
