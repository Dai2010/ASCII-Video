/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 命令行解析和执行分派；路径字符串借用 argv，临时路径由入口释放。
 */

#include "cli.h"
#include "convert.h"
#include "encoder.h"
#include "playback.h"
#include "terminal.h"
#include "util.h"
#include "video.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 参数字符串借用 argv；temporary_output 是唯一拥有的堆路径。
 * request 复用转换接口，避免 CLI 与界面维护两套参数约定。
 */
struct cli_options {
	struct convert_request request;
	const char *positions[4];
	int count;
	int cursor;
	int want_play;
	int play_only;
	int mute;
	int width_given;
	int terminal_cols;
	char *temporary_output;
};

/*
 * cli_usage - 保持既有命令行帮助和档位表，由调用者提供程序名。
 *
 * @program: 用于帮助文本的程序名，借用 argv[0]。
 *
 */
static void cli_usage(const char *program)
{
	int i;
	printf("用法: %s [--play] <input-video> <output-video> [ASCII宽度|auto] [质量|auto]\n",
	       program);
	printf("示例: %s miku.mp4 ascii.mp4\n", program);
	printf("示例: %s --play miku.mp4 ascii.mp4 160 high\n", program);
	printf("示例: %s miku.mp4 ascii.mp4 auto 1920x1080@30\n", program);
	printf("ASCII宽度: %d-%d，越大字符越多越细，但单个字符越小\n",
	       MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);
	printf("            省略或写 auto 时跟随终端列数（不在终端里运行则用 %d）\n",
	       DEFAULT_ASCII_WIDTH);
	printf("质量档位: auto（默认，按核数/内存/可用硬件编码器自动降级）\n");
	for (i = 0; i < TIER_COUNT; ++i) {
		printf("          %-4s %dx%d @ %dfps\n", miku_tiers[i].name,
		       miku_tiers[i].width, miku_tiers[i].height,
		       miku_tiers[i].fps);
	}
	printf("          也可以直接给尺寸：1920x1080 或 1920x1080@30\n");
	printf("帧率: 档位默认值不超过源帧率；WxH@FPS 指定的帧率原样输出（高于源时重复帧补齐）\n");
	printf("64 位 ARM 手机上优先用硬件编码器\n");
	printf("模式: 默认只转码；--play 转完播放；--play-only 不转码直接播放\n");
	printf("      --play 时省略输出文件名 = 用临时文件，播完（或中断）自动删除\n");
	printf("      --mute 播放时不出声（默认有音轨就会用 mpv/ffplay 放声音）\n");
	printf("界面: 不带任何参数直接运行 %s 会打开终端界面（只在终端里有效）\n",
	       program);
}

/*
 * decimal_string - 空串不是宽度；兼容旧数字缩放参数时由质量解析单独处理空
 * 串。
 *
 * @text: 借用的 NUL 结尾文本，UTF-8 要求见接口说明。
 *
 */
static bool decimal_string(const char *text)
{
	if (!*text)
		return false;
	for (; *text; text++)
		if (*text < '0' || *text > '9')
			return false;
	return true;
}

/*
 * cli_flags - 摘出开关，保留最多四个位置参数，维持原命令行兼容性。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 * @argc: 运行时提供的参数个数。
 * @argv: 运行时持有的 NUL 结尾参数数组，不释放。
 *
 */
static void cli_flags(struct cli_options *options, int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--play") == 0) {
			options->want_play = 1;
		} else if (strcmp(argv[i], "--play-only") == 0) {
			options->want_play = 1;
			options->play_only = 1;
		} else if (strcmp(argv[i], "--mute") == 0) {
			options->mute = 1;
		} else if (strcmp(argv[i], "--block") == 0 ||
			   strcmp(argv[i], "--style=block") == 0) {
			options->request.style = STYLE_BLOCK;
		} else if (options->count < 4) {
			options->positions[options->count++] = argv[i];
		}
	}
}

/*
 * cli_paths - 播放时第二位置参数若是数字或 auto，则它是宽度而非输出路径。
 * 转码且省略输出时只允许 --play，临时路径的所有权留给 options。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 *
 */
static int cli_paths(struct cli_options *options)
{
	const char *argument;
	bool width;

	if (!options->count)
		return -EINVAL;
	options->request.input = options->positions[0];
	options->cursor = 1;
	if (options->count > 1) {
		argument = options->positions[1];
		width = strcmp(argument, "auto") == 0 ||
			decimal_string(argument);
		if (options->want_play && width) {
			options->width_given = 1;
		} else {
			options->request.output = argument;
			options->cursor++;
			options->width_given = options->count > options->cursor;
		}
	}
	if (options->play_only) {
		options->request.output = NULL;
		return 0;
	}
	if (options->request.output)
		return 0;
	if (!options->want_play)
		return -EINVAL;
	options->temporary_output = miku_temp_video_path();
	if (!options->temporary_output)
		return -ENOMEM;
	options->request.output = options->temporary_output;
	return 0;
}

/*
 * cli_width - 整数先检查范围再缩窄；auto 跟随终端并夹紧到允许的字符列数。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 *
 */
