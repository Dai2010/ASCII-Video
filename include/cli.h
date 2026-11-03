/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_CLI_H
#define MIKU_CLI_H

/* 命令行入口返回进程退出码：0 成功，1 用法错误或执行失败。 */
/*
 * miku_cli_main - 解析并执行一次命令行请求，返回进程退出码 0/1。
 * 全部参数借用 argv，唯一分配的临时路径在成功和失败时都会释放。
 * 不使用交互界面；SIGPIPE 转为写入失败，由转换和播放函数返回错误。
 *
 * @argc: 运行时提供的参数个数。
 * @argv: 运行时持有的 NUL 结尾参数数组，不释放。
 *
 */
int miku_cli_main(int argc, char *argv[]);

#endif /* MIKU_CLI_H */
