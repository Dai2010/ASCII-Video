/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 编码器能力、成本和档位；探测缓存由主线程串行初始化。
 */

#include "encoder.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const struct tier miku_tiers[TIER_COUNT] = {
	{"high", 2560, 1440, 60, 1500},
	{"mid", 1920, 1080, 30, 900},
	{"low", 1280, 720, 24, 500},
};

const struct encoder_candidate miku_encoders[] = {
	{"h264_videotoolbox", NULL, NULL, NULL, 1, 0}, /* macOS */
	{"h264_nvenc", NULL, NULL, NULL, 1, 0}, /* NVIDIA（Windows/Linux） */
	{"h264_qsv", NULL, NULL, NULL, 1, 0},	/* Intel（Windows/Linux） */
	{"h264_amf", NULL, NULL, NULL, 1, 0},	/* AMD（Windows） */
	{"h264_mf", NULL, NULL, NULL, 1, 0},	/* Windows MediaFoundation */
	{"h264_vaapi", NULL, ",format=nv12,hwupload", NULL, 1,
	 1},					     /* AMD/Intel（Linux） */
	{"h264_mediacodec", NULL, NULL, NULL, 1, 0}, /* Android */
	{"libx264",
	 "-preset " X264_PRESET " -crf " X264_CRF " -x264-params " X264_PARAMS,
	 NULL, NULL, 0, 0},
	{"mpeg4", "-q:v 2", NULL, NULL, 0, 0},
};

/*
 * miku_stamp_bt709 - 用无损流复制补写 BT.709 色彩标签。临时文件完成后才替
 * 换输出；失败清理临时文件并保留原输出。
 *
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 *
 * 返回：0 已补色标，失败返回 -EIO；原输出在替换前保持有效。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_stamp_bt709(const char *output)
{
	char *qout;
	char *qtmp;
	int done;

	const char *dot = strrchr(output, '.');
	char *tmp = (dot && dot != output)
			    ? miku_xasprintf("%.*s_tag%s", (int)(dot - output),
					     output, dot)
			    : miku_xasprintf("%s_tag.mp4", output);
	if (!tmp)
		return -EIO;
	qout = miku_shell_quote(output);
	qtmp = miku_shell_quote(tmp);
	done = 0;
	if (qout && qtmp) {
		char *cmd = miku_xasprintf(
			"ffmpeg -v error -y -i %s -map 0 -c copy "
			"-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
			"-movflags +faststart %s 2>/dev/null",
			qout, qtmp);
		if (cmd) {
			if (miku_exit_ok(system(cmd)) &&
			    rename(tmp, output) == 0)
				done = 1;
			free(cmd);
		}
	}
	if (!done)
		remove(tmp);
	free(qout);
	free(qtmp);
	free(tmp);
	return (done) ? 0 : -EIO;
}

/*
 * miku_total_memory_mb - 读取可用的物理内存信息，Linux 优先使用
 * /proc/meminfo，其他平台尝试现有系统接口。探测失败返回未知值。
 *
 * 返回：物理内存 MiB 数，0 表示探测失败。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
long miku_total_memory_mb(void)
{
	FILE *p;
	long long bytes;
	int got;

	/* Linux / Android */
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) {
		long kb = 0;
		char line[128];
		while (fgets(line, sizeof line, f)) {
			if (sscanf(line, "MemTotal: %ld kB", &kb) == 1)
				break;
		}
		fclose(f);
		if (kb > 0)
			return kb / 1024;
	}

	/* macOS / BSD 没有 /proc，用 sysctl；不同系统字段名不同，两个都试 */
	p = popen(
		"(sysctl -n hw.memsize || /usr/sbin/sysctl -n hw.memsize "
		"|| sysctl -n hw.physmem || /usr/sbin/sysctl -n hw.physmem) 2>/dev/null",
		"r");
	if (!p)
		return 0;
	bytes = 0;
	got = fscanf(p, "%lld", &bytes) == 1;
	pclose(p);
	return (got && bytes > 0) ? (long)(bytes / (1024 * 1024)) : 0;
}

