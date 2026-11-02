/*
 * 彩色ASCII视频转换器 v4.1 (纯 C17 版)
 * 将普通视频转换为ASCII字符艺术风格的彩色视频，并自动合并原视频的音频
 *
 * 编译命令：
 *   gcc -O3 -march=native -std=c17 -Wall -Wextra -o miku miku.c -lm
 *
 * 使用示例：
 *   ./miku input.mp4 output.mp4 [ASCII宽度]
 *   ./miku bad-1.mp4 bad-1-ascii.mp4 320
 *
 * 输出规格（强制，与输入视频无关）：
 *   分辨率 2560x1440（2K），帧率 60fps，H.264 + AAC，BT.709 色彩
 *   输入视频的宽高比与 16:9 不同时，ASCII 画面会居中显示，四周留黑边
 *   输入帧率不是 60 时，通过重复帧补到 60fps（不会让画面更流畅，只是统一规格）
 *
 * 参数说明：
 *   ASCII宽度 ：每行的字符数，80-640，默认320
 *               每个字符的像素大小由它自动决定：字符宽 = 2560 / ASCII宽度（取偶数），高 = 2 倍宽
 *               320 -> 8x16 像素/字符，画面刚好铺满 2K（320 x 90 个字符）
 *               426 -> 6x12 像素/字符，字符更多更细（426 x 120 个字符），左右各留 2 像素黑边
 *               数值越大画面越细腻，但单个字符越小、越不容易看清
 *
 * 作者: LinusTrevian（你猜我在线不）+ Claude Opus 5.5 Medium + GPT 4
 *
 * 为什么不使用 OpenCV：
 *   OpenCV 4 已经移除了 C 语言接口，只剩 C++ 接口，所以 C 版本改用 ffmpeg：
 *     - 解码：ffmpeg 把输入视频缩放到 ASCII 网格大小，通过管道以原始 RGB 数据送给本程序
 *     - 渲染：本程序用内嵌的位图字体把每个网格渲染成字符
 *     - 编码：渲染好的帧通过管道送给另一个 ffmpeg，一次完成 H.264 编码 + 合并原音频
 *   好处：不再需要 OpenCV 和 pkg-config，也不再生成临时文件，只依赖 gcc 和 ffmpeg。
 *
 * 字体说明：
 *   下方的 FONT_BITMAP 是由 DejaVu Sans Mono（Bitstream Vera 许可，允许自由使用和再分发）
 *   渲染得到的 16x32 单色位图，共 95 个字符（ASCII 32-126），每个字符 32 行，每行 16 位。
 *   程序运行时会把它缩放到所需的字符大小，所以不需要任何字体文件。
 *
 * v4.1 相比 v4.0 的改动：
 *   1. 修复饱和度偏低：旧版把三个颜色通道统一乘增益后逐个截断到 255，亮色会被"冲白"
 *      （粉色变浅粉、天蓝变近白）。现在先适度提高饱和度，再按比例整体缩放，保持色相不变
 *   2. 输出强制 2560x1440 @ 60fps，并给视频打上 BT.709 色彩标签，避免播放器解读色彩出现偏差
 *   3. 输入帧只渲染一次，再按需重复写入，补帧到 60fps 时不会增加渲染开销
 */


#define _POSIX_C_SOURCE 200809L  /* 需要 popen / clock_gettime，严格 C17 模式下要显式开启 */
#define _DEFAULT_SOURCE 1        /* 需要 struct winsize（终端尺寸），glibc 在纯 POSIX 模式下不暴露 */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------- 常量 ---------------------------- */

/* 输出规格不再写死：见下面的 Tier 档位表，自动降级时按高→低依次尝试 */

/* ASCII宽度范围（每行字符数） */
#define DEFAULT_ASCII_WIDTH 320
#define MIN_ASCII_WIDTH 80
#define MAX_ASCII_WIDTH 640

/* 单个字符格子的宽度范围（像素），高度固定为宽度的2倍 */
#define MIN_CELL_WIDTH 4
#define MAX_CELL_WIDTH 32

/* 亮度权重（人眼对不同颜色的敏感度） */
#define RED_WEIGHT 0.299
#define GREEN_WEIGHT 0.587
#define BLUE_WEIGHT 0.114

/* 字符选择的 gamma：小于 1 会让画面更偏向"密"的字符，整体更亮、更饱满 */
#define CHAR_GAMMA 0.55

/* 字符颜色处理（全部保持色相不变）：
 *   1. 先把颜色朝远离灰色的方向推一把，提高饱和度（1.0 = 不变）
 *   2. 再按比例整体缩放，让最亮的通道至少达到 COLOR_MIN_VALUE，但绝不超过 255
 *      （旧版逐通道截断会把亮色冲成白色，这是饱和度偏低的主要原因） */
#define SATURATION_BOOST 1.35
#define COLOR_GAIN 1.45       /* 最小增益 */
#define COLOR_MIN_VALUE 190.0 /* 最亮通道至少达到的值 */
#define COLOR_MAX_GAIN 8.0    /* 增益上限，避免放大接近纯黑像素的噪点 */

/* x264 编码参数：2K 60fps 的编码很吃 CPU，默认用 veryfast 保证速度；
 * 想要更小的文件可改成 fast / medium / slow（越慢越小，耗时也越长） */
#define X264_PRESET "veryfast"
#define X264_CRF "19"         /* 越小画质越高、文件越大（常用范围 16-23） */

/* 输出档位：全部 16:9，宽高都是 16 的倍数（yuv420p 与硬件编码器都要求偶数，
 * 硬件编码器普遍还要求对齐到 16）。自动降级时按 high -> mid -> low 往下选 */
typedef struct {
    const char *name;
    int width;
    int height;
    int fps;
    long min_mem_mb; /* 低于这个物理内存就不再选该档 */
} Tier;

#define TIER_COUNT 3

static const Tier TIERS[TIER_COUNT] = {
    {"high", 2560, 1440, 60, 1500},
    {"mid", 1920, 1080, 30, 900},
    {"low", 1280, 720, 24, 500},
};

/* 自动降级的成本模型：每秒输出视频要花多少 CPU 秒 = 输出像素率(Mpx/s) * 系数。
 * 系数来自实测（ASCII 画面边缘多，x264 veryfast crf19 在 2K/1080p/720p 上
 * 分别是 0.087 / 0.091 / 0.103 CPU 秒每 Mpx，取偏保守的 0.10） */
#define SW_CPU_PER_MPX 0.10
#define HW_CPU_PER_MPX 0.003
#define RENDER_CPU_PER_MPX 0.003
#define CPU_BUDGET_RATIO 1.4 /* 允许每秒视频最多占用 1.4*核数 个 CPU 秒 */

/* 硬件编码器的码率系数（硬件编码器普遍没有 CRF，只能按码率控）：
 * 实测 2K60 crf19 约 21 Mbps，折合约 0.10 bit/像素；但硬件编码的量化更粗，
 * 0.10 时亮度与色相都会比 crf19 略偏（YAVG -6%、HUEAVG -5%），
 * 所以默认给到 0.18，用编译时的 -DHW_BITS_PER_PIXEL= 可再调 */
#ifndef HW_BITS_PER_PIXEL
#define HW_BITS_PER_PIXEL 0.18
#endif
#define HW_MIN_KBPS 1500
#define HW_MAX_KBPS 40000

/* 编码器候选，按顺序试，第一个"真跑通"的用：
 *   fixed       软件编码器的固定参数；NULL 表示走码率（硬件编码器）
 *   pipe_filter 追加在 scale/format 之后的滤镜（VAAPI 需要 hwupload）
 *   input_prefix 放在 -i 之前的参数（非 VAAPI 的一般不用）
 *   vaapi       设备路径不固定，运行时扫 /dev/dri/renderD* 再拼参数
 * 顺序刻意把各平台的硬件编码器排在前面，软件 x264 兜底，mpeg4 最后 */
typedef struct {
    const char *codec;
    const char *fixed;
    const char *pipe_filter;
    const char *input_prefix;
    int hardware;
    int vaapi;
} EncoderCand;

static const EncoderCand ENCODERS[] = {
    {"h264_videotoolbox", NULL, NULL, NULL, 1, 0}, /* macOS */
    {"h264_nvenc", NULL, NULL, NULL, 1, 0},        /* NVIDIA（Windows/Linux） */
    {"h264_qsv", NULL, NULL, NULL, 1, 0},          /* Intel（Windows/Linux） */
    {"h264_amf", NULL, NULL, NULL, 1, 0},          /* AMD（Windows） */
    {"h264_mf", NULL, NULL, NULL, 1, 0},           /* Windows MediaFoundation */
    {"h264_vaapi", NULL, ",format=nv12,hwupload", NULL, 1, 1}, /* AMD/Intel（Linux） */
    {"h264_mediacodec", NULL, NULL, NULL, 1, 0},   /* Android */
    {"libx264", "-preset " X264_PRESET " -crf " X264_CRF
                " -x264-params colorprim=bt709:transfer=bt709:colormatrix=bt709",
     NULL, NULL, 0, 0},
    {"mpeg4", "-q:v 2", NULL, NULL, 0, 0},
};

#define ENCODER_COUNT ((int)(sizeof(ENCODERS) / sizeof(ENCODERS[0])))

/* 一次转换实际要用的规格 */
typedef struct {
    char name[32];   /* 档位名，或用户给的 WxH@FPS */
    int width;
    int height;
    int fps;
    const EncoderCand *enc;
} Profile;

/* 内嵌字体参数 */
#define FONT_FIRST_CHAR 32
#define FONT_CHAR_COUNT 95
#define FONT_SRC_W 16
#define FONT_SRC_H 32

/* ---------------------------- 内嵌字体 ---------------------------- */

