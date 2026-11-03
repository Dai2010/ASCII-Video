from pathlib import Path
head='/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n\n'
Path('include/convert.h').write_text(head+'''#ifndef MIKU_CONVERT_H
#define MIKU_CONVERT_H

#include "types.h"

/*
 * 所有路径、质量字符串和回调均为借用，整次转换期间必须有效。
 * cancelled 可为 NULL；非空时由界面回调置位，禁止取消后自动重试。
 */
struct convert_request {
        const char *input;
        const char *output;
        const char *quality;
        const struct convert_hooks *hooks;
        int *cancelled;
        int cols;
        int style;
        int ratio;
        int fps;
};

int miku_run_convert(const struct convert_request *request,
                       struct convert_outcome *outcome);

#endif /* MIKU_CONVERT_H */
''')
Path('src/convert.c').write_text(head+'''#include "convert.h"
#include "encoder.h"
#include "glyph.h"
#include "layout.h"
#include "render.h"
#include "util.h"
#include "video.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 转换任务独占所有管道和缓冲区；request、profile、info 为借用。
 * 各阶段只取得资源，统一由 convert_release 逆序收尾，失败可随时退出。
 */
struct convert_job {
        const struct convert_request *request;
        const struct profile *profile;
        const struct video_info *info;
        struct glyph_atlas atlas;
        struct layout layout;
        struct render_grid grid;
        FILE *decoder;
        FILE *encoder;
        char *decode_command;
        char *encode_command;
        uint8_t *cells;
        uint8_t *canvas;
        size_t cell_bytes;
        size_t canvas_bytes;
        int64_t source_index;
        int64_t output_frames;
        double start;
};

/* CLI 才输出提示，界面提供 hooks 时避免破坏整屏重绘。 */
static void say(const struct convert_hooks *hooks, const char *fmt, ...)
{
        va_list args;

        if (hooks)
                return;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
}

/*
 * 生成编码命令，quoted_input/output 已经完成 shell 转义。
 * 返回堆字符串或 NULL；调用方持有并最终 free，不保留借用参数。
 * 输入设备参数和候选编码器探测保持一致，音轨不存在时允许静音输出。
 */
static char *encode_command(const struct convert_job *job,
                             const char *quoted_input,
                             const char *quoted_output)
{
        const struct profile *p = job->profile;
        const char *prefix = miku_cand_input_prefix(p->enc);
        char *args;
        char *command;

        args = miku_encoder_args(p->enc, p->width, p->height, p->fps);
        if (!args)
                return NULL;
        command = miku_xasprintf(
                "ffmpeg -y -v error %s -f rawvideo -pix_fmt rgb24 -s %dx%d -framerate %d -i pipe:0 -i %s -map 0:v:0 -map '1:a:0?' -vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p%s' -r %d -c:v %s %s -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a aac -b:a 192k -movflags +faststart -shortest %s",
                prefix ? prefix : "", p->width, p->height, p->fps, quoted_input,
                p->enc->pipe_filter ? p->enc->pipe_filter : "", p->fps,
                p->enc->codec, args, quoted_output);
        free(args);
        return command;
}

/* 命令的所有权交给 job；临时转义路径始终在函数内释放。 */
static int convert_commands(struct convert_job *job, int sample_rows)
{
        char *input = miku_shell_quote(job->request->input);
        char *output = miku_shell_quote(job->request->output);
        int ret = -ENOMEM;

        if (input && output) {
                job->decode_command = miku_xasprintf(
                        "ffmpeg -nostdin -v error -i %s -an -sn -vf 'fps=%ld/%ld,scale=%d:%d:flags=area' -f rawvideo -pix_fmt rgb24 pipe:1",
                        input, job->info->fps_num, job->info->fps_den,
                        job->layout.cols, sample_rows);
                job->encode_command = encode_command(job, input, output);
                if (job->decode_command && job->encode_command)
                        ret = 0;
        }
        free(input);
        free(output);
        return ret;
}

/*
 * 分配 RGB24 采样和整帧画布，画布初始全黑，网格以外始终是黑边。
 * 只把网格左上角交给渲染器；stride 保留整张画布的宽度。
 */
static int convert_buffers(struct convert_job *job, int sample_rows)
{
        const struct layout *layout = &job->layout;
        struct render_grid *grid = &job->grid;

        job->cell_bytes = (size_t)layout->cols * sample_rows * 3;
        grid->stride = (size_t)job->profile->width * 3;
        job->canvas_bytes = grid->stride * job->profile->height;
        job->cells = malloc(job->cell_bytes);
        job->canvas = calloc(job->canvas_bytes, 1);
        if (!job->cells || !job->canvas)
                return -ENOMEM;
        grid->cells = job->cells;
        grid->output = job->canvas + layout->off_y * grid->stride +
                       layout->off_x * 3;
        grid->cols = layout->cols;
        grid->rows = layout->rows;
        grid->cell_w = layout->cell_w;
        grid->cell_h = layout->cell_h;
        return 0;
}

/*
 * 初始化完成前不进入帧循环；每一阶段错误直接返回，已取得资源保留给收尾。
 * ffmpeg/ffprobe 缺失返回 -ENOENT，分配失败返回 -ENOMEM。
 */
static int convert_prepare(struct convert_job *job)
{
        int rows;
        int ret;

        if (!miku_command_exists("ffmpeg") || !miku_command_exists("ffprobe")) {
                fprintf(stderr, "未找到 ffmpeg / ffprobe，请先安装 ffmpeg\\n");
                return -ENOENT;
        }
        miku_compute_layout(job->profile, job->request->cols,
                            job->info->width, job->info->height, &job->layout);
        miku_build_gamma_lut();
        if (job->request->style == STYLE_CHAR) {
                ret = miku_atlas_build(&job->atlas, job->layout.cell_w,
                                       job->layout.cell_h);
                if (ret)
                        return ret;
        }
        rows = job->layout.rows;
        if (job->request->style == STYLE_BLOCK)
                rows *= 2;
        ret = convert_commands(job, rows);
        if (ret)
                return ret;
        ret = convert_buffers(job, rows);
        if (ret)
                return ret;
        job->decoder = popen(job->decode_command, "r");
        if (!job->decoder)
                return -errno;
        job->encoder = popen(job->encode_command, "w");
        if (!job->encoder)
                return -errno;
        job->start = miku_now_seconds();
        return 0;
}

/* 帧时长按有理数累计，避免 30000/1001 等帧率长期产生舍入漂移。 */
static int convert_frame(struct convert_job *job)
{
        const struct video_info *info = job->info;
        int fps = job->profile->fps;
        int64_t first;
        int64_t last;
        int64_t repeat;
        int64_t i;

        first = job->source_index * fps * info->fps_den / info->fps_num;
        last = (job->source_index + 1) * fps * info->fps_den / info->fps_num;
        repeat = last - first;
        job->source_index++;
        if (repeat <= 0)
                return 0;
        if (job->request->style == STYLE_BLOCK)
                miku_render_block_cells(&job->grid);
        else
                miku_render_frame(&job->atlas, &job->grid);
        for (i = 0; i < repeat; i++) {
                if (fwrite(job->canvas, 1, job->canvas_bytes, job->encoder) !=
                    job->canvas_bytes)
                        return -EIO;
                job->output_frames++;
        }
        return 0;
}

/*
 * 进度回调同步执行，不转移 ud 所有权。未知总帧数时 eta=-1。
 * should_abort 返回非零即终止，回调必须在本次调用结束前返回。
 */
static int convert_progress(struct convert_job *job)
{
        const struct convert_hooks *hooks = job->request->hooks;
        double elapsed = miku_now_seconds() - job->start;
        double eta = -1;
        long total = job->info->total_frames;

        if (total > 0 && job->source_index > 0)
                eta = miku_dmax(0, elapsed / job->source_index *
                                  (total - job->source_index));
        if (hooks) {
                if (hooks->on_progress)
                        hooks->on_progress(hooks->ud, job->source_index, total,
                                           job->output_frames, elapsed, eta);
                if (hooks->should_abort && hooks->should_abort(hooks->ud))
                        return -ECANCELED;
                return 0;
        }
        if (job->source_index % 30 != 0)
                return 0;
        printf("\\r进度: %lld/%ld", (long long)job->source_index, total);
        if (total > 0)
                printf(" 帧 (%.1f%%)  预计剩余 %.0f 秒   ",
                       job->source_index * 100.0 / total, eta);
        fflush(stdout);
        return 0;
}

/*
 * 完整读取一个采样帧才处理。关闭编码器的写端后等待它完成音频合并。
 * 正常返回 0；取消、空视频、解码错误或编码错误返回负 errno。
 */
static int convert_frames(struct convert_job *job)
{
        int ret = 0;
        int decoder_status;
        int encoder_status;

        while (fread(job->cells, 1, job->cell_bytes, job->decoder) ==
               job->cell_bytes) {
                ret = convert_frame(job);
                if (ret)
                        return ret;
                ret = convert_progress(job);
                if (ret)
                        return ret;
        }
        decoder_status = pclose(job->decoder);
        job->decoder = NULL;
        encoder_status = pclose(job->encoder);
        job->encoder = NULL;
        if (!miku_exit_ok(encoder_status) || !job->source_index)
                return -EIO;
        if (!miku_exit_ok(decoder_status))
                fprintf(stderr, "警告: 解码过程中出现错误，输出视频可能不完整\\n");
        return 0;
}

/*
 * 释放顺序与取得顺序相反。NULL 表示该阶段尚未取得资源或已经交还。
 * 调用方必须等待管道收尾后再删除失败产物，防止编码子进程继续写入。
 */
static void convert_release(struct convert_job *job)
{
        if (job->encoder)
                pclose(job->encoder);
        if (job->decoder)
                pclose(job->decoder);
        free(job->canvas);
        free(job->cells);
        free(job->encode_command);
        free(job->decode_command);
        free(job->atlas.masks);
}

/*
 * 执行一次编码尝试，不自行重试。request/profile/info 均借用。
 * 本函数拥有 job 中全部资源，所有退出路径都经过 convert_release。
 */
static int convert(const struct convert_request *request,
                     const struct profile *profile, const struct video_info *info)
{
        struct convert_job job = {
                .request = request, .profile = profile, .info = info,
        };
        int ret;

        ret = convert_prepare(&job);
        if (!ret) {
                say(request->hooks, "输出规格: %dx%d @ %dfps（%s）\\n",
                    profile->width, profile->height, profile->fps,
                    profile->enc->codec);
                ret = convert_frames(&job);
        }
        convert_release(&job);
        if (ret) {
                remove(request->output);
                fprintf(stderr, "转换失败: %s\\n", strerror(-ret));
                return ret;
        }
        say(request->hooks, "转换完成! 源帧数: %lld，输出帧数: %lld (%dfps)\\n",
            (long long)job.source_index, (long long)job.output_frames,
            profile->fps);
        say(request->hooks, "输出文件: %s\\n", request->output);
        return 0;
}

/* 软件重试也实际编码探测，不能只根据 ffmpeg 列出的名字判断可用性。 */
static const struct encoder_candidate *software_encoder(const struct profile *p)
{
        const struct encoder_candidate *candidate;
        int i;

        for (i = 0; i < ENCODER_COUNT; i++) {
                candidate = &miku_encoders[i];
                if (candidate->hardware || !miku_encoder_listed(i))
                        continue;
                if (miku_encoder_works(candidate, p->width, p->height, p->fps))
                        return candidate;
        }
        return NULL;
}

/*
 * 仅硬件编码失败时退回软件。用户取消保持取消结果，不偷偷再次转码。
 * out 的编码器指针借用静态候选表，软件重试会同步更新它。
 */
static int convert_retry(const struct convert_request *request,
                          struct convert_outcome *out, int ret)
{
        const struct encoder_candidate *software;

        if (!ret || !out->prof.enc->hardware || ret == -ECANCELED)
                return ret;
        if (request->cancelled && *request->cancelled)
                return ret;
        software = software_encoder(&out->prof);
        if (!software)
                return ret;
        fprintf(stderr, "硬件编码 %s 失败，改用软件编码重试一次\\n",
                out->prof.enc->codec);
        out->prof.enc = software;
        return convert(request, &out->prof, &out->info);
}

/*
 * 探测、选档、转换、末段检查和软件回退的唯一入口。
 * request 借用，out 由调用方提供并接收实际采用的视频规格。
 * 成功返回 0；参数/探测失败返回 -EINVAL，执行失败返回对应负 errno。
 * 转换可能同步回调 hooks，并使用 ffmpeg 子进程；终端界面须串行调用。
 */
int miku_run_convert(const struct convert_request *request,
                       struct convert_outcome *out)
{
        int ret;

        if (!miku_probe_video(request->input, &out->info))
                return -EINVAL;
        memset(&out->prof, 0, sizeof(out->prof));
        if (!miku_choose_profile_ex(request->quality, request->ratio,
                                    request->fps, &out->info, &out->prof))
                return -EINVAL;
        ret = convert(request, &out->prof, &out->info);
        if (!ret && out->prof.enc->hardware &&
            !miku_output_tail_ok(request->output, &out->prof))
                ret = -EIO;
        ret = convert_retry(request, out, ret);
        if (ret)
                return ret;
        if (strcmp(out->prof.enc->codec, "libx264") != 0 &&
            miku_stamp_bt709(request->output))
                say(request->hooks, "色标: 已补写 BT.709\\n");
        return 0;
}
''')
# 只替换两个已核对的调用点；它们仍使用本地 ok 真假值。
p=Path('src/ui_progress.c');s=p.read_text();a=s.index('const int ok = miku_run_convert(');b=s.index(';',a)
s=s[:a]+'''const int ok = miku_run_convert(&(struct convert_request) {
.input = st->input, .output = st->output, .cols = cols,
.quality = quality, .hooks = &hooks, .cancelled = &pg.cancelled,
.style = st->style, .ratio = st->ratio, .fps = st->fps,
}, res) == 0'''+s[b:];p.write_text(s)
p=Path('src/main.c');s=p.read_text();a=s.index('miku_run_convert(');b=s.index(';',a)
# 原调用位于 const int ok 赋值中。
s=s[:a]+'''miku_run_convert(&(struct convert_request) {
.input = input, .output = output, .cols = cols, .quality = quality,
.style = style, .ratio = 0, .fps = 0,
}, &res) == 0'''+s[b:];p.write_text(s)