/*
 * miku_cpu_cores - 优先读取正数 MIKU_CORES 覆盖值，否则询问在线处理器数，
 * 失败保守使用一个核心。
 *
 * 返回：至少为 1 的可用核心数。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_cpu_cores(void)
{
	long n;

	const char *env = getenv("MIKU_CORES");
	if (env && *env) {
		const int parsed = atoi(env);
		if (parsed > 0)
			return parsed;
	}
	n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}

/*
 * scan_encoder_line - 在单行编码器输出中匹配所有未命中候选，词边界只认空白
 * 或行首。
 *
 * @line: ffmpeg -encoders 输出的一行文本。
 * @cache: 长度为 ENCODER_COUNT 的状态数组，命中置 1。
 */
static void scan_encoder_line(const char *line, signed char *cache)
{
	const char *codec;
	const char *hit;
	size_t n;
	int i;

	for (i = 0; i < ENCODER_COUNT; i++) {
		if (cache[i] > 0)
			continue;
		codec = miku_encoders[i].codec;
		n = strlen(codec);
		for (hit = strstr(line, codec); hit;
		     hit = strstr(hit + 1, codec)) {
			if ((hit == line || hit[-1] == ' ' ||
			     hit[-1] == '\t') &&
			    (hit[n] == ' ' || hit[n] == '\t')) {
				cache[i] = 1;
				break;
			}
		}
	}
}

/*
 * miku_encoder_listed - 首次调用时执行一次 ffmpeg -encoders 扫描并缓存全部
 * 候选的注册情况。缓存只表示注册情况，不替代实际编码探测。
 *
 * @idx: 候选编码器索引，必须处于候选表范围内。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_encoder_listed(int idx)
{
	static signed char cache[ENCODER_COUNT];
	static int scanned;
	char line[256];
	FILE *p;
	int i;

	if (idx < 0 || idx >= ENCODER_COUNT)
		return false;
	if (!scanned) {
		scanned = 1;
		for (i = 0; i < ENCODER_COUNT; i++)
			cache[i] = -1;
		p = popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
		if (p) {
			while (fgets(line, sizeof line, p))
				scan_encoder_line(line, cache);
			pclose(p);
		}
	}
	return cache[idx] > 0;
}

/*
 * hw_bitrate_kbps - 按像素率估计硬件编码目标码率，并夹紧到预设的最低和最高
 * kbps。
 *
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：夹紧后的码率，单位 kbps。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static long hw_bitrate_kbps(int w, int h, int fps)
{
	const double px_per_sec = (double)w * h * fps;
	long kbps = (long)(px_per_sec * HW_BITS_PER_PIXEL / 1000.0);
	if (kbps < HW_MIN_KBPS)
		kbps = HW_MIN_KBPS;
	if (kbps > HW_MAX_KBPS)
		kbps = HW_MAX_KBPS;
	return kbps;
}

/*
 * vaapi_device_arg - 扫描结果缓存到静态字符串，返回借用指针或 NULL；仅首次
 * 调用扫描。
 * 设备名仅接受 renderD 前缀；长度不足以容纳完整参数时跳过该项。
 * 缓存不支持并发初始化，编码器探测由主线程串行执行。
 *
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 *
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 *
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static const char *vaapi_device_arg(void)
{
	static char buffer[128];
	static int scanned;
	const char *prefix = "-vaapi_device /dev/dri/";
	struct dirent *entry;
	DIR *directory;
	size_t length;

	if (scanned)
		return buffer[0] ? buffer : NULL;
	scanned = 1;
	directory = opendir("/dev/dri");
	if (!directory)
		return NULL;
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, "renderD", 7) != 0)
			continue;
		length = strlen(entry->d_name);
		if (strlen(prefix) + length >= sizeof(buffer))
			continue;
		memcpy(buffer, prefix, strlen(prefix));
		memcpy(buffer + strlen(prefix), entry->d_name, length + 1);
		break;
	}
	closedir(directory);
	return buffer[0] ? buffer : NULL;
}

/*
 * safe_token - 校验环境变量仅含 ASCII 字母、数字、点或连字符，防止拼接到
 * shell 命令时引入空白或元字符。
 *
 * @s: 待检查的环境变量字符串。
 *
 * 返回：1 为非空安全词元，0 为空或含其它字符。
 */
static int safe_token(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	if (!p || !*p)
		return 0;
	for (; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')
			continue;
		return 0;
	}
	return 1;
}

