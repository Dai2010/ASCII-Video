/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 转换进度页与完成页：进度条绘制、取消键轮询、ffmpeg 日志重定向，以及播放动作的衔接。
 */

#include "ui_progress.h"
#include "convert.h"
#include "encoder.h"
#include "layout.h"
#include "playback.h"
#include "strbuf.h"
#include "terminal.h"
#include "ui_config.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * miku_ui_draw_note - 显示简短等待提示和可选副标题。仅在界面主线程使用，临
 * 时输出缓冲在提交后释放。
 *
 * @msg: 借用的 UTF-8 提示正文。
 * @sub: 可选副标题，允许 NULL。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_draw_note(const char *msg, const char *sub)
{
	struct strbuf sb;

	int cols = miku_terminal_columns();
	if (cols <= 0)
		cols = 80;
	sb = (struct strbuf){0};
	miku_ui_clear_screen();
	miku_sb_addf(&sb, "\x1b[H");
	miku_ui_row(&sb, 2);
	miku_sb_addf(&sb, UI_ACCENT UI_BOLD);
	miku_ui_add_text(&sb, msg, cols - 1);
	miku_sb_addf(&sb, UI_RESET);
	if (sub) {
		miku_ui_row(&sb, 4);
		miku_sb_addf(&sb, UI_DIM);
		miku_ui_add_text(&sb, sub, cols - 1);
		miku_sb_addf(&sb, UI_RESET);
	}
	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * ui_progress_specification - 编码器尚未选定时省略规格区域，不读取未初始化
 * 的档位字段。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @pg: 当前进度状态，借用。
 * @avail: 当前行可用的终端显示列数。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_progress_specification(struct strbuf *sb,
				      const struct ui_progress *pg, int avail)
{
	char line[256];
	if (pg->res->prof.enc) {
		struct layout layout;
		miku_compute_layout(&pg->res->prof, pg->width_cols,
				    pg->res->info.width, pg->res->info.height,
				    &layout);
		miku_ui_row(sb, 9);
		miku_sb_addf(sb, UI_DIM);
		snprintf(line, sizeof line, "输出 %dx%d @ %dfps · %s · %s",
			 pg->res->prof.width, pg->res->prof.height,
			 pg->res->prof.fps, pg->res->prof.name,
			 pg->style == STYLE_BLOCK ? "色块" : "字符");
		miku_ui_add_text(sb, line, avail);
		miku_sb_addf(sb, UI_RESET);
		miku_ui_row(sb, 10);
		miku_sb_addf(sb, UI_DIM);
		snprintf(line, sizeof line, "%dx%d 格 · %s%s", layout.cols,
			 layout.rows, pg->res->prof.enc->codec,
			 pg->res->prof.enc->hardware ? "（硬件）" : "（软件）");
		miku_ui_add_text(sb, line, avail);
		miku_sb_addf(sb, UI_RESET);
	}
}

/*
 * ui_draw_progress - 根据当前进度快照绘制百分比、剩余时间和规格。总帧数未
 * 知时不伪造百分比或剩余时间。
 *
 * @pg: 当前进度状态，借用。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_draw_progress(const struct ui_progress *pg)
{
	int avail;
	char line[256];
	struct strbuf sb;
	double frac;
	int bar_w;
	int filled;
	int i;

	int cols = miku_terminal_columns();
	if (cols <= 0)
		cols = 80;
	avail = cols - 1;

	sb = (struct strbuf){0};
	miku_sb_addf(&sb, "\x1b[H");
	miku_ui_row(&sb, 1);
	miku_sb_addf(&sb, UI_BOLD UI_ACCENT "转换中" UI_RESET UI_DIM
					    "   q 取消" UI_RESET);

	miku_ui_row(&sb, 3);
	miku_ui_add_text_tail(&sb, pg->st->input, avail);
	miku_ui_row(&sb, 4);
	miku_sb_addf(&sb, UI_DIM "→ " UI_RESET);
	miku_ui_add_text_tail(&sb,
			      pg->st->output[0] ? pg->st->output : "（自动）",
			      avail - 2);

	frac = 0.0;
	if (pg->total > 0) {
		frac = (double)pg->done / (double)pg->total;
		frac = miku_dmax(0, miku_dmin(frac, 1));
	}
	bar_w = cols - 12;
	bar_w = miku_clamp_int(bar_w, 8, 40);
	filled = (int)(frac * bar_w + 0.5);
	miku_ui_row(&sb, 6);
	miku_sb_addf(&sb, UI_GOOD);
	for (i = 0; i < bar_w; ++i)
		miku_sb_addf(&sb, "%s", i < filled ? "█" : "░");
	if (pg->total > 0)
		miku_sb_addf(&sb, UI_RESET " %5.1f%%", frac * 100.0);
	else
		miku_sb_addf(&sb, UI_RESET " 进度未知");

	miku_ui_row(&sb, 7);
	if (pg->total > 0) {
		if (pg->eta >= 0) {
			snprintf(
				line, sizeof line,
				"%lld / %lld 帧 · 已用 %.1f 秒 · 剩余约 %.0f 秒",
				pg->done, pg->total, pg->elapsed, pg->eta);
		} else {
			snprintf(line, sizeof line,
				 "%lld / %lld 帧 · 已用 %.1f 秒", pg->done,
				 pg->total, pg->elapsed);
		}
	} else {
		snprintf(line, sizeof line, "%lld 帧 · 已用 %.1f 秒", pg->done,
			 pg->elapsed);
	}
	miku_ui_add_text(&sb, line, avail);

	ui_progress_specification(&sb, pg, avail);

	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * ui_progress_cb - 同步接收进度快照并更新界面状态，最多每 150ms 重绘一次，
 * 避免终端刷新挤占编码时间。
 *
 * @ud: 调用方回调上下文，仅借用，不释放。
 * @update: 只在同步回调期间有效的进度快照，不得保存指针。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_progress_cb(void *ud, const struct progress_update *update)
{
	double now;

	struct ui_progress *pg = ud;
	pg->done = update->done;
	pg->total = update->total;
	pg->out_frames = update->output_frames;
	pg->elapsed = update->elapsed;
	pg->eta = update->eta;
	now = miku_now_seconds();
	if (pg->last_draw == 0.0)
		miku_ui_clear_screen();
	if (now - pg->last_draw < 0.15)
		return;
	pg->last_draw = now;
	ui_draw_progress(pg);
}

/*
 * ui_abort_cb - 轮询取消按键并同步界面停止标志。取消状态一旦设置即保持到本
 * 次转换结束。
 *
 * @ud: 调用方回调上下文，仅借用，不释放。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_abort_cb(void *ud)
{
	struct ui_progress *pg = ud;
	for (;;) {
		const int k = miku_ui_key(0);
		if (k == UI_NONE)
			break;
		if (k == 'q' || k == 27 || k == 3)
			pg->cancelled = 1;
	}
	if (miku_g_ui_stop)
		pg->cancelled = 1;
	return pg->cancelled;
}

/*
 * ui_log_open - 保存标准错误描述符后重定向到本次转换日志，避免 ffmpeg 错误
 * 覆盖界面。saved 由配对关闭函数恢复。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @saved: 保存/恢复标准错误所用的文件描述符。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_log_open(const char *path, int *saved)
{
	int fd;

	fflush(stderr);
	*saved = dup(STDERR_FILENO);
	if (*saved < 0)
		return 0;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		close(*saved);
		*saved = -1;
		return 0;
	}
	dup2(fd, STDERR_FILENO);
	close(fd);
	return 1;
}

/*
 * ui_log_close - 刷新并恢复之前保存的标准错误描述符，负描述符表示未取得资
 * 源，可直接返回。
 *
 * @saved: 保存/恢复标准错误所用的文件描述符。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_log_close(int saved)
{
	if (saved < 0)
		return;
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
}

/*
 * ui_log_tail - 读取日志末尾的有限片段并选取最后一个非空行。无法读取时返回
 * 空字符串，由调用方提供通用错误提示。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_log_tail(const char *path, char *dst, size_t cap)
{
	FILE *f;
	long size;
	long from;
	char buf[640];
	size_t n;
	const char *last;
	char *p;

	dst[0] = '\0';
	f = fopen(path, "r");
	if (!f)
		return;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	size = ftell(f);
	from = size > 600 ? size - 600 : 0;
	from = from < 0 ? 0 : from;
	fseek(f, from, SEEK_SET);

	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	last = NULL;
	for (p = buf; *p;) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		if (*p)
			last = p;
		if (!nl)
			break;
		p = nl + 1;
	}
	if (last)
		snprintf(dst, cap, "%.*s", (int)cap - 1, last);
}

/*
 * miku_ui_convert - 把配置翻译为转换请求，安装同步进度和取消回调，并在执行
 * 后恢复日志描述符。回调上下文位于本函数栈上，转换返回前始终有效。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @cols: 宽度，单位为终端显示列或字符网格列。
 * @res: 调用方提供的实际转换结果。
 *
 * 返回：0 转换成功，失败或用户取消返回 -EIO；取消标志由回调保存。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_convert(struct ui_config *st, int cols, struct convert_outcome *res)
{
	struct ui_progress pg;
	struct convert_hooks hooks;
	char *log;
	int saved;
	int ok;

	static const char *quality_names[5] = {"auto", "src", "high", "mid",
					       "low"};
	const char *quality =
		quality_names[(unsigned int)st->quality < 5 ? st->quality : 0];

	if (st->style == STYLE_BLOCK)
		setenv("MIKU_NO_HW", "0", 1);
	unsetenv("MIKU_NO_HW");

	memset(&pg, 0, sizeof pg);
	pg.res = res;
	pg.st = st;
	pg.width_cols = cols;
	pg.style = st->style;
	pg.eta = -1;

	memset(&hooks, 0, sizeof hooks);
	hooks.ud = &pg;
	hooks.on_progress = ui_progress_cb;
	hooks.should_abort = ui_abort_cb;

	miku_ui_draw_note("正在读取视频信息、探测编码器…",
			  "首次运行会真编几帧试每个候选，可能要几秒");

	log = miku_xasprintf("%s/miku-ui.log", miku_temp_dir());
	saved = -1;
	if (log)
		ui_log_open(log, &saved);
	ok = miku_run_convert(
		     &(struct convert_request){
			     .input = st->input,
			     .output = st->output,
			     .cols = cols,
			     .quality = quality,
			     .hooks = &hooks,
			     .cancelled = &pg.cancelled,
			     .style = st->style,
			     .ratio = st->ratio,
			     .fps = st->fps,
		     },
		     res) == 0;
	ui_log_close(saved);

	if (!ok) {
		if (pg.cancelled) {
			miku_ui_status(st, "已取消，半成品已删除");
		} else {
			char tail[240];
			ui_log_tail(log ? log : "", tail, sizeof tail);
			if (tail[0]) {
				miku_ui_status(st, "转换失败：%s", tail);
			} else {
				miku_ui_status(st, "转换失败，详情见 %s",
					       log ? log : "日志");
			}
		}
	}
	free(log);
	return (ok) ? 0 : -EIO;
}

/*
 * miku_ui_play - 退出界面备用屏后同步播放，再重新进入配置界面。帧率不超过
 * 源帧率，播放宽度按配置或终端尺寸决定。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @vp: 已探测的源视频信息，只读。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_play(struct ui_config *st, const struct video_info *vp)
{
	int fps;
	int src;

	int cols = st->cols_want;
	if (cols == 0) {
		const int tc = miku_terminal_columns();
		cols = tc > 0 ? tc : DEFAULT_ASCII_WIDTH;
		cols = miku_clamp_int(cols, MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);
	}
	fps = st->fps > 0 ? st->fps
			  : (int)lround((double)vp->fps_num / vp->fps_den);
	src = (int)lround((double)vp->fps_num / vp->fps_den);
	if (fps > src)
		fps = src;
	fps = miku_clamp_int(fps, 1, 60);

	miku_ui_leave();
	miku_play_terminal(
		&(struct playback_options){
			.input = st->input,
			.cols = cols,
			.fps = fps,
			.with_audio = !st->mute,
			.style = st->style,
		},
		vp);
	miku_ui_enter();
	miku_ui_status(st, "播放结束");
}

/*
 * ui_draw_done - 显示最终输出路径、体积、耗时和可执行的下一步动作，不改变
 * 转换结果。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @res: 调用方提供的实际转换结果。
 * @elapsed: 已耗用时间，单位秒。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_draw_done(const struct ui_config *st,
			 const struct convert_outcome *res, double elapsed)
{
	struct stat stt;
	double mb;
	struct strbuf sb;
	char line[200];

	int cols = miku_terminal_columns();
	if (cols <= 0)
		cols = 80;

	mb = 0.0;
	if (stat(st->output, &stt) == 0)
		mb = (double)stt.st_size / (1024.0 * 1024.0);

	sb = (struct strbuf){0};
	miku_ui_clear_screen();
	miku_sb_addf(&sb, "\x1b[H");
	miku_ui_row(&sb, 2);
	miku_sb_addf(&sb, UI_GOOD UI_BOLD "转换完成" UI_RESET);
	miku_ui_row(&sb, 4);
	miku_sb_addf(&sb, UI_DIM "输出  " UI_RESET);
	miku_ui_add_text_tail(&sb, st->output, cols - 8);
	miku_ui_row(&sb, 5);

	snprintf(line, sizeof line, "%.1f MB · 耗时 %.1f 秒", mb, elapsed);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_add_text(&sb, line, cols - 1);
	miku_ui_row(&sb, 6);
	snprintf(line, sizeof line, "%dx%d @ %dfps · %s · %s", res->prof.width,
		 res->prof.height, res->prof.fps, res->prof.enc->codec,
		 st->style == STYLE_BLOCK ? "色块" : "字符");
	miku_ui_add_text(&sb, line, cols - 1);
	miku_sb_addf(&sb, UI_RESET);
	miku_ui_row(&sb, 8);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_add_text(&sb, "p 在终端播放   其它键返回    q 退出", cols - 1);
	miku_sb_addf(&sb, UI_RESET);
	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * miku_ui_wait_done - 显示完成页并等待返回、播放或退出动作。返回动作索引，
 * 调用方负责实际执行，避免绘制函数启动播放器。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @res: 调用方提供的实际转换结果。
 * @elapsed: 已耗用时间，单位秒。
 *
 * 返回：0 返回配置，1 播放结果，2 退出程序。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_wait_done(const struct ui_config *st,
		      const struct convert_outcome *res, double elapsed)
{
	/* 先丢弃转换期间积压的输入，残留的鼠标事件不能直接触发返回 */
	miku_ui_flush_input();
	ui_draw_done(st, res, elapsed);
	for (;;) {
		const int k = miku_ui_key(-1);
		if (k == UI_NONE || miku_g_ui_stop)
			return 2;
		if (k == 'q' || k == 3 || k == 27)
			return 2;
		if (k == 'p')
			return 1;
		/* 鼠标点击与滚轮不算「其它键」：完成页只响应键盘 */
		if (k == UI_CLICK || k == UI_WHEEL_UP || k == UI_WHEEL_DOWN)
			continue;
		return 0;
	}
}
