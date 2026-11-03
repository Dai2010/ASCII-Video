/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 媒体信息与比例规格；ffprobe 元数据与最终显示尺寸分开处理。
 */

#include "video.h"
#include "encoder.h"
#include "util.h"

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

const char *miku_ratio_names[RATIO_COUNT] = {"原画", "16:9", "9:16",
					     "1:1",  "4:3",  "21:9"};

static const double ratio_values[RATIO_COUNT] = {0.0, 16.0 / 9.0, 9.0 / 16.0,
						 1.0, 4.0 / 3.0,  21.0 / 9.0};

/*
 * ratio_dims - 按画面比例计算偶数尺寸，避免不支持 yuv420p 的奇数尺寸；源比
 * 例仅在 ratio=0 时使用。
 *
 * @ratio: 画面比例索引，范围为 0..RATIO_COUNT-1。
 * @source: 借用的源视频信息。
 * @base_h: 计算比例时使用的基准高度，单位像素。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 */
static void ratio_dims(int ratio, const struct video_info *source, int base_h,
		       int *w, int *h)
{
	const double ar = ratio > 0 ? ratio_values[ratio]
				    : (double)source->width / source->height;
	int ww = (int)lround(base_h * ar) & ~1;
	int hh = base_h & ~1;
	/* 只防「小到 yuv420p 装不下」，绝不上采样 —— 原画档要的就是源的原始尺寸
	 */
	if (ww < 64) {
		ww = 64;
		hh = (int)lround((double)ww / ar) & ~1;
	}
	if (hh < 64)
		hh = 64;
	if (ww > 7680)
		ww = 7680;
	if (hh > 4320)
		hh = 4320;
	*w = ww;
	*h = hh;
}

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
			   const struct video_info *vi, struct profile *p)
{
	int w = 0;
	int h = 0;
	const struct encoder_candidate *enc;

	const double src_fps = (double)vi->fps_num / vi->fps_den;
	const int src_rounded = (int)lround(src_fps);

	/*
	 * 三条路径：
	 *   src          原画分辨率，帧率跟随源（显式 fps 时按显式值）；
	 *   档位/尺寸    交给 miku_choose_profile 决定分辨率与默认帧率；
	 *   显式 fps     无论哪条路径，最后都以用户指定的帧率为准并重新选择编码器。
	 */
	if (strcmp(quality, "src") == 0) {
		int fps;

		ratio_dims(ratio, vi, vi->height & ~1, &w, &h);
		/* 用户显式指定的帧率优先，即使高于源帧率也照此输出 */
		if (fps_want > 0)
			fps = fps_want;
		else
			fps = src_rounded > 0 ? src_rounded : 24;
		enc = miku_pick_encoder(w, h, fps);
		if (!enc)
			return -ENODEV;
		p->width = w;
		p->height = h;
		p->fps = fps;
		p->enc = enc;
		snprintf(p->name, sizeof p->name, "%s", "原画");
		return 0;
	}

	if (!(miku_choose_profile(quality, src_rounded, p) == 0))
		return -ENODEV;

	if (ratio > 0) {

		ratio_dims(ratio, vi, p->height, &w, &h);
		enc = miku_pick_encoder(w, h, p->fps);
		if (!enc)
			return -ENODEV;
		p->width = w;
		p->height = h;
		p->enc = enc;
	}

	/* 显式帧率原样生效；高于源帧率时由转换阶段按源帧重复补齐 */
	if (fps_want > 0 && fps_want != p->fps) {
		enc = miku_pick_encoder(p->width, p->height, fps_want);
		if (!enc)
			return -ENODEV;
		p->fps = fps_want;
		p->enc = enc;
	}
	return 0;
}

/*
 * parse_rate - 解析正有理数或小数帧率，小数转换为分母 1000 的近似分数；成
 * 功时同步写出分子分母。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @num: 输出帧率分子地址。
 * @den: 输出帧率分母地址。
 *
 * 返回：1 成功，0 无法解析正帧率。
 */