/*
 * x264_args - 读取 MIKU_PRESET/MIKU_CRF 覆盖项，未设置或含非法字符时使用项
 * 目默认值；拼接软件编码器参数并返回拥有的字符串。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static char *x264_args(void)
{
	const char *preset = getenv("MIKU_PRESET");
	const char *crf = getenv("MIKU_CRF");

	if (!safe_token(preset))
		preset = X264_PRESET;
	if (!safe_token(crf))
		crf = X264_CRF;
	return miku_xasprintf("-preset %s -crf %s -x264-params " X264_PARAMS,
			      preset, crf);
}

/*
 * miku_encoder_args - 为指定尺寸生成候选编码器的参数。x264 使用质量参数，
 * 其他有固定参数者直接复制，否则根据像素率估算码率。
 *
 * @c: 借用的编码器候选，只读。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
char *miku_encoder_args(const struct encoder_candidate *c, int w, int h,
			int fps)
{
	if (strcmp(c->codec, "libx264") == 0)
		return x264_args();
	if (c->fixed)
		return miku_xasprintf("%s", c->fixed);
	return miku_xasprintf("-b:v %ldk", hw_bitrate_kbps(w, h, fps));
}

/*
 * miku_cand_input_prefix - 返回编码器必须置于输入之前的参数；VAAPI 按本机
 * 设备扫描结果提供前缀，返回值均为借用。
 *
 * @c: 借用的编码器候选，只读。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
const char *miku_cand_input_prefix(const struct encoder_candidate *c)
{
	return c->vaapi ? vaapi_device_arg() : c->input_prefix;
}

/*
 * count_decodable_frames - 解码探测文件并读取实际帧数。不能仅凭编码器退出
 * 码认定输出有效，损坏或空结果返回负计数。
 *
 * @path: 借用的文件系统路径，不转移字符串所有权。
 *
 * 返回：实际帧数，失败返回负值。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static int count_decodable_frames(const char *path)
{
	char *cmd;
	FILE *p;
	int frames;

	char *q = miku_shell_quote(path);
	if (!q)
		return -1;
	cmd = miku_xasprintf(
		"ffprobe -v error -count_frames -select_streams v:0 "
		"-show_entries stream=nb_read_frames -of csv=p=0 %s 2>/dev/null",
		q);
	free(q);
	if (!cmd)
		return -1;
	p = popen(cmd, "r");
	free(cmd);
	if (!p)
		return -1;
	frames = -1;
	if (fscanf(p, "%d", &frames) != 1)
		frames = -1;
	pclose(p);
	return frames;
}

struct probe_record {
	const struct encoder_candidate *c;
	int w;
	int h;
	int fps;
	int ok;
};

static struct probe_record g_probe_cache[32];
static int g_probe_count;

/*
 * probe_cache_lookup - 查询编码器短片探测缓存。高规格已通过则低或同规格直
 * 接复用；已失败且新规格不低于失败规格时直接判负，避免重复拉起子进程。
 *
 * @c: 借用的编码器候选，只读。
 * @w: 宽度像素值。
 * @h: 高度像素值。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：1 命中可用，0 命中不可用，-1 未命中。
 */
static int probe_cache_lookup(const struct encoder_candidate *c, int w, int h,
			      int fps)
{
	const struct probe_record *r;
	int i;

	for (i = 0; i < g_probe_count; i++) {
		r = &g_probe_cache[i];
		if (r->c != c)
			continue;
		if (r->ok && w <= r->w && h <= r->h && fps <= r->fps)
			return 1;
		if (!r->ok &&
		    ((w >= r->w && h >= r->h && fps >= r->fps) || r->w <= 2560))
			return 0;
	}
	return -1;
}

/*
 * miku_encoder_works - 实际编码少量黑帧再解码计数。两个步骤均成功才接受候
 * 选，所有探测命令、临时文件和路径均在返回前释放。
 *
 * @c: 借用的编码器候选，只读。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_encoder_works(const struct encoder_candidate *c, int w, int h,
			int fps)
{
	const char *prefix = miku_cand_input_prefix(c);
	char *args;
	char *out;
	char *cmd;
	int cached;
	int ok;

	if (c->vaapi && !prefix)
		return 0; /* 本机没有 renderD* 设备，vaapi 无从谈起 */
	cached = probe_cache_lookup(c, w, h, fps);
	if (cached >= 0)
		return cached > 0;
	args = miku_encoder_args(c, w, h, fps);
	if (!args)
		return 0;
	out = miku_xasprintf("%s/miku-probe-%ld.mp4", miku_temp_dir(),
			     (long)getpid());
	if (!out) {
		free(args);
		return 0;
	}
	remove(out);
	cmd = miku_xasprintf(
		"ffmpeg -v error -y %s -f lavfi -i 'color=c=black:s=%dx%d:r=%d' "
		"-frames:v %d -vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p%s' "
		"-c:v %s %s %s 2>/dev/null",
		prefix ? prefix : "", w, h, fps, PROBE_FRAMES,
		c->pipe_filter ? c->pipe_filter : "", c->codec, args, out);
	free(args);
	ok = 0;
	if (cmd) {
		ok = miku_exit_ok(system(cmd)) &&
		     count_decodable_frames(out) >= PROBE_FRAMES;
		free(cmd);
	}
	remove(out);
	free(out);
	if (g_probe_count < 32)
		g_probe_cache[g_probe_count++] =
			(struct probe_record){c, w, h, fps, ok};
	return ok;
}

