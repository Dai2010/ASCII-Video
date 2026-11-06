/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#include "video.h"
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * main - 验证实际媒体探测结果与 9:16 顺时针旋转自适应布局。
 *
 * @argc: 参数数量，必须包含路径和预期帧数。
 * @argv: 借用参数；路径由外部测试负责创建与清理。
 *
 * 返回：0 通过，1 失败；仅在测试主线程调用，无所有权转移。
 */
int main(int argc, char **argv)
{
	struct video_info info;
	struct profile p;
	struct layout layout;
	long expected;
	int rot;
	int v_w;
	int v_h;

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
	if (info.total_frames != expected)
		return 1;

	/* 验证横屏视频转 9:16 时的顺时针 90 度宽高互换与全画幅铺满 */
	if (miku_choose_profile_ex("src", 2, 0, &info, &p) != 0)
		return 1;
	if (p.width != 90 || p.height != 160)
		return 1;
	rot = (p.width < p.height && info.width >= info.height);
	if (!rot)
		return 1;
	v_w = rot ? info.height : info.width;
	v_h = rot ? info.width : info.height;
	miku_compute_layout(&p, 45, v_w, v_h, &layout);
	if (layout.cols != 22 || layout.rows != 20 || layout.off_x != 0 ||
	    layout.off_y != 0)
		return 1;
	return 0;
}
