/* SPDX-License-Identifier: GPL-3.0-only */
/* 播放 IPC 回归测试；直接复用实现，不启动音频设备。 */
#define main miku_program_main
#include "../miku.c"
#undef main
#include <assert.h>

int main(void)
{
	int pair[2];
	double start;
	char request[256];
	const char *reply;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	g_audio_ipc = pair[0];
	start = now_seconds();
	assert(audio_time_pos() < 0.0);
	assert(now_seconds() - start < 0.02);
	assert(read(pair[1], request, sizeof request) > 0);
	/* 空数据和事件不能把时钟重置为零。 */
	reply = "{\"event\":\"start-file\"}\n"
		"{\"request_id\":1,\"data\":null}\n";
	assert(write(pair[1], reply, strlen(reply)) == (ssize_t)strlen(reply));
	assert(audio_time_pos() < 0.0);
	/* 半行回复必须保留，直到下一次收到换行。 */
	reply = "{\"request_id\":1,\"data\":";
	assert(write(pair[1], reply, strlen(reply)) == (ssize_t)strlen(reply));
	assert(audio_time_pos() < 0.0);
	reply = "2.5}\n";
	assert(write(pair[1], reply, strlen(reply)) == (ssize_t)strlen(reply));
	assert(audio_time_pos() >= 2.5);
	audio_ipc_close();
	assert(close(pair[1]) == 0);
	puts("playback IPC: PASS");
	return 0;
}
