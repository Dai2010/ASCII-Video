/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_VIDEO_H
#define MIKU_VIDEO_H

#include "types.h"

extern const char *miku_ratio_names[RATIO_COUNT];
/*
 * miku_choose_profile_ex - 在基础档位选择上应用原画、比例和指定帧率。任何
 * 尺寸或帧率改变都重新探测编码器，避免把旧组合的探测结果直接复用。
 *
 * @quality: 质量档位或自定义尺寸字符串。
 * @ratio: 画面比例索引，范围为 0..RATIO_COUNT-1。
 * @fps_want: 用户要求的帧率，零表示跟随源。
 * @vi: 已探测的源视频信息，只读。
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：0 已选定，-ENODEV 表示不能满足请求。
 */
int miku_choose_profile_ex(const char *quality, int ratio, int fps_want,
			   const struct video_info *vi, struct profile *p);
/*
 * miku_probe_video - 探测第一条视频流；path 借用，info 接收尺寸、有理数帧
 * 率和估计帧数。
 * 路径先 shell 转义；管道及命令字符串均在本函数内释放。
 * 成功返回 0；失败不留下动态内存或未回收子进程。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 *
 * 返回：0 获得有效视频信息，失败返回 -EIO。
 */
int miku_probe_video(const char *path, struct video_info *info);

#endif /* MIKU_VIDEO_H */
