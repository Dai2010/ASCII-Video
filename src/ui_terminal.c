/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 界面终端生命周期与按键协议；只由界面主线程访问状态。
 */

#include "ui_terminal.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_ui_term;

static int g_ui_raw = 0;

static int g_ui_alt = 0;

volatile sig_atomic_t miku_g_ui_stop = 0;

int miku_g_ui_my;

static int g_hit_rows[UI_HIT_MAX];

static int g_hit_count = 0;

/*
 * ui_on_signal - 信号处理器只设置界面停止标志，所有终端恢复和资源释放交给
 * 普通执行路径。
 *
 * @sig: 收到的信号编号；处理器只置停止标志。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_on_signal(int sig)
{
	(void)sig;
	miku_g_ui_stop = 1;
}

/*
 * ui_raw_mode - 保存当前终端属性后关闭规范输入和回显；仅成功设置时才标记需
 * 要恢复，所有输入从同一个界面线程读取。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_raw_mode(void)
{
	struct termios t;

	if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_ui_term) != 0)
		return;
	t = g_ui_term;
	t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
		g_ui_raw = 1;
}

/*
 * miku_ui_leave - 关闭鼠标上报、退出备用屏、恢复光标和折行，再还原 termios
 * 。幂等调用不覆盖已交还给 shell 的终端属性。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_leave(void)
{
	if (!g_ui_alt)
		return;
	g_ui_alt = 0;
	fputs(UI_RESET "\x1b[?1006l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l",
	      stdout);
	fflush(stdout);
	if (g_ui_raw) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_ui_term);
		g_ui_raw = 0;
	}
}

/*
 * miku_ui_enter - 进入备用屏并启用鼠标输入，安装只置标志的退出信号处理器。
 * 需与 ui_leave 成对使用，只允许主线程调用。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_enter(void)
{
	ui_raw_mode();
	signal(SIGINT, ui_on_signal);
	signal(SIGTERM, ui_on_signal);
	/* 1000 = 按下/松开上报，1006 = SGR 坐标；触屏点击通常也走这套 */
	fputs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[2J",
	      stdout);
	fflush(stdout);
	g_ui_alt = 1;
}

/*
 * miku_ui_flush_input - 丢弃尚未读取的输入字节。转换期间积压的鼠标松开等事件
 * 在此清空，避免它们被完成页或配置页误解为操作。非原始模式下不做任何事。
 *
 * 线程约束：仅由界面主线程调用。
 */
void miku_ui_flush_input(void)
{
	if (g_ui_raw)
		tcflush(STDIN_FILENO, TCIFLUSH);
}

/*
 * ui_read_raw - 等待并读取一个原始输入字节。负超时表示阻塞，零表示轮询，正
 * 值单位毫秒；超时或读失败返回 UI_NONE。
 *
 * @timeout_ms: 首字节等待毫秒数，负数阻塞、零轮询。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_read_raw(int timeout_ms)
{
	struct timeval tv;
	struct timeval *tvp;
	unsigned char c;

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);

	tvp = NULL;
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}
	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, tvp) <= 0)
		return UI_NONE;
	c = 0;
	if (read(STDIN_FILENO, &c, 1) != 1)
		return UI_NONE;
	return (int)c;
}

/*
 * ui_x10_mouse - X10 鼠标每个字段在原始值上加 32；读取不完整时丢弃该事件。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_x10_mouse(void)
{
	int button = ui_read_raw(20);
	int x = ui_read_raw(20);
	int y = ui_read_raw(20);

	if (button == UI_NONE || x == UI_NONE || y == UI_NONE)
		return UI_NONE;
	miku_g_ui_my = y - 32;
	button -= 32;
	if (button & 64)
		return button & 1 ? UI_WHEEL_DOWN : UI_WHEEL_UP;
	/* 低两位为 3 表示松开，bit 32 为移动；只有按下才算一次点击 */
	if ((button & 3) == 3 || (button & 32))
		return UI_IGNORE;
	return UI_CLICK;
}