static const uint16_t FONT_BITMAP[FONT_CHAR_COUNT * FONT_SRC_H] =
{
    /* 0x20 'space' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x21 '!' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0000,0x0000,0x0000,0x0180,0x0180,
    0x0180,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x22 '"' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0E70,0x0E70,
    0x0E70,0x0E70,0x0E70,0x0E70,0x0E70,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x23 '#' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x010C,0x038C,
    0x031C,0x0318,0x0318,0x0718,0x7FFF,0x3FFF,0x0630,0x0E30,
    0x0C70,0x0C60,0xFFFC,0xFFFC,0x18E0,0x18C0,0x18C0,0x38C0,
    0x31C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x24 '$' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0080,0x0080,0x0080,
    0x03E0,0x0FF8,0x1C98,0x1880,0x3880,0x3880,0x1C80,0x1F80,
    0x0FF0,0x01F8,0x009C,0x008C,0x008C,0x008C,0x309C,0x3FF8,
    0x0FF0,0x0180,0x0080,0x0080,0x0080,0x0000,0x0000,0x0000,
    /* 0x25 '%' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3E00,
    0x7F00,0x6300,0xC180,0xC180,0x6300,0x7F0E,0x3E3C,0x01E0,
    0x0780,0x3C78,0x70FE,0x00C6,0x0182,0x0183,0x0186,0x00FE,
    0x007C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x26 '&' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0FE0,0x0FE0,
    0x1C00,0x1800,0x1800,0x1C00,0x0C00,0x0E00,0x1F00,0x3B82,
    0x7187,0x61C6,0x60E6,0x6076,0x607E,0x703C,0x701C,0x3C7C,
    0x1FEE,0x0780,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x27 ''' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x28 '(' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0060,0x00E0,0x00C0,
    0x01C0,0x0180,0x0180,0x0380,0x0380,0x0300,0x0300,0x0300,
    0x0700,0x0300,0x0300,0x0300,0x0300,0x0380,0x0380,0x0180,
    0x0180,0x00C0,0x00C0,0x0060,0x0060,0x0000,0x0000,0x0000,
    /* 0x29 ')' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0600,0x0600,0x0300,
    0x0380,0x0180,0x0180,0x01C0,0x01C0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x01C0,0x01C0,0x0180,
    0x0380,0x0300,0x0300,0x0600,0x0600,0x0000,0x0000,0x0000,
    /* 0x2A 'asterisk' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0180,
    0x318C,0x1DB8,0x07E0,0x03C0,0x07E0,0x1DB8,0x318C,0x0180,
    0x0180,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x2B '+' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x7FFE,0x7FFE,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x2C ',' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x01C0,0x03C0,0x03C0,
    0x03C0,0x0380,0x0380,0x0300,0x0600,0x0000,0x0000,0x0000,
    /* 0x2D '-' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x07E0,0x07E0,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x2E '.' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0380,0x03C0,0x03C0,
    0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x2F 'slash' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x001C,0x0018,
    0x0038,0x0030,0x0070,0x0060,0x00E0,0x00C0,0x01C0,0x01C0,
    0x0180,0x0380,0x0300,0x0700,0x0600,0x0E00,0x0C00,0x1C00,
    0x1800,0x3800,0x3000,0x3000,0x0000,0x0000,0x0000,0x0000,
    /* 0x30 '0' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0100,0x07E0,0x0FF0,
    0x1C38,0x1818,0x381C,0x381C,0x381C,0x301C,0x318C,0x33CC,
    0x33CC,0x300C,0x381C,0x381C,0x381C,0x3818,0x1C38,0x1E70,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x31 '1' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x07C0,0x1FC0,
    0x1DC0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x0FFC,
    0x1FFC,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x32 '2' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0100,0x1FE0,0x3FF0,
    0x3038,0x0018,0x001C,0x001C,0x0018,0x0038,0x0038,0x0070,
    0x00E0,0x01C0,0x0380,0x0700,0x0E00,0x0C00,0x1C00,0x3FFC,
    0x3FFC,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x33 '3' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0100,0x3FE0,0x3FF0,
    0x0038,0x0018,0x001C,0x0018,0x0038,0x0070,0x07E0,0x07F0,
    0x0038,0x001C,0x001C,0x001C,0x001C,0x001C,0x0038,0x3878,
    0x3FF0,0x0FC0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x34 '4' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0070,0x00F0,
    0x01F0,0x01F0,0x0370,0x0370,0x0670,0x0C70,0x0C70,0x1870,
    0x3870,0x3070,0x7FFC,0x7FFE,0x3FFC,0x0070,0x0070,0x0070,
    0x0070,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x35 '5' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FF0,0x1FF0,
    0x1800,0x1800,0x1800,0x1800,0x1900,0x1FE0,0x1FF0,0x0038,
    0x0018,0x001C,0x001C,0x001C,0x001C,0x001C,0x0038,0x38F0,
    0x3FE0,0x0F80,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x36 '6' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x00C0,0x07F8,0x0FF8,
    0x1C00,0x1800,0x3800,0x3800,0x31C0,0x37F0,0x3E78,0x3C1C,
    0x381C,0x380C,0x380C,0x380C,0x380C,0x381C,0x1C1C,0x1E78,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x37 '7' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3FFC,0x3FFC,
    0x0018,0x0038,0x0038,0x0030,0x0070,0x0070,0x00E0,0x00E0,
    0x00C0,0x01C0,0x01C0,0x0180,0x0380,0x0380,0x0700,0x0700,
    0x0600,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x38 '8' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0FF0,0x1FF8,
    0x1C38,0x381C,0x381C,0x381C,0x1818,0x1C38,0x0FE0,0x0FF0,
    0x1C38,0x381C,0x381C,0x300C,0x300C,0x381C,0x381C,0x1E78,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x39 '9' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0100,0x0FE0,0x1FF0,
    0x3838,0x3818,0x301C,0x301C,0x301C,0x301C,0x381C,0x383C,
    0x1C7C,0x1FEC,0x07CC,0x001C,0x001C,0x0018,0x0038,0x18F0,
    0x1FE0,0x0780,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x3A ':' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0180,0x03C0,0x03C0,0x03C0,0x0180,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0380,0x03C0,0x03C0,
    0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x3B ';' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0180,0x03C0,0x03C0,0x03C0,0x0180,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x01C0,0x03C0,0x03C0,
    0x03C0,0x0380,0x0380,0x0300,0x0600,0x0000,0x0000,0x0000,
    /* 0x3C '<' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x000E,0x003E,0x01F8,0x07E0,0x3F00,
    0x7800,0x7800,0x3F00,0x07E0,0x01F8,0x003E,0x000E,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x3D '=' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x7FFE,0x7FFE,0x3FFC,
    0x0000,0x0000,0x3FFC,0x7FFE,0x7FFE,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x3E '>' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x7000,0x7C00,0x1F80,0x07E0,0x00FC,
    0x001E,0x001E,0x00FC,0x07E0,0x1F80,0x7C00,0x7000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x3F '?' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0FF0,0x1FF8,
    0x1038,0x0018,0x0018,0x0038,0x0038,0x0070,0x00E0,0x01C0,
    0x0180,0x0380,0x0380,0x0380,0x0000,0x0000,0x0380,0x0380,
    0x0380,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x40 '@' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x01E0,
    0x07F8,0x1E1C,0x180E,0x3006,0x7026,0x61FE,0x639E,0x6306,
    0xC306,0xC606,0xC606,0xC706,0xC306,0x638E,0x61FE,0x60F2,
    0x3000,0x3800,0x1C00,0x0F08,0x03F8,0x0020,0x0000,0x0000,
    /* 0x41 'A' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x03C0,0x03C0,
    0x03C0,0x07E0,0x0660,0x0660,0x0E70,0x0E70,0x0C30,0x1C38,
    0x1C38,0x1818,0x1FF8,0x3FFC,0x301C,0x300C,0x700E,0x700E,
    0x6006,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x42 'B' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3FE0,0x3FF8,
    0x3838,0x381C,0x381C,0x381C,0x381C,0x3838,0x3FF0,0x3FF0,
    0x383C,0x381C,0x380E,0x380E,0x380E,0x380E,0x381C,0x3FF8,
    0x3FF0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x43 'C' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0040,0x03FC,0x0FFC,
    0x0E04,0x1C00,0x1800,0x3800,0x3800,0x3800,0x3800,0x3800,
    0x3800,0x3800,0x3800,0x3800,0x3800,0x1C00,0x1C04,0x0F1C,
    0x07FC,0x01F0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x44 'D' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3F80,0x3FE0,
    0x3070,0x3038,0x301C,0x301C,0x301C,0x301C,0x301C,0x300C,
    0x300C,0x301C,0x301C,0x301C,0x301C,0x3038,0x3078,0x3FF0,
    0x3FC0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x45 'E' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FFC,0x1FFC,
    0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1FFC,0x1FFC,
    0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1FFC,
    0x1FFC,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x46 'F' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FFC,0x1FFC,
    0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,0x1FFC,0x1FF8,
    0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,0x1C00,
    0x1C00,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x47 'G' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0040,0x07F8,0x0FFC,
    0x1C0C,0x1800,0x3800,0x3800,0x3000,0x7000,0x7000,0x707C,
    0x707C,0x707C,0x300C,0x300C,0x380C,0x380C,0x1C0C,0x0F1C,
    0x07FC,0x01E0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x48 'H' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x300C,0x300C,
    0x300C,0x300C,0x300C,0x300C,0x300C,0x381C,0x3FFC,0x3FFC,
    0x300C,0x300C,0x300C,0x300C,0x300C,0x300C,0x300C,0x300C,
    0x300C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x49 'I' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FF8,0x1FF8,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x1FF8,
    0x1FF8,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4A 'J' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x07F0,0x07F0,
    0x0030,0x0030,0x0030,0x0030,0x0030,0x0030,0x0030,0x0030,
    0x0030,0x0030,0x0030,0x0030,0x0030,0x0070,0x6070,0x78F0,
    0x3FE0,0x0F80,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4B 'K' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x300E,0x301C,
    0x3038,0x3070,0x30E0,0x31C0,0x3380,0x3780,0x3F80,0x3F80,
    0x3DC0,0x38E0,0x30E0,0x3070,0x3038,0x3038,0x301C,0x300E,
    0x300E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4C 'L' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1800,0x1800,
    0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,
    0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,0x1FFE,
    0x1FFE,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4D 'M' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x781E,0x781E,
    0x783E,0x7C3E,0x7C3E,0x766E,0x766E,0x766E,0x73CE,0x73CE,
    0x73CE,0x718E,0x700E,0x700E,0x700E,0x700E,0x700E,0x700E,
    0x700E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4E 'N' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x380C,0x3C0C,
    0x3C0C,0x3E0C,0x3E0C,0x360C,0x330C,0x330C,0x338C,0x318C,
    0x318C,0x30CC,0x30CC,0x30EC,0x306C,0x307C,0x303C,0x303C,
    0x301C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x4F 'O' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x07E0,0x1FF0,
    0x1C38,0x381C,0x381C,0x381C,0x300C,0x300C,0x300C,0x700C,
    0x300C,0x300C,0x300C,0x381C,0x381C,0x381C,0x1C38,0x1E78,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x50 'P' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FE0,0x1FF8,
    0x183C,0x181C,0x180E,0x180E,0x180E,0x180E,0x181C,0x1FFC,
    0x1FF8,0x1F80,0x1800,0x1800,0x1800,0x1800,0x1800,0x1800,
    0x1800,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x51 'Q' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x07E0,0x1FF0,
    0x1C38,0x381C,0x381C,0x381C,0x300C,0x300C,0x300C,0x700C,
    0x300C,0x300C,0x300C,0x381C,0x381C,0x381C,0x1C38,0x1E78,
    0x0FF0,0x03E0,0x0070,0x0038,0x0010,0x0000,0x0000,0x0000,
    /* 0x52 'R' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3FC0,0x3FF0,
    0x3878,0x3838,0x381C,0x381C,0x381C,0x381C,0x3838,0x3FF0,
    0x3FE0,0x38F0,0x3830,0x3838,0x381C,0x381C,0x380E,0x380E,
    0x3807,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x53 'S' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0FF8,0x1FF8,
    0x3808,0x3800,0x3000,0x3000,0x3800,0x3C00,0x1FC0,0x0FF0,
    0x01F8,0x003C,0x001C,0x000C,0x000C,0x001C,0x201C,0x3C78,
    0x3FF0,0x07C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x54 'T' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x7FFE,0x7FFE,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x55 'U' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x381C,0x381C,
    0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,
    0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x1E78,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x56 'V' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x700E,0x700E,
    0x700E,0x300C,0x381C,0x381C,0x1818,0x1838,0x1C38,0x1C30,
    0x0C30,0x0E70,0x0E70,0x0660,0x0660,0x07E0,0x03C0,0x03C0,
    0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x57 'W' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0xE007,0xE007,
    0xE007,0xE006,0x6006,0x6186,0x63C6,0x63CE,0x73CE,0x73CE,
    0x364C,0x366C,0x366C,0x366C,0x3C7C,0x3C3C,0x3C3C,0x1C38,
    0x1C38,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x58 'X' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x700E,0x380C,
    0x181C,0x1C38,0x0E30,0x0E70,0x0760,0x03E0,0x03C0,0x03C0,
    0x03C0,0x07E0,0x0E70,0x0E30,0x1C38,0x1818,0x381C,0x700E,
    0x700E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x59 'Y' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x700E,0x700E,
    0x381C,0x3818,0x1C38,0x0C30,0x0E70,0x06E0,0x07E0,0x03C0,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x5A 'Z' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3FFE,0x3FFE,
    0x001E,0x001C,0x0038,0x0038,0x0070,0x00E0,0x00E0,0x01C0,
    0x0380,0x0380,0x0700,0x0E00,0x0E00,0x1C00,0x3800,0x3FFE,
    0x3FFE,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x5B '[' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x03F0,0x03E0,0x0300,
    0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,
    0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,
    0x0300,0x0300,0x0300,0x03F0,0x03E0,0x0000,0x0000,0x0000,
    /* 0x5C 'backslash' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x3000,0x3800,
    0x3800,0x1800,0x1C00,0x0C00,0x0E00,0x0600,0x0700,0x0300,
    0x0380,0x0180,0x01C0,0x00C0,0x00E0,0x0060,0x0070,0x0070,
    0x0030,0x0038,0x0018,0x001C,0x0000,0x0000,0x0000,0x0000,
    /* 0x5D ']' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0FC0,0x07C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x0FC0,0x07C0,0x0000,0x0000,0x0000,
    /* 0x5E '^' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x03C0,0x03C0,
    0x07E0,0x0E70,0x1C38,0x381C,0x300C,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x5F '_' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0xFFFF,0xFFFF,
    /* 0x60 '`' */
    0x0000,0x0000,0x0000,0x0000,0x0E00,0x0600,0x0300,0x0180,
    0x0180,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x61 'a' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x07C0,0x1FF0,0x1C78,0x0018,0x001C,0x001C,
    0x07FC,0x1FFC,0x381C,0x301C,0x301C,0x301C,0x383C,0x3CFC,
    0x1FDC,0x0780,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x62 'b' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x3800,0x3800,0x3800,
    0x3800,0x3800,0x39E0,0x3BF0,0x3E38,0x3C1C,0x381C,0x380C,
    0x380C,0x380C,0x380C,0x380C,0x380C,0x381C,0x3C1C,0x3E38,
    0x3BF0,0x01C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x63 'c' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x01F0,0x07FC,0x0F1C,0x0E00,0x1C00,0x1800,
    0x1800,0x1800,0x1800,0x1800,0x1800,0x1C00,0x0E00,0x0F1C,
    0x07FC,0x01F0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x64 'd' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x001C,0x001C,0x001C,
    0x001C,0x001C,0x079C,0x0FDC,0x1C7C,0x383C,0x381C,0x301C,
    0x301C,0x301C,0x301C,0x301C,0x301C,0x381C,0x383C,0x1C7C,
    0x0FDC,0x0780,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x65 'e' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x03C0,0x0FF0,0x1E38,0x1C1C,0x380C,0x300C,
    0x300C,0x3FFC,0x3FFC,0x3000,0x3000,0x3800,0x1800,0x1E1C,
    0x0FFC,0x01E0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x66 'f' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x00FC,0x01FC,0x0180,
    0x0380,0x0380,0x0380,0x3FFC,0x1FF8,0x0380,0x0380,0x0380,
    0x0380,0x0380,0x0380,0x0380,0x0380,0x0380,0x0380,0x0380,
    0x0380,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x67 'g' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0780,0x0FDC,0x1C7C,0x383C,0x381C,0x301C,
    0x301C,0x301C,0x301C,0x301C,0x301C,0x383C,0x1C3C,0x1FFC,
    0x0FDC,0x0018,0x0018,0x0038,0x1C70,0x1FF0,0x07C0,0x0000,
    /* 0x68 'h' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x3800,0x3800,0x3800,
    0x3800,0x3800,0x39E0,0x3BF0,0x3E38,0x3C18,0x381C,0x381C,
    0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,
    0x381C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x69 'i' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0180,0x0180,
    0x0000,0x0000,0x0000,0x1F80,0x0F80,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x1FFC,
    0x3FFC,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x6A 'j' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x00C0,0x00C0,0x00C0,
    0x0000,0x0000,0x0000,0x0FC0,0x0FC0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,0x00C0,
    0x00C0,0x00C0,0x00C0,0x01C0,0x03C0,0x3F80,0x1E00,0x0000,
    /* 0x6B 'k' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x1C00,0x1C00,0x1C00,
    0x1C00,0x1C00,0x1C00,0x1C1C,0x1C38,0x1C70,0x1CE0,0x1DC0,
    0x1F80,0x1FC0,0x1EE0,0x1CE0,0x1C70,0x1C38,0x1C1C,0x1C1C,
    0x1C0E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x6C 'l' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x3F00,0x3F00,0x0300,
    0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,
    0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0380,0x01F8,
    0x00F8,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x6D 'm' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0638,0x7F7C,0x7BCC,0x718E,0x718E,0x718E,
    0x718E,0x718E,0x718E,0x718E,0x718E,0x718E,0x718E,0x718E,
    0x718E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x6E 'n' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x01E0,0x3BF0,0x3E38,0x3C18,0x381C,0x381C,
    0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,0x381C,
    0x381C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x6F 'o' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x03C0,0x0FF0,0x1E78,0x1818,0x381C,0x381C,
    0x300C,0x300C,0x300C,0x300C,0x381C,0x381C,0x1818,0x1E78,
    0x0FF0,0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x70 'p' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x01C0,0x3BF0,0x3E38,0x3C1C,0x381C,0x380C,
    0x380C,0x380C,0x380C,0x380C,0x380C,0x381C,0x3C1C,0x3E38,
    0x3BF0,0x39C0,0x3800,0x3800,0x3800,0x3800,0x1800,0x0000,
    /* 0x71 'q' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0380,0x0FFC,0x1E7C,0x183C,0x381C,0x381C,
    0x301C,0x301C,0x301C,0x301C,0x381C,0x381C,0x183C,0x1E7C,
    0x0FFC,0x039C,0x001C,0x001C,0x001C,0x001C,0x001C,0x0000,
    /* 0x72 'r' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x003C,0x06FE,0x07C6,0x0780,0x0700,0x0600,
    0x0600,0x0600,0x0600,0x0600,0x0600,0x0600,0x0600,0x0600,
    0x0600,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x73 's' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x03E0,0x0FF8,0x1E38,0x1800,0x1800,0x1C00,
    0x1E00,0x0FE0,0x03F0,0x0078,0x0018,0x0018,0x0018,0x1C78,
    0x1FF0,0x07C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x74 't' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0300,
    0x0300,0x0300,0x0700,0x3FF8,0x3FF8,0x0300,0x0300,0x0300,
    0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x03F8,
    0x01F8,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x75 'u' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x381C,0x381C,0x381C,0x381C,0x381C,
    0x381C,0x381C,0x381C,0x381C,0x181C,0x181C,0x183C,0x1E7C,
    0x0FDC,0x0780,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x76 'v' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x300C,0x300C,0x381C,0x1818,0x1818,
    0x1C38,0x0C30,0x0C70,0x0E70,0x0660,0x07E0,0x07E0,0x03C0,
    0x03C0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x77 'w' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0xE007,0xE007,0x6006,0x6006,0x6186,
    0x718E,0x33CC,0x33CC,0x324C,0x367C,0x3E78,0x1C78,0x1C38,
    0x1C38,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x78 'x' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x381C,0x1838,0x0C30,0x0E70,0x07E0,
    0x03C0,0x03C0,0x03C0,0x07E0,0x0660,0x0E70,0x1C38,0x381C,
    0x300C,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x79 'y' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x300E,0x380C,0x381C,0x1818,0x1C18,
    0x0C38,0x0C30,0x0E70,0x0670,0x0760,0x03E0,0x03C0,0x03C0,
    0x01C0,0x0180,0x0380,0x0300,0x0700,0x3E00,0x1C00,0x0000,
    /* 0x7A 'z' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x1FF8,0x1FF8,0x0038,0x0070,0x0060,
    0x00E0,0x01C0,0x0380,0x0700,0x0700,0x0E00,0x1C00,0x1FF8,
    0x1FF8,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    /* 0x7B '{' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0078,0x01F8,0x01C0,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0380,
    0x1F00,0x1E00,0x0780,0x0380,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x01C0,0x01C0,0x00F8,0x0018,0x0000,0x0000,
    /* 0x7C '|' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,
    /* 0x7D '}' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x1E00,0x1F00,0x0380,
    0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x0180,0x01C0,
    0x00F8,0x0078,0x01E0,0x0180,0x0180,0x0180,0x0180,0x0180,
    0x0180,0x0180,0x0380,0x0780,0x1F00,0x1800,0x0000,0x0000,
    /* 0x7E '~' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1F02,
    0x7FFE,0x61FE,0x0078,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
};

/* ---------------------------- 小工具 ---------------------------- */

static int gamma_lut[256]; /* 亮度 -> 经 gamma 调整后的亮度 */

static void build_gamma_lut(void)
{
    for (int i = 0; i < 256; ++i) {
        const int v = (int)(pow(i / 255.0, CHAR_GAMMA) * 255.0 + 0.5);
        gamma_lut[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
}

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

/* printf 风格的字符串拼接，返回 malloc 出来的字符串（用完要 free） */
static char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (buf) {
        vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    return buf;
}

/* 给 shell 参数加单引号，防止文件名里的空格或特殊字符出问题（用完要 free） */
static char *shell_quote(const char *s)
{
    char *r = malloc(strlen(s) * 4 + 3);
    if (!r) {
        return NULL;
    }
    char *p = r;
    *p++ = '\'';
    for (; *s; ++s) {
        if (*s == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = *s;
        }
    }
    *p++ = '\'';
    *p = '\0';
    return r;
}

static int exit_ok(int status)
{
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int command_exists(const char *name)
{
    char *cmd = xasprintf("command -v %s >/dev/null 2>&1", name);
    if (!cmd) {
        return 0;
    }
    const int ok = exit_ok(system(cmd));
    free(cmd);
    return ok;
}

/* ---------------------------- 编码能力探测 ---------------------------- */

/*
 * 硬件编码器（mediacodec / vaapi / videotoolbox 等）写不出 color_primaries 与
 * color_transfer，播放器会自己猜，猜成 smpte170m 就会让颜色发闷 —— 正是 v4.1
 * 修饱和度时最怕的那种偏差。用一次流复制重封装把 BT.709 三件套补齐。
 * 返回 1 表示补写成功。
 */
static int stamp_bt709(const char *output)
{
    const char *dot = strrchr(output, '.');
    char *tmp = (dot && dot != output) ? xasprintf("%.*s_tag%s", (int)(dot - output), output, dot)
                                       : xasprintf("%s_tag.mp4", output);
    if (!tmp) {
        return 0;
    }
    char *qout = shell_quote(output);
    char *qtmp = shell_quote(tmp);
    int done = 0;
    if (qout && qtmp) {
        char *cmd = xasprintf("ffmpeg -v error -y -i %s -map 0 -c copy "
                              "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
                              "-movflags +faststart %s 2>/dev/null",
                              qout, qtmp);
        if (cmd) {
            if (exit_ok(system(cmd))) {
                if (rename(tmp, output) == 0) {
                    done = 1;
                }
            }
            free(cmd);
        }
    }
    if (!done) {
        remove(tmp);
    }
    free(qout);
    free(qtmp);
    free(tmp);
    return done;
}

/* 物理内存（MB）；读不到返回 0（0 表示不做内存限制） */
static long total_memory_mb(void)
{
    /* Linux / Android */
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        long kb = 0;
        char line[128];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) {
                break;
            }
        }
        fclose(f);
        if (kb > 0) {
            return kb / 1024;
        }
    }

    /* macOS / BSD 没有 /proc，用 sysctl；不同系统字段名不同，两个都试 */
    FILE *p = popen("(sysctl -n hw.memsize || /usr/sbin/sysctl -n hw.memsize "
                    "|| sysctl -n hw.physmem || /usr/sbin/sysctl -n hw.physmem) 2>/dev/null",
                    "r");
    if (!p) {
        return 0;
    }
    long long bytes = 0;
    const int got = fscanf(p, "%lld", &bytes) == 1;
    pclose(p);
    return (got && bytes > 0) ? (long)(bytes / (1024 * 1024)) : 0;
}

/* 可用的核数；设 MIKU_CORES=N 可手工压低调度（省电、共享主机、老旧设备） */
static int cpu_cores(void)
{
    const char *env = getenv("MIKU_CORES");
    if (env && *env) {
        const int n = atoi(env);
        if (n > 0) {
            return n;
        }
    }
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* 该编码器是否被当前 ffmpeg 编译进来。列不出来就不必再花一次探测 */
static int encoder_listed_by_name(const char *codec)
{
    FILE *p = popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
    if (!p) {
        return 0;
    }
    const size_t n = strlen(codec);
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, p)) {
        for (const char *hit = strstr(line, codec); hit; hit = strstr(hit + 1, codec)) {
            const int left_ok = hit == line || hit[-1] == ' ' || hit[-1] == '\t';
            const char next = hit[n];
            if (left_ok && (next == ' ' || next == '\t')) {
                found = 1;
                break;
            }
        }
    }
    pclose(p);
    return found;
}

