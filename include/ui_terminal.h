/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_TERMINAL_H
#define MIKU_UI_TERMINAL_H

#include "types.h"
#include <signal.h>

extern volatile sig_atomic_t miku_g_ui_stop;
extern int miku_g_ui_my;
/*
 * miku_ui_leave - 关闭鼠标上报、退出备用屏、恢复光标和折行，再还原 termios
 * 。幂等调用不覆盖已交还给 shell 的终端属性。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_leave(void);
/*
 * miku_ui_enter - 进入备用屏并启用鼠标输入，安装只置标志的退出信号处理器。
 * 需与 ui_leave 成对使用，只允许主线程调用。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_enter(void);
/*
 * miku_ui_flush_input - 丢弃尚未读取的输入字节，用于阶段切换时清掉积压的鼠标事件。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面主线程调用。
 */
void miku_ui_flush_input(void);
/*
 * miku_ui_key - 把输入字节映射为 UI 键值；UTF-8 高位字节加 UI_TEXT_BYTE 保
 * 留原值。
 * timeout_ms<0 表示等待首字节，0 为轮询。只允许界面主线程调用。
 * 未进入原始模式或无输入返回 UI_NONE，单独 Esc 返回 27。
 *
 * @timeout_ms: 首字节等待毫秒数，负数阻塞、零轮询。
 *
 * 返回：UI_*、普通字节或编码后的 UTF-8 字节；无输入为 UI_NONE。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_key(int timeout_ms);
/*
 * miku_ui_hit_index - 查找精确命中行，未命中时再容忍上下各一行，便于触屏选
 * 择。返回槽位索引或 -1。
 *
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_hit_index(int row);
/*
 * miku_ui_hit_reset - 每次重绘前清空点击行表，旧屏幕的命中信息不再参与后续
 * 输入解释。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_hit_reset(void);
/*
 * miku_ui_hit_add - 登记当前屏幕一条可点击行，达到固定容量后不再追加，防止
 * 命中表越界。
 *
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_hit_add(int row);

#endif /* MIKU_UI_TERMINAL_H */