/*
 * ui_sgr_mouse - SGR 鼠标序列已经包含终止字节，正文仅含按钮和坐标。
 *
 * @sequence: 以 NUL 结尾的鼠标序列正文，已去掉终止字节。
 * @final: CSI 终止字节，'M' 为按下，'m' 为松开。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_sgr_mouse(const char *sequence, int final)
{
	int button;
	int x;
	int y;

	if (sscanf(sequence, "<%d;%d;%d", &button, &x, &y) != 3)
		return UI_NONE;
	miku_g_ui_my = y;
	if (button & 64)
		return button & 1 ? UI_WHEEL_DOWN : UI_WHEEL_UP;
	/* 'm' 为松开，bit 32 为移动：二者都不触发点击，避免一次点击执行两遍 */
	if (final == 'm' || (button & 32))
		return UI_IGNORE;
	return UI_CLICK;
}

/*
 * ui_csi_sequence - CSI 终止字节为 0x40..0x7e；多余正文被丢弃但仍消费完整
 * 序列。
 * 每字节等待上限 20ms，保持旧键盘和鼠标协议兼容。
 *
 * @first: CSI 序列的第一个正文字节。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_csi_sequence(int first)
{
	char sequence[24];
	int length = 0;
	int last;

	if (first == UI_NONE)
		return UI_NONE;
	if (first == 'M')
		return ui_x10_mouse();
	switch (first) {
	case 'A':
		return UI_UP;
	case 'B':
		return UI_DOWN;
	case 'C':
		return UI_RIGHT;
	case 'D':
		return UI_LEFT;
	default:
		break;
	}
	if (first >= 0x40 && first <= 0x7e)
		return UI_NONE;
	sequence[length++] = (char)first;
	for (;;) {
		last = ui_read_raw(20);
		if (last == UI_NONE)
			return UI_NONE;
		if (last >= 0x40 && last <= 0x7e)
			break;
		if (length < (int)sizeof(sequence) - 1)
			sequence[length++] = (char)last;
	}
	sequence[length] = '\0';
	if (sequence[0] == '<' && (last == 'M' || last == 'm'))
		return ui_sgr_mouse(sequence, last);
	return UI_NONE;
}

/*
 * miku_ui_key - 把输入字节映射为 UI 键值；UTF-8 高位字节加 UI_TEXT_BYTE 保
 * 留原值。
 * timeout_ms<0 表示等待首字节，0 为轮询。只允许界面主线程调用。
 * 未进入原始模式或无输入返回 UI_NONE，单独 Esc 返回 27。
 *
 * @timeout_ms: 首字节等待毫秒数，负数阻塞、零轮询。
 *
 * 返回：UI_*、普通字节或编码后的 UTF-8 字节；无输入为 UI_NONE。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_key(int timeout_ms)
{
	int key;

	if (!g_ui_raw || miku_g_ui_stop)
		return UI_NONE;
	for (;;) {
		key = ui_read_raw(timeout_ms);
		if (key == 0x1b) {
			if (ui_read_raw(20) != '[')
				return 27;
			key = ui_csi_sequence(ui_read_raw(20));
			if (key == UI_IGNORE)
				continue; /* 松开/移动事件对调用方不可见 */
			return key;
		}
		if (key >= 0x80)
			return UI_TEXT_BYTE + key;
		if (key == 8)
			return 127;
		return key;
	}
}

/*
 * miku_ui_hit_index - 查找精确命中行，未命中时再容忍上下各一行，便于触屏选
 * 择。返回槽位索引或 -1。
 *
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_hit_index(int row)
{
	int i;
	int i_next;

	for (i = 0; i < g_hit_count; ++i) {
		if (g_hit_rows[i] == row)
			return i;
	}
	/* 触屏点得没那么准：上下各放宽一行 */
	for (i_next = 0; i_next < g_hit_count; ++i_next) {
		if (g_hit_rows[i_next] == row - 1 ||
		    g_hit_rows[i_next] == row + 1)
			return i_next;
	}
	return -1;
}

/*
 * miku_ui_hit_reset - 每次重绘前清空点击行表，旧屏幕的命中信息不再参与后续
 * 输入解释。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_hit_reset(void)
{
	g_hit_count = 0;
}

/*
 * miku_ui_hit_add - 登记当前屏幕一条可点击行，达到固定容量后不再追加，防止
 * 命中表越界。
 *
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_hit_add(int row)
{
	if (g_hit_count < UI_HIT_MAX)
		g_hit_rows[g_hit_count++] = row;
}
