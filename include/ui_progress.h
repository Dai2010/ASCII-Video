/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_PROGRESS_H
#define MIKU_UI_PROGRESS_H

#include "types.h"

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
void miku_ui_draw_note(const char *msg, const char *sub);
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
int miku_ui_convert(struct ui_config *st, int cols,
		    struct convert_outcome *res);
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
void miku_ui_play(struct ui_config *st, const struct video_info *vp);
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
		      const struct convert_outcome *res, double elapsed);

#endif /* MIKU_UI_PROGRESS_H */
