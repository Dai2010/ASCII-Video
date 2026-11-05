/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 转换与播放的配置页：各配置项的显示与调整、字段编辑、状态栏提示，以及由输入路径推导输出路径。
 */

#include "ui_config.h"
#include "strbuf.h"
#include "terminal.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "video.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * miku_ui_status - 在配置状态对象中格式化提示，不直接输出终端。结果按状态
 * 栏容量截断，由下一次整屏重绘显示。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @fmt: printf 格式串，变参类型必须与占位符一致。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_status(struct ui_config *st, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(st->status, sizeof st->status, fmt, ap);
	va_end(ap);
}

/*
 * miku_ui_derive_output - 从输入路径派生带风格后缀的输出名。只移除文件名
 * 部分的扩展名，避免把目录中的点误当成扩展名。字符风格加 `-ascii.mp4`，
 * 色块风格加 `-block.mp4`，同一部片子转两种风格不会互相覆盖。
 *
 * @in: 借用的输入媒体路径，必须以 NUL 结尾。
 * @style: STYLE_CHAR 或 STYLE_BLOCK，决定后缀。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_derive_output(const char *in, int style, char *out, size_t cap)
{
	const char *suffix = style == STYLE_BLOCK ? "-block.mp4" : "-ascii.mp4";
	const char *dot = strrchr(in, '.');
	const char *slash = strrchr(in, '/');
	size_t need = strlen(suffix) + 1;
	size_t keep = (dot && (!slash || dot > slash)) ? (size_t)(dot - in)
						       : strlen(in);
	if (keep + need > cap)
		keep = cap > need ? cap - need : 0;
	memcpy(out, in, keep);
	out[keep] = '\0';
	strncat(out, suffix, cap - keep - 1);
}

/*
 * ui_style_name - 返回画面风格的静态显示名。未知非色块值沿用字符风格，不分
 * 配字符串。
 *
 * @style: STYLE_CHAR 或 STYLE_BLOCK。
 *
 * 返回：借用的静态 UTF-8 名称，不得释放。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static const char *ui_style_name(int style)
{
	return style == STYLE_BLOCK ? "色块（半块 ▀）" : "字符（ASCII 字形）";
}

/*
 * ui_fps_name - 返回帧率显示文本。预设项使用静态文本，其他正值格式化到本函
 * 数的静态缓冲。
 *
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：借用的静态文本或静态缓冲，下一次调用可能覆盖内容。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static const char *ui_fps_name(int fps)
{
	static char buf[16];
	if (fps <= 0)
		return "原画";
	snprintf(buf, sizeof buf, "%d", fps);
	return buf;
}

/*
 * ui_quality_name - 将质量索引映射为静态显示名，无效索引回退自动档位。
 *
 * @q: 质量档位索引。
 *
 * 返回：借用的静态 UTF-8 名称，不得释放。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static const char *ui_quality_name(int q)
{
	switch (q) {
	case 1:
		return "原画分辨率";
	case 2:
		return "high 2K";
	case 3:
		return "mid 1080p";
	case 4:
		return "low 720p";
	default:
		return "自动（按设备降级）";
	}
}

/*
 * ui_delete_character - UTF-8 退格先跳过延续字节，保证不留下半个多字节字符
 * 。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @length: 接收/更新字符串字节长度的地址，不含结尾 NUL。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_delete_character(char *buffer, size_t *length)
{
	if (!*length)
		return;
	(*length)--;
	while (*length && ((unsigned char)buffer[*length] & 0xc0) == 0x80)
		(*length)--;
	buffer[*length] = '\0';
}

/*
 * miku_ui_edit_field - 在调用方路径数组内直接编辑。退格按 UTF-8 字符回退，
 * 容量不足时拒绝继续追加；Esc 结束编辑但保留已输入内容。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @which: 需要编辑的配置项枚举。
 * @title: 借用的 UTF-8 页面标题。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_edit_field(struct ui_config *st, int which, const char *title)
{
	char *buf = which == CFG_INPUT ? st->input : st->output;
	size_t len = strlen(buf);
	st->status[0] = '\0';

	for (;;) {
		struct strbuf sb;
		int k;

		int cols = miku_terminal_columns();
		if (cols <= 0)
			cols = 80;
		sb = (struct strbuf){0};
		miku_ui_clear_screen();
		miku_sb_addf(&sb, "\x1b[H");
		miku_ui_row(&sb, 1);
		miku_sb_addf(&sb, UI_BOLD UI_ACCENT);
		miku_ui_add_text(&sb, title, -1);
		miku_sb_addf(&sb, UI_RESET);
		miku_ui_row(&sb, 2);
		miku_sb_addf(&sb, UI_DIM);
		miku_ui_add_text(&sb, "Enter 确认   Esc 取消   Backspace 删除",
				 cols - 1);
		miku_sb_addf(&sb, UI_RESET);
		miku_ui_row(&sb, 4);
		miku_sb_addf(&sb, "> ");
		miku_ui_add_text_tail(&sb, buf, cols - 4);
		miku_sb_addf(&sb, "\x1b[7m \x1b[0m");
		miku_ui_flush(&sb);
		free(sb.buf);

		k = miku_ui_key(-1);
		if (k == UI_NONE || miku_g_ui_stop)
			break;
		if (k == 27 || k == '\r' || k == '\n')
			break;
		if (k == 127) {
			ui_delete_character(buf, &len);
			continue;
		}
		/* 方向键、滚轮与点击是命令键（1001..1008），不是文本；当成
		 * UTF-8 字节会把半个序列塞进路径，显示成乱码。 */
		if (k >= UI_UP && k <= UI_IGNORE)
			continue;
		if (k >= UI_TEXT_BYTE) {
			if (len + 1 < UI_PATH_MAX) {
				buf[len++] = (char)(k - UI_TEXT_BYTE);
				buf[len] = '\0';
			}
			continue;
		}
		if (k >= 32 && k < 127 && len + 1 < UI_PATH_MAX) {
			buf[len++] = (char)k;
			buf[len] = '\0';
		}
	}
	/*
	 * 输出名一旦被手动改成别的名字，就不再随输入或风格自动改写；留空或
	 * 仍等于派生名时视为自动。输入手打改变后，自动输出名同步刷新。
	 */
	if (which == CFG_OUTPUT) {
		char derived[UI_PATH_MAX];

		miku_ui_derive_output(st->input, st->style, derived,
				      sizeof derived);
		st->output_auto = !buf[0] || strcmp(buf, derived) == 0;
	} else if (which == CFG_INPUT && st->output_auto) {
		if (st->input[0])
			miku_ui_derive_output(st->input, st->style, st->output,
					      sizeof st->output);
		else
			st->output[0] = '\0';
	}
}