static int encoder_listed(int idx)
{
    static signed char cache[ENCODER_COUNT]; /* 0 = 还没查过 */
    if (cache[idx] == 0) {
        cache[idx] = encoder_listed_by_name(ENCODERS[idx].codec) ? 1 : -1;
    }
    return cache[idx] > 0;
}

/* 硬件编码器只能按码率控，按输出像素率折算 */
static long hw_bitrate_kbps(int w, int h, int fps)
{
    const double px_per_sec = (double)w * h * fps;
    long kbps = (long)(px_per_sec * HW_BITS_PER_PIXEL / 1000.0);
    if (kbps < HW_MIN_KBPS) {
        kbps = HW_MIN_KBPS;
    }
    if (kbps > HW_MAX_KBPS) {
        kbps = HW_MAX_KBPS;
    }
    return kbps;
}

/* VAAPI 的设备节点不固定（renderD128/129/…），运行时扫一遍；找不到就没有 vaapi */
static const char *vaapi_device_arg(void)
{
    static char buf[128];
    static int scanned = 0;

    if (!scanned) {
        scanned = 1;
        DIR *d = opendir("/dev/dri");
        if (d) {
            const struct dirent *e;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, "renderD", 7) == 0) {
                    snprintf(buf, sizeof buf, "-vaapi_device /dev/dri/%s", e->d_name);
                    break;
                }
            }
            closedir(d);
        }
    }
    return buf[0] ? buf : NULL;
}

