/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_SETTINGS_H
#define MIKU_UI_SETTINGS_H

/*
 * miku_settings_get_font_size - 获取当前设置的 Kitty 播放字号。
 *
 * 返回：4.0 到 12.5 之间的字号值；未保存时默认返回 6.0。
 * 线程约束：仅由主线程调用。
 */
double miku_settings_get_font_size(void);

/*
 * miku_settings_set_font_size - 设置并保存 Kitty 播放字号。
 *
 * @size: 目标字号，截断至 4.0 到 12.5，四舍五入到 0.5 步长。
 *
 * 返回：无；更新内存状态并保存到配置文件。
 * 线程约束：仅由主线程调用。
 */
void miku_settings_set_font_size(double size);

/*
 * miku_ui_settings - 主菜单「设置」界面：直接使用方向键选择字体大小。
 *
 * 返回：0 正常返回主菜单，1 退出程序。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_settings(void);

#endif /* MIKU_UI_SETTINGS_H */
