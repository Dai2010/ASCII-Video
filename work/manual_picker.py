from pathlib import Path
import re
from edit_helpers import function_span, replace_function
head='/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n\n'
p=Path('src/ui_picker.c');original=p.read_text()
standard=original[original.index('#include <dirent.h>'):original.index('static int ui_is_video')]
helpers=['ui_is_video','ui_cmp_entry','ui_load_dir','ui_parent_dir','ui_join','ui_read_result','ui_accept_path']
public={'ui_load_dir':'int miku_ui_load_dir(const char *dir, struct directory_entry **out, int *count);',
        'ui_parent_dir':'void miku_ui_parent_dir(char *dir);',
        'ui_join':'void miku_ui_join(char *dst, size_t cap, const char *dir, const char *name);',
        'ui_read_result':'int miku_ui_read_result(const char *path, char *out, size_t cap);',
        'ui_accept_path':'int miku_ui_accept_path(char *path, size_t cap);'}
chunks=[]
for name in helpers:
 a,b=function_span(original,name);part=original[a:b]
 if name in public: part=part.replace('static ', '',1)
 chunks.append(part)
source=head+'#include "ui_directory.h"\n#include "util.h"\n\n'+standard+'\n\n'.join(chunks)
for old in public:source=re.sub(r'\b'+old+r'\b','miku_'+old,source)
source+='''
/* 将输入路径解析为起始目录，不可访问时回退到当前目录。 */
void miku_ui_initial_dir(char *dir, size_t capacity, const char *start)
{
        struct stat status;

        snprintf(dir, capacity, "%s", start && *start ? start : ".");
        if (stat(dir, &status) == 0 && !S_ISDIR(status.st_mode))
                miku_ui_parent_dir(dir);
        if (stat(dir, &status) != 0 || !S_ISDIR(status.st_mode))
                snprintf(dir, capacity, ".");
}

/* 进入子目录或父目录，先写临时缓冲，避免路径拼接的输入输出重叠。 */
void miku_ui_descend(char *dir, size_t capacity, const char *name)
{
        char next[1024];

        if (strcmp(name, "..") == 0) {
                miku_ui_parent_dir(dir);
                return;
        }
        miku_ui_join(next, sizeof(next), dir, name);
        snprintf(dir, capacity, "%s", next);
}
'''
Path('src/ui_directory.c').write_text(source)
Path('include/ui_directory.h').write_text(head+'''#ifndef MIKU_UI_DIRECTORY_H
#define MIKU_UI_DIRECTORY_H

#include "types.h"

'''+ '\n'.join(public.values())+'''
void miku_ui_initial_dir(char *dir, size_t capacity, const char *start);
void miku_ui_descend(char *dir, size_t capacity, const char *name);

#endif /* MIKU_UI_DIRECTORY_H */
''')
a,b=function_span(original,'ui_pick_with_dialog');dialog=original[a:b].replace('static int ui_pick_with_dialog','int miku_ui_pick_with_dialog')
for old in public:dialog=re.sub(r'\b'+old+r'\b','miku_'+old,dialog)
Path('src/ui_picker_external.c').write_text(head+'''#include "ui_picker_external.h"
#include "ui_directory.h"
#include "ui_terminal.h"
#include "util.h"
#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * 给 fzf 的列表含文件名和体积两列；仅写本次扫描的稳定快照。
 * 成功返回 0，写失败返回 -1，流始终在本函数内关闭。
 */
static int fzf_write_list(const char *path, const char *dir)
{
        struct directory_entry *list = NULL;
        FILE *file;
        int count;
        int i;
        int ret = 0;
        double megabytes;

        if (!miku_ui_load_dir(dir, &list, &count))
                return -1;
        file = fopen(path, "w");
        if (!file) {
                free(list);
                return -1;
        }
        for (i = 0; i < count; i++) {
                megabytes = list[i].size / (1024.0 * 1024.0);
                if (list[i].is_dir)
                        ret = fprintf(file, "%s/\\tdir\\n", list[i].name);
                else if (megabytes >= 1)
                        ret = fprintf(file, "%s\\t%.1f MB\\n",
                                      list[i].name, megabytes);
                else
                        ret = fprintf(file, "%s\\t%lld KB\\n", list[i].name,
                                      list[i].size / 1024);
                if (ret < 0)
                        break;
        }
        if (fclose(file) != 0)
                ret = -1;
        free(list);
        return ret < 0 ? -1 : 0;
}

/* 三个路径分别转义，返回的完整命令由调用方释放。 */
static char *fzf_command(const char *dir, const char *input, const char *result)
{
        char *quoted_dir = miku_shell_quote(dir);
        char *quoted_input = miku_shell_quote(input);
        char *quoted_result = miku_shell_quote(result);
        char *command = NULL;

        if (quoted_dir && quoted_input && quoted_result)
                command = miku_xasprintf(
                        "fzf --height=100%% --reverse --cycle --print0 --border=rounded --info=inline --pointer='▶' --color='fg:#d7dde7,bg:#0b0e14,hl:#7eff60,fg+:#7eff60,hl+:#7eff60,border:#2ad4d4,header:#6b7686,prompt:#2ad4d4' --delimiter='\\t' --with-nth=1,2 --prompt='%s > ' --header='输入可搜索 · Enter 进入/选定 · Esc 取消' < %s > %s",
                        quoted_dir, quoted_input, quoted_result);
        free(quoted_dir);
        free(quoted_input);
        free(quoted_result);
        return command;
}

/*
 * fzf 临时接管终端；无论工具如何退出都重新进入界面。
 * 返回 1 已选、-1 取消、0 无法启动，供调用方决定是否使用内置选择器。
 */
static int fzf_select(const char *dir, char *chosen, size_t capacity)
{
        char *input;
        char *result;
        char *command = NULL;
        int status;
        int ret = 0;

        input = miku_xasprintf("%s/miku-pick-%ld.in", miku_temp_dir(),
                               (long)getpid());
        result = miku_xasprintf("%s/miku-pick-%ld.out", miku_temp_dir(),
                                (long)getpid());
        if (!input || !result) {
                /* 跳到 out_free_paths，仅释放成功分配的路径。 */
                goto out_free_paths;
        }
        if (fzf_write_list(input, dir) == 0)
                command = fzf_command(dir, input, result);
        if (command) {
                miku_ui_leave();
                status = system(command);
                miku_ui_enter();
                ret = status == 0 &&
                      miku_ui_read_result(result, chosen, capacity) ? 1 : -1;
        }
        remove(input);
        remove(result);
        free(command);
        /* 所有执行路径汇合，临时路径字符串不逃出本函数。 */
out_free_paths:
        free(input);
        free(result);
        return ret;
}

/* 选择目录时重新扫描，选择文件时再次核对可读性，防止扫描后文件消失。 */
int miku_ui_pick_with_fzf(char *out, size_t cap, const char *start)
{
        char dir[1024];
        char chosen[1024];
        char *metadata;
        size_t length;
        int ret;

        miku_ui_initial_dir(dir, sizeof(dir), start);
        for (;;) {
                ret = fzf_select(dir, chosen, sizeof(chosen));
                if (ret != 1)
                        return ret;
                metadata = strchr(chosen, '\\t');
                if (metadata)
                        *metadata = '\\0';
                length = strlen(chosen);
                if (length && chosen[length - 1] == '/') {
                        chosen[length - 1] = '\\0';
                        miku_ui_descend(dir, sizeof(dir), chosen);
                        continue;
                }
                miku_ui_join(out, cap, dir, chosen);
                return miku_ui_accept_path(out, cap) ? 1 : -2;
        }
}

'''+dialog+'\n')
Path('include/ui_picker_external.h').write_text(head+'''#ifndef MIKU_UI_PICKER_EXTERNAL_H
#define MIKU_UI_PICKER_EXTERNAL_H

#include <stddef.h>

int miku_ui_pick_with_fzf(char *out, size_t cap, const char *start);
int miku_ui_pick_with_dialog(char *out, size_t cap, const char *start);

#endif /* MIKU_UI_PICKER_EXTERNAL_H */
''')
Path('src/ui_picker.c').write_text(head+'''#include "ui_picker.h"
#include "ui_picker_external.h"
#include "ui_directory.h"
#include "ui_terminal.h"
#include "ui_text.h"
#include "terminal.h"
#include "strbuf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* list 由扫描分配并由选择器释放；屏幕尺寸随每次重绘更新。 */
struct picker_state {
        char directory[1024];
        struct directory_entry *list;
        int count;
        int selected;
        int top;
        int columns;
        int rows;
        int visible;
};

/* 目录切换后丢弃旧快照；无法读取时按既有行为回退当前目录。 */
static void picker_reload(struct picker_state *state)
{
        free(state->list);
        state->list = NULL;
        state->count = 0;
        state->selected = 0;
        state->top = 0;
        if (miku_ui_load_dir(state->directory, &state->list, &state->count))
                return;
        snprintf(state->directory, sizeof(state->directory), ".");
        miku_ui_load_dir(state->directory, &state->list, &state->count);
}

/* 体积右对齐；空间不足时省略体积而不挤掉文件名。 */
static void picker_metadata(struct strbuf *buffer,
                             const struct directory_entry *entry, int columns)
{
        char text[32];
        double megabytes = entry->size / (1024.0 * 1024.0);
        int padding;

        if (entry->is_dir)
                return;
        if (megabytes >= 1)
                snprintf(text, sizeof(text), "%.1f MB", megabytes);
        else
                snprintf(text, sizeof(text), "%lld KB", entry->size / 1024);
        padding = columns - miku_ui_text_width(entry->name) -
                  (int)strlen(text) - 4;
        if (padding <= 0)
                return;
        miku_ui_pad(buffer, padding);
        miku_sb_addf(buffer, UI_DIM "%s" UI_RESET, text);
}

/* 渲染一条文件记录；列表起始行固定为 4，与鼠标命中计算一致。 */
static void picker_row(struct strbuf *buffer, const struct picker_state *state,
                        int index)
{
        const struct directory_entry *entry = &state->list[index];
        int focused = index == state->selected;
        const char *color;

        color = focused ? UI_BOLD : (entry->is_dir ? UI_ACCENT : UI_DIM);
        miku_ui_row(buffer, 4 + index - state->top);
        miku_sb_addf(buffer, "%s%s", focused ? UI_ACCENT "▸ " : "  ", color);
        miku_ui_add_text(buffer, entry->name, state->columns - 14);
        if (entry->is_dir)
                miku_sb_addf(buffer, "/");
        miku_sb_addf(buffer, UI_RESET);
        picker_metadata(buffer, entry, state->columns);
}

/* 尺寸变化时夹紧滚动窗口，至少保留一条可见记录，避免负窗口尺寸。 */
static void picker_geometry(struct picker_state *state)
{
        state->columns = miku_terminal_columns();
        state->rows = miku_terminal_rows();
        if (state->columns <= 0)
                state->columns = 80;
        if (state->rows <= 0)
                state->rows = 24;
        state->visible = state->rows > 6 ? state->rows - 6 : 1;
        if (state->selected < state->top)
                state->top = state->selected;
        if (state->selected >= state->top + state->visible)
                state->top = state->selected - state->visible + 1;
        if (state->top < 0)
                state->top = 0;
}

/* 只构建并提交屏幕，不读取按键，避免绘制与导航状态相互交织。 */
static void picker_draw(struct picker_state *state)
{
        struct strbuf buffer = {0};
        int i;

        picker_geometry(state);
        miku_ui_clear_screen();
        miku_sb_addf(&buffer, "\\x1b[H");
        miku_ui_row(&buffer, 1);
        miku_sb_addf(&buffer, UI_BOLD UI_ACCENT "选择视频" UI_RESET UI_DIM
                     "  Enter 选定  ← 返回上级  q 取消" UI_RESET);
        miku_ui_row(&buffer, 2);
        miku_sb_addf(&buffer, UI_DIM);
        miku_ui_add_text_tail(&buffer, state->directory, state->columns - 1);
        miku_sb_addf(&buffer, UI_RESET);
        for (i = state->top; i < state->count &&
                            i < state->top + state->visible; i++)
                picker_row(&buffer, state, i);
        if (!state->count) {
                miku_ui_row(&buffer, 4);
                miku_sb_addf(&buffer, UI_DIM);
                miku_ui_add_text(&buffer, "（这个目录里没有子目录或视频文件）",
                                 state->columns - 1);
                miku_sb_addf(&buffer, UI_RESET);
        }
        miku_ui_row(&buffer, state->rows - 1);
        miku_sb_addf(&buffer, UI_DIM "%d 项 · 当前第 %d 项" UI_RESET,
                     state->count, state->count ? state->selected + 1 : 0);
        miku_ui_flush(&buffer);
        free(buffer.buf);
}

/*
 * 处理只改变目录/焦点的按键，返回 1 表示该按键已处理。
 * 空列表取模使用 1 兜底，不允许以零作除数。
 */
static int picker_navigation(struct picker_state *state, int key)
{
        int count = state->count ? state->count : 1;

        if (key == UI_UP || key == 'k' || key == UI_WHEEL_UP) {
                state->selected = (state->selected + count - 1) % count;
                return 1;
        }
        if (key == UI_DOWN || key == 'j' || key == UI_WHEEL_DOWN) {
                state->selected = (state->selected + 1) % count;
                return 1;
        }
        if (key == UI_LEFT || key == 127 || key == 'h') {
                miku_ui_parent_dir(state->directory);
                picker_reload(state);
                return 1;
        }
        return 0;
}

/* 内置选择器返回 1 选定、0 取消、-1 文件已失效，始终释放列表。 */
static int picker_builtin(char *out, size_t capacity, const char *start)
{
        struct picker_state state = {0};
        const struct directory_entry *entry;
        int key;
        int index;
        int ret = 0;

        miku_ui_initial_dir(state.directory, sizeof(state.directory), start);
        picker_reload(&state);
        for (;;) {
                picker_draw(&state);
                key = miku_ui_key(-1);
                if (key == 'q' || key == 27 || key == 3 || miku_g_ui_stop)
                        break;
                if (key == UI_CLICK) {
                        index = state.top + miku_g_ui_my - 4;
                        if (miku_g_ui_my < 4 || index < 0 || index >= state.count)
                                continue;
                        state.selected = index;
                        key = '\\r';
                }
                if (picker_navigation(&state, key) || !state.count)
                        continue;
                if (key != '\\r' && key != '\\n')
                        continue;
                entry = &state.list[state.selected];
                if (entry->is_dir) {
                        miku_ui_descend(state.directory, sizeof(state.directory),
                                        entry->name);
                        picker_reload(&state);
                        continue;
                }
                miku_ui_join(out, capacity, state.directory, entry->name);
                ret = miku_ui_accept_path(out, capacity) ? 1 : -1;
                break;
        }
        free(state.list);
        return ret;
}

/*
 * 优先外部工具，启动失败才回退。用户取消不会再次弹出另一种选择器。
 * out 由调用方提供并容纳 capacity 字节；成功路径会再次验证普通文件可读。
 * 返回 1 成功、0 用户取消、-1 选中文件不可读；不转移字符串所有权。
 */
int miku_ui_pick_file(char *out, size_t capacity, const char *start)
{
        int ret = 0;

        if (miku_command_exists("fzf"))
                ret = miku_ui_pick_with_fzf(out, capacity, start);
        if (!ret && miku_command_exists("dialog"))
                ret = miku_ui_pick_with_dialog(out, capacity, start);
        if (ret == 1)
                return 1;
        if (ret == -1)
                return 0;
        if (ret == -2)
                return -1;
        return picker_builtin(out, capacity, start);
}
''')