/*
 * miku_pick_encoder - 按候选表顺序返回首个实际可用编码器。MIKU_NO_HW 非零
 * 时跳过硬件项；返回静态表内借用指针。
 *
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
const struct encoder_candidate *miku_pick_encoder(int w, int h, int fps)
{
	int i;

	const char *no_hw = getenv("MIKU_NO_HW");
	const int skip_hw = no_hw && *no_hw && strcmp(no_hw, "0") != 0;
	for (i = 0; i < ENCODER_COUNT; ++i) {
		if (skip_hw && miku_encoders[i].hardware)
			continue;
		if (!miku_encoder_listed(i))
			continue;
		if (miku_encoder_works(&miku_encoders[i], w, h, fps))
			return &miku_encoders[i];
	}
	return NULL;
}

/*
 * profile_cpu - 由每秒像素率、软件或硬件系数估计每秒视频消耗的 CPU 秒数，
 * 供自动档位预算判断。
 *
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：估计的 CPU 秒/视频秒。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static double profile_cpu(const struct profile *p)
{
	const double mpx = (double)p->width * p->height * p->fps / 1e6;
	return mpx * (p->enc->hardware ? HW_CPU_PER_MPX : SW_CPU_PER_MPX) +
	       mpx * RENDER_CPU_PER_MPX;
}

/*
 * miku_output_tail_ok - 抽查输出末尾两秒的可解码帧数，至少需要一秒输出帧数
 * 。用来发现部分硬件编码器中途漏帧。
 *
 * @output: 输出媒体路径或输出位置，生命周期覆盖本次调用。
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
bool miku_output_tail_ok(const char *output, const struct profile *p)
{
	char *cmd;
	FILE *f;
	int frames;

	char *q = miku_shell_quote(output);
	if (!q)
		return 0;
	cmd = miku_xasprintf(
		"ffprobe -v error -read_intervals -2%% -count_frames -select_streams v:0 "
		"-show_entries stream=nb_read_frames -of csv=p=0 %s 2>/dev/null",
		q);
	free(q);
	if (!cmd)
		return 0;
	f = popen(cmd, "r");
	free(cmd);
	if (!f)
		return 0;
	frames = -1;
	if (fscanf(f, "%d", &frames) != 1)
		frames = -1;
	pclose(f);
	return frames >= p->fps;
}

/*
 * miku_parse_custom_size - 解析 WxH 或 WxH@FPS。宽高必须为支持范围内的偶数
 * ，省略帧率时写入零，表示跟随源帧率。
 *
 * @s: 有效的 NUL 结尾文本或待解析字段。
 * @w: 宽度像素值或接收宽度的输出地址。
 * @h: 高度像素值或接收高度的输出地址。
 * @fps: 帧率，单位为帧/秒。
 *
 * 返回：0 成功并写出宽高帧率，-EINVAL 表示尺寸或帧率不合法。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_parse_custom_size(const char *s, int *w, int *h, int *fps)
{
	int ww = 0;
	int hh = 0;
	int ff = 0;
	const int n = sscanf(s, "%dx%d@%d", &ww, &hh, &ff);
	if (n < 2)
		return -EINVAL;
	if (n < 3)
		ff = 0;
	if (ww < 256 || hh < 256 || ww > 7680 || hh > 4320)
		return -EINVAL;
	if ((ww & 1) != 0 || (hh & 1) != 0) /* yuv420p 与硬件编码器都要求偶数 */
		return -EINVAL;
	if (n >= 3 && (ff < 1 || ff > 240))
		return -EINVAL;
	*w = ww;
	*h = hh;
	*fps = ff;
	return 0;
}

