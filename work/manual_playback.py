from pathlib import Path
head = '/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n\n'
Path('include/terminal_frame.h').write_text(head+'''#ifndef MIKU_TERMINAL_FRAME_H
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

int miku_terminal_frame_build(struct strbuf *buffer,
                              const struct terminal_frame *frame);

#endif /* MIKU_TERMINAL_FRAME_H */
''')
Path('src/terminal_frame.c').write_text(head+'''#include "terminal_frame.h"
#include "render.h"
#include "strbuf.h"
#include "util.h"

#include <errno.h>
#include <string.h>

/*
 * 只在颜色改变时追加 ANSI 前景或背景序列，减少终端输出量。
 * previous 初始值应为 {-1,-1,-1}，仅在追加成功时更新。
 * buffer 的内存仍归调用方；失败返回 -ENOMEM，已追加内容可以直接丢弃。
 */
static int append_color(struct strbuf *buffer, struct rgb color,
                         struct rgb *previous, int background)
{
        int ok;

        if (color.r == previous->r && color.g == previous->g &&
            color.b == previous->b)
                return 0;
        ok = miku_sb_addf(buffer, "\\x1b[%d;2;%d;%d;%dm",
                          background ? 48 : 38, color.r, color.g, color.b);
        if (!ok)
                return -ENOMEM;
        *previous = color;
        return 0;
}

/*
 * 追加一个字符格。色块使用 UTF-8 的 ▀，上采样为前色、下采样为背景色。
 * 单字符风格只使用前色和亮度图集；三个 UTF-8 字节必须整体写入。
 */
static int append_cell(struct strbuf *buffer,
                        const struct terminal_frame *frame,
                        const uint8_t *pixel, struct rgb colors[2])
{
        int level;
        int ret;

        ret = append_color(buffer, miku_cell_color(pixel), &colors[0], 0);
        if (ret)
                return ret;
        if (frame->style == STYLE_BLOCK) {
                ret = append_color(buffer,
                                    miku_cell_color(pixel + frame->cols * 3),
                                    &colors[1], 1);
                if (ret)
                        return ret;
                if (!miku_sb_reserve(buffer, 3))
                        return -ENOMEM;
                memcpy(buffer->buf + buffer->len, "\\xe2\\x96\\x80", 3);
                buffer->len += 3;
                return 0;
        }
        if (!miku_sb_reserve(buffer, 1))
                return -ENOMEM;
        level = miku_pixel_level(pixel);
        buffer->buf[buffer->len++] =
                frame->atlas->chars[frame->atlas->lut[miku_gamma_lut[level]]];
        return 0;
}

/* 每行末复位颜色，因此每行的颜色缓存必须重新初始化。 */
static int append_row(struct strbuf *buffer,
                       const struct terminal_frame *frame, int row)
{
        struct rgb colors[2] = {{-1, -1, -1}, {-1, -1, -1}};
        const uint8_t *pixel;
        int sample_row = frame->style == STYLE_BLOCK ? row * 2 : row;
        int x;
        int ret;

        for (x = 0; x < frame->cols; x++) {
                pixel = frame->cells +
                        ((size_t)sample_row * frame->cols + x) * 3;
                ret = append_cell(buffer, frame, pixel, colors);
                if (ret)
                        return ret;
        }
        return miku_sb_addf(buffer, "\\x1b[0m\\r\\n") ? 0 : -ENOMEM;
}

/*
 * 重建一整帧终端输出，成功返回 0，扩容失败返回 -ENOMEM。
 * 原 buffer 可复用：只重置 len，保留容量。函数不执行终端 I/O，
 * 使播放时钟与昂贵的字符拼接分离；失败时调用方不得输出半帧。
 */
int miku_terminal_frame_build(struct strbuf *buffer,
                              const struct terminal_frame *frame)
{
        int y;
        int ret;

        buffer->len = 0;
        if (!miku_sb_addf(buffer, "\\x1b[H"))
                return -ENOMEM;
        for (y = 0; y < frame->rows; y++) {
                ret = append_row(buffer, frame, y);
                if (ret)
                        return ret;
        }
        return 0;
}
''')
p=Path('src/playback.c')
s=p.read_text();start=s.index('int miku_play_terminal(')
p.write_text(s[:start]+'''/*
 * 单次播放状态：所有堆内存和解码管道均在调用结束前释放。
 * 音频是进程级单实例，终端原始模式同样不可重入。
 */
struct playback {
        struct glyph_atlas atlas;
        struct terminal_frame frame;
        struct strbuf buffer;
        FILE *decoder;
        uint8_t *cells;
        size_t cell_bytes;
        int fps;
        int audio;
        double origin;
        char socket_path[256];
};

/*
 * 按终端宽高夹紧网格，保留一行余量。终端字符高宽比约为 2:1，
 * 因此纵向格数为列数除视频宽高比再除以 2。三次夹紧沿用旧版布局。
 */
static void playback_grid(struct terminal_frame *frame, int wanted,
                           const struct video_info *info)
{
        int cols = miku_terminal_columns();
        int rows = miku_terminal_rows();
        int max_rows;
        int pass;
        double aspect = (double)info->width / info->height;

        if (cols <= 0)
                cols = 80;
        if (rows <= 0)
                rows = 24;
        max_rows = rows > 2 ? rows - 1 : 1;
        frame->cols = wanted > 0 ? wanted : cols;
        if (frame->cols > cols)
                frame->cols = cols;
        for (pass = 0; pass < 3; pass++) {
                rows = (int)lround(frame->cols / aspect * 0.5);
                if (rows <= max_rows)
                        break;
                frame->cols = (int)lround(max_rows * aspect * 2.0);
                if (frame->cols > cols)
                        frame->cols = cols;
        }
        if (frame->cols < 8)
                frame->cols = 8;
        frame->rows = (int)lround(frame->cols / aspect * 0.5);
        if (frame->rows < 1)
                frame->rows = 1;
        if (frame->rows > max_rows)
                frame->rows = max_rows;
}

/*
 * 准备采样缓冲和只读解码管道。文件名先独立 shell 转义。
 * 失败返回负 errno；已取得资源保留在 play 中，由统一收尾释放。
 */
static int playback_decoder(struct playback *play, const char *input)
{
        char *quoted;
        char *command;
        int rows = play->frame.rows;
        int ret = 0;

        if (play->frame.style == STYLE_BLOCK)
                rows *= 2;
        play->cell_bytes = (size_t)play->frame.cols * rows * 3;
        play->cells = malloc(play->cell_bytes);
        if (!play->cells)
                return -ENOMEM;
        play->frame.cells = play->cells;
        quoted = miku_shell_quote(input);
        if (!quoted)
                return -ENOMEM;
        command = miku_xasprintf(
                "ffmpeg -nostdin -v error -i %s -an -sn -vf 'fps=%d,scale=%d:%d:flags=area' -f rawvideo -pix_fmt rgb24 pipe:1 2>/dev/null",
                quoted, play->fps, play->frame.cols, rows);
        free(quoted);
        if (!command)
                return -ENOMEM;
        play->decoder = popen(command, "r");
        if (!play->decoder)
                ret = -errno;
        free(command);
        return ret;
}

/*
 * 等待 mpv 的位置开始前进后建立单调时钟原点。
 * 每次取样均非阻塞，最多等待 80*50ms；没有 IPC 的 ffplay 不等待。
 * 信号只设置停止标志，终端和子进程的清理由正常执行路径完成。
 */
static void playback_clock(struct playback *play, const char *input,
                            int with_audio)
{
        struct timespec delay = {0, 50000000};
        double position;
        int i;

        snprintf(play->socket_path, sizeof(play->socket_path),
                 "%s/miku-audio-%ld.sock", miku_temp_dir(), (long)getpid());
        remove(play->socket_path);
        play->audio = with_audio && miku_has_audio_stream(input) &&
                      miku_start_audio(input, play->socket_path);
        play->origin = miku_now_seconds();
        if (!play->audio)
                return;
        snprintf(miku_g_audio_sock, sizeof(miku_g_audio_sock), "%s",
                 play->socket_path);
        atexit(miku_stop_audio);
        if (!miku_command_exists("mpv"))
                return;
        for (i = 0; i < 80 && !g_play_stop; i++) {
                position = miku_audio_time_pos();
                if (position >= 0.12) {
                        play->origin = miku_now_seconds() - position;
                        break;
                }
                nanosleep(&delay, NULL);
        }
}

/*
 * 顺序解码，过期帧跳过字符拼接；有效帧等到其显示时间后整体输出。
 * 音频查询没有完整回复时沿用单调时钟，绝不等待 IPC。
 * q 和信号均正常结束；输出错误返回 -EIO，内存错误返回 -ENOMEM。
 */
static int playback_frames(struct playback *play)
{
        int64_t frame = 0;
        double position;
        int ret;

        while (!g_play_stop &&
               fread(play->cells, 1, play->cell_bytes, play->decoder) ==
               play->cell_bytes) {
                position = play->audio ? miku_audio_time_pos() : -1.0;
                if (position >= 0)
                        play->origin = miku_now_seconds() - position;
                if ((double)(frame + 1) / play->fps >=
                    miku_now_seconds() - play->origin) {
                        ret = miku_terminal_frame_build(&play->buffer,
                                                        &play->frame);
                        if (ret)
                                return ret;
                        miku_sleep_until(play->origin + (double)frame / play->fps);
                        if (fwrite(play->buffer.buf, 1, play->buffer.len, stdout) !=
                            play->buffer.len || fflush(stdout) != 0)
                                return -EIO;
                }
                frame++;
                if (play_poll_key() == 'q')
                        break;
        }
        return ferror(play->decoder) ? -EIO : 0;
}

/* 无论初始化停在哪一步都可调用；每个指针仅释放本次播放持有的资源。 */
static void playback_release(struct playback *play)
{
        miku_stop_audio();
        miku_audio_ipc_close();
        if (play->socket_path[0])
                remove(play->socket_path);
        play_restore_term();
        if (play->decoder)
                pclose(play->decoder);
        free(play->atlas.masks);
        free(play->cells);
        free(play->buffer.buf);
}

/*
 * 播放 options 指定的输入；info 必须来自已成功的视频探测。
 * 两者均借用，调用期间有效。初始化失败和播放失败均返回负 errno，
 * 正常播完或按 q 返回 0。同步恢复终端，避免把原始模式留给下一界面。
 * 函数使用终端与音频单实例，不允许并发或递归调用。
 */
int miku_play_terminal(const struct playback_options *options,
                        const struct video_info *info)
{
        struct playback play = {0};
        int ret;

        play.fps = options->fps;
        play.frame.style = options->style;
        play.frame.atlas = &play.atlas;
        playback_grid(&play.frame, options->cols, info);
        miku_build_gamma_lut();
        if (options->style == STYLE_CHAR &&
            !miku_atlas_build(&play.atlas, 8, 16))
                return -ENOMEM;
        ret = playback_decoder(&play, options->input);
        if (!ret) {
                g_play_stop = 0;
                signal(SIGINT, play_on_signal);
                signal(SIGTERM, play_on_signal);
                play_raw_mode();
                atexit(play_restore_term);
                fputs("\\x1b[?25l\\x1b[2J", stdout);
                playback_clock(&play, options->input, options->with_audio);
                ret = playback_frames(&play);
                fputs("\\x1b[0m\\r\\n", stdout);
        }
        playback_release(&play);
        return ret;
}
''')
s=p.read_text().replace('#include "terminal.h"','#include "terminal.h"\n#include "terminal_frame.h"');p.write_text(s)
Path('include/playback.h').write_text(head+'''#ifndef MIKU_PLAYBACK_H
#define MIKU_PLAYBACK_H

#include "types.h"

/* input 借用路径；fps 为正数，cols 为 0 表示跟随终端。 */
struct playback_options {
        const char *input;
        int cols;
        int fps;
        int with_audio;
        int style;
};

int miku_play_terminal(const struct playback_options *options,
                        const struct video_info *info);

#endif /* MIKU_PLAYBACK_H */
''')
p=Path('src/ui_progress.c');s=p.read_text().replace('miku_play_terminal(st->input, cols, vp, fps, !st->mute, st->style);','miku_play_terminal(&(struct playback_options) {\n.input = st->input, .cols = cols, .fps = fps,\n.with_audio = !st->mute, .style = st->style,\n}, vp);');p.write_text(s)
p=Path('src/main.c');s=p.read_text();import re
s=re.sub(r'miku_play_terminal\(input, cols, &res.info, play_fps, !mute,\s*style\);','miku_play_terminal(&(struct playback_options) {\n.input = input, .cols = cols, .fps = play_fps,\n.with_audio = !mute, .style = style,\n}, &res.info);',s);p.write_text(s)
# 先接通新渲染接口，转换管线的手工拆分随后进行。
p=Path('src/convert.c');s=p.read_text();a=s.index('\t\t\tif (style == STYLE_BLOCK)');b=s.index('\n\t\t\tfor (',a)
s=s[:a]+'''                        struct render_grid grid = {
                                .cells = cells, .output = grid_origin,
                                .stride = canvas_stride, .cols = L.cols,
                                .rows = L.rows, .cell_w = L.cell_w,
                                .cell_h = L.cell_h,
                        };
                        if (style == STYLE_BLOCK)
                                miku_render_block_cells(&grid);
                        else
                                miku_render_frame(&atlas, &grid);'''+s[b:];p.write_text(s)
