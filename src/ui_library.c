/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 已有视频库：管理已转换视频的规格命名、按「视频名+规格」去重持久化，以及列表展示与播放。
 */

#include "ui_library.h"
#include "playback.h"
#include "strbuf.h"
#include "terminal.h"
#include "ui_config.h"
#include "ui_progress.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "util.h"
#include "video.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 单条已有视频记录：保存显示名、输出与源路径，以及去重和播放所需的规格。 */
struct library_item {
	char name[128];
	char output[UI_PATH_MAX];
	char source[UI_PATH_MAX];
	int width;
	int height;
	int fps;
	int style;
	int cols;
	long long bytes;
};

/* 已有视频列表状态：由单次记录或页面循环独占持有并释放。 */
struct library_list {
	struct library_item items[UI_LIBRARY_MAX];
	int count;
	int selected;
	int top;
};

/*
 * ui_lib_base_name - 提取路径的文件名并剥离扩展名及已知风格后缀，得到视频名。
 *
 * @path: 借用的输入或输出路径，不转移所有权。
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只写入 dst。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_base_name(const char *path, char *dst, size_t cap)
{
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	const char *dot = strrchr(base, '.');
	size_t len = dot ? (size_t)(dot - base) : strlen(base);
	char *tag;

	if (len >= cap)
		len = cap - 1;
	memcpy(dst, base, len);
	dst[len] = '\0';
	tag = strstr(dst, "-ascii");
	if (!tag)
		tag = strstr(dst, "-block");
	if (tag && tag > dst)
		*tag = '\0';
}

/*
 * ui_lib_same_spec - 判断两条记录是否属于同名且同规格（分辨率、帧率、风格一致）。
 *
 * @a: 第一条待比较的视频记录，仅借用。
 * @b: 第二条待比较的视频记录，仅借用。
 *
 * 返回：true 表示同视频同规格，false 表示不同视频或不同规格。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static bool ui_lib_same_spec(const struct library_item *a,
			     const struct library_item *b)
{
	return strcmp(a->name, b->name) == 0 && a->width == b->width &&
	       a->height == b->height && a->fps == b->fps &&
	       a->style == b->style;
}

/*
 * ui_lib_format_path - 在基础路径扩展名前插入风格与分辨率帧率后缀。
 *
 * @base_path: 借用的原始路径，允许与 out 指向同一缓冲。
 * @style: STYLE_CHAR 或 STYLE_BLOCK。
 * @prof: 已选定的目标输出规格，仅借用。
 * @out: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只写入 out。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_format_path(const char *base_path, int style,
			       const struct profile *prof, char *out,
			       size_t cap)
{
	const char *tag = style == STYLE_BLOCK ? "block" : "ascii";
	const char *dot = strrchr(base_path, '.');
	const char *slash = strrchr(base_path, '/');
	size_t keep = (dot && (!slash || dot > slash))
			      ? (size_t)(dot - base_path)
			      : strlen(base_path);
	char prefix[UI_PATH_MAX];

	/* 预留 48 字节给风格、分辨率、帧率后缀与扩展名，避免截断 */
	if (keep + 48 >= cap)
		keep = cap > 48 ? cap - 48 : 0;
	memcpy(prefix, base_path, keep);
	prefix[keep] = '\0';
	snprintf(out, cap, "%.*s-%s-%dx%d@%d.mp4", (int)keep, prefix, tag,
		 prof->width, prof->height, prof->fps);
}

