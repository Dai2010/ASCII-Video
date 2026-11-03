/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 音频进程与非阻塞 IPC；所有播放状态只由播放主线程访问。
 */

#include "audio.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * miku_has_audio_stream - 探测第一条音轨是否存在。只检查可见流，不启动音频
 * 播放。
 *
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 *
 * 返回：true 满足所述条件，false 不满足；不转移输入对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
bool miku_has_audio_stream(const char *input)
{
	char *cmd;
	FILE *p;
	char line[32];
	int has;

	char *q = miku_shell_quote(input);
	if (!q)
		return 0;
	cmd = miku_xasprintf(
		"ffprobe -v error -select_streams a:0 -show_entries stream=index "
		"-of csv=p=0 %s 2>/dev/null",
		q);
	free(q);
	if (!cmd)
		return 0;
	p = popen(cmd, "r");
	free(cmd);
	if (!p)
		return 0;

	has = fgets(line, sizeof line, p) != NULL;
	pclose(p);
	return has;
}

static pid_t miku_g_audio_pid = -1;

static char miku_g_audio_sock[256];

/*
 * miku_start_audio - 优先启动 mpv 的纯音频子进程，缺失时回退 ffplay。成功
 * 只表示 fork 完成，播放器的后续错误通过 IPC 可用性体现。
 *
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 * @ipc_sock: 本次音频进程的 Unix 套接字路径，允许空串表示不启用 IPC。
 *
 * 返回：0 表示已创建子进程；无播放器为 -ENOENT，fork 失败为负 errno。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_start_audio(const char *input, const char *ipc_sock)
{
	pid_t pid;

	const char *player = NULL;
	const char *args[12];
	int n = 0;
	static char ipc_arg[300];

	if (miku_command_exists("mpv")) {
		player = "mpv";
		args[n++] = "mpv";
		args[n++] = "--no-config";
		args[n++] = "--no-video";
		args[n++] = "--really-quiet";
		args[n++] = "--no-terminal";
		if (ipc_sock && *ipc_sock) { /* 有了它才能问到音频播到第几秒 */
			snprintf(ipc_arg, sizeof ipc_arg,
				 "--input-ipc-server=%s", ipc_sock);
			args[n++] = ipc_arg;
		}
		args[n++] = "--";
		args[n++] = input;
	} else if (miku_command_exists("ffplay")) {
		player = "ffplay";
		args[n++] = "ffplay";
		args[n++] = "-nodisp";
		args[n++] = "-autoexit";
		args[n++] = "-loglevel";
		args[n++] = "quiet";
		args[n++] = input;
	} else {
		return -ENOENT;
	}
	args[n] = NULL;

	pid = fork();
	if (pid == 0) {
		const int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execvp(player, (char *const *)args);
		_exit(127);
	}
	if (pid < 0)
		return -errno;
	snprintf(miku_g_audio_sock, sizeof(miku_g_audio_sock), "%s",
		 ipc_sock ? ipc_sock : "");
	miku_g_audio_pid = pid;
	return 0;
}

