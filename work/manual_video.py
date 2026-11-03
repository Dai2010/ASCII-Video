from pathlib import Path
from edit_helpers import replace_function
p=Path('src/video.c');s=p.read_text().replace('static void ratio_dims(int ratio, int src_w, int src_h, int base_h, int *w,\n\t\t       int *h)', 'static void ratio_dims(int ratio, const struct video_info *source,\n                        int base_h, int *w, int *h)').replace('(double)src_w / (double)src_h','(double)source->width / source->height').replace('ratio_dims(ratio, vi->width, vi->height, vi->height & ~1, &w,\n\t\t\t   &h)', 'ratio_dims(ratio, vi, vi->height & ~1, &w, &h)').replace('ratio_dims(ratio, vi->width, vi->height, p->height, &w, &h)','ratio_dims(ratio, vi, p->height, &w, &h)');p.write_text(s)
replace_function('src/video.c','miku_probe_video','''
/* ffprobe 中间信息独立于最终尺寸，用于帧率回退和旋转修正。 */
struct probe_metadata {
        long average_num;
        long average_den;
        long rate_num;
        long rate_den;
        double duration;
        double rotation;
};

/*
 * 只解释所请求的 key=value 行。未知或损坏的字段留给最终校验拒绝。
 * line 是可修改缓冲区；键和值均不逃出本次调用。
 */
static void probe_line(char *line, struct video_info *info,
                         struct probe_metadata *metadata)
{
        char *value = strchr(line, '=');

        if (!value)
                return;
        *value++ = '\\0';
        value[strcspn(value, "\\r\\n")] = '\\0';
        if (strcmp(line, "width") == 0)
                info->width = atoi(value);
        else if (strcmp(line, "height") == 0)
                info->height = atoi(value);
        else if (strcmp(line, "avg_frame_rate") == 0)
                parse_rate(value, &metadata->average_num, &metadata->average_den);
        else if (strcmp(line, "r_frame_rate") == 0)
                parse_rate(value, &metadata->rate_num, &metadata->rate_den);
        else if (strcmp(line, "nb_frames") == 0)
                info->total_frames = atol(value);
        else if (strcmp(line, "duration") == 0)
                metadata->duration = atof(value);
        else if (strcmp(line, "rotation") == 0 || strcmp(line, "TAG:rotate") == 0)
                metadata->rotation = atof(value);
}

/*
 * ffmpeg 自动旋转画面，所以 90/270 度元数据必须交换尺寸。
 * 帧率依次使用平均值、基础值和 30fps；帧数未知时由时长估算。
 */
static void probe_finalize(struct video_info *info,
                             const struct probe_metadata *metadata)
{
        int temporary;

        if ((int)lround(fabs(metadata->rotation)) % 180 == 90) {
                temporary = info->width;
                info->width = info->height;
                info->height = temporary;
        }
        if (metadata->average_num > 0) {
                info->fps_num = metadata->average_num;
                info->fps_den = metadata->average_den;
        } else if (metadata->rate_num > 0) {
                info->fps_num = metadata->rate_num;
                info->fps_den = metadata->rate_den;
        } else {
                info->fps_num = 30;
                info->fps_den = 1;
        }
        if (info->total_frames <= 0 && metadata->duration > 0)
                info->total_frames = (long)(metadata->duration * info->fps_num /
                                           info->fps_den + 0.5);
}

/*
 * 探测第一条视频流；path 借用，info 接收尺寸、有理数帧率和估计帧数。
 * 路径先 shell 转义；管道及命令字符串均在本函数内释放。
 * 当前为 1 成功、0 无有效视频信息；不留下动态内存或未回收子进程。
 */
int miku_probe_video(const char *path, struct video_info *info)
{
        struct probe_metadata metadata = {0};
        char *quoted;
        char *command;
        FILE *pipe;
        char line[256];
        int status;

        quoted = miku_shell_quote(path);
        if (!quoted)
                return 0;
        command = miku_xasprintf(
                "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration:stream_side_data=rotation:stream_tags=rotate -of default=noprint_wrappers=1 %s 2>/dev/null",
                quoted);
        free(quoted);
        if (!command)
                return 0;
        pipe = popen(command, "r");
        free(command);
        if (!pipe)
                return 0;
        memset(info, 0, sizeof(*info));
        while (fgets(line, sizeof(line), pipe))
                probe_line(line, info, &metadata);
        status = pclose(pipe);
        if (!miku_exit_ok(status) || info->width <= 0 || info->height <= 0)
                return 0;
        probe_finalize(info, &metadata);
        return 1;
}
''')
