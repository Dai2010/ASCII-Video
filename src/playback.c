/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 终端播放调度；按单调时钟显示有效帧，统一恢复终端和回收资源。
 */

#include "playback.h"
#include "audio.h"
#include "encoder.h"
#include "glyph.h"
#include "render.h"
#include "strbuf.h"
#include "terminal.h"
#include "terminal_frame.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios g_saved_term;

static int g_term_raw = 0;

static volatile sig_atomic_t g_play_stop = 0;

/*
 * play_restore_term - 恢复播放前保存的 termios、颜色和光标。原始模式标志避
 * 免重复恢复覆盖下一阶段的终端状态。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void play_restore_term(void)
{
	if (g_term_raw) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_term);
		g_term_raw = 0;
	}
	fputs("\x1b[0m\x1b[?25h", stdout); /* 复位颜色、恢复光标 */
	fflush(stdout);
}

/*
 * play_on_signal - 信号处理器只置停止标志，不执行内存、I/O 或等待子进程等
 * 不安全操作。实际清理由播放主循环完成。
 *
 * @sig: 收到的信号编号；处理器只置停止标志。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void play_on_signal(int sig)
{
	(void)sig;
	g_play_stop = 1;
}

/*
 * play_raw_mode - 保存当前终端属性并关闭规范输入及回显。仅在设置成功后标记
 * 原始模式，失败时不伪造可恢复状态。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void play_raw_mode(void)
{
	struct termios t;

	if (!isatty(STDIN_FILENO) ||
	    tcgetattr(STDIN_FILENO, &g_saved_term) != 0) {
		return;
	}
	t = g_saved_term;
	t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
		g_term_raw = 1;
}

/*
 * play_poll_key - 非阻塞读取单个播放按键。终端未进入原始模式或没有可读字节
 * 时返回零。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int play_poll_key(void)
{
	fd_set fds;
	struct timeval tv;
	unsigned char c;

	if (!g_term_raw)
		return 0;

	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	tv = (struct timeval){0, 0};
	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
		return 0;
	c = 0;
	return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : 0;
}

/*
 * 单次播放状态：所有堆内存和解码管道均在调用结束前释放。
 * 音频是进程级单实例，终端原始模式同样不可重入。
 */
struct playback {
	struct glyph_atlas atlas;
	struct terminal_frame frame;
	struct strbuf buffer;
	FILE *decoder;
	uint8_t *cells;
	size_t cell_bytes;
	int fps;
	int audio;
	double origin;
	char socket_path[256];
};