/*
 * miku_stop_audio - 终止并等待本模块启动的音频子进程。重复调用不会误杀其他
 * 进程，不取得外部进程的所有权。
 *
 * 返回：无；音频子进程已请求终止并等待回收。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_stop_audio(void)
{
	if (miku_g_audio_pid > 0) {
		int status;

		kill(miku_g_audio_pid, SIGTERM);
		status = 0;
		waitpid(miku_g_audio_pid, &status, 0);
		miku_g_audio_pid = -1;
	}
}

static int g_audio_ipc = -1;

/*
 * audio_ipc_open - 按需连接本次播放的 Unix 套接字。已经连接时直接复用，失
 * 败不阻塞等待播放器启动。
 *
 * 返回：1 已连接，0 尚不可连接；不改变音频播放状态。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int audio_ipc_open(void)
{
	struct sockaddr_un addr;
	int fd;

	if (g_audio_ipc >= 0)
		return 1;

	if (miku_g_audio_sock[0] == '\0' ||
	    strlen(miku_g_audio_sock) >= sizeof addr.sun_path) {
		return 0;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, miku_g_audio_sock, strlen(miku_g_audio_sock) + 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return 0;
	}
	g_audio_ipc = fd;
	return 1;
}

static int g_audio_pending;

static double g_audio_sent;

static char g_audio_reply[4096];

static size_t g_audio_reply_len;

/*
 * audio_parse_position - 仅接受当前请求编号的数值位置。事件、null 和非有限
 * 数不能用来校时，避免时钟错误回到零。
 *
 * @line: 可修改的单行解析缓冲，调用期间有效。
 *
 * 返回：当前估计音频秒数，无有效回复返回 -1。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static double audio_parse_position(char *line)
{
	char *id = strstr(line, "\"request_id\"");
	char *data = strstr(line, "\"data\"");
	char *end;
	double pos;

	if (!id || !(id = strchr(id, ':')) || strtol(id + 1, NULL, 10) != 1)
		return -1.0;
	g_audio_pending = 0;
	if (!data || !(data = strchr(data, ':')))
		return -1.0;
	pos = strtod(data + 1, &end);
	if (end == data + 1 || !isfinite(pos) || pos < 0.0)
		return -1.0;
	/* 把采样时刻外推到当前时刻，减小查询往返带来的误差。 */
	return pos + (miku_now_seconds() - g_audio_sent) * 0.5;
}

/*
 * miku_audio_time_pos - 非阻塞读取完整 JSON 行并推进一个在途查询。最多每
 * 50ms 发送一次，半行保留到下次调用；超时连接会被丢弃重建。
 *
 * 返回：有效音频位置，单位秒；尚无完整有效回复返回 -1。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
double miku_audio_time_pos(void)
{
	const char *req =
		"{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":1}\n";
	double pos = -1.0;
	ssize_t n;
	char *line;

	if (!audio_ipc_open())
		return -1.0;
	n = recv(g_audio_ipc, g_audio_reply + g_audio_reply_len,
		 sizeof g_audio_reply - 1 - g_audio_reply_len, MSG_DONTWAIT);
	if (n > 0) {
		g_audio_reply_len += (size_t)n;
		g_audio_reply[g_audio_reply_len] = '\0';
		while ((line = strchr(g_audio_reply, '\n')) != NULL) {
			double sample;
			size_t used;

			*line = '\0';
			sample = audio_parse_position(g_audio_reply);
			if (sample >= 0.0)
				pos = sample;
			used = (size_t)(line - g_audio_reply) + 1;
			g_audio_reply_len -= used;
			memmove(g_audio_reply, line + 1, g_audio_reply_len + 1);
		}
	}
	if (n == 0 ||
	    (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
	     errno != EINTR) ||
	    g_audio_reply_len == sizeof g_audio_reply - 1 ||
	    (g_audio_pending && miku_now_seconds() - g_audio_sent > 0.5)) {
		close(g_audio_ipc);
		g_audio_ipc = -1;
		g_audio_pending = 0;
		g_audio_reply_len = 0;
		return -1.0;
	}
	/* 每 50 毫秒最多一个请求，不积压查询。 */
	if (!g_audio_pending && miku_now_seconds() - g_audio_sent >= 0.05) {
		n = send(g_audio_ipc, req, strlen(req),
			 MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n != (ssize_t)strlen(req)) {
			close(g_audio_ipc);
			g_audio_ipc = -1;
			g_audio_reply_len = 0;
			return pos;
		}
		g_audio_pending = 1;
		g_audio_sent = miku_now_seconds();
	}
	return pos;
}

/*
 * miku_audio_ipc_close - 关闭 IPC 并清空未完成请求、半行缓存及查询时间。音
 * 频子进程的终止另由 stop_audio 负责。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_audio_ipc_close(void)
{
	g_audio_pending = 0;
	g_audio_reply_len = 0;
	g_audio_sent = 0.0;
	if (g_audio_ipc >= 0) {
		close(g_audio_ipc);
		g_audio_ipc = -1;
	}
}
