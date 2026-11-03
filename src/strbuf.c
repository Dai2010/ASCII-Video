/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 可复用输出缓冲；容量增长和首个错误状态均封装在缓冲对象内。
 */

#include "strbuf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
int miku_sb_reserve(struct strbuf *buffer, size_t extra)
{
	size_t capacity;
	size_t required;
	char *replacement;

	if (buffer->error)
		return buffer->error;
	if (extra > SIZE_MAX - buffer->len) {
		buffer->error = -EOVERFLOW;
		return buffer->error;
	}
	required = buffer->len + extra;
	if (required <= buffer->cap)
		return 0;
	/* 4KiB 起步并按倍数增长，避免逐字符拼接时频繁 realloc。 */
	capacity = buffer->cap ? buffer->cap : 4096;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2) {
			capacity = required;
			break;
		}
		capacity *= 2;
	}
	replacement = realloc(buffer->buf, capacity);
	if (!replacement) {
		buffer->error = -ENOMEM;
		return buffer->error;
	}
	buffer->buf = replacement;
	buffer->cap = capacity;
	return 0;
}

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
int miku_sb_addf(struct strbuf *buffer, const char *format, ...)
{
	va_list args;
	va_list copy;
	int length;
	int written;
	int ret;

	if (buffer->error)
		return buffer->error;
	va_start(args, format);
	va_copy(copy, args);
	length = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (length < 0) {
		va_end(copy);
		buffer->error = -EINVAL;
		return buffer->error;
	}
	ret = miku_sb_reserve(buffer, (size_t)length + 1);
	if (ret) {
		va_end(copy);
		return ret;
	}
	written = vsnprintf(buffer->buf + buffer->len, (size_t)length + 1,
			    format, copy);
	va_end(copy);
	if (written != length) {
		buffer->error = -EIO;
		return buffer->error;
	}
	buffer->len += (size_t)length;
	return 0;
}
