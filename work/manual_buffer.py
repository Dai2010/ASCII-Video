from pathlib import Path
from edit_helpers import replace_function
head='/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n\n'
p=Path('include/types.h');s=p.read_text().replace('size_t cap;\n};','size_t cap;\n        int error; /* 首个分配/格式错误，提交屏幕时统一处理。 */\n};');p.write_text(s)
Path('src/strbuf.c').write_text(head+'''#include "strbuf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * 保留 len 之后至少 extra 字节。成功返回 0，溢出返回 -EOVERFLOW，
 * 分配失败返回 -ENOMEM。失败保留原 buf 和 cap，并记录首个错误。
 * buf 的所有权不变；不同缓冲可并发，同一缓冲必须串行访问。
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
 * 追加 printf 格式文本，先测量再分配，避免临时小数组造成越界读取。
 * len 不含结尾 NUL；空文本也是成功。成功返回 0，失败返回负 errno。
 * 错误保存在 buffer 中，UI 可以在整帧提交时统一检查，禁止输出半帧。
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
''')
replace_function('src/ui_text.c','miku_ui_flush','''
/*
 * 提交完整屏幕，统一检查此前的缓冲错误和终端写入错误。
 * 失败设置界面停止标志，正常流程会退出并恢复终端；不输出分配失败的半帧。
 * 只清空 len，buf 的所有权和容量仍属于调用方，调用方最终 free。
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
''')
p=Path('src/ui_text.c');s=p.read_text().replace('#include "strbuf.h"','#include "strbuf.h"\n#include "ui_terminal.h"');p.write_text(s)
# 所有 UI 缓冲从零初始化，包括新增的粘性错误字段。
for p in Path('src').glob('*.c'):
 s=p.read_text().replace('{NULL, 0, 0}', '{0}')
 p.write_text(s)
# 把进度页面的输出规格详情提取为纯绘制辅助函数。
p=Path('src/ui_progress.c');s=p.read_text();a=s.index('\tif (pg->res->prof.enc) {');b=s.index('\n\tmiku_ui_flush(&sb);',a);code=s[a:b].replace('&sb','sb');a_fn,b_fn=__import__('edit_helpers').function_span(s,'ui_draw_progress');s=s[:a]+'        ui_progress_specification(&sb, pg, avail);\n'+s[b:];helper='''/* 编码器尚未选定时省略规格区域，不读取未初始化的档位字段。 */
static void ui_progress_specification(struct strbuf *sb,
                                        const struct ui_progress *pg, int avail)
{
        char line[256];
'''+code+'\n}\n\n';s=s[:a_fn]+helper+s[a_fn:];p.write_text(s)