static int cli_width(struct cli_options *options)
{
	const char *argument = NULL;
	char *end;
	long width;
	bool automatic = !options->width_given;

	if (options->width_given && options->count > options->cursor) {
		argument = options->positions[options->cursor++];
		automatic = strcmp(argument, "auto") == 0;
		if (!automatic) {
			errno = 0;
			width = strtol(argument, &end, 10);
			if (errno || end == argument || *end ||
			    width < MIN_ASCII_WIDTH || width > MAX_ASCII_WIDTH)
				return -EINVAL;
			options->request.cols = (int)width;
		}
	}
	if (!automatic)
		return 0;
	options->terminal_cols = miku_terminal_columns();
	if (options->terminal_cols > 0)
		options->request.cols = (int)miku_dmax(
			MIN_ASCII_WIDTH,
			miku_dmin(MAX_ASCII_WIDTH, options->terminal_cols));
	return 0;
}

/*
 * cli_quality - 质量可以是档位名或偶数自定义尺寸；旧纯数字参数继续提示后忽
 * 略。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 *
 */
static int cli_quality(struct cli_options *options)
{
	const char *argument;
	int width;
	int height;
	int fps;

	if (options->count <= options->cursor)
		return 0;
	argument = options->positions[options->cursor];
	if (!*argument || decimal_string(argument)) {
		fprintf(stderr,
			"提示: 第4个参数（缩放系数）已取消，字符大小由ASCII宽度自动决定，已忽略\n");
		return 0;
	}
	if (!(miku_parse_custom_size(argument, &width, &height, &fps) == 0) &&
	    strcmp(argument, "auto") != 0 && strcmp(argument, "src") != 0 &&
	    strcmp(argument, "high") != 0 && strcmp(argument, "mid") != 0 &&
	    strcmp(argument, "low") != 0)
		return -EINVAL;
	options->request.quality = argument;
	return 0;
}

/*
 * cli_parse - 顺序验证路径、宽度与质量；临时路径即使后续失败也由入口统一释
 * 放。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 * @argc: 运行时提供的参数个数。
 * @argv: 运行时持有的 NUL 结尾参数数组，不释放。
 *
 */
static int cli_parse(struct cli_options *options, int argc, char *argv[])
{
	int ret;

	cli_flags(options, argc, argv);
	ret = cli_paths(options);
	if (ret)
		return ret;
	ret = cli_width(options);
	if (ret)
		return ret;
	ret = cli_quality(options);
	if (ret)
		return ret;
	if (options->request.output &&
	    strcmp(options->request.input, options->request.output) == 0)
		return -EINVAL;
	return 0;
}

/*
 * cli_play - CLI 播放上限为 24fps；界面可自行选择更高帧率，二者保持既有约
 * 定。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 *
 */
static int cli_play(const struct cli_options *options,
		    const struct video_info *info)
{
	struct playback_options playback = {
		.input = options->request.input,
		.cols = options->request.cols,
		.with_audio = !options->mute,
		.style = options->request.style,
	};

	playback.fps = (int)lround((double)info->fps_num / info->fps_den);
	if (playback.fps < 1)
		playback.fps = 1;
	if (playback.fps > 24)
		playback.fps = 24;
	printf("在终端播放（按 q 退出%s）...\n", options->mute ? "，静音" : "");
	fflush(stdout);
	return miku_play_terminal(&playback, info);
}

/*
 * cli_execute - 转换和直接播放共用探测结果，转换失败时不启动播放。
 *
 * @options: 调用方提供的命令行或播放配置，路径字段均为借用。
 *
 */
static int cli_execute(const struct cli_options *options)
{
	struct convert_outcome result = {0};
	int ret;

	if (options->play_only) {
		if (!(miku_probe_video(options->request.input, &result.info) ==
		      0))
			return -EINVAL;
	} else {
		ret = miku_run_convert(&options->request, &result);
		if (ret)
			return ret;
		printf("成功创建彩色ASCII视频!\n输出文件: %s\n",
		       options->request.output);
	}
	if (options->want_play)
		return cli_play(options, &result.info);
	return 0;
}

/*
 * miku_cli_main - 解析并执行一次命令行请求，返回进程退出码 0/1。
 * 全部参数借用 argv，唯一分配的临时路径在成功和失败时都会释放。
 * 不使用交互界面；SIGPIPE 转为写入失败，由转换和播放函数返回错误。
 *
 * @argc: 运行时提供的参数个数。
 * @argv: 运行时持有的 NUL 结尾参数数组，不释放。
 *
 */
int miku_cli_main(int argc, char *argv[])
{
	struct cli_options options = {
		.request = {.cols = DEFAULT_ASCII_WIDTH, .quality = "auto"},
	};
	int ret;

	if (argc < 3) {
		cli_usage(argv[0]);
		return 1;
	}
	ret = cli_parse(&options, argc, argv);
	if (!ret) {
		signal(SIGPIPE, SIG_IGN);
		ret = cli_execute(&options);
	}
	if (ret)
		fprintf(stderr, "执行失败: %s\n", strerror(-ret));
	if (options.temporary_output && remove(options.temporary_output) != 0 &&
	    errno != ENOENT)
		fprintf(stderr, "临时文件删除失败，请手动清理: %s\n",
			options.temporary_output);
	free(options.temporary_output);
	return ret ? 1 : 0;
}
