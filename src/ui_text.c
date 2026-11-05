/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * UTF-8 显示列计算与输出；调用方必须提供有效完整的 UTF-8 文本。
 */

#include "ui_text.h"
#include "strbuf.h"
#include "ui_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ui_utf8_len - 根据 UTF-8 首字节与后续延续字节返回安全步进长度。若多字节序
 * 列被 NUL 或非延续字节截断，按单字节返回 1，防止跨过字符串末尾越界读取。
 *
 * @s: 非空且以 NUL 结尾的字节序列。
 *
 * 返回：安全的编码字节数，范围 1..4。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_utf8_len(const unsigned char *s)
{
	unsigned char c = s[0];
	int len = 1;
	int i;

	if ((c & 0xE0) == 0xC0)
		len = 2;
	else if ((c & 0xF0) == 0xE0)
		len = 3;
	else if ((c & 0xF8) == 0xF0)
		len = 4;
	for (i = 1; i < len; ++i) {
		if ((s[i] & 0xC0) != 0x80)
			return 1;
	}
	return len;
}

/*
 * ui_codepoint - 合并一个完整 UTF-8 编码的位段。len 必须与首字节一致，输入
 * 至少包含 len 字节，不执行编码合法性校验。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @len: 当前字符的 UTF-8 字节数。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static unsigned ui_codepoint(const unsigned char *s, int len)
{
	unsigned int cp;
	int i;

	if (len <= 1)
		return s[0];
	cp = s[0] & (unsigned)(0xFFu >> (len + 1));
	for (i = 1; i < len; ++i)
		cp = (cp << 6) | (unsigned)(s[i] & 0x3F);
	return cp;
}

/*
 * ui_cell_width - 按既有宽字符区间估算显示列数：控制符零列，常见中日韩字符
 * 及部分符号两列，其余一列。
 *
 * @cp: 已经解码的 Unicode 码点。
 *
 * 返回：0、1 或 2 显示列。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_cell_width(unsigned cp)
{
	if (cp < 32 || (cp >= 0x7f && cp < 0xa0))
		return 0;
	if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) ||
	    (cp >= 0x3041 && cp <= 0x33ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
	    (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
	    (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
	    (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
	    (cp >= 0xffe0 && cp <= 0xffe6) ||
	    (cp >= 0x1f300 && cp <= 0x1f9ff)) {
		return 2;
	}
	return 1;
}

/*
 * miku_ui_text_width - 累计有效 UTF-8 文本的显示列数，供居中和截断使用；不
 * 把 ANSI 控制序列视为可解析的文本格式。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_text_width(const char *s)
{
	int w = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		const int len = ui_utf8_len(p);
		w += ui_cell_width(ui_codepoint(p, len));
		p += len;
	}
	return w;
}

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
void miku_ui_add_text(struct strbuf *sb, const char *s, int max_w)
{
	const unsigned char *p = (const unsigned char *)s;
	int used = 0;
	while (*p) {
		const int len = ui_utf8_len(p);
		const int w = ui_cell_width(ui_codepoint(p, len));
		if (max_w >= 0 && used + w > max_w)
			break;
		if (!(miku_sb_reserve(sb, (size_t)len) == 0))
			return;
		memcpy(sb->buf + sb->len, p, (size_t)len);
		sb->len += (size_t)len;
		used += w;
		p += len;
	}
}

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
void miku_ui_add_text_tail(struct strbuf *sb, const char *s, int max_w)
{
	const unsigned char *p = (const unsigned char *)s;
	int remain;
	int len;

	if (max_w < 2) {
		miku_ui_add_text(sb, s, max_w);
		return;
	}
	remain = miku_ui_text_width(s);
	while (*p && remain > max_w - 1) {
		len = ui_utf8_len(p);
		remain -= ui_cell_width(ui_codepoint(p, len));
		p += len;
	}
	if (p != (const unsigned char *)s)
		miku_sb_addf(sb, "…");
	miku_ui_add_text(sb, (const char *)p, max_w - 1);
}

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
int miku_ui_center_col(int width, int cols)
{
	const int left = (cols - width) / 2;
	return left < 0 ? 0 : left;
}

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
void miku_ui_pad(struct strbuf *sb, int n)
{
	if (n <= 0)
		return;
	if (miku_sb_reserve(sb, (size_t)n) != 0)
		return;
	memset(sb->buf + sb->len, ' ', (size_t)n);
	sb->len += (size_t)n;
}

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
void miku_ui_row(struct strbuf *sb, int row)
{
	miku_sb_addf(sb, "\x1b[%d;1H\x1b[2K", row);
}

/*
 * miku_ui_flush - 提交完整屏幕，统一检查此前的缓冲错误和终端写入错误。
 * 失败设置界面停止标志，正常流程会退出并恢复终端；不输出分配失败的半帧。
 * 只清空 len，buf 的所有权和容量仍属于调用方，调用方最终 free。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_flush(struct strbuf *buffer)
{
	if (buffer->error) {
		miku_g_ui_stop = 1;
		return;
	}
	if (buffer->len &&
	    fwrite(buffer->buf, 1, buffer->len, stdout) != buffer->len)
		miku_g_ui_stop = 1;
	buffer->len = 0;
	if (fflush(stdout) != 0)
		miku_g_ui_stop = 1;
}

/*
 * miku_ui_clear_screen - 清空可见屏幕并把光标移到起点，用于模式切换；终端
 * 生命周期由界面模块负责。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_clear_screen(void)
{
	fputs("\x1b[H\x1b[2J", stdout);
	fflush(stdout);
}
