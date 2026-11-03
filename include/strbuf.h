/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_STRBUF_H
#define MIKU_STRBUF_H

#include "attrs.h"
#include "types.h"

/*
 * miku_sb_reserve - 保留 len 之后至少 extra 字节。成功返回 0，溢出返回
 * -EOVERFLOW，
 * 分配失败返回 -ENOMEM。失败保留原 buf 和 cap，并记录首个错误。
 * buf 的所有权不变；不同缓冲可并发，同一缓冲必须串行访问。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @extra: 在当前长度之后还需保留的字节数，包含调用方所需的 NUL。
 *
 */
int miku_sb_reserve(struct strbuf *buffer, size_t extra);
/*
 * miku_sb_addf - 追加 printf 格式文本，先测量再分配，避免临时小数组造成越
 * 界读取。
 * len 不含结尾 NUL；空文本也是成功。成功返回 0，失败返回负 errno。
 * 错误保存在 buffer 中，UI 可以在整帧提交时统一检查，禁止输出半帧。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @format: printf 格式串，变参类型必须与占位符一致。
 *
 */
int miku_sb_addf(struct strbuf *buffer, const char *format, ...) MIKU_PRINTF(2, 3);

#endif /* MIKU_STRBUF_H */
