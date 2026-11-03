/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_AUDIO_H
#define MIKU_AUDIO_H

#include "types.h"

/*
 * miku_has_audio_stream - 探测第一条音轨是否存在。只检查可见流，不启动音频
 * 播放。
 *
 * @input: 借用的输入媒体路径，需以 NUL 结尾。
 *
 * 返回：true 满足所述条件，false 不满足；不转移输入对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
bool miku_has_audio_stream(const char *input);
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
int miku_start_audio(const char *input, const char *ipc_sock);
/*
 * miku_stop_audio - 终止并等待本模块启动的音频子进程。重复调用不会误杀其他
 * 进程，不取得外部进程的所有权。
 *
 * 返回：无；音频子进程已请求终止并等待回收。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_stop_audio(void);
/*
 * miku_audio_time_pos - 非阻塞读取完整 JSON 行并推进一个在途查询。最多每
 * 50ms 发送一次，半行保留到下次调用；超时连接会被丢弃重建。
 *
 * 返回：有效音频位置，单位秒；尚无完整有效回复返回 -1。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
double miku_audio_time_pos(void);
/*
 * miku_audio_ipc_close - 关闭 IPC 并清空未完成请求、半行缓存及查询时间。音
 * 频子进程的终止另由 stop_audio 负责。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_audio_ipc_close(void);

#endif /* MIKU_AUDIO_H */
