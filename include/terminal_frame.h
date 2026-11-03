/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_TERMINAL_FRAME_H
#define MIKU_TERMINAL_FRAME_H

#include "types.h"

/* cells 和 atlas 均为借用；调用期间不得改变尺寸或释放缓冲区。 */
struct terminal_frame {
	const uint8_t *cells;
	const struct glyph_atlas *atlas;
	int cols;
	int rows;
	int style;
};

/*
 * miku_terminal_frame_build - 重建一整帧终端输出，成功返回 0，扩容失败返回
 * -ENOMEM。
 * 原 buffer 可复用：只重置 len，保留容量。函数不执行终端 I/O，
 * 使播放时钟与昂贵的字符拼接分离；失败时调用方不得输出半帧。
 *
 * @buffer: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @frame: 借用的终端采样帧和网格尺寸，不转移图集所有权。
 *
 */
int miku_terminal_frame_build(struct strbuf *buffer,
			      const struct terminal_frame *frame);

#endif /* MIKU_TERMINAL_FRAME_H */
