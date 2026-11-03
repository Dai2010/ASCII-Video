/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_CONVERT_H
#define MIKU_CONVERT_H

#include "types.h"

/*
 * 所有路径、质量字符串和回调均为借用，整次转换期间必须有效。
 * cancelled 可为 NULL；非空时由界面回调置位，禁止取消后自动重试。
 */
struct convert_request {
	const char *input;
	const char *output;
	const char *quality;
	const struct convert_hooks *hooks;
	int *cancelled;
	int cols;
	int style;
	int ratio;
	int fps;
};

/*
 * miku_run_convert - 探测、选档、转换、末段检查和软件回退的唯一入口。
 * request 借用，out 由调用方提供并接收实际采用的视频规格。
 * 成功返回 0；参数/探测失败返回 -EINVAL，执行失败返回对应负 errno。
 * 转换可能同步回调 hooks，并使用 ffmpeg 子进程；终端界面须串行调用。
 *
 * @request: 借用的转换请求，路径和回调在本次执行期间必须有效。
 * @out: 调用方提供的结果地址，不转移其所有权。
 *
 */
int miku_run_convert(const struct convert_request *request,
		     struct convert_outcome *out);

#endif /* MIKU_CONVERT_H */
