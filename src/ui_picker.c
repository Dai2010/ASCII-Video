/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 文件选择器：默认使用内置单列列表 —— 常驻输入栏既能过滤，也能当场输入
 * 路径回车进入；行可直接点击。fzf / dialog 只在显式指定时使用，因为
 * Termux 打包的 fzf 不支持鼠标，点了没有反应。
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

/* list 由扫描分配；visible 保存过滤后的下标，两者都由选择器释放。 */
struct picker_state {
	char directory[1024];
	char filter[128];	   /* 常驻输入栏内容，空串表示不过滤。 */
	struct directory_entry *list;
	int *visible;		   /* 过滤后可见记录在 list 中的下标。 */
	int count;
	int shown;		   /* 过滤后可见记录数。 */
	int selected;
	int top;
	int columns;
	int rows;
	int window;
};

/*
 * picker_contains - 大小写不敏感的 ASCII 子串匹配，供输入栏过滤使用。
 * 不解码多字节字符，遇到非 ASCII 字节按原样比较，因此中文目录名也能按
 * 原样匹配。
 *
 * @name: 借用的文件或目录名。
 * @filter: 借用的过滤串。
 *
 */
static int picker_contains(const char *name, const char *filter)
{
	size_t need = strlen(filter);
	size_t i;
	size_t j;

	if (!need)
		return 1;
	for (i = 0; name[i]; i++) {
		for (j = 0; j < need; j++) {
			unsigned char a = (unsigned char)name[i + j];
			unsigned char b = (unsigned char)filter[j];

			if (!a)
				return 0;
			if (a >= 'A' && a <= 'Z')
				a = (unsigned char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (unsigned char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == need)
			return 1;
	}
	return 0;
}

/*
 * picker_apply_filter - 重建可见下标并夹紧焦点。空过滤串表示全部可见。
 *
 * @state: 调用方持有的当前界面状态，非 const 指针允许更新。
 *
 */
static void picker_apply_filter(struct picker_state *state)
{
	int i;

	state->shown = 0;
	for (i = 0; i < state->count; i++) {
		if (picker_contains(state->list[i].name, state->filter))
			state->visible[state->shown++] = i;
	}
	if (state->selected >= state->shown)
		state->selected = state->shown > 0 ? state->shown - 1 : 0;
	if (state->selected < 0)
		state->selected = 0;
	if (state->top > state->selected)
		state->top = state->selected;
}

/*
 * picker_reload - 目录切换后丢弃旧快照与过滤结果；无法读取时回退当前目录。
 *
 * @state: 调用方持有的当前界面状态。
 *
 */
static void picker_reload(struct picker_state *state)
{
	free(state->list);
	free(state->visible);
	state->list = NULL;
	state->visible = NULL;
	state->count = 0;
	state->shown = 0;
	state->selected = 0;
	state->top = 0;
	if (miku_ui_load_dir(state->directory, &state->list, &state->count) !=
	    0) {
		snprintf(state->directory, sizeof(state->directory), ".");
		miku_ui_load_dir(state->directory, &state->list,
				 &state->count);
	}
	state->visible = calloc(state->count > 0 ? (size_t)state->count : 1,
				sizeof(int));
	if (!state->visible) {
		free(state->list);
		state->list = NULL;
		state->count = 0;
		return;
	}
	picker_apply_filter(state);
}

/*
 * picker_metadata - 体积右对齐；空间不足时省略体积而不挤掉文件名。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @entry: 借用的目录记录，仅在目录快照释放前有效。
 * @columns: 可用终端显示列数。
 *
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
 * picker_row - 渲染一条可见记录；列表起始行固定为 4，与鼠标命中计算一致。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @state: 调用方持有的当前界面状态。
 * @slot: 过滤后的零基序号，必须在 shown 之内。
 *
 */
static void picker_row(struct strbuf *buffer, const struct picker_state *state,
		       int slot)
{
	const struct directory_entry *entry =
		&state->list[state->visible[slot]];
	int focused = slot == state->selected;
	const char *color;

	color = entry->is_dir ? UI_ACCENT : UI_DIM;
	if (focused)
		color = UI_BOLD;
	miku_ui_row(buffer, 4 + slot - state->top);
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
 */
static void picker_geometry(struct picker_state *state)
{
	state->columns = miku_terminal_columns();
	state->rows = miku_terminal_rows();
	if (state->columns <= 0)
		state->columns = 80;
	if (state->rows <= 0)
		state->rows = 24;
	state->window = state->rows > 6 ? state->rows - 6 : 1;
	if (state->selected < state->top)
		state->top = state->selected;
	if (state->selected >= state->top + state->window)
		state->top = state->selected - state->window + 1;
	if (state->top < 0)
		state->top = 0;
}

/*
 * picker_draw - 只构建并提交屏幕，不读取按键。第 3 行是常驻输入栏，第 4 行
 * 起是列表，最后一行显示计数。
 *
 * @state: 调用方持有的当前界面状态。
 *
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
		     "  Enter 打开/选定  ← 返回上级  点行即选  Esc 清空"
		     UI_RESET);
	miku_ui_row(&buffer, 2);
	miku_sb_addf(&buffer, UI_DIM);
	miku_ui_add_text_tail(&buffer, state->directory, state->columns - 1);
	miku_sb_addf(&buffer, UI_RESET);
	miku_ui_row(&buffer, 3);
	miku_sb_addf(&buffer, UI_GOOD "> " UI_RESET);
	if (state->filter[0]) {
		miku_ui_add_text_tail(&buffer, state->filter,
				      state->columns - 3);
	} else {
		miku_sb_addf(&buffer, UI_DIM);
		miku_ui_add_text(&buffer, "输入搜索或路径后回车",
				 state->columns - 4);
	}
	miku_sb_addf(&buffer, "\x1b[7m \x1b[0m");
	for (i = state->top;
	     i < state->shown && i < state->top + state->window; i++)
		picker_row(&buffer, state, i);
	if (!state->shown) {
		miku_ui_row(&buffer, 4);
		miku_sb_addf(&buffer, UI_DIM);
		miku_ui_add_text(&buffer,
				 state->count ? "（没有匹配的项）"
					      : "（这个目录里没有子目录或视频文件）",
				 state->columns - 1);
		miku_sb_addf(&buffer, UI_RESET);
	}
	miku_ui_row(&buffer, state->rows - 1);
	miku_sb_addf(&buffer, UI_DIM "%d 项 · 可见 %d · 第 %d 项" UI_RESET,
		     state->count, state->shown,
		     state->shown ? state->selected + 1 : 0);
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
 */
static int picker_navigation(struct picker_state *state, int key)
{
	int count = state->shown > 0 ? state->shown : 1;

	if (key == UI_UP || key == 'k' || key == UI_WHEEL_UP) {
		state->selected = (state->selected + count - 1) % count;
		return 1;
	}
	if (key == UI_DOWN || key == 'j' || key == UI_WHEEL_DOWN) {
		state->selected = (state->selected + 1) % count;
		return 1;
	}
	if (key == UI_LEFT) {
		miku_ui_parent_dir(state->directory);
		state->filter[0] = '\0';
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
 * picker_try_path - 把输入栏内容当路径解析：绝对路径、相对当前目录、`~/`
 * 展开、以及 `.` / `..` 都支持。是目录就切过去，是可读文件就选定。
 *
 * @state: 调用方持有的当前界面状态，命中目录时原地改写。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 已选定可读文件，2 已切换目录，0 未处理。
 *
 */
static int picker_try_path(struct picker_state *state, char *out,
			   size_t capacity)
{
	char full[1024];
	struct stat info;
	const char *home;
	size_t length;

	if (!state->filter[0])
		return 0;
	if (strncmp(state->filter, "~/", 2) == 0) {
		home = getenv("HOME");
		if (!home || !*home)
			return 0;
		snprintf(full, sizeof full, "%s/%s", home, state->filter + 2);
	} else if (state->filter[0] == '/') {
		snprintf(full, sizeof full, "%s", state->filter);
	} else {
		miku_ui_join(full, sizeof full, state->directory,
			     state->filter);
	}
	length = strlen(full);
	while (length > 1 && full[length - 1] == '/')
		full[--length] = '\0';
	if (stat(full, &info) != 0) {
		if (getenv("MIKU_DEBUG_PICK"))
			fprintf(stderr, "\r\ntry_path %s: stat 失败 errno=%d\r\n",
				full, errno);
		return 0;
	}
	if (S_ISDIR(info.st_mode)) {
		snprintf(state->directory, sizeof state->directory, "%s",
			 full);
		state->filter[0] = '\0';
		picker_reload(state);
		return 2;
	}
	snprintf(out, capacity, "%s", full);
	return miku_ui_accept_path(out, capacity) ? 1 : 0;
}

/*
 * picker_builtin - 内置选择器返回 1 选定、0 取消、-1 文件已失效，始终释放
 * 列表与过滤下标。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 */
static int picker_builtin(char *out, size_t capacity, const char *start)
{
	struct picker_state state = {0};
	const struct directory_entry *entry;
	size_t length = 0;
	int key;
	int slot;
	int ret = 0;

	miku_ui_initial_dir(state.directory, sizeof(state.directory), start);
	picker_reload(&state);
	for (;;) {
		picker_draw(&state);
		key = miku_ui_key(-1);
		if (key == 'q' || key == 3 || miku_g_ui_stop)
			break;
		if (key == 27) {
			/* 先清空输入栏，再按一次才退出选择器。 */
			if (state.filter[0]) {
				state.filter[0] = '\0';
				length = 0;
				picker_apply_filter(&state);
				continue;
			}
			break;
		}
		if (key == UI_CLICK) {
			slot = state.top + miku_g_ui_my - 4;
			if (miku_g_ui_my < 4 || slot < 0 ||
			    slot >= state.shown)
				continue;
			state.selected = slot;
			key = '\r';
		}
		if (key == '\r' || key == '\n') {
			int acted = picker_try_path(&state, out, capacity);

			/* 已经切到目标目录就停下，等下一次按键再选文件。 */
			if (acted == 2) {
				length = 0;
				continue;
			}
			if (acted == 1) {
				ret = 1;
				break;
			}
			if (!state.shown)
				continue;
			entry = &state.list[state.visible[state.selected]];
			if (entry->is_dir) {
				miku_ui_descend(state.directory,
						sizeof(state.directory),
						entry->name);
				state.filter[0] = '\0';
				length = 0;
				picker_reload(&state);
				continue;
			}
			miku_ui_join(out, capacity, state.directory,
				     entry->name);
			ret = miku_ui_accept_path(out, capacity) ? 1 : -1;
			break;
		}
		if (key == 127) {
			picker_delete_char(state.filter, &length);
			picker_apply_filter(&state);
			continue;
		}
		if (picker_navigation(&state, key))
			continue;
		/* 方向键、滚轮、点击是命令键，不写进输入栏。 */
		if (key >= UI_UP && key <= UI_IGNORE)
			continue;
		if (key >= UI_TEXT_BYTE) {
			if (length + 1 < sizeof state.filter) {
				state.filter[length++] =
					(char)(key - UI_TEXT_BYTE);
				state.filter[length] = '\0';
				picker_apply_filter(&state);
			}
			continue;
		}
		if (key >= 32 && key < 127 && length + 1 < sizeof state.filter) {
			state.filter[length++] = (char)key;
			state.filter[length] = '\0';
			picker_apply_filter(&state);
		}
	}
	free(state.list);
	free(state.visible);
	return ret;
}

/*
 * miku_ui_pick_file - 默认走内置列表，可以用 MIKU_PICKER=fzf 或
 * MIKU_PICKER=dialog 显式选用外部工具。外部工具启动失败时退回内置列表。
 * out 由调用方提供并容纳 capacity 字节；成功路径会再次验证普通文件可读。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：0 选定，-ECANCELED 用户取消，-ENOENT 所选文件不可读。
 */
int miku_ui_pick_file(char *out, size_t capacity, const char *start)
{
	const char *prefer = getenv("MIKU_PICKER");
	int ret = 0;

	if (prefer && strcmp(prefer, "fzf") == 0 &&
	    miku_command_exists("fzf"))
		ret = miku_ui_pick_with_fzf(out, capacity, start);
	else if (prefer && strcmp(prefer, "dialog") == 0 &&
		 miku_command_exists("dialog"))
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