/* x264 参数：默认 veryfast/crf19；MIKU_PRESET / MIKU_CRF 可覆盖。
 * 想快就 MIKU_PRESET=ultrafast：编码约快 2.2 倍、同画质体积大 1.6 倍
 * （ultrafast 关了 CABAC）。返回的字符串用完要 free */
static char *x264_args(void)
{
    const char *preset = getenv("MIKU_PRESET");
    const char *crf = getenv("MIKU_CRF");
    if (!preset || !*preset) {
        preset = X264_PRESET;
    }
    if (!crf || !*crf) {
        crf = X264_CRF;
    }
    return xasprintf("-preset %s -crf %s -x264-params colorprim=bt709:transfer=bt709:colormatrix=bt709",
                     preset, crf);
}

/* 该编码器实际要用的参数；x264 走环境变量，其余用表里的固定值或码率 */
static char *encoder_args(const EncoderCand *c, int w, int h, int fps)
{
    if (strcmp(c->codec, "libx264") == 0) {
        return x264_args();
    }
    if (c->fixed) {
        return xasprintf("%s", c->fixed);
    }
    return xasprintf("-b:v %ldk", hw_bitrate_kbps(w, h, fps));
}

/* 候选编码器需要的 -i 前参数；返回 NULL 表示本机缺条件（如没有 renderD*） */
static const char *cand_input_prefix(const EncoderCand *c)
{
    return c->vaapi ? vaapi_device_arg() : c->input_prefix;
}

/*
 * 真编 PROBE_FRAMES 帧来判断编码器在这个尺寸上是否可用，用的是和正式转换
 * 相同的滤镜链，写完再把帧数数回来。
 *   - 只看 -encoders 列表不够：h264_vulkan 会列出来但滤镜协商就失败；
 *   - 只看退出码也不够：某些设备/版本的 mediacodec 会正常退出，却把帧编码成
 *     解不出来的包（实测这一版 ffmpeg 上 96 个包只能解出 73 帧），
 *     出来的片子会缺帧甚至短一截。所以必须校验解码帧数。
 */
static const char *temp_dir(void)
{
    const char *t = getenv("TMPDIR");
    if (t && *t) {
        return t;
    }
    t = getenv("TMP");
    return (t && *t) ? t : "/tmp";
}

/* 数出文件里能真正解码的视频帧数；失败返回 -1 */
static int count_decodable_frames(const char *path)
{
    char *q = shell_quote(path);
    if (!q) {
        return -1;
    }
    char *cmd = xasprintf("ffprobe -v error -count_frames -select_streams v:0 "
                          "-show_entries stream=nb_read_frames -of csv=p=0 %s 2>/dev/null",
                          q);
    free(q);
    if (!cmd) {
        return -1;
    }
    FILE *p = popen(cmd, "r");
    free(cmd);
    if (!p) {
        return -1;
    }
    int frames = -1;
    if (fscanf(p, "%d", &frames) != 1) {
        frames = -1;
    }
    pclose(p);
    return frames;
}

/* 探测用帧数：够短不拖启动，又足以暴露"吞尾部/坏包"的问题
 * （实测坏掉的 mediacodec 无论喂多少帧都只交出 1 帧） */
#define PROBE_FRAMES 8

static int encoder_works(const EncoderCand *c, int w, int h, int fps)
{
    const char *prefix = cand_input_prefix(c);
    if (c->vaapi && !prefix) {
        return 0; /* 本机没有 renderD* 设备，vaapi 无从谈起 */
    }
    char *args = encoder_args(c, w, h, fps);
    if (!args) {
        return 0;
    }
    char *out = xasprintf("%s/miku-probe-%ld.mp4", temp_dir(), (long)getpid());
    if (!out) {
        free(args);
        return 0;
    }
    remove(out);
    char *cmd = xasprintf("ffmpeg -v error -y %s -f lavfi -i 'color=c=black:s=%dx%d:r=%d' "
                          "-frames:v %d -vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p%s' "
                          "-c:v %s %s %s 2>/dev/null",
                          prefix ? prefix : "", w, h, fps, PROBE_FRAMES,
                          c->pipe_filter ? c->pipe_filter : "", c->codec, args, out);
    free(args);
    int ok = 0;
    if (cmd) {
        ok = exit_ok(system(cmd)) && count_decodable_frames(out) >= PROBE_FRAMES;
        free(cmd);
    }
    remove(out);
    free(out);
    return ok;
}

/* 按候选表顺序找第一个在本机该尺寸下真能用的编码器。
 * 设 MIKU_NO_HW=1 可跳过所有硬件编码器（驱动有问题时的逃生口） */
static const EncoderCand *pick_encoder(int w, int h, int fps)
{
    const char *no_hw = getenv("MIKU_NO_HW");
    const int skip_hw = no_hw && *no_hw && strcmp(no_hw, "0") != 0;
    for (int i = 0; i < ENCODER_COUNT; ++i) {
        if (skip_hw && ENCODERS[i].hardware) {
            continue;
        }
        if (!encoder_listed(i)) {
            continue;
        }
        if (encoder_works(&ENCODERS[i], w, h, fps)) {
            return &ENCODERS[i];
        }
    }
    return NULL;
}

/* 每秒输出视频预计要占几个 CPU 秒 */
static double profile_cpu(const Profile *p)
{
    const double mpx = (double)p->width * p->height * p->fps / 1e6;
    return mpx * (p->enc->hardware ? HW_CPU_PER_MPX : SW_CPU_PER_MPX) + mpx * RENDER_CPU_PER_MPX;
}

/* 收尾抽查：解出最后 2 秒，帧数至少要有一秒的量。
 * 硬件编码器在个别设备上会漏帧，探测通过也可能在长片中途开始丢，所以再兜一道 */
static int output_tail_ok(const char *output, const Profile *p)
{
    char *q = shell_quote(output);
    if (!q) {
        return 0;
    }
    char *cmd = xasprintf("ffprobe -v error -read_intervals -2%% -count_frames -select_streams v:0 "
                          "-show_entries stream=nb_read_frames -of csv=p=0 %s 2>/dev/null",
                          q);
    free(q);
    if (!cmd) {
        return 0;
    }
    FILE *f = popen(cmd, "r");
    free(cmd);
    if (!f) {
        return 0;
    }
    int frames = -1;
    if (fscanf(f, "%d", &frames) != 1) {
        frames = -1;
    }
    pclose(f);
    return frames >= p->fps;
}

/* 解析 "1920x1080" / "1920x1080@30"；fps 省略时返回 0 表示跟着源帧率走 */
static int parse_custom_size(const char *s, int *w, int *h, int *fps)
{
    int ww = 0, hh = 0, ff = 0;
    const int n = sscanf(s, "%dx%d@%d", &ww, &hh, &ff);
    if (n < 2) {
        return 0;
    }
    if (n < 3) {
        ff = 0;
    }
    if (ww < 256 || hh < 256 || ww > 7680 || hh > 4320) {
        return 0;
    }
    if ((ww & 1) != 0 || (hh & 1) != 0) { /* yuv420p 与硬件编码器都要求偶数 */
        return 0;
    }
    if (n >= 3 && (ff < 1 || ff > 240)) {
        return 0;
    }
    *w = ww;
    *h = hh;
    *fps = ff;
    return 1;
}

/*
 * 选档位：
 *   quality 为 "auto" 时按 高→低 找第一个「有可用编码器 + 成本进预算 + 内存够」的档；
 *   给了档位名或 WxH[@fps] 就按用户指定的来（仍然会挑可用编码器）。
 * 输出帧率不超过源帧率 —— 源是 24fps 时补到 60fps 只是白算，画面不会更流畅。
 */
