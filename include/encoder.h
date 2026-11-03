/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_ENCODER_H
#define MIKU_ENCODER_H

#include "types.h"

extern const struct tier miku_tiers[TIER_COUNT];
extern const struct encoder_candidate miku_encoders[];
/*
 * miku_stamp_bt709 - 用无损流复制补写 BT.709 色彩标签。临时文件完成后才替
 * 换输出；失败清理临时文件并保留原输出。
 *
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 *
 * 返回：0 已补色标，失败返回 -EIO；原输出在替换前保持有效。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_stamp_bt709(const char *output);
/*
 * miku_total_memory_mb - 读取可用的物理内存信息，Linux 优先使用
 * /proc/meminfo，其他平台尝试现有系统接口。探测失败返回未知值。
 *
 * 返回：物理内存 MiB 数，0 表示探测失败。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
long miku_total_memory_mb(void);
/*
 * miku_cpu_cores - 优先读取正数 MIKU_CORES 覆盖值，否则询问在线处理器数，
 * 失败保守使用一个核心。
 *
 * 返回：至少为 1 的可用核心数。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_cpu_cores(void);
/*
 * miku_encoder_listed - 按候选索引查询并缓存 ffmpeg 是否列出编码器。缓存只
 * 表示注册情况，不替代实际编码探测。
 *
 * @idx: 候选编码器索引，必须处于候选表范围内。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_encoder_listed(int idx);
/*
 * miku_encoder_args - 为指定尺寸生成候选编码器的参数。x264 使用质量参数，
 * 其他有固定参数者直接复制，否则根据像素率估算码率。
 *
 * @c: 借用的编码器候选，只读。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
char *miku_encoder_args(const struct encoder_candidate *c, int w, int h,
			int fps);
/*
 * miku_cand_input_prefix - 返回编码器必须置于输入之前的参数；VAAPI 按本机
 * 设备扫描结果提供前缀，返回值均为借用。
 *
 * @c: 借用的编码器候选，只读。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
const char *miku_cand_input_prefix(const struct encoder_candidate *c);
/*
 * miku_encoder_works - 实际编码少量黑帧再解码计数。两个步骤均成功才接受候
 * 选，所有探测命令、临时文件和路径均在返回前释放。
 *
 * @c: 借用的编码器候选，只读。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_encoder_works(const struct encoder_candidate *c, int w, int h,
			int fps);
/*
 * miku_pick_encoder - 按候选表顺序返回首个实际可用编码器。MIKU_NO_HW 非零
 * 时跳过硬件项；返回静态表内借用指针。
 *
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
const struct encoder_candidate *miku_pick_encoder(int w, int h, int fps);
/*
 * miku_output_tail_ok - 抽查输出末尾两秒的可解码帧数，至少需要一秒输出帧数
 * 。用来发现部分硬件编码器中途漏帧。
 *
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_output_tail_ok(const char *output, const struct profile *p);
/*
 * miku_parse_custom_size - 解析 WxH 或 WxH@FPS。宽高必须为支持范围内的偶数
 * ，省略帧率时写入零，表示跟随源帧率。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：0 成功并写出宽高帧率，-EINVAL 表示尺寸或帧率不合法。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_parse_custom_size(const char *s, int *w, int *h, int *fps);
/*
 * miku_choose_profile - 解析显式尺寸或档位，或执行自动预算选择；所有结果都
 * 必须匹配实际可用的编码器。档位默认帧率不超过源帧率；WxH@FPS 显式帧率保持原值。
 *
 * @quality: 质量档位或自定义尺寸字符串。
 * @src_fps: 源帧率的整数估计，非正表示未知。
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：0 已选定，-ENODEV 表示档位无效或没有可用组合。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_choose_profile(const char *quality, int src_fps, struct profile *p);

#endif /* MIKU_ENCODER_H */
