/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#ifndef MIKU_TYPES_H
#define MIKU_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "constants.h"

/* 预设质量档位，字符串和表项具有静态生命周期。 */
struct tier {
	const char *name; /* 静态档位名。 */
	int width;	  /* 输出宽度，单位像素。 */
	int height;	  /* 输出高度，单位像素。 */
	int fps;	  /* 默认输出帧率。 */
	long min_mem_mb;  /* 选择此档位所需最低物理内存 MiB。 */
};

/* 编码器候选，所有字符串均借用静态表；NULL 表示无附加参数。 */
struct encoder_candidate {
	const char *codec;	 /* ffmpeg 编码器名称。 */
	const char *fixed;	 /* 固定编码参数，NULL 时按像素率估算。 */
	const char *pipe_filter; /* 追加到 RGB 转换之后的滤镜片段。 */
	const char *
		input_prefix; /* 必须放在输入前的参数，VAAPI 运行时另行解析。 */
	int hardware;	      /* 非零表示硬件编码。 */
	int vaapi;	      /* 非零表示必须先选择 VAAPI 渲染设备。 */
};

/* 一次转换最终采用的规格；enc 借用静态候选，不能释放。 */
struct profile {
	char name[32]; /* 供界面和日志显示的档位名称。 */
	int width;     /* 实际输出宽度，单位像素。 */
	int height;    /* 实际输出高度，单位像素。 */
	/* 实际输出帧率。档位默认值不超过源帧率；显式指定的可高于源帧率。 */
	int fps;
	const struct encoder_candidate
		*enc; /* 已通过真实短片探测的编码器候选。 */
};

/* 字形蒙版和亮度索引；masks 由建图分配，调用方最终 free。 */
struct glyph_atlas {
	int cell_w;	/* 单个字形宽度，单位像素。 */
	int cell_h;	/* 单个字形高度，单位像素。 */
	uint8_t *masks; /* 按墨量升序排列的连续灰度字形，归调用方所有。 */
	char chars[FONT_CHAR_COUNT +
		   1]; /* 与 masks 同序的 ASCII 字符，包含结尾 NUL。 */
	int lut[256];  /* 输入亮度 0..255 到字形索引的映射。 */
};

/* 探测结果，不持有动态内存；帧率以正有理数保存。 */
struct video_info {
	int width;	   /* 解码并应用旋转后的宽度，单位像素。 */
	int height;	   /* 解码并应用旋转后的高度，单位像素。 */
	long fps_num;	   /* 源帧率分子。 */
	long fps_den;	   /* 源帧率分母，探测成功时为正数。 */
	long total_frames; /* 源总帧数或根据时长的估计，零表示未知。 */
};

/* 画布中的字符网格与偶数黑边偏移，不持有内存。 */
struct layout {
	int cols;   /* 网格列数，单位字符格。 */
	int rows;   /* 网格行数，单位字符格。 */
	int cell_w; /* 格子宽度，单位像素。 */
	int cell_h; /* 格子高度，单位像素。 */
	int off_x;  /* 水平方向黑边偏移，单位像素，取偶数。 */
	int off_y;  /* 垂直方向黑边偏移，单位像素，取偶数。 */
};

/* 所有计数为帧，时间单位为秒；total=0、eta<0 分别表示未知。 */
/* 同步回调的只读快照；回调返回后不得保存其地址。 */
struct progress_update {
	long long done;		 /* 已经读取的源帧数。 */
	long long total;	 /* 源总帧数，零表示未知。 */
	long long output_frames; /* 已经写入的输出帧数。 */
	double elapsed;		 /* 已经过时间，单位秒。 */
	double eta;		 /* 预计剩余时间，负数表示未知。 */
};

/* 回调在转换线程同步执行；update 仅在回调期间有效，不得保存其指针。 */
/* 回调和 ud 均由调用方持有；转换过程同步调用，不创建回调线程。 */
struct convert_hooks {
	void *ud; /* 回调上下文，必须覆盖整个转换调用。 */
	void (*on_progress)(
		void *ud,
		const struct progress_update
			*update); /* 可选进度回调，update 仅在调用期间有效。 */
	int (*should_abort)(
		void *ud); /* 可选取消谓词，非零表示取消本次转换。 */
};

/* 返回实际规格和源信息；编码器指针借用静态候选，不需单独释放。 */
struct convert_outcome {
	struct profile prof;	/* 最终成功或正在尝试的输出规格。 */
	struct video_info info; /* 已经探测的源媒体信息。 */
};

/* 可复用字节缓冲；buf 唯一拥有，len<=cap，调用方最终 free。 */
struct strbuf {
	char *buf;  /* 分配的字节存储，可为 NULL。 */
	size_t len; /* 已使用字节，不含可选结尾 NUL。 */
	size_t cap; /* 已分配字节容量。 */
	int error;  /* 首个负 errno；提交时必须检查，不输出半帧。 */
};

/* 单条目录快照，名称保存在记录内部，不依赖 readdir 的临时指针。 */
struct directory_entry {
	char name[256]; /* 目录项名称，NUL 结尾。 */
	int is_dir;	/* 非零表示目录，目录优先排序。 */
	long long size; /* 普通文件字节数，目录为零。 */
};

/* 转换和播放页面各保存一份，切换页面不会混用输入文件。 */
struct ui_config {
	char input[UI_PATH_MAX];  /* 用户输入路径，UTF-8，NUL 结尾。 */
	char output[UI_PATH_MAX]; /* 输出路径，空串表示自动派生。 */
	int output_auto;	  /* 非零表示输出名仍随输入与风格自动派生。 */
	int style;		  /* STYLE_CHAR 或 STYLE_BLOCK。 */
	int ratio;		  /* 比例索引，零表示自动铺满。 */
	int fps;		  /* 指定帧率，零表示跟随源。 */
	int quality;		  /* 0 自动、1 原画、2 高、3 中、4 低。 */
	int cols_want;		  /* 播放列数，零表示跟随终端。 */
	int mute;		  /* 非零时不启动音轨。 */
	int focus;		  /* 当前页面的零基焦点槽位。 */
	char status[256];	  /* 状态栏提示，固定容量，NUL 结尾。 */
};

/* 一次转换的界面上下文，借用配置和结果，生命周期覆盖同步转换。 */
struct ui_progress {
	struct convert_outcome *res; /* 借用的转换输出对象。 */
	const struct ui_config *st;  /* 借用的页面配置，只读。 */
	int width_cols;		     /* 本次转换采用的字符列数。 */
	int style;		     /* 本次转换画面风格。 */
	long long done;		     /* 已消费的源帧数。 */
	long long total;	     /* 源总帧数，零表示未知。 */
	long long out_frames;	     /* 已写入编码器的输出帧数。 */
	double elapsed;		     /* 从开始转换起经过的单调时钟秒数。 */
	double eta;		     /* 预计剩余秒数，负数表示未知。 */
	double last_draw;	     /* 上次提交进度屏幕的单调时钟秒数。 */
	int cancelled;		     /* 非零表示用户取消，禁止软件回退重试。 */
};

#endif /* MIKU_TYPES_H */