/*
 * playback_grid - 按终端宽高夹紧网格，保留一行余量。终端字符高宽比约为 2:1
 * ，
 * 因此纵向格数为列数除视频宽高比再除以 2。三次夹紧沿用旧版布局。
 *
 * @frame: 借用的终端采样帧和网格尺寸，不转移图集所有权。
 * @wanted: 请求列数，非正时跟随终端。
 * @info: 源视频信息，非 const 指针用于接收探测结果。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void playback_grid(struct terminal_frame *frame, int wanted,
			  const struct video_info *info)
{
	int cols = miku_terminal_columns();
	int rows = miku_terminal_rows();
	int max_rows;
	int pass;
	double aspect = (double)info->width / info->height;

	if (cols <= 0)
		cols = 80;
	if (rows <= 0)
		rows = 24;
	max_rows = rows > 2 ? rows - 1 : 1;
	frame->cols = wanted > 0 ? wanted : cols;
	if (frame->cols > cols)
		frame->cols = cols;
	for (pass = 0; pass < 3; pass++) {
		rows = (int)lround(frame->cols / aspect * 0.5);
		if (rows <= max_rows)
			break;
		frame->cols = (int)lround(max_rows * aspect * 2.0);
		if (frame->cols > cols)
			frame->cols = cols;
	}
	if (frame->cols < 8)
		frame->cols = 8;
	frame->rows = (int)lround(frame->cols / aspect * 0.5);
	if (frame->rows < 1)
		frame->rows = 1;
	if (frame->rows > max_rows)
		frame->rows = max_rows;
}

/*
 * playback_decoder - 准备采样缓冲和只读解码管道。文件名先独立 shell 转义。
 * 失败返回负 errno；已取得资源保留在 play 中，由统一收尾释放。
 *
 * @play: 单次播放状态，拥有本次解码资源。
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int playback_decoder(struct playback *play, const char *input)
{
	char *quoted;
	char *command;
	int rows = play->frame.rows;
	int ret = 0;

	if (play->frame.style == STYLE_BLOCK)
		rows *= 2;
	play->cell_bytes = (size_t)play->frame.cols * rows * 3;
	play->cells = malloc(play->cell_bytes);
	if (!play->cells)
		return -ENOMEM;
	play->frame.cells = play->cells;
	quoted = miku_shell_quote(input);
	if (!quoted)
		return -ENOMEM;
	command = miku_xasprintf(
		"ffmpeg -nostdin -v error -i %s -an -sn -vf 'fps=%d,scale=%d:%d:flags=area' -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null",
		quoted, play->fps, play->frame.cols, rows);
	free(quoted);
	if (!command)
		return -ENOMEM;
	play->decoder = popen(command, "r");
	if (!play->decoder)
		ret = -errno;
	free(command);
	return ret;
}

/*
 * playback_clock - 等待 mpv 的位置开始前进后建立单调时钟原点。
 * 每次取样均非阻塞，最多等待 80*50ms；没有 IPC 的 ffplay 不等待。
 * 信号只设置停止标志，终端和子进程的清理由正常执行路径完成。
 *
 * @play: 单次播放状态，拥有本次解码资源。
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 * @with_audio: 非零时尝试播放原音轨，失败时允许无声播放。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void playback_clock(struct playback *play, const char *input,
			   int with_audio)
{
	struct timespec delay = {0, 50000000};
	double position;
	int i;

	snprintf(play->socket_path, sizeof(play->socket_path),
		 "%s/miku-audio-%ld.sock", miku_temp_dir(), (long)getpid());
	remove(play->socket_path);
	play->audio = with_audio && miku_has_audio_stream(input) &&
		      (miku_start_audio(input, play->socket_path) == 0);
	play->origin = miku_now_seconds();
	if (!play->audio)
		return;
	atexit(miku_stop_audio);
	if (!miku_command_exists("mpv"))
		return;
	for (i = 0; i < 80 && !g_play_stop; i++) {
		position = miku_audio_time_pos();
		if (position >= 0.12) {
			play->origin = miku_now_seconds() - position;
			break;
		}
		nanosleep(&delay, NULL);
	}
}

/*
 * playback_frames - 顺序解码，过期帧跳过字符拼接；有效帧等到其显示时间后整
 * 体输出。
 * 音频查询没有完整回复时沿用单调时钟，绝不等待 IPC。
 * q 和信号均正常结束；输出错误返回 -EIO，内存错误返回 -ENOMEM。
 *
 * @play: 单次播放状态，拥有本次解码资源。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int playback_frames(struct playback *play)
{
	int64_t frame = 0;
	double position;
	int ret;

	while (!g_play_stop && fread(play->cells, 1, play->cell_bytes,
				     play->decoder) == play->cell_bytes) {
		position = play->audio ? miku_audio_time_pos() : -1.0;
		if (position >= 0)
			play->origin = miku_now_seconds() - position;
		if ((double)(frame + 1) / play->fps >=
		    miku_now_seconds() - play->origin) {
			ret = miku_terminal_frame_build(&play->buffer,
							&play->frame);
			if (ret)
				return ret;
			miku_sleep_until(play->origin +
					 (double)frame / play->fps);
			if (fwrite(play->buffer.buf, 1, play->buffer.len,
				   stdout) != play->buffer.len ||
			    fflush(stdout) != 0)
				return -EIO;
		}
		frame++;
		if (play_poll_key() == 'q')
			break;
	}
	return ferror(play->decoder) ? -EIO : 0;
}

/*
 * playback_release - 无论初始化停在哪一步都可调用；每个指针仅释放本次播放
 * 持有的资源。
 *
 * @play: 单次播放状态，拥有本次解码资源。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void playback_release(struct playback *play)
{
	miku_stop_audio();
	miku_audio_ipc_close();
	if (play->socket_path[0])
		remove(play->socket_path);
	play_restore_term();
	if (play->decoder)
		pclose(play->decoder);
	free(play->atlas.masks);
	free(play->cells);
	free(play->buffer.buf);
}

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
		       const struct video_info *info)
{
	struct playback play = {0};
	int ret;

	play.fps = options->fps;
	play.frame.style = options->style;
	play.frame.atlas = &play.atlas;
	playback_grid(&play.frame, options->cols, info);
	miku_build_gamma_lut();
	if (options->style == STYLE_CHAR &&
	    miku_atlas_build(&play.atlas, 8, 16) != 0)
		return -ENOMEM;
	ret = playback_decoder(&play, options->input);
	if (!ret) {
		g_play_stop = 0;
		signal(SIGINT, play_on_signal);
		signal(SIGTERM, play_on_signal);
		play_raw_mode();
		atexit(play_restore_term);
		fputs("\x1b[?25l\x1b[2J", stdout);
		playback_clock(&play, options->input, options->with_audio);
		ret = playback_frames(&play);
		fputs("\x1b[0m\r\n", stdout);
	}
	playback_release(&play);
	return ret;
}
