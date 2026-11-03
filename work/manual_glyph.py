from pathlib import Path
head = '/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n\n'
Path('src/glyph.c').write_text(head+'''#include "glyph.h"
#include "font_data.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 位图的一行由高位向低位表示从左到右；调用方保证坐标在字体范围内。 */
static int font_bit(int glyph, int x, int y)
{
        return (miku_font_bitmap[glyph * FONT_SRC_H + y] >>
                (FONT_SRC_W - 1 - x)) & 1;
}

/* 面积采样范围使用源字体坐标，右、下边界为开区间。 */
struct sample_area {
        double x0;
        double x1;
        double y0;
        double y1;
};

/*
 * 积分一个目标像素在源字形上覆盖的面积；部分覆盖的源像素按面积加权。
 * 不改变遍历和累加次序，保持与原版缩小算法的浮点舍入一致。
 * area 是正面积矩形，字形与边界已由 render_glyph 的尺寸保证。
 */
static uint8_t average_pixel(int glyph, const struct sample_area *area)
{
        double sum = 0;
        double weight = 0;
        double wx;
        double wy;
        int xmax = (int)ceil(area->x1) - 1;
        int ymax = (int)ceil(area->y1) - 1;
        int x;
        int y;
        int value;

        if (xmax >= FONT_SRC_W)
                xmax = FONT_SRC_W - 1;
        if (ymax >= FONT_SRC_H)
                ymax = FONT_SRC_H - 1;
        for (y = (int)floor(area->y0); y <= ymax; y++) {
                wy = miku_dmin(area->y1, y + 1.0) -
                     miku_dmax(area->y0, (double)y);
                if (wy <= 0)
                        continue;
                for (x = (int)floor(area->x0); x <= xmax; x++) {
                        wx = miku_dmin(area->x1, x + 1.0) -
                             miku_dmax(area->x0, (double)x);
                        if (wx <= 0)
                                continue;
                        sum += wx * wy * font_bit(glyph, x, y);
                        weight += wx * wy;
                }
        }
        value = weight > 0 ? (int)(sum / weight * 255.0 + 0.5) : 0;
        return (uint8_t)(value > 255 ? 255 : value);
}

/* 插值轴的两个邻接采样点与高点权重，边缘钳位到同一像素。 */
struct sample_axis {
        int low;
        int high;
        double weight;
};

/* 以像素中心对齐生成插值轴；limit 是源方向的像素总数。 */
static struct sample_axis sample_axis(double position, int limit)
{
        struct sample_axis axis;

        position = miku_dmax(0, miku_dmin(position, limit - 1.0));
        axis.low = (int)position;
        axis.high = axis.low + 1 < limit ? axis.low + 1 : axis.low;
        axis.weight = position - axis.low;
        return axis;
}

/* 四邻点双线性插值，结果为 0..255 的字形覆盖率。 */
static uint8_t interpolate_pixel(int glyph, struct sample_axis x,
                                  struct sample_axis y)
{
        double top;
        double bottom;
        int value;

        top = (1 - x.weight) * font_bit(glyph, x.low, y.low) +
              x.weight * font_bit(glyph, x.high, y.low);
        bottom = (1 - x.weight) * font_bit(glyph, x.low, y.high) +
                 x.weight * font_bit(glyph, x.high, y.high);
        value = (int)(((1 - y.weight) * top + y.weight * bottom) * 255.0 + 0.5);
        return (uint8_t)(value > 255 ? 255 : value);
}

/*
 * 缩小采用面积平均，放大采用双线性插值。dst 至少容纳 cw*ch 字节。
 * cw/ch 为正数；本函数只借用缓冲区，且没有全局可变状态。
 */
static void render_glyph(int glyph, uint8_t *dst, int cw, int ch)
{
        struct sample_area area;
        struct sample_axis ax;
        struct sample_axis ay;
        double sx = (double)FONT_SRC_W / cw;
        double sy = (double)FONT_SRC_H / ch;
        int y;
        int x;

        for (y = 0; y < ch; y++) {
                for (x = 0; x < cw; x++) {
                        if (cw <= FONT_SRC_W) {
                                area.x0 = x * sx;
                                area.x1 = area.x0 + sx;
                                area.y0 = y * sy;
                                area.y1 = area.y0 + sy;
                                dst[(size_t)y * cw + x] =
                                        average_pixel(glyph, &area);
                        } else {
                                ax = sample_axis((x + 0.5) * FONT_SRC_W / cw -
                                                 0.5, FONT_SRC_W);
                                ay = sample_axis((y + 0.5) * FONT_SRC_H / ch -
                                                 0.5, FONT_SRC_H);
                                dst[(size_t)y * cw + x] =
                                        interpolate_pixel(glyph, ax, ay);
                        }
                }
        }
}

/* 临时统计由一次建图独占；raw 在成功和失败路径均由建图函数释放。 */
struct atlas_samples {
        uint8_t *raw;
        double ink[FONT_CHAR_COUNT];
        double norm[FONT_CHAR_COUNT];
        int order[FONT_CHAR_COUNT];
};

/* 渲染所有字形，并计算覆盖率（0..1）。gsize 是单个字形字节数。 */
static void atlas_sample(struct atlas_samples *samples, int cw, int ch)
{
        size_t gsize = (size_t)cw * ch;
        uint8_t *mask;
        double sum;
        size_t k;
        int glyph;

        for (glyph = 0; glyph < FONT_CHAR_COUNT; glyph++) {
                mask = samples->raw + glyph * gsize;
                render_glyph(glyph, mask, cw, ch);
                sum = 0;
                for (k = 0; k < gsize; k++)
                        sum += mask[k];
                samples->ink[glyph] = sum / (gsize * 255.0);
                samples->order[glyph] = glyph;
        }
}

/* 稳定插入排序保证相同墨量的字符仍维持原 ASCII 次序。 */
static void atlas_sort(struct atlas_samples *samples)
{
        int i;
        int j;
        int key;

        for (i = 1; i < FONT_CHAR_COUNT; i++) {
                key = samples->order[i];
                j = i - 1;
                while (j >= 0 && samples->ink[samples->order[j]] >
                                 samples->ink[key]) {
                        samples->order[j + 1] = samples->order[j];
                        j--;
                }
                samples->order[j + 1] = key;
        }
}

/* 查找最接近的归一化墨量；二分边界为半开区间 [0,FONT_CHAR_COUNT)。 */
static int nearest_glyph(const double *norm, double target)
{
        int low = 0;
        int high = FONT_CHAR_COUNT;
        int mid;

        while (low < high) {
                mid = (low + high) / 2;
                if (norm[mid] < target)
                        low = mid + 1;
                else
                        high = mid;
        }
        if (low >= FONT_CHAR_COUNT)
                return FONT_CHAR_COUNT - 1;
        if (low > 0 && target - norm[low - 1] < norm[low] - target)
                low--;
        return low;
}

/*
 * 排序后归一化墨量并生成 256 项亮度表。1e-9 用来拒绝无动态范围图集，
 * 防止除以近零数。成功返回 0，无法区分字形时返回 -ERANGE。
 */
static int atlas_index(struct glyph_atlas *atlas,
                        struct atlas_samples *samples, size_t gsize)
{
        double minimum = samples->ink[samples->order[0]];
        double maximum = samples->ink[samples->order[FONT_CHAR_COUNT - 1]];
        int i;

        if (maximum - minimum < 1e-9)
                return -ERANGE;
        for (i = 0; i < FONT_CHAR_COUNT; i++) {
                memcpy(atlas->masks + i * gsize,
                       samples->raw + samples->order[i] * gsize, gsize);
                atlas->chars[i] = (char)(FONT_FIRST_CHAR + samples->order[i]);
                samples->norm[i] = (samples->ink[samples->order[i]] - minimum) /
                                   (maximum - minimum);
        }
        atlas->chars[FONT_CHAR_COUNT] = '\\0';
        for (i = 0; i < 256; i++)
                atlas->lut[i] = nearest_glyph(samples->norm, i / 255.0);
        return 0;
}

/*
 * 为正数尺寸构建一次字形图集；atlas 必须是尚未持有 masks 的空对象。
 * 成功返回 0，并把 masks 的所有权交给调用方，调用方最终 free。
 * 失败返回 -EINVAL/-ENOMEM/-ERANGE，masks 保持 NULL，不转移资源。
 * 不访问可变全局状态；不同图集可并行构建。
 */
int miku_atlas_build(struct glyph_atlas *atlas, int cell_w, int cell_h)
{
        struct atlas_samples samples = {0};
        size_t gsize;
        int ret;

        if (cell_w < 1 || cell_h < 1 || cell_w > MAX_CELL_WIDTH ||
            cell_h > MAX_CELL_WIDTH * 2)
                return -EINVAL;
        gsize = (size_t)cell_w * cell_h;
        samples.raw = calloc(gsize, FONT_CHAR_COUNT);
        if (!samples.raw)
                return -ENOMEM;
        atlas->masks = calloc(gsize, FONT_CHAR_COUNT);
        if (!atlas->masks) {
                free(samples.raw);
                return -ENOMEM;
        }
        atlas_sample(&samples, cell_w, cell_h);
        atlas_sort(&samples);
        ret = atlas_index(atlas, &samples, gsize);
        free(samples.raw);
        if (ret) {
                free(atlas->masks);
                atlas->masks = NULL;
                return ret;
        }
        atlas->cell_w = cell_w;
        atlas->cell_h = cell_h;
        return 0;
}
''')
for file in ['src/playback.c', 'src/convert.c']:
 p=Path(file);s=p.read_text();s=s.replace('!miku_atlas_build(&play.atlas, 8, 16)', 'miku_atlas_build(&play.atlas, 8, 16) != 0').replace('!miku_atlas_build(&atlas, L.cell_w, L.cell_h)', 'miku_atlas_build(&atlas, L.cell_w, L.cell_h) != 0');p.write_text(s)
