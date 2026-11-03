/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_ATTRS_H
#define MIKU_ATTRS_H

/*
 * 声明"这个参数是 printf 格式串"。作用有两层：
 *   1. 编译器会按格式串检查调用点的实参类型；
 *   2. 函数体内 vprintf/vsnprintf(fmt, ...) 不再触发 -Wformat-nonliteral ——
 *      否则 clang/gcc 打开 -Wformat=2 后所有 printf 包装都编不过（-Werror
 *      时直接失败），而包装本身正是唯一正确的写法。
 * 下标从 1 开始，分别是格式串与第一个变参的位置。其它编译器展开为空。
 */
#if defined(__GNUC__) || defined(__clang__)
#define MIKU_PRINTF(fmt_index, arg_index) \
	__attribute__((format(printf, fmt_index, arg_index)))
#else
#define MIKU_PRINTF(fmt_index, arg_index)
#endif

#endif