/*
 * miku_ui_cfg_count - 返回当前页面可聚焦项目数，转换页和播放页使用不同项目
 * 映射。
 *
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 返回：当前页面可聚焦的项目数。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_cfg_count(int kind)
{
	return kind == 0 ? 7 : 5;
}

/*
 * miku_ui_cfg_index - 把可见槽位映射为配置项枚举。slot 必须小于当前页面项
 * 目数，映射表为静态只读数据。
 *
 * @kind: 页面种类，0 表示转换，1 表示播放。
 * @slot: 当前页面中的零基焦点槽位。
 *
 * 返回：CFG_* 配置项枚举。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_cfg_index(int kind, int slot)
{
	static const int conv[7] = {CFG_INPUT,	 CFG_STYLE,  CFG_RATIO, CFG_FPS,
				    CFG_QUALITY, CFG_OUTPUT, CFG_START};
	static const int play[5] = {CFG_INPUT, CFG_STYLE, CFG_WIDTH, CFG_FPS,
				    CFG_START};
	return kind == 0 ? conv[slot] : play[slot];
}

/*
 * ui_cfg_value - 把单个配置项格式化为显示文本，路径可用占位提示代替空值；
 * 结果写入调用方数组。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @item: 配置项枚举。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_cfg_value(const struct ui_config *st, int item, char *out,
			 size_t cap)
{
	char derived[512];
	switch (item) {
	case CFG_INPUT:
		snprintf(out, cap, "%s",
			 st->input[0] ? st->input
				      : "（未选，Enter 打开文件列表）");
		break;
	case CFG_OUTPUT:
		if (st->output[0]) {
			snprintf(out, cap, "%s", st->output);
		} else if (st->input[0]) {
			miku_ui_derive_output(st->input, st->style, derived,
					      sizeof derived);
			snprintf(out, cap, "自动 → %s", derived);
		} else {
			snprintf(out, cap, "自动（输入名%s）",
				 st->style == STYLE_BLOCK ? "-block.mp4"
							  : "-ascii.mp4");
		}
		break;
	case CFG_STYLE:
		snprintf(out, cap, "%s", ui_style_name(st->style));
		break;
	case CFG_RATIO:
		snprintf(out, cap, "%s", miku_ratio_names[st->ratio]);
		break;
	case CFG_FPS:
		snprintf(out, cap, "%s", ui_fps_name(st->fps));
		break;
	case CFG_QUALITY:
		snprintf(out, cap, "%s", ui_quality_name(st->quality));
		break;
	case CFG_WIDTH:
		if (st->cols_want > 0) {
			snprintf(out, cap, "%d 列", st->cols_want);
		} else {
			snprintf(out, cap, "自动（终端 %d 列）",
				 miku_terminal_columns());
		}
		break;
	default:
		snprintf(out, cap, "%s", "");
		break;
	}
}

/*
 * ui_config_rows - 配置项和鼠标命中行同步生成，避免增删一项后点击位置错位
 * 。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 * @cols: 宽度，单位为终端显示列或字符网格列。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_config_rows(struct strbuf *sb, const struct ui_config *st,
			   int kind, int cols)
{
	int value_col;
	int row;
	int slot;

	static const char *labels_all[8] = {"输入", "风格", "比例", "帧率",
					    "质量", "输出", "宽度", "开始"};
	const int count = miku_ui_cfg_count(kind);

	miku_ui_hit_reset();
	value_col = 12;
	row = 4;
	for (slot = 0; slot < count; ++slot, ++row) {
		int focused;
		char value[552];
		int pad;
		int avail;

		const int item = miku_ui_cfg_index(kind, slot);
		miku_ui_hit_add(row);
		focused = st->focus == slot;

		if (item == CFG_START) {
			miku_ui_row(sb, row + 1);
			miku_ui_hit_add(row + 1);
			if (focused)
				miku_sb_addf(sb, UI_GOOD UI_BOLD "▶ 开始");
			else
				miku_sb_addf(sb, UI_DIM "▶ 开始");
			miku_sb_addf(sb, UI_RESET);
			break;
		}

		ui_cfg_value(st, item, value, sizeof value);

		miku_ui_row(sb, row);
		miku_sb_addf(sb, "%s", focused ? UI_ACCENT "▸ " : "  ");
		miku_sb_addf(sb, "%s", focused ? UI_BOLD : UI_DIM);
		miku_ui_add_text(sb, labels_all[item], -1);
		miku_sb_addf(sb, UI_RESET);
		for (pad = value_col - 2 - miku_ui_text_width(labels_all[item]);
		     pad > 0; --pad) {
			miku_sb_addf(sb, " ");
		}
		if (focused)
			miku_sb_addf(sb, UI_BOLD);
		avail = cols - value_col - 1;
		if (item == CFG_INPUT || item == CFG_OUTPUT)
			miku_ui_add_text_tail(sb, value, avail);
		else
			miku_ui_add_text(sb, value, avail);
		miku_sb_addf(sb, UI_RESET);
	}
}

/*
 * miku_ui_draw_config - 生成配置页标题、项目、状态栏和帮助提示。构建完成后
 * 一次提交屏幕，不在绘制过程中改变配置。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_draw_config(const struct ui_config *st, int kind)
{
	const char *title;
	struct strbuf sb;

	int cols = miku_terminal_columns();
	int rows = miku_terminal_rows();
	if (cols <= 0)
		cols = 80;
	if (rows <= 0)
		rows = 24;
	title = kind == 0 ? "视频转换" : "视频播放";

	sb = (struct strbuf){0};
	miku_ui_clear_screen();
	miku_sb_addf(&sb, "\x1b[H");
	miku_ui_row(&sb, 1);
	miku_sb_addf(&sb, UI_BOLD UI_MIKU);
	miku_ui_add_text(&sb, "MIKU", -1);
	miku_sb_addf(&sb, UI_RESET UI_DIM "  /  " UI_RESET UI_BOLD);
	miku_ui_add_text(&sb, title, -1);
	miku_sb_addf(&sb, UI_RESET);
	miku_ui_row(&sb, 2);
	miku_sb_addf(&sb, UI_DIM "────────────────────────────────" UI_RESET);

	ui_config_rows(&sb, st, kind, cols);

	miku_ui_row(&sb, rows - 1);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_add_text(&sb,
			 "↑↓ 移动  ←→ 调整  Enter 选择/确认  e 手打  Esc 返回",
			 cols - 1);
	miku_sb_addf(&sb, UI_RESET);
	if (st->status[0]) {
		miku_ui_row(&sb, rows);
		miku_sb_addf(&sb, UI_WARN);
		miku_ui_add_text(&sb, st->status, cols - 1);
		miku_sb_addf(&sb, UI_RESET);
	}
	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * miku_ui_cfg_adjust - 按方向切换当前配置项；枚举项循环选择，帧率和宽度保
 * 持既有可选范围。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @item: 配置项枚举。
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_cfg_adjust(struct ui_config *st, int item, int dir)
{
	st->status[0] = '\0';
	switch (item) {
	case CFG_STYLE:
		st->style = !st->style;
		/* 自动派生的输出名跟着风格换后缀，两种风格才各有各的文件。 */
		if (st->output_auto && st->input[0])
			miku_ui_derive_output(st->input, st->style, st->output,
					      sizeof st->output);
		break;
	case CFG_RATIO:
		st->ratio = (st->ratio + dir + RATIO_COUNT) % RATIO_COUNT;
		break;
	case CFG_FPS: {
		int i;

		static const int opts[4] = {0, 24, 30, 60};
		int idx = 0;
		for (i = 0; i < 4; ++i) {
			if (opts[i] == st->fps)
				idx = i;
		}
		idx = (idx + dir + 4) % 4;
		st->fps = opts[idx];
		break;
	}
	case CFG_QUALITY:
		st->quality = (st->quality + dir + 5) % 5;
		break;
	case CFG_WIDTH:
		if (st->cols_want == 0) {
			st->cols_want =
				dir > 0 ? MIN_ASCII_WIDTH : MAX_ASCII_WIDTH;
		} else {
			st->cols_want += dir * 10;
			if (st->cols_want < MIN_ASCII_WIDTH)
				st->cols_want = 0;
			if (st->cols_want > MAX_ASCII_WIDTH)
				st->cols_want = MAX_ASCII_WIDTH;
		}
		break;
	default:
		break;
	}
}
