/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 界面主状态机：配置页循环、点击与按键分派、开始转换或播放的动作路由，以及交互入口 miku_tui_main。
 */

#include "ui.h"
#include "terminal.h"
#include "ui_config.h"
#include "ui_home.h"
#include "ui_picker.h"
#include "ui_progress.h"
#include "ui_terminal.h"
#include "util.h"
#include "video.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * ui_start - 校验输入及输出配置后执行转换或直接播放，转换完成后把后续动作
 * 交给完成页；所有对象均由配置页持有。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_start(struct ui_config *st, int kind)
{
	int cols;
	struct convert_outcome res;
	double t0;
	double elapsed;
	int choice;

	if (!st->input[0]) {
		miku_ui_status(st, "先填输入文件路径（Enter 编辑）");
		return 0;
	}
	if (access(st->input, R_OK) != 0) {
		miku_ui_status(st, "读不到输入文件: %s", st->input);
		return 0;
	}

	cols = 0;
	if (kind == 0) {
		int tc;

		if (!st->output[0]) {
			miku_ui_derive_output(st->input, st->output,
					      sizeof st->output);
		}
		if (strcmp(st->input, st->output) == 0) {
			miku_ui_status(st, "输出不能和输入同名");
			return 0;
		}
		tc = miku_terminal_columns();
		cols = tc > 0 ? tc : DEFAULT_ASCII_WIDTH;
		cols = miku_clamp_int(cols, MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);
	}

	memset(&res, 0, sizeof res);

	if (kind == 1) { /* 播放：读完信息直接播 */
		miku_ui_draw_note("正在读取视频信息…", NULL);
		if (!(miku_probe_video(st->input, &res.info) == 0)) {
			miku_ui_status(st, "读不到视频信息: %s", st->input);
			return 0;
		}
		miku_ui_play(st, &res.info);
		return 0;
	}

	t0 = miku_now_seconds();
	if (!(miku_ui_convert(st, cols, &res) == 0))
		return 0;
	elapsed = miku_now_seconds() - t0;

	choice = miku_ui_wait_done(st, &res, elapsed);
	if (choice == 1) {
		miku_ui_play(st, &res.info);
		return 0;
	}
	return choice == 2 ? 2 : 0;
}

/*
 * ui_pick_input - 文件选择返回三态，取消时保留原路径，失效时把原因写入状态
 * 栏。
 *
 * @state: 调用方持有的当前界面状态。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_pick_input(struct ui_config *state)
{
	char picked[512];
	int ret;

	ret = miku_ui_pick_file(picked, sizeof(picked),
				state->input[0] ? state->input : ".");
	if (ret == 0)
		snprintf(state->input, sizeof(state->input), "%s", picked);
	else if (ret < 0)
		miku_ui_status(
			state,
			"选中的文件读不了（可能已被删除或没有权限），换一个试试");
}

/*
 * ui_activate - 回车和点击共用激活动作；只有完成页要求退出时返回 1。
 *
 * @state: 调用方持有的当前界面状态。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 * @item: 配置项枚举。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_activate(struct ui_config *state, int kind, int item)
{
	if (item == CFG_START)
		return ui_start(state, kind) == 2;
	if (item == CFG_INPUT)
		ui_pick_input(state);
	else if (item == CFG_OUTPUT)
		miku_ui_edit_field(state, CFG_OUTPUT,
				   "输出视频路径（留空 = 自动）");
	else
		miku_ui_cfg_adjust(state, item, 1);
	return 0;
}

/*
 * ui_config_screen - 配置页循环只管理焦点与分派；参数修改和执行分别由专门
 * 函数负责。
 * kind=0 转换、kind=1 播放；state 由上层保存，返回后仍保持用户设置。
 *
 * @state: 调用方持有的当前界面状态。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_config_screen(struct ui_config *state, int kind)
{
	int key;
	int count;
	int item;
	int hit;
	int direction;

	state->focus = 0;
	state->status[0] = '\0';
	for (;;) {
		miku_ui_draw_config(state, kind);
		key = miku_ui_key(-1);
		if (key == 'q' || key == 27 || key == 3 || miku_g_ui_stop)
			return 0;
		count = miku_ui_cfg_count(kind);
		/*
		 * 点击：先按行定位焦点，再当作一次回车处理。输入层已吞掉松开事件，
		 * 所以一次点击只激活一次；点不到任何行时忽略。
		 */
		if (key == UI_CLICK) {
			hit = miku_ui_hit_index(miku_g_ui_my);
			if (hit < 0)
				continue;
			state->focus = hit;
			key = '\r';
		}
		if (key == UI_UP || key == 'k' || key == UI_DOWN ||
		    key == 'j' || key == '\t' || key == UI_WHEEL_DOWN ||
		    key == UI_WHEEL_UP) {
			direction =
				key == UI_UP || key == 'k' || key == UI_WHEEL_UP
					? -1
					: 1;
			state->focus =
				(state->focus + direction + count) % count;
			continue;
		}
		item = miku_ui_cfg_index(kind, state->focus);
		if (key == 'e' && item == CFG_INPUT)
			miku_ui_edit_field(state, CFG_INPUT,
					   "输入视频路径（手打）");
		else if (key == UI_LEFT || key == UI_RIGHT)
			miku_ui_cfg_adjust(state, item,
					   key == UI_LEFT ? -1 : 1);
		else if ((key == '\r' || key == '\n') &&
			 ui_activate(state, kind, item))
			return 1;
	}
}

/*
 * miku_tui_main - 交互入口分别保存转换和播放配置。终端进入和离开成对管理，
 * 信号或退出菜单都经同一正常收尾路径。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_tui_main(void)
{
	int i;

	/*
	 * 转换与播放各用一份配置，切换时不把另一页面的路径带进来。
	 * 两份配置使用相同初始默认值，后续分别保存用户选择。
	 */
	struct ui_config st[2];
	memset(st, 0, sizeof st);
	for (i = 0; i < 2; ++i) {
		st[i].style = STYLE_CHAR;
		st[i].quality =
			1; /* 默认原画分辨率；要降级再选 auto/high/mid/low */
		if (access("miku.mp4", R_OK) == 0)
			snprintf(st[i].input, sizeof st[i].input, "miku.mp4");
	}
	srand((unsigned)time(NULL) ^ (unsigned)getpid());

	atexit(miku_ui_leave);
	miku_ui_enter();

	for (;;) {
		const int choice = miku_ui_home();
		if (choice == 2 || miku_g_ui_stop)
			break;
		if (ui_config_screen(&st[choice], choice) == 1)
			break;
	}

	miku_ui_leave();
	return 0;
}
