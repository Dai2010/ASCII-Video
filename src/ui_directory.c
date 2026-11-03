/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 目录扫描与路径处理；返回的列表由选择器持有并释放。
 */

#include "ui_directory.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * ui_is_video - 按扩展名识别可在选择器中展示的视频文件，ASCII 字母大小写不
 * 敏感；不代替媒体内容探测。
 *
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_is_video(const char *name)
{
	size_t i;

	static const char *exts[13] = {"mp4",  "mkv", "mov", "webm", "avi",
				       "m4v",  "ts",  "flv", "wmv",  "mpg",
				       "mpeg", "3gp", "ogv"};
	const char *dot = strrchr(name, '.');
	if (!dot)
		return 0;
	for (i = 0; i < sizeof exts / sizeof exts[0]; ++i) {
		const char *a = dot + 1;
		const char *b = exts[i];
		while (*a && *b && (*a | 0x20) == *b) {
			++a;
			++b;
		}
		if (*a == '\0' && *b == '\0')
			return 1;
	}
	return 0;
}

/*
 * ui_cmp_entry - 目录优先，其余按名称排序。接收 qsort 提供的两个有效记录地
 * 址，不修改或保留记录。
 *
 * @pa: 第一个待比较目录记录地址。
 * @pb: 第二个待比较目录记录地址。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_cmp_entry(const void *pa, const void *pb)
{
	const struct directory_entry *x = pa;
	const struct directory_entry *y = pb;
	if (x->is_dir != y->is_dir)
		return y->is_dir - x->is_dir; /* 目录排在前面 */
	return strcmp(x->name, y->name);
}

/*
 * 从 readdir 的临时记录取得类型和字节数，不能把其名称指针保存到列表中。
 * dir 和 entry 只借用；is_dir/bytes 由调用方提供，成功时同步填写。
 * 返回 true 表示目录或可展示的视频，false 表示隐藏、不可读或非视频项。
 * 本函数不取得资源所有权，最终目录记录仍由调用方复制和管理。
 */
static bool ui_entry_stat(const char *dir, const struct dirent *entry,
			  int *is_dir, long long *bytes)
{
	char path[1400];
	struct stat status;

	if (strcmp(entry->d_name, ".") == 0)
		return false;
	*is_dir = 1;
	*bytes = 0;
	if (strcmp(entry->d_name, "..") == 0)
		return true;
	snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
	if (stat(path, &status) != 0)
		return false;
	*is_dir = S_ISDIR(status.st_mode);
	if (!*is_dir && !ui_is_video(entry->d_name))
		return false;
	*bytes = *is_dir ? 0 : (long long)status.st_size;
	return true;
}

/*
 * miku_ui_load_dir - 扫描子目录和视频文件，生成排序后的目录快照。成功时把
 * 列表所有权交给调用方，由调用方 free；失败释放已分配列表。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @count: 接收实际目录记录数的输出地址。
 *
 * 返回：0 成功且 out/count 已填写，失败返回 -EIO。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_load_dir(const char *dir, struct directory_entry **out, int *count)
{
	struct directory_entry *list;
	int n;
	int cap;
	struct dirent *de;
	long long dir_size;

	DIR *d = opendir(dir);
	if (!d)
		return -EIO;
	list = NULL;
	n = 0;
	cap = 0;

	dir_size = 0;
	while ((de = readdir(d)) != NULL) {
		int is_dir;

		if (!ui_entry_stat(dir, de, &is_dir, &dir_size))
			continue;
		if (n == cap) {
			const int ncap = cap ? cap * 2 : 32;
			struct directory_entry *tmp =
				realloc(list, (size_t)ncap * sizeof *tmp);
			if (!tmp) {
				free(list);
				closedir(d);
				return -EIO;
			}
			list = tmp;
			cap = ncap;
		}
		snprintf(list[n].name, sizeof list[n].name, "%s", de->d_name);
		list[n].is_dir = is_dir;
		list[n].size = is_dir ? 0 : dir_size;
		++n;
	}
	closedir(d);
	/* 空列表没有存储，避免把 NULL 传给要求有效基址的排序接口。 */
	if (n > 1)
		qsort(list, (size_t)n, sizeof *list, ui_cmp_entry);
	*out = list;
	*count = n;
	return 0;
}

/*
 * miku_ui_parent_dir - 在原路径数组中退回父目录，根目录保持根目录。调用方
 * 保证数组有 NUL 结尾且至少可容纳当前目录表示。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_parent_dir(char *dir)
{
	char *slash = strrchr(dir, '/');
	if (!slash)
		snprintf(dir, 4, ".");
	else if (slash == dir)
		dir[1] = '\0';
	else
		*slash = '\0';
}

/*
 * miku_ui_join - 把目录和名称连接到独立输出数组，根目录只保留一个分隔符。
 * 输入输出不得重叠，容量不足时沿用有界截断行为。
 *
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_join(char *dst, size_t cap, const char *dir, const char *name)
{
	if (strcmp(dir, "/") == 0)
		snprintf(dst, cap, "/%s", name);
	else
		snprintf(dst, cap, "%s/%s", dir, name);
}

/*
 * miku_ui_read_result - 读取外部选择器的首条结果，支持 NUL 或换行终止；拒
 * 绝超出调用方数组容量的记录。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 已读到完整结果，0 结果缺失、过长或读取失败。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_read_result(const char *path, char *out, size_t cap)
{
	FILE *f;
	size_t n;
	size_t i;

	out[0] = '\0';
	f = fopen(path, "rb");
	if (!f)
		return 0;
	n = fread(out, 1, cap - 1, f);
	fclose(f);
	out[n] = '\0';
	for (i = 0; i < n; ++i) {
		if (out[i] == '\n' || out[i] == '\0') {
			out[i] = '\0';
			break;
		}
	}
	return out[0] != '\0';
}

/*
 * miku_ui_accept_path - 重新检查选择结果是否为可读普通文件；可解析时规范化
 * 为绝对路径，防止工具成功但文件不可用。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 普通文件可读，0 不可用。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_accept_path(char *path, size_t cap)
{
	struct stat st;
	char abs[1024];

	if (!path[0])
		return 0;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
	    access(path, R_OK) != 0) {
		return 0;
	}

	if (realpath(path, abs) != NULL)
		snprintf(path, cap, "%s", abs);
	return 1;
}

/*
 * miku_ui_initial_dir - 将输入路径解析为起始目录，不可访问时回退到当前目录
 * 。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_initial_dir(char *dir, size_t capacity, const char *start)
{
	struct stat status;

	snprintf(dir, capacity, "%s", start && *start ? start : ".");
	if (stat(dir, &status) == 0 && !S_ISDIR(status.st_mode))
		miku_ui_parent_dir(dir);
	if (stat(dir, &status) != 0 || !S_ISDIR(status.st_mode))
		snprintf(dir, capacity, ".");
}

/*
 * miku_ui_descend - 进入子目录或父目录，先写临时缓冲，避免路径拼接的输入输
 * 出重叠。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_descend(char *dir, size_t capacity, const char *name)
{
	char next[1024];

	if (strcmp(name, "..") == 0) {
		miku_ui_parent_dir(dir);
		return;
	}
	miku_ui_join(next, sizeof(next), dir, name);
	snprintf(dir, capacity, "%s", next);
}
