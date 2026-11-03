/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UTIL_H
#define MIKU_UTIL_H

#include "attrs.h"
#include "types.h"

/*
 * miku_dmin - 返回两个浮点数中较小的一个，供布局与颜色计算复用，无资源和全
 * 局状态。
 *
 * @a: 第一个数值。
 * @b: 第二个数值。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 */
double miku_dmin(double a, double b);
/*
 * miku_dmax - 返回两个浮点数中较大的一个，供布局与颜色计算复用，无资源和全
 * 局状态。
 *
 * @a: 第一个数值。
 * @b: 第二个数值。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 */
double miku_dmax(double a, double b);
/*
 * miku_xasprintf - 两次格式化生成精确长度的字符串，避免固定大小临时数组。
 * 成功返回拥有的堆字符串，调用方 free；格式化或分配失败返回 NULL。
 *
 * @fmt: printf 格式串，变参类型必须与占位符一致。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 */
char *miku_xasprintf(const char *fmt, ...) MIKU_PRINTF(1, 2);
/*
 * miku_shell_quote - 为一个完整 shell 参数添加单引号，并转义参数内部的单引
 * 号。返回拥有的堆字符串，不能再放进另一层单引号中使用。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 */
char *miku_shell_quote(const char *s);
/*
 * miku_exit_ok - 解释 system/pclose/wait 风格的原始退出状态。只有正常退出
 * 且退出码为零才返回 true，不能传入已解码退出码。
 *
 * @status: system/pclose/wait 返回的原始状态值。
 *
 * 返回：true 满足所述条件，false 不满足；不转移输入对象所有权。
 */
bool miku_exit_ok(int status);
/*
 * miku_command_exists - 通过 shell 的 command -v 检查固定工具名。name 必须
 * 是程序内确定的命令名，不能直接传入未验证的用户文本。
 *
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 返回：true 满足所述条件，false 不满足；不转移输入对象所有权。
 */
bool miku_command_exists(const char *name);
/*
 * miku_now_seconds - 读取单调时钟并换算为秒。只用于持续时间和播放调度，不
 * 代表日历时间，不受系统时间调整影响。
 *
 * 返回：单调时钟秒数，只适合比较同一运行环境的时刻。
 */
double miku_now_seconds(void);
/*
 * miku_sleep_until - 等待到单调时钟指定时刻。被信号打断后重新计算剩余时间
 * ，已过期的截止时刻直接返回。
 *
 * @deadline: 单调时钟截止秒数，必须与 now_seconds 使用同一时钟。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 */
void miku_sleep_until(double deadline);
/*
 * miku_temp_video_path - 按临时目录、进程号和当前秒数生成临时视频路径，只
 * 分配路径字符串，不创建文件。调用方负责文件和字符串的清理。
 *
 * 返回：拥有的堆路径，调用方 free；失败返回 NULL。
 */
char *miku_temp_video_path(void);

/*
 * miku_temp_dir - 借用 TMPDIR，其次 TMP，未设置时使用 /tmp。调用方不能
 * free 返回值，也不能在使用期间改写相应环境变量。
 *
 * 返回：借用的环境变量或静态字符串，不得 free。
 */
const char *miku_temp_dir(void);
/*
 * miku_clamp_int - 返回闭区间 [minimum,maximum] 内的值；调用方保证 minimum
 * <=maximum。
 *
 * @value: 待限制到区间内的整数。
 * @minimum: 夹紧区间下界，包含端点。
 * @maximum: 夹紧区间上界，包含端点。
 *
 */
int miku_clamp_int(int value, int minimum, int maximum);

#endif /* MIKU_UTIL_H */