static int choose_profile(const char *quality, int src_fps, Profile *p)
{
    const char *q = (quality && *quality) ? quality : "auto";
    int w = 0, h = 0, want_fps = 0;
    const int cores = cpu_cores();
    const long mem_mb = total_memory_mb();
    const double budget = (double)cores * CPU_BUDGET_RATIO;

    if (parse_custom_size(q, &w, &h, &want_fps)) {
        if (want_fps > 0) {
            snprintf(p->name, sizeof p->name, "%dx%d@%d", w, h, want_fps);
        } else {
            snprintf(p->name, sizeof p->name, "%dx%d", w, h);
        }
    } else {
        int tier = -1;
        for (int i = 0; i < TIER_COUNT; ++i) {
            if (strcmp(q, TIERS[i].name) == 0) {
                tier = i;
            }
        }
        if (tier < 0 && strcmp(q, "auto") != 0) {
            return 0; /* 非法的档位名 */
        }
        if (tier >= 0) {
            w = TIERS[tier].width;
            h = TIERS[tier].height;
            want_fps = TIERS[tier].fps;
            snprintf(p->name, sizeof p->name, "%s", TIERS[tier].name);
        } else {
            /* auto：逐个档位试，第一个「编码器可用且算得过来」的就用；
             * 都不进预算时取最低那档能跑的（别退到最高档去） */
            const EncoderCand *fallback = NULL;
            int fb_w = 0, fb_h = 0, fb_fps = 0, fb_tier = 0;
            for (int i = 0; i < TIER_COUNT; ++i) {
                const EncoderCand *enc = pick_encoder(TIERS[i].width, TIERS[i].height, TIERS[i].fps);
                if (!enc) {
                    continue;
                }
                const int fps = src_fps > 0 && src_fps < TIERS[i].fps ? src_fps : TIERS[i].fps;
                Profile tmp;
                snprintf(tmp.name, sizeof tmp.name, "%s", TIERS[i].name);
                tmp.width = TIERS[i].width;
                tmp.height = TIERS[i].height;
                tmp.fps = fps;
                tmp.enc = enc;
                fallback = enc;
                fb_w = tmp.width;
                fb_h = tmp.height;
                fb_fps = fps;
                fb_tier = i;
                if (mem_mb > 0 && mem_mb < TIERS[i].min_mem_mb) {
                    continue;
                }
                if (profile_cpu(&tmp) <= budget) {
                    *p = tmp;
                    return 1;
                }
            }
            if (!fallback) {
                return 0;
            }
            w = fb_w;
            h = fb_h;
            want_fps = fb_fps;
            snprintf(p->name, sizeof p->name, "%s", TIERS[fb_tier].name);
        }
    }

    if (src_fps > 0 && (want_fps <= 0 || src_fps < want_fps)) {
        want_fps = src_fps;
    }
    if (want_fps < 1) {
        want_fps = TIERS[TIER_COUNT - 1].fps;
    }

    const EncoderCand *enc = pick_encoder(w, h, want_fps);
    if (!enc) {
        return 0;
    }
    p->width = w;
    p->height = h;
    p->fps = want_fps;
    p->enc = enc;
    return 1;
}

/* ---------------------------- 字形图集 ---------------------------- */

/*
 * 启动时把每个字符缩放成一张 cell_w x cell_h 的灰度蒙版（只做一次），
 * 并统计每个字符的"墨量"（字形覆盖格子的比例），按墨量从小到大排序。
 * 之后根据亮度直接查表就能得到最合适的字符，非常快。
 */
typedef struct {
    int cell_w;
    int cell_h;
    uint8_t *masks;                 /* FONT_CHAR_COUNT 张蒙版，已按墨量由小到大排序 */
    char chars[FONT_CHAR_COUNT + 1]; /* 排序后的字符，由暗到亮 */
    int lut[256];                   /* 亮度(0-255) -> 蒙版索引 */
} GlyphAtlas;

static int font_bit(int glyph, int x, int y)
{
    return (FONT_BITMAP[glyph * FONT_SRC_H + y] >> (FONT_SRC_W - 1 - x)) & 1;
}

