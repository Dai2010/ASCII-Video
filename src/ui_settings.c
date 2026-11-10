/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 设置界面：提供 Kitty 终端播放字号选择（4.0 ~ 12.5，步长 0.5），方向键微调并持久化。
 */

#include "ui_settings.h"
#include "constants.h"
#include "strbuf.h"
#include "terminal.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double g_font_size = UI_FONT_SIZE_DEFAULT;
static int g_settings_loaded = 0;

/*
 * ui_settings_path - 把配置目录与设置文件名拼成完整路径。
 *
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只写入 dst。
 * 线程约束：仅由主线程串行调用。
 */
static void ui_settings_path(char *dst, size_t cap)
{
	snprintf(dst, cap, "%s/%s", miku_config_dir(), UI_SETTINGS_FILE);
}

/*
 * ui_settings_load - 读取持久化设置文件，解析播放字号；若文件不存在或字段
 * 无效则保留默认值。
 *
 * 返回：无；更新模块内静态状态。
 * 线程约束：仅由主线程串行调用。
 */
static void ui_settings_load(void)
{
	char path[UI_PATH_MAX];
	FILE *fp;
	char line[128];

	g_settings_loaded = 1;
	ui_settings_path(path, sizeof path);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char key[64];
		double val;

		if (sscanf(line, "%63s %lf", key, &val) == 2 &&
		    strcmp(key, "font_size") == 0) {
			if (val < UI_FONT_SIZE_MIN)
				val = UI_FONT_SIZE_MIN;
			if (val > UI_FONT_SIZE_MAX)
				val = UI_FONT_SIZE_MAX;
			g_font_size = round(val * 2.0) / 2.0;
			break;
		}
	}
	fclose(fp);
}

/*
 * ui_settings_save - 将当前设置写入持久化文件，供下次启动复用。
 *
 * 返回：无；文件写入失败时静默忽略。
 * 线程约束：仅由主线程串行调用。
 */
static void ui_settings_save(void)
{
	char path[UI_PATH_MAX];
	FILE *fp;

	ui_settings_path(path, sizeof path);
	fp = fopen(path, "w");
	if (!fp)
		return;
	fprintf(fp, "font_size\t%.1f\n", g_font_size);
	fclose(fp);
}

/*
 * miku_settings_get_font_size - 获取当前设置的 Kitty 播放字号。
 *
 * 返回：4.0 到 12.5 之间的字号值；未保存时默认返回 6.0。
 * 线程约束：仅由主线程调用。
 */
double miku_settings_get_font_size(void)
{
	if (!g_settings_loaded)
		ui_settings_load();
	return g_font_size;
}

/*
 * miku_settings_set_font_size - 设置并保存 Kitty 播放字号。
 *
 * @size: 目标字号，截断至 4.0 到 12.5，四舍五入到 0.5 步长。
 *
 * 返回：无；更新内存状态并保存到配置文件。
 * 线程约束：仅由主线程调用。
 */
void miku_settings_set_font_size(double size)
{
	if (size < UI_FONT_SIZE_MIN)
		size = UI_FONT_SIZE_MIN;
	if (size > UI_FONT_SIZE_MAX)
		size = UI_FONT_SIZE_MAX;
	g_font_size = round(size * 2.0) / 2.0;
	g_settings_loaded = 1;
	ui_settings_save();
}

