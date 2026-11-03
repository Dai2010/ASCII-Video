/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 原单文件实现署名：LinusTrevian（你猜我在线不）
 * 拆分后保留原项目的算法和字体来源说明。
 */

#include "cli.h"
#include "ui.h"

#include <unistd.h>

/*
 * main - 无参数且标准输入输出均为终端时进入交互界面，其余情况交给 CLI。
 * argc/argv 由运行时持有；本函数不取得参数字符串的所有权。
 * 返回所选入口的进程退出码，不提交或启动额外的后台任务。
 *
 * @argc: 运行时提供的参数个数。
 * @argv: 运行时持有的 NUL 结尾参数数组，不释放。
 *
 */
int main(int argc, char *argv[])
{
	if (argc == 1 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
		return miku_tui_main();
	return miku_cli_main(argc, argv);
}
