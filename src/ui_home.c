/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

/*
 * 首页：绘制 MIKU 大字标题与随机歌词，提供转换、播放、退出三个入口，并处理键盘与鼠标选择。
 */

#include "ui_home.h"
#include "strbuf.h"
#include "terminal.h"
#include "ui_terminal.h"
#include "ui_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ui_logo[4][8] = {
	{"##   ##", "##   ##", "### ###", "## # ##", "##   ##", "##   ##",
	 "##   ##", "##   ##"},
	{"#####", "  ## ", "  ## ", "  ## ", "  ## ", "  ## ", "#####",
	 "#####"},
	{"##   ##", "##  ## ", "## ##  ", "####   ", "####   ", "## ##  ",
	 "##  ## ", "##   ##"},
	{"##   ##", "##   ##", "##   ##", "##   ##", "##   ##", "##   ##",
	 " ## ## ", "  ###  "},
};

static const int ui_logo_w[4] = {7, 5, 7, 7};

/*
 * ui_logo_width - 累计四个字母的字符宽度并加入三个字母间隔，返回欢迎页图标
 * 的显示列数。
 *
 * 返回：上述计算结果或状态；其单位和取值含义见说明。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static int ui_logo_width(void)
{
	int i;

	int w = 0;
	for (i = 0; i < 4; ++i)
		w += ui_logo_w[i];
	return w + 3; /* 三个单格间隔 */
}

/*
 * ui_draw_logo - 把双行位图压为终端半块字符，保持四个字母居中；只追加输出
 * 缓冲，不操作输入或焦点。
 *
 * @sb: 调用方持有的输出缓冲，失败状态保存在 error 中。
 * @top: 图标首行的一基终端行号。
 * @cols: 宽度，单位为终端显示列或字符网格列。
 *
 * 返回：无；只执行上述更新，不转移借用对象所有权。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_draw_logo(struct strbuf *sb, int top, int cols)
{
	int cr;

	static const char *halves[4] = {" ", "▄", "▀", "█"};
	const int total = ui_logo_width();
	for (cr = 0; cr < 4; ++cr) {
		int l;

		miku_ui_row(sb, top + cr);
		miku_ui_pad(sb, miku_ui_center_col(total, cols));
		miku_sb_addf(sb, UI_MIKU UI_BOLD);
		for (l = 0; l < 4; ++l) {
			int c;

			for (c = 0; c < ui_logo_w[l]; ++c) {
				const int t = ui_logo[l][cr * 2][c] == '#';
				const int b = ui_logo[l][cr * 2 + 1][c] == '#';
				miku_sb_addf(sb, "%s", halves[(t << 1) | b]);
			}
			if (l < 3)
				miku_sb_addf(sb, " ");
		}
		miku_sb_addf(sb, UI_RESET);
	}
}

static const char *ui_lyrics[] = {
	"千本桜 夜ニ紛レ",	   /* 千本桜 / 黒うさP */
	"メルト 溶けてしまいそう", /* メルト / ryo */
	"世界で一番おひめさま",	   /* ワールドイズマイン / ryo */
	"だから妄想感傷代償連盟 愛を懐いて理想を号んだ", /* 妄想感傷代償連盟 /
							    DECO*27 */
	"終わんない愛を抱いていたくないの",		 /* ヒバナ / DECO*27 */
	"誰も知らない おとぎばなし",	   /* 六兆年と一夜物語 / kemu */
	"今後千年草も生えない 砂の惑星さ", /* 砂の惑星 / ハチ */
	"愛のネタバレ「別れ」っぽいな",	   /* 神っぽいな / ピノキオピー */
	"あたしヴァンパイア",		   /* ヴァンパイア / DECO*27 */
	"私の恋を悲劇のジュリエットにしないで", /* ロミオとシンデレラ / doriko
						 */
	"所詮 ヒトの真似事だと", /* 初音ミクの消失 / cosMo@暴走P */
	"君に伝えたいことが 君に届けたいことが", /* Tell Your World / livetune
						  */
};

static const char *ui_menu[5] = {"视频转换", "视频播放", "已有视频", "设置",
				 "退出"};