/* 把第 glyph 个字符的 16x32 位图缩放到 cw x ch，输出 0-255 的灰度蒙版 */
static void render_glyph(int glyph, uint8_t *dst, int cw, int ch)
{
    if (cw <= FONT_SRC_W) {
        /* 缩小：面积平均（带小数权重），得到平滑的抗锯齿效果 */
        const double sx = (double)FONT_SRC_W / cw;
        const double sy = (double)FONT_SRC_H / ch;
        for (int j = 0; j < ch; ++j) {
            const double y0 = j * sy, y1 = y0 + sy;
            const int iy0 = (int)floor(y0);
            int iy1 = (int)ceil(y1) - 1;
            if (iy1 > FONT_SRC_H - 1) {
                iy1 = FONT_SRC_H - 1;
            }
            for (int i = 0; i < cw; ++i) {
                const double x0 = i * sx, x1 = x0 + sx;
                const int ix0 = (int)floor(x0);
                int ix1 = (int)ceil(x1) - 1;
                if (ix1 > FONT_SRC_W - 1) {
                    ix1 = FONT_SRC_W - 1;
                }
                double sum = 0.0, area = 0.0;
                for (int yy = iy0; yy <= iy1; ++yy) {
                    const double wy = dmin(y1, yy + 1.0) - dmax(y0, (double)yy);
                    if (wy <= 0.0) {
                        continue;
                    }
                    for (int xx = ix0; xx <= ix1; ++xx) {
                        const double wx = dmin(x1, xx + 1.0) - dmax(x0, (double)xx);
                        if (wx <= 0.0) {
                            continue;
                        }
                        sum += wx * wy * font_bit(glyph, xx, yy);
                        area += wx * wy;
                    }
                }
                const int v = area > 0.0 ? (int)(sum / area * 255.0 + 0.5) : 0;
                dst[(size_t)j * cw + i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    } else {
        /* 放大：双线性插值 */
        for (int j = 0; j < ch; ++j) {
            double fy = (j + 0.5) * FONT_SRC_H / ch - 0.5;
            fy = dmax(0.0, dmin(fy, FONT_SRC_H - 1.0));
            const int y0 = (int)fy;
            const int y1 = y0 + 1 < FONT_SRC_H ? y0 + 1 : y0;
            const double ty = fy - y0;
            for (int i = 0; i < cw; ++i) {
                double fx = (i + 0.5) * FONT_SRC_W / cw - 0.5;
                fx = dmax(0.0, dmin(fx, FONT_SRC_W - 1.0));
                const int x0 = (int)fx;
                const int x1 = x0 + 1 < FONT_SRC_W ? x0 + 1 : x0;
                const double tx = fx - x0;
                const double top = (1.0 - tx) * font_bit(glyph, x0, y0) + tx * font_bit(glyph, x1, y0);
                const double bot = (1.0 - tx) * font_bit(glyph, x0, y1) + tx * font_bit(glyph, x1, y1);
                const int v = (int)(((1.0 - ty) * top + ty * bot) * 255.0 + 0.5);
                dst[(size_t)j * cw + i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

static int atlas_build(GlyphAtlas *a, int cell_w, int cell_h)
{
    const size_t gsize = (size_t)cell_w * cell_h;
    uint8_t *raw = malloc(gsize * FONT_CHAR_COUNT);
    uint8_t *sorted = malloc(gsize * FONT_CHAR_COUNT);
    double ink[FONT_CHAR_COUNT];
    double norm[FONT_CHAR_COUNT];
    int order[FONT_CHAR_COUNT];

    if (!raw || !sorted) {
        free(raw);
        free(sorted);
        return 0;
    }

    /* 第一步：渲染每个字符并统计墨量 */
    for (int g = 0; g < FONT_CHAR_COUNT; ++g) {
        uint8_t *m = raw + (size_t)g * gsize;
        render_glyph(g, m, cell_w, cell_h);
        double sum = 0.0;
        for (size_t k = 0; k < gsize; ++k) {
            sum += m[k];
        }
        ink[g] = sum / (gsize * 255.0);
        order[g] = g;
    }

    /* 第二步：按墨量从小到大做稳定的插入排序 */
    for (int i = 1; i < FONT_CHAR_COUNT; ++i) {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0 && ink[order[j]] > ink[key]) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    const double min_ink = ink[order[0]];
    const double max_ink = ink[order[FONT_CHAR_COUNT - 1]];
    if (max_ink - min_ink < 1e-9) {
        free(raw);
        free(sorted);
        return 0;
    }

    for (int i = 0; i < FONT_CHAR_COUNT; ++i) {
        memcpy(sorted + (size_t)i * gsize, raw + (size_t)order[i] * gsize, gsize);
        a->chars[i] = (char)(FONT_FIRST_CHAR + order[i]);
        norm[i] = (ink[order[i]] - min_ink) / (max_ink - min_ink);
    }
    a->chars[FONT_CHAR_COUNT] = '\0';

    /* 第三步：建立 亮度(0-255) -> 字符索引 的查找表（取墨量最接近的字符） */
    for (int level = 0; level < 256; ++level) {
        const double target = level / 255.0;
        int lo = 0, hi = FONT_CHAR_COUNT; /* lower_bound */
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (norm[mid] < target) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        int idx = lo;
        if (idx >= FONT_CHAR_COUNT) {
            idx = FONT_CHAR_COUNT - 1;
        } else if (idx > 0 && (target - norm[idx - 1]) < (norm[idx] - target)) {
            idx -= 1;
        }
        a->lut[level] = idx;
    }

    a->cell_w = cell_w;
    a->cell_h = cell_h;
    a->masks = sorted;
    free(raw);
    return 1;
}

/* ---------------------------- 视频信息 ---------------------------- */

typedef struct {
    int width;
    int height;
    long fps_num;
    long fps_den;
    long total_frames; /* 0 表示未知 */
} VideoInfo;

/* 解析 "30000/1001" 或 "24" 这样的帧率 */
static int parse_rate(const char *s, long *num, long *den)
{
    long n, d;
    double v;
    if (sscanf(s, "%ld/%ld", &n, &d) == 2 && n > 0 && d > 0) {
        *num = n;
        *den = d;
        return 1;
    }
    if (sscanf(s, "%lf", &v) == 1 && v > 0.0) {
        *num = (long)lround(v * 1000.0);
        *den = 1000;
        return 1;
    }
    return 0;
}

/* 用 ffprobe 读取视频的分辨率、帧率、总帧数 */
static int probe_video(const char *path, VideoInfo *info)
{
    char *q = shell_quote(path);
    if (!q) {
        return 0;
    }
    char *cmd = xasprintf("ffprobe -v error -select_streams v:0 "
                          "-show_entries stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration"
                          ":stream_side_data=rotation:stream_tags=rotate "
                          "-of default=noprint_wrappers=1 %s 2>/dev/null", q);
    free(q);
    if (!cmd) {
        return 0;
    }
    FILE *p = popen(cmd, "r");
    free(cmd);
    if (!p) {
        return 0;
    }

    memset(info, 0, sizeof *info);
    long avg_n = 0, avg_d = 0, r_n = 0, r_d = 0;
    double duration = 0.0, rotation = 0.0;
    char line[256];
    while (fgets(line, sizeof line, p)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        if (strcmp(key, "width") == 0) {
            info->width = atoi(val);
        } else if (strcmp(key, "height") == 0) {
            info->height = atoi(val);
        } else if (strcmp(key, "avg_frame_rate") == 0) {
            parse_rate(val, &avg_n, &avg_d);
        } else if (strcmp(key, "r_frame_rate") == 0) {
            parse_rate(val, &r_n, &r_d);
        } else if (strcmp(key, "nb_frames") == 0) {
            info->total_frames = atol(val);
        } else if (strcmp(key, "duration") == 0) {
            duration = atof(val);
        } else if (strcmp(key, "rotation") == 0 || strcmp(key, "TAG:rotate") == 0) {
            rotation = atof(val);
        }
    }
    pclose(p);

    if (info->width <= 0 || info->height <= 0) {
        return 0;
    }

    /* 手机拍的竖屏视频带有旋转信息，ffmpeg 解码时会自动旋转，所以这里要交换宽高 */
    if (((int)lround(fabs(rotation))) % 180 == 90) {
        const int t = info->width;
        info->width = info->height;
        info->height = t;
    }

    /* 帧率：优先用平均帧率，其次用基础帧率，都没有就按 30 处理 */
    if (avg_n > 0) {
        info->fps_num = avg_n;
        info->fps_den = avg_d;
    } else if (r_n > 0) {
        info->fps_num = r_n;
        info->fps_den = r_d;
    } else {
        info->fps_num = 30;
        info->fps_den = 1;
    }

    if (info->total_frames <= 0 && duration > 0.0) {
        info->total_frames = (long)(duration * info->fps_num / info->fps_den + 0.5);
    }
    return 1;
}

/* ---------------------------- 渲染 ---------------------------- */

/* 计算字符颜色：提高饱和度，再按比例整体缩放（保持色相，绝不逐通道截断） */
static void make_cell_color(int r, int g, int b, int *out_r, int *out_g, int *out_b)
{
    const double gray = RED_WEIGHT * r + GREEN_WEIGHT * g + BLUE_WEIGHT * b;
    double fr = gray + (r - gray) * SATURATION_BOOST;
    double fg = gray + (g - gray) * SATURATION_BOOST;
    double fb = gray + (b - gray) * SATURATION_BOOST;
    fr = fr < 0.0 ? 0.0 : fr;
    fg = fg < 0.0 ? 0.0 : fg;
    fb = fb < 0.0 ? 0.0 : fb;

    const double m = dmax(fr, dmax(fg, fb));
    if (m < 1.0) { /* 近乎纯黑 */
        *out_r = *out_g = *out_b = 0;
        return;
    }

    double target = m * COLOR_GAIN;
    const double floor_target = dmin(COLOR_MIN_VALUE, m * COLOR_MAX_GAIN);
    if (target < floor_target) {
        target = floor_target;
    }
    if (target > 255.0) {
        target = 255.0;
    }
    const double k = target / m;

    const int ir = (int)(fr * k + 0.5), ig = (int)(fg * k + 0.5), ib = (int)(fb * k + 0.5);
    *out_r = ir > 255 ? 255 : ir;
    *out_g = ig > 255 ? 255 : ig;
    *out_b = ib > 255 ? 255 : ib;
}

/*
 * 把缩小后的 RGB 图（每个像素对应一个字符格）渲染成 ASCII 图像（RGB24）
 * out 指向网格左上角在整张画布中的位置，row_stride 是整张画布一行的字节数，
 * 这样网格可以直接画在居中的位置，画布其余部分保持黑色
 */
static void render_frame(const GlyphAtlas *atlas, const uint8_t *cells, int cols, int rows,
                         uint8_t *out, size_t row_stride)
{
    const int cw = atlas->cell_w;
    const int ch = atlas->cell_h;
    const size_t glyph_size = (size_t)cw * ch;

    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const uint8_t *px = cells + ((size_t)y * cols + x) * 3;
            const int r = px[0], g = px[1], b = px[2];

            /* 亮度决定用哪个字符 */
            const double lum = RED_WEIGHT * r + GREEN_WEIGHT * g + BLUE_WEIGHT * b;
            int level = (int)(lum + 0.5);
            level = level < 0 ? 0 : (level > 255 ? 255 : level);
            const uint8_t *mask = atlas->masks + (size_t)atlas->lut[gamma_lut[level]] * glyph_size;

            /* 原始像素颜色决定字符颜色 */
            int cr, cg, cb;
            make_cell_color(r, g, b, &cr, &cg, &cb);

            for (int j = 0; j < ch; ++j) {
                uint8_t *d = out + ((size_t)y * ch + j) * row_stride + (size_t)x * cw * 3;
                const uint8_t *m = mask + (size_t)j * cw;
                for (int i = 0; i < cw; ++i) {
                    /* 混合用 (x*a+128+((x*a+128)>>8))>>8 代替 /255：
                     * 结果是四舍五入的 x*a/255，但对 8 位输入是精确的 */
                    const unsigned a = m[i];
                    const unsigned tr = (unsigned)cr * a + 128;
                    const unsigned tg = (unsigned)cg * a + 128;
                    const unsigned tb = (unsigned)cb * a + 128;
                    d[0] = (uint8_t)((tr + (tr >> 8)) >> 8);
                    d[1] = (uint8_t)((tg + (tg >> 8)) >> 8);
                    d[2] = (uint8_t)((tb + (tb >> 8)) >> 8);
                    d += 3;
                }
            }
        }
    }
}

/* ---------------------------- 转换主流程 ---------------------------- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ASCII 网格在输出画布中的布局 */
typedef struct {
    int cols, rows;     /* 网格的字符列数、行数 */
    int cell_w, cell_h; /* 每个字符的像素大小 */
    int off_x, off_y;   /* 网格左上角在画布中的位置（居中，取偶数） */
} Layout;

static void compute_layout(const Profile *p, int req_cols, int video_w, int video_h, Layout *L)
{
    /* 字符宽度 = 画布宽度 / 列数，取偶数；高度是宽度的2倍（字符格子是 1:2 的） */
    int cell_w = (p->width / req_cols) & ~1;
    cell_w = cell_w < MIN_CELL_WIDTH ? MIN_CELL_WIDTH : (cell_w > MAX_CELL_WIDTH ? MAX_CELL_WIDTH : cell_w);
    const int cell_h = cell_w * 2;

    const int max_cols = p->width / cell_w;
    const int max_rows = p->height / cell_h;

    int cols = req_cols > max_cols ? max_cols : req_cols;
    /* 行数 = 列数 * 视频高宽比 * (cell_w / cell_h) */
    int rows = (int)lround((double)cols * video_h / video_w * cell_w / cell_h);

    if (rows > max_rows) { /* 视频比 16:9 更"方"或更"高"：按高度铺满，列数相应减少 */
        rows = max_rows;
        cols = (int)lround((double)rows * video_w / video_h * cell_h / cell_w);
        cols = cols > max_cols ? max_cols : cols;
    }
    cols = cols < 1 ? 1 : cols;
    rows = rows < 1 ? 1 : rows;

    L->cols = cols;
    L->rows = rows;
    L->cell_w = cell_w;
    L->cell_h = cell_h;
    L->off_x = ((p->width - cols * cell_w) / 2) & ~1;
    L->off_y = ((p->height - rows * cell_h) / 2) & ~1;
}

static int convert(const char *input, const char *output, int req_cols, const Profile *p,
                   const VideoInfo *vp)
{
    int ret = 0;
    GlyphAtlas atlas;
    memset(&atlas, 0, sizeof atlas);
    uint8_t *cells = NULL, *canvas = NULL;
    char *qin = NULL, *qout = NULL, *dec_cmd = NULL, *enc_cmd = NULL;
    FILE *dec = NULL, *enc = NULL;

    /* 步骤1：检查 ffmpeg / ffprobe */
    if (!command_exists("ffmpeg") || !command_exists("ffprobe")) {
        fprintf(stderr, "未找到 ffmpeg / ffprobe，请先安装 ffmpeg\n");
        return 0;
    }

    /* 步骤2：视频信息已经在入口处读过一次，这里直接用 */
    const VideoInfo info = *vp;
    const double src_fps = (double)info.fps_num / info.fps_den;

    /* 步骤3：计算字符大小和网格布局 */
    Layout L;
    compute_layout(p, req_cols, info.width, info.height, &L);

    printf("输出规格: %dx%d @ %dfps（档位 %s，%s编码）\n", p->width, p->height, p->fps,
           p->name, p->enc->hardware ? "硬件" : "软件");
    printf("ASCII网格: %dx%d 字符，每个字符 %dx%d 像素\n", L.cols, L.rows, L.cell_w, L.cell_h);
    if (L.off_x > 0 || L.off_y > 0) {
        printf("画面居中显示，黑边: 左右各 %d 像素，上下各 %d 像素\n", L.off_x, L.off_y);
    }
    if ((int)lround(src_fps) > p->fps) {
        printf("提示: 输入是 %.2ffps，输出取 %dfps（补到更高帧率只会重复帧，画面不会更流畅）\n",
               src_fps, p->fps);
    } else if (src_fps + 0.5 < p->fps) {
        printf("提示: 输入是 %.2ffps，将通过重复帧补到 %dfps\n", src_fps, p->fps);
    }

    /* 步骤4：预渲染字形图集 */
    build_gamma_lut();
    if (!atlas_build(&atlas, L.cell_w, L.cell_h)) {
        fprintf(stderr, "字形图集生成失败\n");
        goto cleanup;
    }
    printf("使用字符集: %d 个字符（由暗到亮）\n[%s]\n", FONT_CHAR_COUNT, atlas.chars);

    /* 步骤5：启动解码和编码两个 ffmpeg 进程
     * 编码器已经在 choose_profile 里真跑过 8 帧，这里直接用选中的那个 */
    char *enc_args = encoder_args(p->enc, p->width, p->height, p->fps);
    if (!enc_args) {
        goto cleanup;
    }
    printf("视频编码器: %s%s\n", p->enc->codec, p->enc->hardware ? "（硬件）" : "");

    qin = shell_quote(input);
    qout = shell_quote(output);
    if (!qin || !qout) {
        free(enc_args);
        goto cleanup;
    }

    /* 解码：统一为源帧率 -> 缩放到 ASCII 网格大小（面积插值）-> 原始 RGB24 输出到管道 */
    dec_cmd = xasprintf("ffmpeg -nostdin -v error -i %s -an -sn "
                        "-vf 'fps=%ld/%ld,scale=%d:%d:flags=area' -f rawvideo -pix_fmt rgb24 pipe:1",
                        qin, info.fps_num, info.fps_den, L.cols, L.rows);
    /* 编码：从管道读取画布的 RGB 帧，转成 BT.709 的 yuv420p 并打上色彩标签，
     * 同时从原视频取第一条音轨（没有音轨则忽略）。
     * 硬件编码器需要的 -i 前参数（如 vaapi 设备）必须和探测时完全一致 */
    const char *enc_prefix = cand_input_prefix(p->enc);
    enc_cmd = xasprintf("ffmpeg -y -v error %s -f rawvideo -pix_fmt rgb24 -s %dx%d -framerate %d -i pipe:0 "
                        "-i %s -map 0:v:0 -map '1:a:0?' "
                        "-vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p%s' -r %d -c:v %s %s "
                        "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
                        "-c:a aac -b:a 192k -movflags +faststart -shortest %s",
                        enc_prefix ? enc_prefix : "", p->width, p->height, p->fps, qin,
                        p->enc->pipe_filter ? p->enc->pipe_filter : "", p->fps,
                        p->enc->codec, enc_args, qout);
    free(enc_args);
    if (!dec_cmd || !enc_cmd) {
        goto cleanup;
    }

    const size_t cell_bytes = (size_t)L.cols * L.rows * 3;
    const size_t canvas_stride = (size_t)p->width * 3;
    const size_t canvas_bytes = canvas_stride * (size_t)p->height;
    cells = malloc(cell_bytes);
    canvas = calloc(canvas_bytes, 1); /* 全黑；网格以外的区域始终保持黑色 */
    if (!cells || !canvas) {
        fprintf(stderr, "内存不足\n");
        goto cleanup;
    }
    uint8_t *grid_origin = canvas + (size_t)L.off_y * canvas_stride + (size_t)L.off_x * 3;

    dec = popen(dec_cmd, "r");
    enc = popen(enc_cmd, "w");
    if (!dec || !enc) {
        fprintf(stderr, "无法启动 ffmpeg 进程\n");
        goto cleanup;
    }

    /* 步骤6：逐帧处理 */
    printf("开始转换视频...\n");
    int64_t src_index = 0;   /* 已读取的源帧数 */
    int64_t out_frames = 0;  /* 已写出的输出帧数 */
    const double start = now_seconds();
    int write_failed = 0;

    while (fread(cells, 1, cell_bytes, dec) == cell_bytes) {
        /* 第 i 帧源画面要在输出帧率里占几帧：floor((i+1)*out/fps) - floor(i*out/fps)
         * 输出帧率不超过源帧率时这个值恒为 1，不会白白重复写同一张画布 */
        const int64_t a = (src_index * p->fps * info.fps_den) / info.fps_num;
        const int64_t b = ((src_index + 1) * p->fps * info.fps_den) / info.fps_num;
        const int64_t repeat = b - a;
        src_index++;

        if (repeat > 0) {
            render_frame(&atlas, cells, L.cols, L.rows, grid_origin, canvas_stride);
            for (int64_t k = 0; k < repeat; ++k) {
                if (fwrite(canvas, 1, canvas_bytes, enc) != canvas_bytes) {
                    write_failed = 1;
                    break;
                }
                out_frames++;
            }
            if (write_failed) {
                break;
            }
        }

        if (src_index % 30 == 0) {
            printf("\r进度: %lld/%ld", (long long)src_index, info.total_frames);
            if (info.total_frames > 0) {
                const double elapsed = now_seconds() - start;
                const double eta = elapsed / src_index * (info.total_frames - src_index);
                printf(" 帧 (%.1f%%)  预计剩余 %.0f 秒   ", src_index * 100.0 / info.total_frames, eta > 0 ? eta : 0);
            }
            fflush(stdout);
        }
    }
    printf("\n");

    /* 步骤7：关闭管道，等待 ffmpeg 收尾（编码器在这里完成音频合并并写完文件） */
    const int dec_status = pclose(dec);
    dec = NULL;
    const int enc_status = pclose(enc);
    enc = NULL;

    if (write_failed || !exit_ok(enc_status)) {
        fprintf(stderr, "编码失败（ffmpeg 编码进程异常退出），请检查输出路径和磁盘空间\n");
        remove(output); /* 别把半成品留在磁盘上 */
        goto cleanup;
    }
    if (src_index == 0) {
        fprintf(stderr, "没有解码到任何画面\n");
        remove(output);
        goto cleanup;
    }
    if (!exit_ok(dec_status)) {
        fprintf(stderr, "警告: 解码过程中出现错误，输出视频可能不完整\n");
    }

    printf("转换完成! 源帧数: %lld，输出帧数: %lld (%dfps)\n", (long long)src_index,
           (long long)out_frames, p->fps);
    printf("已合并原视频音频（原视频没有音轨则无声）\n");
    printf("输出文件: %s\n", output);
    ret = 1;

cleanup:
    if (dec) {
        pclose(dec);
    }
    if (enc) {
        pclose(enc);
    }
    free(atlas.masks);
    free(cells);
    free(canvas);
    free(qin);
    free(qout);
    free(dec_cmd);
    free(enc_cmd);
    return ret;
}

/* ---------------------------- 入口 ---------------------------- */

/* 当前终端列数：先问标准输出，再问标准输入；都不通（重定向 / 非终端）返回 0 */
static int terminal_columns(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
            return 0;
        }
    }
    return (int)ws.ws_col;
}

/* ---------------------------- 终端播放 ---------------------------- */

/*
 * 播放不走视频编码：直接把解码出来的网格按「字符 + 颜色」打进终端。
 * 复用和转码完全相同的字符选择与取色，所以看到的就是成片的样子。
 */

static struct termios g_saved_term;
static int g_term_raw = 0;
static volatile sig_atomic_t g_play_stop = 0;

static void play_restore_term(void)
{
    if (g_term_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_term);
        g_term_raw = 0;
    }
    fputs("\x1b[0m\x1b[?25h", stdout); /* 复位颜色、恢复光标 */
    fflush(stdout);
}

static void play_on_signal(int sig)
{
    (void)sig;
    g_play_stop = 1;
}

static void play_raw_mode(void)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_saved_term) != 0) {
        return;
    }
    struct termios t = g_saved_term;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
        g_term_raw = 1;
    }
}

