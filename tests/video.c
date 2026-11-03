/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#include "video.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * main - 验证实际媒体的探测结果，避免缺失元数据让进度固定为零。
 *
 * @argc: 参数数量，必须包含路径和预期帧数。
 * @argv: 借用参数；路径由外部测试负责创建与清理。
 *
 * 返回：0 通过，1 失败；仅在测试主线程调用，无所有权转移。
 */
int main(int argc, char **argv)
{
	struct video_info info;
	long expected;

	if (argc != 3)
		return 1;
	expected = strtol(argv[2], NULL, 10);
	if (miku_probe_video(argv[1], &info) != 0)
		return 1;
	printf("%s: %ld frames, %ld/%ld fps\n", argv[1], info.total_frames,
	       info.fps_num, info.fps_den);
	if (info.width != 160 || info.height != 90)
		return 1;
	if (info.fps_num != 25 * info.fps_den)
		return 1;
	return info.total_frames == expected ? 0 : 1;
}
