/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_UI_DIRECTORY_H
#define MIKU_UI_DIRECTORY_H

#include "types.h"

/*
 * miku_ui_load_dir - 扫描子目录和视频文件，生成排序后的目录快照。成功时把
 * 列表所有权交给调用方，由调用方 free；失败释放已分配列表。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @count: 接收实际目录记录数的输出地址。
 *
 * 返回：0 成功且 out/count 已填写，失败返回 -EIO。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_load_dir(const char *dir, struct directory_entry **out, int *count);
/*
 * miku_ui_parent_dir - 在原路径数组中退回父目录，根目录保持根目录。调用方
 * 保证数组有 NUL 结尾且至少可容纳当前目录表示。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_parent_dir(char *dir);
/*
 * miku_ui_join - 把目录和名称连接到独立输出数组，根目录只保留一个分隔符。
 * 输入输出不得重叠，容量不足时沿用有界截断行为。
 *
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_join(char *dst, size_t cap, const char *dir, const char *name);
/*
 * miku_ui_read_result - 读取外部选择器的首条结果，支持 NUL 或换行终止；拒
 * 绝超出调用方数组容量的记录。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @out: 调用方提供的结果地址，不转移其所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 已读到完整结果，0 结果缺失、过长或读取失败。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_read_result(const char *path, char *out, size_t cap);
/*
 * miku_ui_accept_path - 重新检查选择结果是否为可读普通文件；可解析时规范化
 * 为绝对路径，防止工具成功但文件不可用。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：1 普通文件可读，0 不可用。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_accept_path(char *path, size_t cap);
/*
 * miku_ui_initial_dir - 将输入路径解析为起始目录，不可访问时回退到当前目录
 * 。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @start: 起始文件或目录路径，NULL/空串表示当前目录。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_initial_dir(char *dir, size_t capacity, const char *start);
/*
 * miku_ui_descend - 进入子目录或父目录，先写临时缓冲，避免路径拼接的输入输
 * 出重叠。
 *
 * @dir: 目录路径或调整方向，具体单位见函数说明。
 * @capacity: 输出数组容量，单位字节，包含结尾 NUL 空间。
 * @name: 固定名称或单个文件名，不转移所有权。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_descend(char *dir, size_t capacity, const char *name);

#endif /* MIKU_UI_DIRECTORY_H */