/*
 * ui_settings_draw - 绘制设置菜单主体，包含字号选择微调框、进度槽与操作说明。
 *
 * @font_size: 当前调节中的临时字号值。
 *
 * 返回：无；只输出到终端缓冲，不持有借用对象。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_settings_draw(double font_size)
{
	struct strbuf sb;
	double frac;
	int bar_w;
	int filled;
	int i;

	int cols = miku_terminal_columns();
	int rows = miku_terminal_rows();
	if (cols <= 0)
		cols = 80;
	if (rows <= 0)
		rows = 24;

	sb = (struct strbuf){0};
	miku_ui_clear_screen();
	miku_sb_addf(&sb, "\x1b[H");

	miku_ui_row(&sb, 2);
	miku_ui_pad(&sb, miku_ui_center_col(6, cols));
	miku_sb_addf(&sb, UI_MIKU UI_BOLD "设  置" UI_RESET);

	miku_ui_row(&sb, 4);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_pad(&sb, miku_ui_center_col(30, cols));
	miku_ui_add_text(&sb, "Kitty 终端播放时的临时字体大小", cols - 1);
	miku_sb_addf(&sb, UI_RESET);

	miku_ui_row(&sb, 7);
	miku_ui_pad(&sb, miku_ui_center_col(20, cols));
	miku_sb_addf(&sb,
		     UI_GOOD UI_BOLD "◄" UI_RESET "     " UI_MIKU UI_BOLD
				     "%.1f pt" UI_RESET "     " UI_GOOD UI_BOLD
				     "►" UI_RESET,
		     font_size);

	frac = (font_size - UI_FONT_SIZE_MIN) /
	       (UI_FONT_SIZE_MAX - UI_FONT_SIZE_MIN);
	frac = miku_dmax(0.0, miku_dmin(frac, 1.0));
	bar_w = 26;
	filled = (int)(frac * bar_w + 0.5);

	miku_ui_row(&sb, 9);
	miku_ui_pad(&sb, miku_ui_center_col(bar_w + 14, cols));
	miku_sb_addf(&sb, UI_DIM "4.0  " UI_RESET UI_MIKU);
	for (i = 0; i < bar_w; ++i)
		miku_sb_addf(&sb, "%s", i < filled ? "█" : "░");
	miku_sb_addf(&sb, UI_RESET UI_DIM "  12.5" UI_RESET);

	miku_ui_row(&sb, 12);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_pad(&sb, miku_ui_center_col(42, cols));
	miku_ui_add_text(&sb, "• 方向键：←/↓ 减小，→/↑ 增大（步长 0.5）",
			 cols - 1);

	miku_ui_row(&sb, 14);
	miku_ui_pad(&sb, miku_ui_center_col(42, cols));
	miku_ui_add_text(&sb, "• 字号越小，终端网格密度越高、字符画越精细",
			 cols - 1);

	miku_ui_row(&sb, 16);
	miku_ui_pad(&sb, miku_ui_center_col(42, cols));
	miku_ui_add_text(&sb, "• 仅在播放视频时临时应用，退出播放恢复默认",
			 cols - 1);
	miku_sb_addf(&sb, UI_RESET);

	miku_ui_row(&sb, rows - 1);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_pad(&sb, miku_ui_center_col(36, cols));
	miku_ui_add_text(&sb, "←/→/↑/↓ 调节   Enter/Esc/q 保存返回", cols - 1);
	miku_sb_addf(&sb, UI_RESET);

	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * miku_ui_settings - 主菜单「设置」界面：直接使用方向键选择字体大小。
 *
 * 返回：0 正常返回主菜单，1 退出程序。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_settings(void)
{
	double font_size = miku_settings_get_font_size();
	double old_size = font_size;

	while (!miku_g_ui_stop) {
		int k;

		ui_settings_draw(font_size);
		k = miku_ui_key(-1);
		if (k == 'q' || k == 27 || k == '\r' || k == '\n' || k == 3 ||
		    k == UI_CLICK)
			break;
		if (k == UI_LEFT || k == UI_DOWN || k == 'h' || k == 'j' ||
		    k == '-' || k == UI_WHEEL_DOWN) {
			font_size -= UI_FONT_SIZE_STEP;
			if (font_size < UI_FONT_SIZE_MIN)
				font_size = UI_FONT_SIZE_MIN;
		} else if (k == UI_RIGHT || k == UI_UP || k == 'l' ||
			   k == 'k' || k == '+' || k == '=' ||
			   k == UI_WHEEL_UP) {
			font_size += UI_FONT_SIZE_STEP;
			if (font_size > UI_FONT_SIZE_MAX)
				font_size = UI_FONT_SIZE_MAX;
		}
	}
	if (font_size != old_size)
		miku_settings_set_font_size(font_size);
	return miku_g_ui_stop ? 1 : 0;
}