/* 读一个键，没有就返回 0（非阻塞） */
static int play_poll_key(void)
{
    if (!g_term_raw) {
        return 0;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
        return 0;
    }
    unsigned char c = 0;
    return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : 0;
}

static void sleep_until(double deadline)
{
    for (;;) {
        const double left = deadline - now_seconds();
        if (left <= 0.0) {
            return;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)left;
        ts.tv_nsec = (long)((left - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
}

/* 终端行数；拿不到返回 0 */
static int terminal_rows(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0) {
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0) {
            return 0;
        }
    }
    return (int)ws.ws_row;
}

/* 临时视频路径：$TMPDIR 或 /tmp，播放完就删 */
static char *temp_video_path(void)
{
    return xasprintf("%s/miku-%ld-%ld.mp4", temp_dir(), (long)getpid(), (long)time(NULL));
}

/* 源里有没有音轨 */
static int has_audio_stream(const char *input)
{
    char *q = shell_quote(input);
    if (!q) {
        return 0;
    }
    char *cmd = xasprintf("ffprobe -v error -select_streams a:0 -show_entries stream=index "
                          "-of csv=p=0 %s 2>/dev/null",
                          q);
    free(q);
    if (!cmd) {
        return 0;
    }
    FILE *p = popen(cmd, "r");
    free(cmd);
    if (!p) {
        return 0;
    }
    char line[32];
    const int has = fgets(line, sizeof line, p) != NULL;
    pclose(p);
    return has;
}

static pid_t g_audio_pid = -1;

/*
 * 只放声音：优先 mpv（--no-video 连视频都不解，CPU 很轻），其次 ffplay。
 * 低配设备上真正吃 CPU 的是往终端刷字符，音频解码只占个位数百分比；
 * 真跟不上就 --mute 或降低播放帧率。
 */
static int start_audio(const char *input)
{
    const char *player = NULL;
    const char *args[8];
    int n = 0;

    if (command_exists("mpv")) {
        player = "mpv";
        args[n++] = "mpv";
        args[n++] = "--no-video";
        args[n++] = "--really-quiet";
        args[n++] = "--no-terminal";
        args[n++] = "--";
        args[n++] = input;
    } else if (command_exists("ffplay")) {
        player = "ffplay";
        args[n++] = "ffplay";
        args[n++] = "-nodisp";
        args[n++] = "-autoexit";
        args[n++] = "-loglevel";
        args[n++] = "quiet";
        args[n++] = input;
    } else {
        return 0;
    }
    args[n] = NULL;

    const pid_t pid = fork();
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(player, (char *const *)args);
        _exit(127);
    }
    if (pid < 0) {
        return 0;
    }
    g_audio_pid = pid;
    return 1;
}

static void stop_audio(void)
{
    if (g_audio_pid > 0) {
        kill(g_audio_pid, SIGTERM);
        int status = 0;
        waitpid(g_audio_pid, &status, 0);
        g_audio_pid = -1;
    }
}

/* 按字节数扩容的缓冲区 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_reserve(StrBuf *sb, size_t extra)
{
    if (sb->len + extra <= sb->cap) {
        return 1;
    }
    size_t cap = sb->cap ? sb->cap : 4096;
    while (cap < sb->len + extra) {
        cap *= 2;
    }
    char *p = realloc(sb->buf, cap);
    if (!p) {
        return 0;
    }
    sb->buf = p;
    sb->cap = cap;
    return 1;
}

static int sb_addf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[64];
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0 || !sb_reserve(sb, (size_t)n + 1)) {
        return 0;
    }
    memcpy(sb->buf + sb->len, tmp, (size_t)n);
    sb->len += (size_t)n;
    return 1;
}

static int play_terminal(const char *input, int req_cols, const VideoInfo *vp, int play_fps,
                         int with_audio)
{
    GlyphAtlas atlas;
    memset(&atlas, 0, sizeof atlas);
    FILE *dec = NULL;
    char *qin = NULL, *cmd = NULL;
    uint8_t *cells = NULL;
    StrBuf sb = {NULL, 0, 0};
    int ret = 0;

    build_gamma_lut();
    /* 只借用图集里的字符顺序与亮度查找表，字符位图本身用不上 */
    if (!atlas_build(&atlas, 8, 16)) {
        fprintf(stderr, "字形图集生成失败\n");
        return 0;
    }

    /* 网格：列宽按参数，行数按视频宽高比；终端放不下就缩列数 */
    int cols = req_cols;
    const int aspect_rows = (int)lround((double)cols * vp->height / vp->width * 0.5);
    int rows = aspect_rows < 1 ? 1 : aspect_rows;
    const int trows = terminal_rows();
    if (trows > 1 && rows > trows - 1) { /* 留一行给提示 */
        rows = trows - 1;
        cols = (int)lround((double)rows * vp->width / vp->height * 2.0);
        cols = cols < 8 ? 8 : cols;
    }

    const size_t cell_bytes = (size_t)cols * rows * 3;
    cells = malloc(cell_bytes);
    if (!cells) {
        goto cleanup;
    }

    qin = shell_quote(input);
    if (!qin) {
        goto cleanup;
    }
    cmd = xasprintf("ffmpeg -nostdin -v error -i %s -an -sn -vf 'fps=%d,scale=%d:%d:flags=area' "
                    "-f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null",
                    qin, play_fps, cols, rows);
    if (!cmd) {
        goto cleanup;
    }
    dec = popen(cmd, "r");
    if (!dec) {
        fprintf(stderr, "无法启动 ffmpeg 解码\n");
        goto cleanup;
    }

    signal(SIGINT, play_on_signal);
    signal(SIGTERM, play_on_signal);
    play_raw_mode();
    atexit(play_restore_term);
    fputs("\x1b[?25l\x1b[2J", stdout); /* 藏光标、清屏 */

    const int audio = with_audio && has_audio_stream(input) && start_audio(input);
    if (audio) {
        atexit(stop_audio); /* 异常退出也别让声音一直响 */
    }

    const double start = now_seconds();
    int64_t frame = 0;
    int quit = 0;

    while (!g_play_stop && fread(cells, 1, cell_bytes, dec) == cell_bytes) {
        sb.len = 0;
        if (!sb_addf(&sb, "\x1b[H")) {
            break;
        }
        int last_r = -1, last_g = -1, last_b = -1;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const uint8_t *px = cells + ((size_t)y * cols + x) * 3;
                const int r = px[0], g = px[1], b = px[2];
                const double lum = RED_WEIGHT * r + GREEN_WEIGHT * g + BLUE_WEIGHT * b;
                int level = (int)(lum + 0.5);
                level = level < 0 ? 0 : (level > 255 ? 255 : level);
                int cr, cg, cb;
                make_cell_color(r, g, b, &cr, &cg, &cb);
                if (cr != last_r || cg != last_g || cb != last_b) { /* 颜色变了才发 SGR */
                    if (!sb_addf(&sb, "\x1b[38;2;%d;%d;%dm", cr, cg, cb)) {
                        quit = 1;
                        break;
                    }
                    last_r = cr;
                    last_g = cg;
                    last_b = cb;
                }
                if (!sb_reserve(&sb, 2)) {
                    quit = 1;
                    break;
                }
                sb.buf[sb.len++] = atlas.chars[atlas.lut[gamma_lut[level]]];
            }
            if (quit || !sb_addf(&sb, "\x1b[0m\r\n")) {
                quit = 1;
                break;
            }
            last_r = last_g = last_b = -1; /* 行末复位颜色 */
        }
        if (quit) {
            break;
        }
        fwrite(sb.buf, 1, sb.len, stdout);
        fflush(stdout);

        ++frame;
        if (play_poll_key() == 'q') {
            break;
        }
        if (g_play_stop) {
            break;
        }
        sleep_until(start + (double)frame / play_fps);
    }

    fputs("\x1b[0m\r\n", stdout);
    if (g_audio_pid > 0) {
        stop_audio(); /* 播完或中途退出都停掉声音 */
    }
    ret = 1;