/*
 * profile_from_tier - 档位信息复制到结果，不持有档位表之外的内存。
 *
 * @profile: 输出规格，非 const 时由本函数填写，只读编码器指针借用静态表。
 * @tier: 档位表索引，范围为 0..TIER_COUNT-1。
 * @source_fps: 源帧率整数估计，非正表示未知。
 *
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static void profile_from_tier(struct profile *profile, int tier, int source_fps)
{
	const struct tier *specification = &miku_tiers[tier];

	snprintf(profile->name, sizeof(profile->name), "%s",
		 specification->name);
	profile->width = specification->width;
	profile->height = specification->height;
	profile->fps = specification->fps;
	if (source_fps > 0 && source_fps < profile->fps)
		profile->fps = source_fps;
}

/*
 * choose_auto_profile - 高到低探测并选择进 CPU/内存预算的首档，否则保留最
 * 低可用档。
 * 预算是每秒视频可占用的 CPU 秒数，内存未知时不作内存筛选。
 *
 * @source_fps: 源帧率整数估计，非正表示未知。
 * @profile: 输出规格，非 const 时由本函数填写，只读编码器指针借用静态表。
 *
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
static int choose_auto_profile(int source_fps, struct profile *profile)
{
	struct profile candidate = {0};
	long memory = miku_total_memory_mb();
	double budget = miku_cpu_cores() * CPU_BUDGET_RATIO;
	int i;

	/*
	 * 按档位从高到低逐个尝试：编码器可用、内存不低于该档下限、预计 CPU 占用在预算内，
	 * 三项都满足即采用。全部不满足时退到最后一个可用候选（最低档），保证总能输出。
	 */
	memset(profile, 0, sizeof(*profile));
	for (i = 0; i < TIER_COUNT; i++) {
		profile_from_tier(&candidate, i, source_fps);
		candidate.enc = miku_pick_encoder(
			candidate.width, candidate.height, candidate.fps);
		if (!candidate.enc)
			continue;
		*profile = candidate;
		if (memory > 0 && memory < miku_tiers[i].min_mem_mb)
			continue;
		if (profile_cpu(&candidate) <= budget)
			return 1;
	}
	return profile->enc != NULL;
}

/*
 * miku_choose_profile - 解析显式尺寸或档位，或执行自动预算选择；所有结果都
 * 必须匹配实际可用的编码器。档位默认帧率不超过源帧率；WxH@FPS 显式帧率保持
 * 原值，由转换阶段用重复帧补齐。
 *
 * @quality: 质量档位或自定义尺寸字符串。
 * @src_fps: 源帧率的整数估计，非正表示未知。
 * @p: 借用的规格对象，非 const 指针用于接收计算结果。
 *
 * 返回：0 已选定，-ENODEV 表示档位无效或没有可用组合。
 * 线程约束：编码器探测及其静态缓存由主线程串行访问。
 */
int miku_choose_profile(const char *quality, int src_fps, struct profile *p)
{
	const char *name = quality && *quality ? quality : "auto";
	int tier;
	int explicit_fps = 0;

	if (strcmp(name, "auto") == 0)
		return (choose_auto_profile(src_fps, p)) ? 0 : -ENODEV;
	if ((miku_parse_custom_size(name, &p->width, &p->height, &p->fps) ==
	     0)) {
		explicit_fps = p->fps > 0; /* WxH@FPS 显式帧率不受源帧率限制 */
		if (p->fps > 0)
			snprintf(p->name, sizeof(p->name), "%dx%d@%d", p->width,
				 p->height, p->fps);
		else
			snprintf(p->name, sizeof(p->name), "%dx%d", p->width,
				 p->height);
	} else {
		for (tier = 0; tier < TIER_COUNT; tier++) {
			if (strcmp(name, miku_tiers[tier].name) == 0)
				break;
		}
		if (tier == TIER_COUNT)
			return -ENODEV;
		profile_from_tier(p, tier, src_fps);
	}
	if (!explicit_fps && src_fps > 0 && (p->fps <= 0 || src_fps < p->fps))
		p->fps = src_fps;
	if (p->fps < 1)
		p->fps = miku_tiers[TIER_COUNT - 1].fps;
	p->enc = miku_pick_encoder(p->width, p->height, p->fps);
	return (p->enc != NULL) ? 0 : -ENODEV;
}
