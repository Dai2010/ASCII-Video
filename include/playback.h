/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_PLAYBACK_H
#define MIKU_PLAYBACK_H

#include "types.h"

/* input 借用路径；fps 为正数，cols 为 0 表示跟随终端。 */
struct playback_options {
	const char *input;
	int cols;
	int fps;
	int with_audio;
	int style;
};

/*
 * miku_play_terminal - 播放 options 指定的输入；info 必须来自已成功的视频
 * 探测。
 * 两者均借用，调用期间有效。初始化失败和播放失败均返回负 errno，
 * 正常播完或按 q 返回 0。同步恢复终端，避免把原始模式留给下一界面。
 * 函数使用终端与音频单实例，不允许并发或递归调用。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_play_terminal(const struct playback_options *options,
		       const struct video_info *info);

#endif /* MIKU_PLAYBACK_H */
