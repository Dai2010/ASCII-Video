/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 文件选择器入口：优先调用 fzf，其次 dialog；两者都不可用时使用内置单列列表，并统一回传绝对路径。
 */

#include "ui_picker.h"
#include "ui_picker_external.h"
#include "ui_directory.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "terminal.h"
#include "strbuf.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* list 由扫描分配并由选择器释放；屏幕尺寸随每次重绘更新。 */
struct picker_state {
	char directory[1024];
	struct directory_entry *list;
	int count;
	int selected;
	int top;
	int columns;
	int rows;
	int visible;
};

/*
 * picker_reload - 目录切换后丢弃旧快照；无法读取时按既有行为回退当前目录。
 *
 * @state: 调用方持有的当前界面状态。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void picker_reload(struct picker_state *state)
{
	free(state->list);
	state->list = NULL;
	state->count = 0;
	state->selected = 0;
	state->top = 0;
	if ((miku_ui_load_dir(state->directory, &state->list, &state->count) ==
	     0))
		return;
	snprintf(state->directory, sizeof(state->directory), ".");
	miku_ui_load_dir(state->directory, &state->list, &state->count);
}

/*
 * picker_metadata - 体积右对齐；空间不足时省略体积而不挤掉文件名。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @entry: 借用的目录记录，仅在目录快照释放前有效。
 * @columns: 可用终端显示列数。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void picker_metadata(struct strbuf *buffer,
			    const struct directory_entry *entry, int columns)
{
	char text[32];
	double megabytes = entry->size / (1024.0 * 1024.0);
	int padding;

	if (entry->is_dir)
		return;
	if (megabytes >= 1)
		snprintf(text, sizeof(text), "%.1f MB", megabytes);
	else
		snprintf(text, sizeof(text), "%lld KB", entry->size / 1024);
	padding = columns - miku_ui_text_width(entry->name) -
		  (int)strlen(text) - 4;
	if (padding <= 0)
		return;
	miku_ui_pad(buffer, padding);
	miku_sb_addf(buffer, UI_DIM "%s" UI_RESET, text);
}

/*
 * picker_row - 渲染一条文件记录；列表起始行固定为 4，与鼠标命中计算一致。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @state: 调用方持有的当前界面状态。
 * @index: 零基记录索引，必须在当前列表内。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void picker_row(struct strbuf *buffer, const struct picker_state *state,
		       int index)
{
	const struct directory_entry *entry = &state->list[index];
	int focused = index == state->selected;
	const char *color;

	color = entry->is_dir ? UI_ACCENT : UI_DIM;
	if (focused)
		color = UI_BOLD;
	miku_ui_row(buffer, 4 + index - state->top);
	miku_sb_addf(buffer, "%s%s", focused ? UI_ACCENT "▸ " : "  ", color);
	miku_ui_add_text(buffer, entry->name, state->columns - 14);
	if (entry->is_dir)
		miku_sb_addf(buffer, "/");
	miku_sb_addf(buffer, UI_RESET);
	picker_metadata(buffer, entry, state->columns);
}

/*
 * picker_geometry - 尺寸变化时夹紧滚动窗口，至少保留一条可见记录，避免负窗
 * 口尺寸。
 *
 * @state: 调用方持有的当前界面状态。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void picker_geometry(struct picker_state *state)
{
	state->columns = miku_terminal_columns();
	state->rows = miku_terminal_rows();
	if (state->columns <= 0)
		state->columns = 80;
	if (state->rows <= 0)
		state->rows = 24;
	state->visible = state->rows > 6 ? state->rows - 6 : 1;
	if (state->selected < state->top)
		state->top = state->selected;
	if (state->selected >= state->top + state->visible)
		state->top = state->selected - state->visible + 1;
	if (state->top < 0)
		state->top = 0;
}

/*
 * picker_draw - 只构建并提交屏幕，不读取按键，避免绘制与导航状态相互交织。
 *
 * @state: 调用方持有的当前界面状态。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void picker_draw(struct picker_state *state)
{
	struct strbuf buffer = {0};
	int i;

	picker_geometry(state);
	miku_ui_clear_screen();
	miku_sb_addf(&buffer, "\x1b[H");
	miku_ui_row(&buffer, 1);
	miku_sb_addf(&buffer, UI_BOLD UI_ACCENT
		     "选择视频" UI_RESET UI_DIM
		     "  Enter 选定  ← 返回上级  / 输入路径  q 取消"
		     UI_RESET);
	miku_ui_row(&buffer, 2);
	miku_sb_addf(&buffer, UI_DIM);
	miku_ui_add_text_tail(&buffer, state->directory, state->columns - 1);
	miku_sb_addf(&buffer, UI_RESET);
	for (i = state->top;
	     i < state->count && i < state->top + state->visible; i++)
		picker_row(&buffer, state, i);
	if (!state->count) {
		miku_ui_row(&buffer, 4);
		miku_sb_addf(&buffer, UI_DIM);
		miku_ui_add_text(&buffer, "（这个目录里没有子目录或视频文件）",
				 state->columns - 1);
		miku_sb_addf(&buffer, UI_RESET);
	}
	miku_ui_row(&buffer, state->rows - 1);
	miku_sb_addf(&buffer, UI_DIM "%d 项 · 当前第 %d 项" UI_RESET,
		     state->count, state->count ? state->selected + 1 : 0);
	miku_ui_flush(&buffer);
	free(buffer.buf);
}

/*
 * picker_navigation - 处理只改变目录/焦点的按键，返回 1 表示该按键已处理。
 * 空列表取模使用 1 兜底，不允许以零作除数。
 *
 * @state: 调用方持有的当前界面状态。
 * @key: 已解码的 UI 键值或普通输入字节。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int picker_navigation(struct picker_state *state, int key)
{
	int count = state->count ? state->count : 1;

	if (key == UI_UP || key == 'k' || key == UI_WHEEL_UP) {
		state->selected = (state->selected + count - 1) % count;
		return 1;
	}
	if (key == UI_DOWN || key == 'j' || key == UI_WHEEL_DOWN) {
		state->selected = (state->selected + 1) % count;
		return 1;
	}
	if (key == UI_LEFT || key == 127 || key == 'h') {
		miku_ui_parent_dir(state->directory);
		picker_reload(state);
		return 1;
	}
	return 0;
}

/*
 * picker_delete_char - 退格按 UTF-8 字符回退，避免把多字节字符拆成半个。
 *
 * @buffer: 调用方持有的输入缓冲，NUL 结尾。
 * @length: 调用方持有的当前字节数。
 *
 */