/*
 * ui_lib_ensure_mp4 - 手动输入输出名未带扩展名时补齐 .mp4，避免 ffmpeg 报错。
 *
 * @path: 调用方持有的输出路径缓冲，原地追加扩展名。
 * @cap: 路径缓冲容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只更新 path。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_ensure_mp4(char *path, size_t cap)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	size_t len = strlen(path);

	if (dot && (!slash || dot > slash))
		return;
	if (len + 4 < cap)
		memcpy(path + len, ".mp4", 5);
}

/*
 * ui_lib_abs_path - 把路径规范化为绝对路径；解析失败或路径为空时原样复制。
 * 记录一律存绝对路径，配置目录里的列表才能在任意工作目录下找回成品。
 *
 * @path: 借用的输入路径，允许与 dst 指向同一缓冲。
 * @dst: 调用方持有的输出字符数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；只写入 dst。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_abs_path(const char *path, char *dst, size_t cap)
{
	char *full = path[0] ? realpath(path, NULL) : NULL;

	if (full) {
		snprintf(dst, cap, "%s", full);
		free(full);
		return;
	}
	snprintf(dst, cap, "%s", path);
}

/*
 * miku_ui_prepare_output - 在转换开始前根据选定规格生成或调整输出路径。
 * 自动命名时附加风格与分辨率帧率后缀；手动命名且同名异规格文件已存在时，
 * 同样附加规格后缀以避免覆盖旧规格成品。
 *
 * @st: 调用方持有的界面配置，非 const 指针允许原地更新 output，不转移所有权。
 *
 * 返回：无；只更新 st->output 与 st->output_auto。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_prepare_output(struct ui_config *st)
{
	static const char *qnames[5] = {"auto", "src", "high", "mid", "low"};
	const char *q = qnames[(unsigned int)st->quality < 5 ? st->quality : 0];
	struct video_info vi;
	struct profile prof = {0};
	struct video_info exist;
	int exist_fps;

	if (miku_probe_video(st->input, &vi) != 0)
		return;
	if (miku_choose_profile_ex(q, st->ratio, st->fps, &vi, &prof) != 0)
		return;
	if (st->output_auto || !st->output[0]) {
		ui_lib_format_path(st->input, st->style, &prof, st->output,
				   sizeof(st->output));
		st->output_auto = 1;
		return;
	}
	ui_lib_ensure_mp4(st->output, sizeof(st->output));
	if (access(st->output, R_OK) != 0 ||
	    miku_probe_video(st->output, &exist) != 0)
		return;
	exist_fps = (int)lround((double)exist.fps_num / exist.fps_den);
	if (exist.width != prof.width || exist.height != prof.height ||
	    exist_fps != prof.fps) {
		ui_lib_format_path(st->output, st->style, &prof, st->output,
				   sizeof(st->output));
	}
}

/*
 * ui_lib_upsert - 向列表中插入或更新一条记录；同名同规格覆盖旧项，不同规格新增。
 *
 * @list: 调用方持有的已有视频列表，原地更新。
 * @item: 待插入或更新的记录，仅借用。
 *
 * 返回：无；文件不存在或超过容量时忽略。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_upsert(struct library_list *list,
			  const struct library_item *item)
{
	struct stat st;
	int i;

	if (!item->output[0] || stat(item->output, &st) != 0)
		return;
	for (i = 0; i < list->count; ++i) {
		if (ui_lib_same_spec(&list->items[i], item) ||
		    strcmp(list->items[i].output, item->output) == 0) {
			char old_src[UI_PATH_MAX];

			snprintf(old_src, sizeof(old_src), "%s",
				 list->items[i].source);
			list->items[i] = *item;
			if (!item->source[0] && old_src[0])
				snprintf(list->items[i].source,
					 sizeof(list->items[i].source), "%s",
					 old_src);
			list->items[i].bytes = (long long)st.st_size;
			return;
		}
	}
	if (list->count >= UI_LIBRARY_MAX)
		return;
	list->items[list->count] = *item;
	list->items[list->count].bytes = (long long)st.st_size;
	++list->count;
}

/*
 * ui_lib_parse_line - 解析持久化文件中的单行 TSV 记录并加入列表。
 *
 * @list: 调用方持有的已有视频列表，原地更新。
 * @line: 可修改的单行文本缓冲，仅在本次调用内借用。
 *
 * 返回：无；字段不全或规格无效时直接忽略。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_parse_line(struct library_list *list, char *line)
{
	struct library_item item = {0};
	char *fields[8];
	char *p = line;
	int i;

	line[strcspn(line, "\r\n")] = '\0';
	for (i = 0; i < 8; ++i) {
		fields[i] = p;
		p = strchr(p, '\t');
		if (!p && i < 7)
			return;
		if (p)
			*p++ = '\0';
	}
	snprintf(item.name, sizeof(item.name), "%.*s",
		 (int)sizeof(item.name) - 1, fields[0]);
	item.width = atoi(fields[1]);
	item.height = atoi(fields[2]);
	item.fps = atoi(fields[3]);
	item.style = atoi(fields[4]);
	item.cols = atoi(fields[5]);
	snprintf(item.output, sizeof(item.output), "%.*s",
		 (int)sizeof(item.output) - 1, fields[6]);
	snprintf(item.source, sizeof(item.source), "%.*s",
		 (int)sizeof(item.source) - 1, fields[7]);
	if (item.width > 0 && item.height > 0 && item.fps > 0)
		ui_lib_upsert(list, &item);
}

/*
 * ui_lib_load_file - 读取持久化文件中的历史转换记录。
 *
 * @list: 调用方持有的已有视频列表，原地更新。
 *
 * 返回：无；持久化文件不存在时保持空列表。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_load_file(struct library_list *list)
{
	char path[UI_PATH_MAX];
	FILE *f;
	char line[1400];

	snprintf(path, sizeof path, "%s/%s", miku_config_dir(),
		 UI_LIBRARY_FILE);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		ui_lib_parse_line(list, line);
	fclose(f);
}

/*
 * ui_lib_save_file - 将去重后的有效视频列表写回持久化文件。
 *
 * @list: 调用方持有的已有视频列表，仅借用。
 *
 * 返回：无；写入失败时静默保持内存列表可用。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_save_file(const struct library_list *list)
{
	char path[UI_PATH_MAX];
	FILE *f;
	int i;

	snprintf(path, sizeof path, "%s/%s", miku_config_dir(),
		 UI_LIBRARY_FILE);
	f = fopen(path, "w");
	if (!f)
		return;
	for (i = 0; i < list->count; ++i) {
		const struct library_item *it = &list->items[i];

		fprintf(f, "%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n", it->name,
			it->width, it->height, it->fps, it->style, it->cols,
			it->output, it->source);
	}
	fclose(f);
}

/*
 * ui_lib_guess_source - 为扫描到的成品视频尝试匹配同目录下的源视频文件。
 *
 * @base: 已剥离后缀的视频基础名，仅借用。
 * @src: 调用方持有的源路径输出数组。
 * @cap: 输出数组容量，单位字节，包含结尾 NUL 空间。
 *
 * 返回：无；匹配失败时 src 置为空串。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_guess_source(const char *base, char *src, size_t cap)
{
	static const char *exts[4] = {".mp4", ".mkv", ".webm", ".mov"};
	char cand[UI_PATH_MAX];
	int i;

	src[0] = '\0';
	for (i = 0; i < 4; ++i) {
		snprintf(cand, sizeof(cand), "%s%s", base, exts[i]);
		if (access(cand, R_OK) == 0) {
			snprintf(src, cap, "%s", cand);
			return;
		}
	}
}

/*
 * ui_lib_scan_cwd - 扫描当前目录下已存在的 *-ascii*.mp4 与 *-block*.mp4 成品。
 *
 * @list: 调用方持有的已有视频列表，原地合入新发现的成品。
 *
 * 返回：无；只执行目录扫描与去重合入。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_scan_cwd(struct library_list *list)
{
	DIR *d = opendir(".");
	struct dirent *de;
	struct video_info vi;
	struct library_item item;
	const char *dot;

	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		dot = strrchr(de->d_name, '.');
		if (!dot || strcmp(dot, ".mp4") != 0)
			continue;
		if (!strstr(de->d_name, "-ascii") &&
		    !strstr(de->d_name, "-block"))
			continue;
		if (miku_probe_video(de->d_name, &vi) != 0)
			continue;
		memset(&item, 0, sizeof(item));
		ui_lib_base_name(de->d_name, item.name, sizeof(item.name));
		ui_lib_abs_path(de->d_name, item.output, sizeof(item.output));
		ui_lib_guess_source(item.name, item.source,
				    sizeof(item.source));
		ui_lib_abs_path(item.source, item.source, sizeof(item.source));
		item.width = vi.width;
		item.height = vi.height;
		item.fps = (int)lround((double)vi.fps_num / vi.fps_den);
		item.style =
			strstr(de->d_name, "-block") ? STYLE_BLOCK : STYLE_CHAR;
		ui_lib_upsert(list, &item);
	}
	closedir(d);
}

/*
 * miku_ui_library_record - 将一次成功的转换写入持久化列表，按「名称+规格」
 * 去重：同视频同规格更新为最新路径，同视频不同规格并列保留。
 *
 * @st: 调用方持有的界面配置，仅借用，不释放。
 * @cols: 本次转换使用的字符列数。
 * @res: 转换完成后的实际规格与源信息，仅借用，不释放。
 *
 * 返回：无；只执行列表持久化更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
void miku_ui_library_record(const struct ui_config *st, int cols,
			    const struct convert_outcome *res)
{
	struct library_list *list = calloc(1, sizeof(*list));
	struct library_item item = {0};

	if (!list)
		return;
	ui_lib_load_file(list);
	if (!st->output_auto && st->output[0])
		ui_lib_base_name(st->output, item.name, sizeof(item.name));
	else
		ui_lib_base_name(st->input, item.name, sizeof(item.name));
	ui_lib_abs_path(st->output, item.output, sizeof(item.output));
	ui_lib_abs_path(st->input, item.source, sizeof(item.source));
	item.width = res->prof.width;
	item.height = res->prof.height;
	item.fps = res->prof.fps;
	item.style = st->style;
	item.cols = cols;
	ui_lib_upsert(list, &item);
	ui_lib_save_file(list);
	free(list);
}

/*
 * ui_lib_draw_row - 绘制单条已有视频及其右侧规格说明。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @list: 调用方持有的已有视频列表，仅借用。
 * @idx: 待绘制条目的零基下标。
 * @row: 该条目在终端中的一基行号。
 * @cols: 终端可用显示列数。
 *
 * 返回：无；只向 sb 追加行内容。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_draw_row(struct strbuf *sb, const struct library_list *list,
			    int idx, int row, int cols)
{
	const struct library_item *it = &list->items[idx];
	int focused = idx == list->selected;
	char spec[160];
	int spec_w;
	int name_w;
	int name_max;
	int pad;

	snprintf(spec, sizeof(spec), "[%dx%d @ %dfps · %s · %.1f MB]",
		 it->width, it->height, it->fps,
		 it->style == STYLE_BLOCK ? "色块" : "字符",
		 (double)it->bytes / (1024.0 * 1024.0));
	spec_w = (int)miku_ui_text_width(spec);
	name_max = cols - spec_w - 3; /* 前缀两列 + 至少一列间隔 */
	if (name_max < 8) {
		/* 窄屏先收窄规格说明，保住分辨率这一个关键区分信息 */
		snprintf(spec, sizeof(spec), "[%dx%d %s]", it->width,
			 it->height, it->style == STYLE_BLOCK ? "块" : "字");
		spec_w = (int)miku_ui_text_width(spec);
		name_max = cols - spec_w - 3;
	}
	miku_ui_row(sb, row);
	miku_sb_addf(sb, "%s%s", focused ? UI_GOOD UI_BOLD "▶ " : "  ",
		     focused ? UI_MIKU : "");
	if (name_max < 8) {
		/* 连压缩后的规格都放不下：只画名字，不再挤出屏幕 */
		miku_ui_add_text_tail(sb, it->name, cols > 4 ? cols - 4 : 1);
		miku_sb_addf(sb, UI_RESET);
		return;
	}
	name_w = (int)miku_ui_text_width(it->name);
	if (name_w > name_max)
		name_w = name_max;
	miku_ui_add_text_tail(sb, it->name, name_max);
	miku_sb_addf(sb, UI_RESET);
	pad = cols - name_w - spec_w - 3;
	if (pad < 1)
		pad = 1;
	miku_ui_pad(sb, pad);
	miku_sb_addf(sb, "%s%s" UI_RESET, focused ? UI_BOLD : UI_DIM, spec);
}

