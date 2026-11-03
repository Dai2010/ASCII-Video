/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_GLYPH_H
#define MIKU_GLYPH_H

#include "types.h"

/*
 * miku_atlas_build - 为正数尺寸构建一次字形图集；atlas 必须是尚未持有
 * masks 的空对象。
 * 成功返回 0，并把 masks 的所有权交给调用方，调用方最终 free。
 * 失败返回 -EINVAL/-ENOMEM/-ERANGE，masks 保持 NULL，不转移资源。
 * 不访问可变全局状态；不同图集可并行构建。
 *
 * @atlas: 字形图集，创建成功后 masks 所有权交给调用方。
 * @cell_w: 格子宽度，单位像素。
 * @cell_h: 格子高度，单位像素。
 *
 */
int miku_atlas_build(struct glyph_atlas *atlas, int cell_w, int cell_h);

extern int miku_gamma_lut[256];
/*
 * miku_build_gamma_lut - 在开始渲染前建立 256 项亮度 gamma 表。表由字体模
 * 块持有，初始化后渲染器只读；同一进程的初始化必须串行。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 */
void miku_build_gamma_lut(void);

#endif /* MIKU_GLYPH_H */
