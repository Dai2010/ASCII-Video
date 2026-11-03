/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_CONFIG_H
#define MIKU_UI_CONFIG_H

#include "attrs.h"
#include "types.h"

/*
 * miku_ui_status - 在配置状态对象中格式化提示，不直接输出终端。结果按状态
 * 栏容量截断，由下一次整屏重绘显示。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @fmt: printf 格式串，变参类型必须与占位符一致。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_status(struct ui_config *st, const char *fmt, ...) MIKU_PRINTF(2, 3);
/*
 * miku_ui_derive_output - 从输入路径派生带 ASCII 后缀的输出名。只移除文件
 * 名部分的扩展名，避免把目录中的点误当成扩展名。
 *
 * @in: 借用的输入媒体路径，必须以 NUL 结尾。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_derive_output(const char *in, char *out, size_t cap);
/*
 * miku_ui_edit_field - 在调用方路径数组内直接编辑。退格按 UTF-8 字符回退，
 * 容量不足时拒绝继续追加；Esc 结束编辑但保留已输入内容。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @which: 需要编辑的配置项枚举。
 * @title: 借用的 UTF-8 页面标题。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_edit_field(struct ui_config *st, int which, const char *title);
/*
 * miku_ui_cfg_count - 返回当前页面可聚焦项目数，转换页和播放页使用不同项目
 * 映射。
 *
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 返回：当前页面可聚焦的项目数。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_cfg_count(int kind);
/*
 * miku_ui_cfg_index - 把可见槽位映射为配置项枚举。slot 必须小于当前页面项
 * 目数，映射表为静态只读数据。
 *
 * @kind: 页面种类，0 表示转换，1 表示播放。
 * @slot: 当前页面中的零基焦点槽位。
 *
 * 返回：CFG_* 配置项枚举。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_cfg_index(int kind, int slot);
/*
 * miku_ui_draw_config - 生成配置页标题、项目、状态栏和帮助提示。构建完成后
 * 一次提交屏幕，不在绘制过程中改变配置。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @kind: 页面种类，0 表示转换，1 表示播放。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_draw_config(const struct ui_config *st, int kind);
/*
 * miku_ui_cfg_adjust - 按方向切换当前配置项；枚举项循环选择，帧率和宽度保
 * 持既有可选范围。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许更新。
 * @item: 配置项枚举。
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_cfg_adjust(struct ui_config *st, int item, int dir);

#endif /* MIKU_UI_CONFIG_H */