cleanup:
    if (dec) {
        pclose(dec);
    }
    free(atlas.masks);
    free(cells);
    free(cmd);
    free(qin);
    free(sb.buf);
    return ret;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("用法: %s [--play] <input-video> <output-video> [ASCII宽度|auto] [质量|auto]\n", argv[0]);
        printf("示例: %s miku.mp4 ascii.mp4\n", argv[0]);
        printf("示例: %s --play miku.mp4 ascii.mp4 160 high\n", argv[0]);
        printf("示例: %s miku.mp4 ascii.mp4 auto 1920x1080@30\n", argv[0]);
        printf("ASCII宽度: %d-%d，越大字符越多越细，但单个字符越小\n",
               MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);
        printf("            省略或写 auto 时跟随终端列数（不在终端里运行则用 %d）\n",
               DEFAULT_ASCII_WIDTH);
        printf("质量档位: auto（默认，按核数/内存/可用硬件编码器自动降级）\n");
        for (int i = 0; i < TIER_COUNT; ++i) {
            printf("          %-4s %dx%d @ %dfps\n", TIERS[i].name, TIERS[i].width,
                   TIERS[i].height, TIERS[i].fps);
        }
        printf("          也可以直接给尺寸：1920x1080 或 1920x1080@30\n");
        printf("输出帧率不会超过源帧率；64 位 ARM 手机上优先用硬件编码器\n");
        printf("模式: 默认只转码；--play 转完播放；--play-only 不转码直接播放\n");
        printf("      --play 时省略输出文件名 = 用临时文件，播完（或中断）自动删除\n");
        printf("      --mute 播放时不出声（默认有音轨就会用 mpv/ffplay 放声音）\n");
        return 1;
    }

    /* 先把 --flag 摘出来，其余按位置解析 */
    int want_play = 0, play_only = 0, mute = 0;
    const char *pos[4] = {NULL, NULL, NULL, NULL};
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--play") == 0) {
            want_play = 1;
        } else if (strcmp(argv[i], "--play-only") == 0) {
            want_play = 1;
            play_only = 1;
        } else if (strcmp(argv[i], "--mute") == 0) {
            mute = 1;
        } else if (npos < 4) {
            pos[npos++] = argv[i];
        }
    }
    if (npos < 1) {
        fprintf(stderr, "错误: 至少要给输入文件名\n");
        return 1;
    }

    const char *input = pos[0];
    int pi = 1; /* 位置参数游标 */

    /* 播放模式下，第二个位置参数可能是宽度（纯数字或 auto）——那就没有输出文件 */
    const char *output = NULL;
    int width_given = 0;
    if (npos > pi) {
        const char *a = pos[pi];
        int is_width = strcmp(a, "auto") == 0;
        if (!is_width && *a != '\0') {
            is_width = 1;
            for (const char *c = a; *c; ++c) {
                if (*c < '0' || *c > '9') {
                    is_width = 0;
                    break;
                }
            }
        }
        if (want_play && is_width) {
            width_given = 1;
        } else {
            output = a;
            ++pi;
            width_given = (npos > pi);
        }
    }

    char *temp_out = NULL;
    int output_is_temp = 0;
    if (play_only) {
        output = NULL; /* 不转码，不需要输出文件 */
    } else if (!output) {
        if (!want_play) {
            fprintf(stderr, "错误: 转码需要输出文件名\n");
            return 1;
        }
        temp_out = temp_video_path();
        if (!temp_out) {
            fprintf(stderr, "错误: 申请临时文件路径失败\n");
            return 1;
        }
        output = temp_out;
        output_is_temp = 1;
    }

    int cols = DEFAULT_ASCII_WIDTH;
    int term_cols = 0;  /* 非 0 表示这次宽度来自终端，稍后打印适配结果 */
    char *end = NULL;
    int auto_width = !width_given;

    if (width_given && npos > pi) {
        if (strcmp(pos[pi], "auto") == 0) {
            auto_width = 1;
        } else {
            cols = (int)strtol(pos[pi], &end, 10);
            if (end == pos[pi] || *end != '\0') {
                fprintf(stderr, "错误: ASCII宽度必须是整数或 auto\n");
                return 1;
            }
        }
        ++pi;
    }
    if (auto_width) {
        term_cols = terminal_columns();
        if (term_cols > 0) {
            if (term_cols < MIN_ASCII_WIDTH) {
                cols = MIN_ASCII_WIDTH;
            } else if (term_cols > MAX_ASCII_WIDTH) {
                cols = MAX_ASCII_WIDTH;
            } else {
                cols = term_cols;
            }
        }
    }

    /* 质量档位。旧版这里是已取消的缩放系数，给纯数字时仍按旧提示忽略 */
    const char *quality = "auto";
    if (npos > pi) {
        int w = 0, h = 0, f = 0;
        int numeric = 1;
        for (const char *c = pos[pi]; *c; ++c) {
            if (*c < '0' || *c > '9') {
                numeric = 0;
                break;
            }
        }
        if (numeric) {
            fprintf(stderr, "提示: 第4个参数（缩放系数）已取消，字符大小由ASCII宽度自动决定，已忽略\n");
        } else if (parse_custom_size(pos[pi], &w, &h, &f) || strcmp(pos[pi], "auto") == 0 ||
                   strcmp(pos[pi], "high") == 0 || strcmp(pos[pi], "mid") == 0 ||
                   strcmp(pos[pi], "low") == 0) {
            quality = pos[pi];
        } else {
            fprintf(stderr, "错误: 质量参数无效，可用 auto / high / mid / low / 1920x1080[@30]\n");
            return 1;
        }
    }

    if (output && strcmp(input, output) == 0) {
        fprintf(stderr, "错误: 输出文件不能和输入文件相同\n");
        return 1;
    }
    if (cols < MIN_ASCII_WIDTH || cols > MAX_ASCII_WIDTH) {
        fprintf(stderr, "错误: ASCII宽度应在%d-%d之间\n", MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);
        return 1;
    }

    /* 编码进程意外退出时，写管道会触发 SIGPIPE；忽略它，改由 fwrite 的返回值来处理 */
    signal(SIGPIPE, SIG_IGN);

    printf("========================================\n");
    printf("原彩ASCII视频转换器 v4.5 (C17)\n");
    printf("========================================\n");

    if (term_cols > 0) {
        if (term_cols == cols) {
            printf("宽度: 已按终端 %d 列自动适配\n", cols);
        } else {
            printf("宽度: 终端 %d 列，超出 %d-%d，按 %d 列输出\n",
                   term_cols, MIN_ASCII_WIDTH, MAX_ASCII_WIDTH, cols);
        }
    }

    VideoInfo info;
    if (!probe_video(input, &info)) {
        fprintf(stderr, "无法读取视频信息: %s\n", input);
        return 1;
    }
    const double src_fps = (double)info.fps_num / info.fps_den;
    printf("视频信息: %dx%d, %.3ffps, %ld帧\n", info.width, info.height, src_fps, info.total_frames);

    if (play_only) {
        printf("模式: 不转码，直接从源播放\n");
    } else {
        Profile prof;
        memset(&prof, 0, sizeof prof);
        if (!choose_profile(quality, (int)lround(src_fps), &prof)) {
            fprintf(stderr, "错误: 没有可用的视频编码器，或质量参数无效\n");
            return 1;
        }
        printf("性能诊断: %d 核 / %ld MB 内存 / 预算 %.2f CPU秒每视频秒\n", cpu_cores(),
               total_memory_mb(), cpu_cores() * CPU_BUDGET_RATIO);

        int ok = convert(input, output, cols, &prof, &info);

        /* 硬件编码器可能"成功"却漏帧，抽查最后一秒；不合格就当失败走软件重试 */
        if (ok && prof.enc->hardware && !output_tail_ok(output, &prof)) {
            fprintf(stderr, "硬件编码的输出缺帧，改用软件编码重做一次\n");
            ok = 0;
        }

        /* 硬件编码器在真机上仍可能中途失败（驱动/会话被占用），自动退回软件再试一次 */
        if (!ok && prof.enc->hardware) {
            fprintf(stderr, "硬件编码 %s 失败，改用软件编码重试一次\n", prof.enc->codec);
            const EncoderCand *sw = NULL;
            for (int i = 0; i < ENCODER_COUNT && !sw; ++i) {
                if (ENCODERS[i].hardware || !encoder_listed(i)) {
                    continue;
                }
                if (encoder_works(&ENCODERS[i], prof.width, prof.height, prof.fps)) {
                    sw = &ENCODERS[i];
                }
            }
            if (sw) {
                prof.enc = sw;
                remove(output);
                ok = convert(input, output, cols, &prof, &info);
            }
        }

        if (!ok) {
            if (output_is_temp) {
                remove(output);
            }
            fprintf(stderr, "========================================\n");
            fprintf(stderr, "转换失败!\n");
            fprintf(stderr, "========================================\n");
            free(temp_out);
            return 1;
        }

        /* libx264 已经通过 -x264-params 写全了色标，其它编码器补一次 */
        if (strcmp(prof.enc->codec, "libx264") != 0) {
            if (stamp_bt709(output)) {
                printf("色标: 已补写 BT.709（该编码器本身不写 color_primaries）\n");
            }
        }
        printf("========================================\n");
        printf("成功创建彩色ASCII视频!\n");
        printf("输出文件: %s%s\n", output, output_is_temp ? "（临时，播完自动删除）" : "");
        printf("========================================\n");
    }

    if (want_play) {
        /* 播放不需要编码，直接按终端网格重走一遍源；帧率封到 24，终端刷不过来 */
        int play_fps = (int)lround(src_fps);
        play_fps = play_fps < 1 ? 1 : (play_fps > 24 ? 24 : play_fps);
        printf("在终端播放（按 q 退出%s）...\n", mute ? "，静音" : "");
        fflush(stdout);
        play_terminal(input, cols, &info, play_fps, !mute);

        if (output_is_temp) {
            if (remove(output) == 0) {
                printf("已删除临时文件: %s\n", output);
            } else {
                fprintf(stderr, "临时文件删除失败，请手动清理: %s\n", output);
            }
        }
    }

    free(temp_out);
    return 0;
}