static void picker_delete_char(char *buffer, size_t *length)
{
	if (!*length)
		return;
	(*length)--;
	while (*length && ((unsigned char)buffer[*length] & 0xc0) == 0x80)
		(*length)--;
	buffer[*length] = '\0';
}

/*
 * picker_ask_path - 手输一行路径：是目录就切过去重新扫描，是可读文件就选定
 * 回传。相对路径按当前目录拼接，`~/` 展开为家目录。
 *
 * @state: 调用方持有的当前界面状态，命中目录时原地改写。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 已选定可读文件，0 取消或已切换目录。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int picker_ask_path(struct picker_state *state, char *out,
			   size_t capacity)
{
	char buffer[1024];
	char full[1024];
	struct stat info;
	struct strbuf sb;
	const char *home;
	size_t length = 0;
	int columns;
	int key;

	buffer[0] = '\0';
	columns = state->columns > 0 ? state->columns : 80;
	for (;;) {
		sb = (struct strbuf){0};
		miku_ui_clear_screen();
		miku_sb_addf(&sb, "\x1b[H");
		miku_ui_row(&sb, 1);
		miku_sb_addf(&sb, UI_BOLD UI_ACCENT "输入目录或文件路径" UI_RESET
			     UI_DIM "  Enter 进入/选定   Esc 取消" UI_RESET);
		miku_ui_row(&sb, 3);
		miku_sb_addf(&sb, "> ");
		miku_ui_add_text_tail(&sb, buffer, columns - 4);
		miku_sb_addf(&sb, "\x1b[7m \x1b[0m");
		miku_ui_flush(&sb);
		free(sb.buf);

		key = miku_ui_key(-1);
		if (key == UI_NONE || miku_g_ui_stop)
			return 0;
		if (key == 27)
			return 0;
		if (key == '\r' || key == '\n')
			break;
		if (key == 127) {
			picker_delete_char(buffer, &length);
			continue;
		}
		if (key >= UI_UP && key <= UI_IGNORE)
			continue;
		if (key >= UI_TEXT_BYTE) {
			if (length + 1 < sizeof buffer) {
				buffer[length++] = (char)(key - UI_TEXT_BYTE);
				buffer[length] = '\0';
			}
			continue;
		}
		if (key >= 32 && key < 127 && length + 1 < sizeof buffer) {
			buffer[length++] = (char)key;
			buffer[length] = '\0';
		}
	}
	if (!buffer[0])
		return 0;
	if (strncmp(buffer, "~/", 2) == 0) {
		home = getenv("HOME");
		if (!home || !*home)
			return 0;
		snprintf(full, sizeof full, "%s/%s", home, buffer + 2);
	} else if (buffer[0] == '/') {
		snprintf(full, sizeof full, "%s", buffer);
	} else {
		miku_ui_join(full, sizeof full, state->directory, buffer);
	}
	length = strlen(full);
	while (length > 1 && full[length - 1] == '/')
		full[--length] = '\0';
	if (stat(full, &info) != 0)
		return 0;
	if (S_ISDIR(info.st_mode)) {
		snprintf(state->directory, sizeof state->directory, "%s",
			 full);
		picker_reload(state);
		return 0;
	}
	snprintf(out, capacity, "%s", full);
	return miku_ui_accept_path(out, capacity) ? 1 : 0;
}

/*
 * picker_builtin - 内置选择器返回 1 选定、0 取消、-1 文件已失效，始终释放
 * 列表。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int picker_builtin(char *out, size_t capacity, const char *start)
{
	struct picker_state state = {0};
	const struct directory_entry *entry;
	int key;
	int index;
	int ret = 0;

	miku_ui_initial_dir(state.directory, sizeof(state.directory), start);
	picker_reload(&state);
	for (;;) {
		picker_draw(&state);
		key = miku_ui_key(-1);
		if (key == 'q' || key == 27 || key == 3 || miku_g_ui_stop)
			break;
		if (key == UI_CLICK) {
			index = state.top + miku_g_ui_my - 4;
			if (miku_g_ui_my < 4 || index < 0 ||
			    index >= state.count)
				continue;
			state.selected = index;
			key = '\r';
		}
		if (key == '/') {
			if (picker_ask_path(&state, out, capacity) == 1) {
				ret = 1;
				break;
			}
			continue;
		}
		if (picker_navigation(&state, key) || !state.count)
			continue;
		if (key != '\r' && key != '\n')
			continue;
		entry = &state.list[state.selected];
		if (entry->is_dir) {
			miku_ui_descend(state.directory,
					sizeof(state.directory), entry->name);
			picker_reload(&state);
			continue;
		}
		miku_ui_join(out, capacity, state.directory, entry->name);
		ret = miku_ui_accept_path(out, capacity) ? 1 : -1;
		break;
	}
	free(state.list);
	return ret;
}

/*
 * miku_ui_pick_file - 优先外部工具，启动失败才回退。用户取消不会再次弹出另
 * 一种选择器。
 * out 由调用方提供并容纳 capacity 字节；成功路径会再次验证普通文件可读。
 * 返回 0 成功，取消或文件不可读时返回负 errno；不转移字符串所有权。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：0 选定，-ECANCELED 用户取消，-ENOENT 所选文件不可读。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_file(char *out, size_t capacity, const char *start)
{
	int ret = 0;

	if (miku_command_exists("fzf"))
		ret = miku_ui_pick_with_fzf(out, capacity, start);
	if (!ret && miku_command_exists("dialog"))
		ret = miku_ui_pick_with_dialog(out, capacity, start);
	if (ret == 1)
		return 0;
	if (ret == -1)
		return -ECANCELED;
	if (ret == -2)
		return -ENOENT;
	ret = picker_builtin(out, capacity, start);
	if (ret == 1)
		return 0;
	return ret == 0 ? -ECANCELED : -ENOENT;
}