/*
 * ui_lib_draw - 绘制「已有视频」完整列表界面并一次刷新到终端。
 *
 * @list: 调用方持有的已有视频列表，仅借用并夹紧滚动窗口。
 *
 * 返回：无；只执行屏幕绘制，不读取输入。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_draw(struct library_list *list)
{
	struct strbuf sb = {0};
	int cols = miku_terminal_columns();
	int rows = miku_terminal_rows();
	int win;
	int i;

	if (cols <= 0)
		cols = 80;
	if (rows <= 0)
		rows = 24;
	win = rows > 7 ? rows - 7 : 1;
	if (list->selected < list->top)
		list->top = list->selected;
	if (list->selected >= list->top + win)
		list->top = list->selected - win + 1;
	miku_ui_clear_screen();
	miku_sb_addf(&sb, "\x1b[H");
	miku_ui_row(&sb, 1);
	miku_sb_addf(&sb, UI_BOLD UI_MIKU "MIKU" UI_RESET UI_DIM
					  "  /  " UI_RESET UI_BOLD
					  "已有视频" UI_RESET);
	miku_ui_row(&sb, 2);
	miku_sb_addf(&sb, UI_DIM "────────────────────────────────" UI_RESET);
	for (i = list->top; i < list->count && i < list->top + win; ++i)
		ui_lib_draw_row(&sb, list, i, 4 + i - list->top, cols);
	if (!list->count) {
		miku_ui_row(&sb, 4);
		miku_sb_addf(&sb, UI_DIM "（暂无已转换视频，先去「视频转换」生"
					 "成吧）" UI_RESET);
	}
	miku_ui_row(&sb, rows - 1);
	miku_sb_addf(&sb,
		     UI_DIM "↑↓/点按 选择   Enter 播放   Esc/q 返回" UI_RESET);
	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * ui_lib_play_item - 按条目的规格在终端全屏播放，优先用源视频还原该规格字符画。
 *
 * @it: 选中的已有视频记录，仅借用。
 *
 * 返回：无；播放结束后自动恢复备用屏。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_lib_play_item(const struct library_item *it)
{
	const char *media = (it->source[0] && access(it->source, R_OK) == 0)
				    ? it->source
				    : it->output;
	struct video_info vi;
	int fps = it->fps > 0 ? it->fps : 24;

	miku_ui_draw_note("正在读取视频信息…", it->output);
	if (miku_probe_video(media, &vi) != 0)
		return;
	if (it->width > 0 && it->height > 0) {
		vi.width = it->width;
		vi.height = it->height;
	}
	fps = miku_clamp_int(fps, 1, 60);
	miku_ui_leave();
	miku_play_terminal(
		&(struct playback_options){
			.input = media,
			.cols = 0,
			.fps = fps,
			.with_audio = 1,
			.style = it->style,
		},
		&vi);
	miku_ui_enter();
}

/*
 * miku_ui_library - 主菜单「已有视频」页面：汇总历史转换与当前目录成品，
 * 同视频同规格去重、同视频异规格并列显示规格，回车或点按直接在终端播放。
 *
 * 返回：0 返回主菜单，1 退出程序。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_library(void)
{
	struct library_list *list = calloc(1, sizeof(*list));
	int k;
	int hit;

	if (!list)
		return 0;
	ui_lib_load_file(list);
	ui_lib_scan_cwd(list);
	ui_lib_save_file(list);
	while (!miku_g_ui_stop) {
		ui_lib_draw(list);
		k = miku_ui_key(-1);
		if (k == 'q' || k == 27 || k == UI_LEFT || k == 3)
			break;
		if ((k == UI_UP || k == 'k' || k == UI_WHEEL_UP) && list->count)
			list->selected = (list->selected + list->count - 1) %
					 list->count;
		else if ((k == UI_DOWN || k == 'j' || k == '\t' ||
			  k == UI_WHEEL_DOWN) &&
			 list->count)
			list->selected = (list->selected + 1) % list->count;
		else if (k == UI_CLICK && list->count) {
			hit = list->top + miku_g_ui_my - 4;
			if (miku_g_ui_my >= 4 && hit >= 0 &&
			    hit < list->count) {
				list->selected = hit;
				ui_lib_play_item(&list->items[list->selected]);
			}
		} else if ((k == '\r' || k == '\n') && list->count) {
			ui_lib_play_item(&list->items[list->selected]);
		}
	}
	free(list);
	return miku_g_ui_stop ? 1 : 0;
}
