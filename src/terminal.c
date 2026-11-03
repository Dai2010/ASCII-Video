/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 终端尺寸查询：列数与行数。查询失败时返回非正值，由调用方决定回退值。
 */

#include "terminal.h"

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * miku_terminal_columns - 优先从标准输出读取终端列数，失败再尝试标准输入；
 * 两者都不可用时返回零，布局方自行选择默认值。
 *
 * 返回：正列数，无法探测返回 0。
 */
int miku_terminal_columns(void)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
			return 0;
	}
	return (int)ws.ws_col;
}

/*
 * miku_terminal_rows - 优先从标准输出读取终端行数，失败再尝试标准输入；返
 * 回零表示当前无法获得尺寸。
 *
 * 返回：正行数，无法探测返回 0。
 */
int miku_terminal_rows(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0) {
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0)
			return 0;
	}
	return (int)ws.ws_row;
}
