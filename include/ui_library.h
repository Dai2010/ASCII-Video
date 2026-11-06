/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_LIBRARY_H
#define MIKU_UI_LIBRARY_H

#include "types.h"

/*
 * miku_ui_prepare_output - 在转换开始前根据选定规格生成或调整输出路径。
 * 自动命名时附加风格与分辨率帧率后缀；手动命名且同名异规格文件已存在时，
 * 同样附加规格后缀以避免覆盖旧规格成品。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许原地更新 output，不转移所有权。
 *
 * 返回：无；只更新 st->output 与 st->output_auto。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_prepare_output(struct ui_config *st);
/*
 * miku_ui_library_record - 将一次成功的转换写入持久化列表，按「名称+规格」
 * 去重：同视频同规格更新为最新路径，同视频不同规格并列保留。
 *
 * @st: 调用方持有的界面配置，仅借用，不释放。
 * @cols: 本次转换使用的字符列数。
 * @res: 转换完成后的实际规格与源信息，仅借用，不释放。
 *
 * 返回：无；只执行列表持久化更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_library_record(const struct ui_config *st, int cols,
			    const struct convert_outcome *res);
/*
 * miku_ui_library - 主菜单「已有视频」页面：汇总历史转换与当前目录成品，
 * 同视频同规格去重、同视频异规格并列显示规格，回车或点按直接在终端播放。
 *
 * 返回：0 返回主菜单，1 退出程序。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_library(void);

#endif /* MIKU_UI_LIBRARY_H */