static int parse_rate(const char *s, long *num, long *den)
{
	long n;
	long d;
	double v;
	if (sscanf(s, "%ld/%ld", &n, &d) == 2 && n > 0 && d > 0) {
		*num = n;
		*den = d;
		return 1;
	}
	if (sscanf(s, "%lf", &v) == 1 && v > 0.0) {
		*num = (long)lround(v * 1000.0);
		*den = 1000;
		return 1;
	}
	return 0;
}

/* ffprobe 中间信息独立于最终尺寸，用于帧率回退和旋转修正。 */
struct probe_metadata {
	long average_num;
	long average_den;
	long rate_num;
	long rate_den;
	double duration;
	double rotation;
};

/*
 * probe_line - 只解释所请求的 key=value 行。未知或损坏的字段留给最终校验拒
 * 绝。
 * line 是可修改缓冲区；键和值均不逃出本次调用。
 *
 * @line: 可修改的单行解析缓冲，调用期间有效。
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 * @metadata: 本次探测的中间元数据，由调用方持有。
 *
 */
static void probe_line(char *line, struct video_info *info,
		       struct probe_metadata *metadata)
{
	char *value = strchr(line, '=');

	if (!value)
		return;
	*value++ = '\0';
	value[strcspn(value, "\r\n")] = '\0';
	if (strcmp(line, "width") == 0)
		info->width = atoi(value);
	else if (strcmp(line, "height") == 0)
		info->height = atoi(value);
	else if (strcmp(line, "avg_frame_rate") == 0)
		parse_rate(value, &metadata->average_num,
			   &metadata->average_den);
	else if (strcmp(line, "r_frame_rate") == 0)
		parse_rate(value, &metadata->rate_num, &metadata->rate_den);
	else if (strcmp(line, "nb_frames") == 0)
		info->total_frames = atol(value);
	else if (strcmp(line, "duration") == 0)
		metadata->duration = atof(value);
	else if (strcmp(line, "rotation") == 0 ||
		 strcmp(line, "TAG:rotate") == 0)
		metadata->rotation = atof(value);
}

/*
 * probe_finalize - ffmpeg 自动旋转画面，所以 90/270 度元数据必须交换尺寸。
 * 帧率依次使用平均值、基础值和 30fps；帧数未知时由时长估算。
 *
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 * @metadata: 本次探测的中间元数据，由调用方持有。
 *
 */
static void probe_finalize(struct video_info *info,
			   const struct probe_metadata *metadata)
{
	int temporary;

	if ((int)lround(fabs(metadata->rotation)) % 180 == 90) {
		temporary = info->width;
		info->width = info->height;
		info->height = temporary;
	}
	if (metadata->average_num > 0) {
		info->fps_num = metadata->average_num;
		info->fps_den = metadata->average_den;
	} else if (metadata->rate_num > 0) {
		info->fps_num = metadata->rate_num;
		info->fps_den = metadata->rate_den;
	} else {
		info->fps_num = 30;
		info->fps_den = 1;
	}
	if (info->total_frames <= 0 && metadata->duration > 0)
		info->total_frames = (long)(metadata->duration * info->fps_num /
						    info->fps_den +
					    0.5);
}

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
int miku_probe_video(const char *path, struct video_info *info)
{
	struct probe_metadata metadata = {0};
	char *quoted;
	char *command;
	FILE *pipe;
	char line[256];
	int status;

	quoted = miku_shell_quote(path);
	if (!quoted)
		return -EIO;
	command = miku_xasprintf(
		"ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration:stream_side_data=rotation:stream_tags=rotate -of default=noprint_wrappers=1 %s 2>/dev/null",
		quoted);
	free(quoted);
	if (!command)
		return -EIO;
	pipe = popen(command, "r");
	free(command);
	if (!pipe)
		return -EIO;
	memset(info, 0, sizeof(*info));
	while (fgets(line, sizeof(line), pipe))
		probe_line(line, info, &metadata);
	status = pclose(pipe);
	if (!miku_exit_ok(status) || info->width <= 0 || info->height <= 0)
		return -EIO;
	probe_finalize(info, &metadata);
	return 0;
}
