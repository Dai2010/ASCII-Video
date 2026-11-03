/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_TEXT_H
#define MIKU_UI_TEXT_H

#include "types.h"

/*
 * miku_ui_text_width - 累计有效 UTF-8 文本的显示列数，供居中和截断使用；不
 * 把 ANSI 控制序列视为可解析的文本格式。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_text_width(const char *s);
/*
 * miku_ui_add_text - 从有效 UTF-8 文本开头按显示列宽追加，不拆分字符。max_
 * w<0 表示不限制，失败记录在缓冲的 error 字段。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @max_w: 允许的最大显示列数，负数表示不限。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_add_text(struct strbuf *sb, const char *s, int max_w);
/*
 * miku_ui_add_text_tail - 宽度不足时从完整字符边界保留文本尾部，优先显示长
 * 路径的文件名；沿用缓冲的粘性错误处理。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @max_w: 允许的最大显示列数，负数表示不限。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_add_text_tail(struct strbuf *sb, const char *s, int max_w);
/*
 * miku_ui_center_col - 计算非负居中左边距，内容宽于终端时从最左列开始，调
 * 用方仍负责文本截断。
 *
 * @width: 宽度，单位为显示列或像素，见函数说明。
 * @cols: 宽度，单位为终端显示列或字符网格列。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_center_col(int width, int cols);
/*
 * miku_ui_pad - 追加指定数量空格；n<=0 不追加，分配错误保存在缓冲中并由屏
 * 幕提交函数处理。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @n: 数量或帧率显示值，见函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_pad(struct strbuf *sb, int n);
/*
 * miku_ui_row - 追加定位到行首并清除该行的 ANSI 序列，row 使用终端的一基行
 * 号；不直接写终端。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @row: 一基终端行号或零基采样行，见具体函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_row(struct strbuf *sb, int row);
/*
 * miku_ui_flush - 提交完整屏幕，统一检查此前的缓冲错误和终端写入错误。
 * 失败设置界面停止标志，正常流程会退出并恢复终端；不输出分配失败的半帧。
 * 只清空 len，buf 的所有权和容量仍属于调用方，调用方最终 free。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_flush(struct strbuf *buffer);
/*
 * miku_ui_clear_screen - 清空可见屏幕并把光标移到起点，用于模式切换；终端
 * 生命周期由界面模块负责。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_clear_screen(void);

#endif /* MIKU_UI_TEXT_H */