/*
 * ui_home_draw - 欢迎页只负责绘制，菜单焦点和歌词由调用方借用传入。
 *
 * @lyric: 借用的 UTF-8 欢迎页文本，只在本次绘制期间使用。
 * @focus: 当前菜单焦点的零基索引。
 *
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
static void ui_home_draw(const char *lyric, int focus)
{
	struct strbuf sb;
	int logo_top;
	int menu_top;
	int menu_left;
	int i;

	int cols = miku_terminal_columns();
	int rows = miku_terminal_rows();
	if (cols <= 0)
		cols = 80;
	if (rows <= 0)
		rows = 24;

	sb = (struct strbuf){0};
	miku_ui_clear_screen();
	logo_top = rows > 18 ? 3 : 2;
	ui_draw_logo(&sb, logo_top, cols);

	/* 歌词紧贴在大字下面一行 */
	miku_ui_row(&sb, logo_top + 5);
	miku_ui_pad(&sb, miku_ui_center_col(miku_ui_text_width(lyric), cols));
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_add_text(&sb, lyric, cols - 1);
	miku_sb_addf(&sb, UI_RESET);

	miku_ui_hit_reset();
	menu_top = logo_top + (rows > 22 ? 8 : 7);
	menu_left = miku_ui_center_col(16, cols);
	for (i = 0; i < UI_MENU_COUNT; ++i) {
		const int row = menu_top + i * 2;
		miku_ui_hit_add(row);
		miku_ui_row(&sb, row);
		miku_ui_pad(&sb, menu_left);
		if (i == focus) {
			miku_sb_addf(&sb, UI_GOOD UI_BOLD "▶ " UI_MIKU);
			miku_ui_add_text(&sb, ui_menu[i], cols - 2);
			miku_sb_addf(&sb, UI_RESET);
		} else {
			miku_sb_addf(&sb, UI_DIM "  " UI_RESET);
			miku_ui_add_text(&sb, ui_menu[i], cols - 2);
		}
	}

	miku_ui_row(&sb, rows - 1);
	miku_sb_addf(&sb, UI_DIM);
	miku_ui_pad(&sb, miku_ui_center_col(30, cols));
	miku_ui_add_text(&sb, "↑↓/点按 选择   Enter 确认   q 退出", cols - 1);
	miku_sb_addf(&sb, UI_RESET);
	miku_ui_flush(&sb);
	free(sb.buf);
}

/*
 * miku_ui_home - 维护欢迎菜单焦点，支持方向键、滚轮、数字键和点击。返回转
 * 换、播放、已有视频、设置或退出菜单索引，歌词每次进入时选择一次。
 *
 * 返回：0 转换，1 播放，2 已有视频，3 设置，4 退出。
 * 线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。
 */
int miku_ui_home(void)
{
	static int focus = 0;
	const char *lyric = ui_lyrics[(unsigned)rand() % UI_LYRIC_COUNT];
	int chosen = -1;

	while (chosen < 0 && !miku_g_ui_stop) {
		int k;

		ui_home_draw(lyric, focus);
		k = miku_ui_key(-1);
		if (k == UI_NONE)
			continue;
		if (k == 'q' || k == 27 || k == 3)
			return 4;
		if (k == UI_UP || k == 'k') {
			focus = (focus + UI_MENU_COUNT - 1) % UI_MENU_COUNT;
			continue;
		}
		if (k == UI_DOWN || k == 'j' || k == '\t') {
			focus = (focus + 1) % UI_MENU_COUNT;
			continue;
		}
		if (k == UI_WHEEL_UP || k == UI_WHEEL_DOWN) {
			focus = (focus +
				 (k == UI_WHEEL_DOWN ? 1 : UI_MENU_COUNT - 1)) %
				UI_MENU_COUNT;
			continue;
		}
		if (k == UI_CLICK) {
			const int hit = miku_ui_hit_index(miku_g_ui_my);
			if (hit >= 0) {
				focus = hit;
				chosen = hit;
			}
			continue;
		}
		if (k >= '1' && k <= '5') { /* 数字键直选，手机键盘也方便 */
			focus = k - '1';
			chosen = focus;
			continue;
		}
		if (k == '\r' || k == '\n')
			chosen = focus;
	}
	return chosen < 0 ? 4 : chosen;
}
