/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 外部选择器适配：生成 fzf 列表与命令，调用 fzf 或 dialog，读取其输出的路径。
 */

#include "ui_picker_external.h"
#include "ui_directory.h"
#include "ui_terminal.h"
#include "util.h"
#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * fzf_write_list - 给 fzf 的列表含文件名和体积两列；仅写本次扫描的稳定快照
 * 。
 * 成功返回 0，写失败返回 -1，流始终在本函数内关闭。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int fzf_write_list(const char *path, const char *dir)
{
	struct directory_entry *list = NULL;
	FILE *file;
	int count;
	int i;
	int ret = 0;
	double megabytes;

	if (!(miku_ui_load_dir(dir, &list, &count) == 0))
		return -1;
	file = fopen(path, "w");
	if (!file) {
		free(list);
		return -1;
	}
	for (i = 0; i < count; i++) {
		megabytes = list[i].size / (1024.0 * 1024.0);
		if (list[i].is_dir)
			ret = fprintf(file, "%s/\tdir\n", list[i].name);
		else if (megabytes >= 1)
			ret = fprintf(file, "%s\t%.1f MB\n", list[i].name,
				      megabytes);
		else
			ret = fprintf(file, "%s\t%lld KB\n", list[i].name,
				      list[i].size / 1024);
		if (ret < 0)
			break;
	}
	if (fclose(file) != 0)
		ret = -1;
	free(list);
	return ret < 0 ? -1 : 0;
}

/*
 * fzf_command - 三个路径分别转义，返回的完整命令由调用方释放。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 * @result: 外部选择器结果路径或调用方结果对象，见接口说明。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static char *fzf_command(const char *dir, const char *input, const char *result)
{
	char *quoted_dir = miku_shell_quote(dir);
	char *quoted_input = miku_shell_quote(input);
	char *quoted_result = miku_shell_quote(result);
	char *command = NULL;

	if (quoted_dir && quoted_input && quoted_result)
		command = miku_xasprintf(
			"fzf --height=100%% --reverse --cycle --print0 --border=rounded --info=inline --pointer='▶' --color='fg:#d7dde7,bg:#0b0e14,hl:#7eff60,fg+:#7eff60,hl+:#7eff60,border:#2ad4d4,header:#6b7686,prompt:#2ad4d4' --delimiter='\t' --with-nth=1,2 --prompt='%s > ' --header='输入可搜索 · Enter 进入/选定 · Esc 取消' < %s > %s",
			quoted_dir, quoted_input, quoted_result);
	free(quoted_dir);
	free(quoted_input);
	free(quoted_result);
	return command;
}

/*
 * fzf_select - fzf 临时接管终端；无论工具如何退出都重新进入界面。
 * 返回 1 已选、-1 取消、0 无法启动，供调用方决定是否使用内置选择器。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @chosen: 调用方持有的所选结果数组。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int fzf_select(const char *dir, char *chosen, size_t capacity)
{
	char *input;
	char *result;
	char *command = NULL;
	int status;
	int ret = 0;

	input = miku_xasprintf("%s/miku-pick-%ld.in", miku_temp_dir(),
			       (long)getpid());
	result = miku_xasprintf("%s/miku-pick-%ld.out", miku_temp_dir(),
				(long)getpid());
	if (!input || !result) {
		/* 跳到 out_free_paths，仅释放成功分配的路径。 */
		goto out_free_paths;
	}
	if (fzf_write_list(input, dir) == 0)
		command = fzf_command(dir, input, result);
	if (command) {
		miku_ui_leave();
		status = system(command);
		miku_ui_enter();
		ret = status == 0 && miku_ui_read_result(result, chosen,
							 capacity)
			      ? 1
			      : -1;
	}
	remove(input);
	remove(result);
	free(command);
	/* 所有执行路径汇合，临时路径字符串不逃出本函数。 */
out_free_paths:
	if (result)
		free(result);
	if (input)
		free(input);
	return ret;
}

/*
 * miku_ui_pick_with_fzf - 选择目录时重新扫描，选择文件时再次核对可读性，防
 * 止扫描后文件消失。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_with_fzf(char *out, size_t cap, const char *start)
{
	char dir[1024];
	char chosen[1024];
	char *metadata;
	size_t length;
	int ret;

	miku_ui_initial_dir(dir, sizeof(dir), start);
	for (;;) {
		ret = fzf_select(dir, chosen, sizeof(chosen));
		if (ret != 1)
			return ret;
		metadata = strchr(chosen, '\t');
		if (metadata)
			*metadata = '\0';
		length = strlen(chosen);
		if (length && chosen[length - 1] == '/') {
			chosen[length - 1] = '\0';
			miku_ui_descend(dir, sizeof(dir), chosen);
			continue;
		}
		miku_ui_join(out, cap, dir, chosen);
		return miku_ui_accept_path(out, cap) ? 1 : -2;
	}
}

/*
 * miku_ui_pick_with_dialog - 调用外部 dialog 接管终端并读取结果。工具返回
 * 后重新进入备用屏，结果必须再次验证为可读普通文件。
 *
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_pick_with_dialog(char *out, size_t cap, const char *start)
{
	struct stat st;
	char dir_slash[1024];
	char *q;
	char *cmd;
	FILE *p;
	int rc;

	char dir[1024];
	snprintf(dir, sizeof dir, "%s", (start && *start) ? start : ".");

	if (stat(dir, &st) == 0 && !S_ISDIR(st.st_mode))
		miku_ui_parent_dir(dir);
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
		snprintf(dir, sizeof dir, ".");

	/* dialog 把 "名字" 直接接在基路径后面；基路径没有结尾斜杠时会拼成
	 * ".miku.mp4" */

	snprintf(dir_slash, sizeof dir_slash, "%.*s%s",
		 (int)sizeof(dir_slash) - 2, dir,
		 dir[strlen(dir) - 1] == '/' ? "" : "/");
	q = miku_shell_quote(dir_slash);
	if (!q)
		return 0;
	cmd = miku_xasprintf(
		"dialog --stdout --title ' 选择视频 ' --fselect %s 0 0", q);
	free(q);
	if (!cmd)
		return 0;

	miku_ui_leave();
	p = popen(cmd, "r");
	free(cmd);
	rc = -1;
	out[0] = '\0';
	if (p) {
		char line[1024];
		if (fgets(line, sizeof line, p)) {
			const size_t n = strlen(line);
			if (n > 0 && line[n - 1] == '\n')
				line[n - 1] = '\0';
			snprintf(out, cap, "%s", line);
		}
		rc = pclose(p);
	}
	miku_ui_enter();
	if (rc != 0 || !out[0])
		return -1;
	return miku_ui_accept_path(out, cap) ? 1 : -2;
}
