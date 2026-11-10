/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_CONSTANTS_H
#define MIKU_CONSTANTS_H

/* 字符列数范围；不把终端太宽或太窄直接带入编码布局。 */
#define DEFAULT_ASCII_WIDTH 320
#define MIN_ASCII_WIDTH 80
#define MAX_ASCII_WIDTH 640
/* 格子像素宽度，格子高度始终为宽度的两倍。 */
#define MIN_CELL_WIDTH 4
#define MAX_CELL_WIDTH 32
/* 感知亮度的 RGB 权重，总和为 1。 */
#define RED_WEIGHT 0.299
#define GREEN_WEIGHT 0.587
#define BLUE_WEIGHT 0.114
/* gamma<1 偏向更密字形；颜色增益保持色相并压制高光白底防止漂白。 */
#define CHAR_GAMMA 0.62
#define SATURATION_BOOST 1.45
#define COLOR_GAIN 1.25	       /* 最小增益 */
#define COLOR_MIN_VALUE 170.0  /* 暗部最亮通道至少达到的值 */
#define COLOR_MAX_GAIN 8.0     /* 增益上限，避免放大接近纯黑像素的噪点 */
#define WHITE_FLOOR_KNEE 90.0  /* 白底分量软膝起点，高于此值开始压制白底增益 */
#define WHITE_FLOOR_CEIL 160.0 /* 白底分量高光阈值，高于此值按斜率温和压缩 */
#define WHITE_FLOOR_SLOPE 0.72 /* 高光白底压缩斜率，防止亮色和近白区域惨白 */
/* 字符边缘密集，CRF 29 配合去块滤波与关闭心理视觉控制体积。 */
#define X264_PRESET "veryfast"
#define X264_CRF "29"
#define X264_PARAMS                                                            \
	"colorprim=bt709:transfer=bt709:colormatrix=bt709:aq-mode=1:"          \
	"psy-rd=0,0:deblock=1,1"
/* 跨帧稳定：候选字形与上一帧的字形只差这么几档墨量时沿用上一帧的字形。
 * 95 个字形均匀覆盖 0..1 的墨量，3 档约等于 3% 的浓淡差，肉眼不可辨，却能
 * 去掉亮度在档位边界抖动引起的逐帧翻字。可用 -D 覆盖，便于测量收益。 */
#ifndef GLYPH_HOLD_TOLERANCE
#define GLYPH_HOLD_TOLERANCE 3
#endif /* GLYPH_HOLD_TOLERANCE */
#ifndef COLOR_HOLD_TOLERANCE
#define COLOR_HOLD_TOLERANCE 12
#endif /* COLOR_HOLD_TOLERANCE */
#define TIER_COUNT 3
/* 实测成本：每百万像素每秒所占 CPU 秒，预算按在线核心估算。 */
#define SW_CPU_PER_MPX 0.10
#define HW_CPU_PER_MPX 0.003
#define RENDER_CPU_PER_MPX 0.003
#define CPU_BUDGET_RATIO 1.4 /* 允许每秒视频最多占用 1.4*核数 个 CPU 秒 */
#ifndef HW_BITS_PER_PIXEL
#define HW_BITS_PER_PIXEL 0.18
#endif /* HW_BITS_PER_PIXEL */
/* 硬件编码码率夹紧区间，单位 kbps。 */
#define HW_MIN_KBPS 1500
#define HW_MAX_KBPS 40000
#define ENCODER_COUNT 9
/* 可打印 ASCII 32..126，共 95 个字形，每字形为 16x32 位图。 */
#define FONT_FIRST_CHAR 32
#define FONT_CHAR_COUNT 95
#define FONT_SRC_W 16
#define FONT_SRC_H 32
/* 每个编码器实际短片探测使用的帧数。 */
#define PROBE_FRAMES 8
#define RATIO_COUNT 7
#define STYLE_CHAR 0
#define STYLE_BLOCK 1
/* UI 专用键值避开单字节范围；行列坐标仍按终端的一基编号。 */
#define UI_NONE (-1)
#define UI_UP 1001
#define UI_DOWN 1002
#define UI_LEFT 1003
#define UI_RIGHT 1004
#define UI_CLICK 1005
#define UI_WHEEL_UP 1006
#define UI_WHEEL_DOWN 1007
/* 内部哨兵：鼠标松开与移动事件，miku_ui_key 会吞掉它，上层永远看不到 */
#define UI_IGNORE 1008
#define UI_TEXT_BYTE                                                           \
	256 /* >= 这个数表示一个 UTF-8 字节：b = k - UI_TEXT_BYTE */
#define UI_PATH_MAX 512
#define UI_HIT_MAX 16
#define UI_RESET "\x1b[0m"
#define UI_DIM "\x1b[2m"
#define UI_BOLD "\x1b[1m"
#define UI_ACCENT "\x1b[36m"
#define UI_GOOD "\x1b[32m"
#define UI_WARN "\x1b[33m"
#define UI_MIKU "\x1b[38;2;126;255;96m" /* 葱绿 */
#define UI_LYRIC_COUNT 12
#define UI_MENU_COUNT 5
/* 两个持久化文件名，均位于 miku_config_dir() 返回的配置目录内。 */
#define UI_LIBRARY_FILE "library.tsv"
#define UI_LIBRARY_MAX 128
#define UI_SETTINGS_FILE "settings.tsv"
#define UI_FONT_SIZE_MIN 4.0
#define UI_FONT_SIZE_MAX 12.5
#define UI_FONT_SIZE_DEFAULT 6.0
#define UI_FONT_SIZE_STEP 0.5

enum {
	CFG_INPUT = 0,
	CFG_STYLE,
	CFG_RATIO,
	CFG_FPS,
	CFG_QUALITY,
	CFG_OUTPUT,
	CFG_WIDTH,
	CFG_START,
	CFG_ITEM_MAX
};

#endif /* MIKU_CONSTANTS_H */
