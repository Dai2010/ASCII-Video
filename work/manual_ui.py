from pathlib import Path
from edit_helpers import replace_function, function_span

p=Path('src/ui_home.c');s=p.read_text();a,b=function_span(s,'miku_ui_home');fn=s[a:b];start=fn.index('\t\tint cols =');end=fn.index('\n\t\tconst int k =',start);draw=fn[start:end];fn=fn[:start]+'                ui_home_draw(lyric, focus);'+fn[end:];helper='''/* 欢迎页只负责绘制，菜单焦点和歌词由调用方借用传入。 */
static void ui_home_draw(const char *lyric, int focus)
{
'''+draw+'\n}\n\n';p.write_text(s[:a]+helper+fn+s[b:])
p=Path('src/ui_config.c');s=p.read_text();a=s.index('\t\tif (k == 127) {');b=s.index('\n\t\tif (k >= UI_TEXT_BYTE)',a);s=s[:a]+'''                if (k == 127) {
                        ui_delete_character(buf, &len);
                        continue;
                }'''+s[b:];a,b=function_span(s,'miku_ui_edit_field');s=s[:a]+'''/* UTF-8 退格先跳过延续字节，保证不留下半个多字节字符。 */
static void ui_delete_character(char *buffer, size_t *length)
{
        if (!*length)
                return;
        (*length)--;
        while (*length && ((unsigned char)buffer[*length] & 0xc0) == 0x80)
                (*length)--;
        buffer[*length] = '\\0';
}

'''+s[a:]
a,b=function_span(s,'miku_ui_draw_config');fn=s[a:b];start=fn.index('\tmiku_ui_hit_reset();');end=fn.index('\n\tmiku_ui_row(&sb, rows - 1)',start);rowcode=fn[start:end].replace('&sb','sb');labels_start=fn.index('\tstatic const char *labels_all');labels_end=fn.index('\n\tstruct strbuf sb',labels_start);declarations=fn[labels_start:labels_end];fn=fn[:labels_start]+fn[labels_end:];start=fn.index('\tmiku_ui_hit_reset();');end=fn.index('\n\tmiku_ui_row(&sb, rows - 1)',start);fn=fn[:start]+'        ui_config_rows(&sb, st, kind, cols);\n'+fn[end:];helper='''/* 配置项和鼠标命中行同步生成，避免增删一项后点击位置错位。 */
static void ui_config_rows(struct strbuf *sb, const struct ui_config *st,
                            int kind, int cols)
{
'''+declarations+'\n'+rowcode+'\n}\n\n';s=s[:a]+helper+fn+s[b:];p.write_text(s)
replace_function('src/ui.c','ui_config_screen','''
/* 文件选择返回三态，取消时保留原路径，失效时把原因写入状态栏。 */
static void ui_pick_input(struct ui_config *state)
{
        char picked[UI_PATH_MAX];
        int ret;

        ret = miku_ui_pick_file(picked, sizeof(picked),
                                state->input[0] ? state->input : ".");
        if (ret > 0)
                snprintf(state->input, sizeof(state->input), "%s", picked);
        else if (ret < 0)
                miku_ui_status(state,
                        "选中的文件读不了（可能已被删除或没有权限），换一个试试");
}

/* 回车和点击共用激活动作；只有完成页要求退出时返回 1。 */
static int ui_activate(struct ui_config *state, int kind, int item)
{
        if (item == CFG_START)
                return ui_start(state, kind) == 2;
        if (item == CFG_INPUT)
                ui_pick_input(state);
        else if (item == CFG_OUTPUT)
                miku_ui_edit_field(state, CFG_OUTPUT,
                                   "输出视频路径（留空 = 自动）");
        else
                miku_ui_cfg_adjust(state, item, 1);
        return 0;
}

/*
 * 配置页循环只管理焦点与分派；参数修改和执行分别由专门函数负责。
 * kind=0 转换、kind=1 播放；state 由上层保存，返回后仍保持用户设置。
 */
static int ui_config_screen(struct ui_config *state, int kind)
{
        int key;
        int count;
        int item;
        int hit;
        int direction;

        state->focus = 0;
        state->status[0] = '\\0';
        for (;;) {
                miku_ui_draw_config(state, kind);
                key = miku_ui_key(-1);
                if (key == 'q' || key == 27 || key == 3 || miku_g_ui_stop)
                        return 0;
                count = miku_ui_cfg_count(kind);
                if (key == UI_CLICK) {
                        hit = miku_ui_hit_index(miku_g_ui_my);
                        if (hit < 0)
                                continue;
                        state->focus = hit;
                        key = '\\r';
                }
                if (key == UI_UP || key == 'k' || key == UI_DOWN ||
                    key == 'j' || key == '\\t' || key == UI_WHEEL_DOWN ||
                    key == UI_WHEEL_UP) {
                        direction = key == UI_UP || key == 'k' ||
                                    key == UI_WHEEL_UP ? -1 : 1;
                        state->focus = (state->focus + direction + count) % count;
                        continue;
                }
                item = miku_ui_cfg_index(kind, state->focus);
                if (key == 'e' && item == CFG_INPUT)
                        miku_ui_edit_field(state, CFG_INPUT,
                                           "输入视频路径（手打）");
                else if (key == UI_LEFT || key == UI_RIGHT)
                        miku_ui_cfg_adjust(state, item, key == UI_LEFT ? -1 : 1);
                else if ((key == '\\r' || key == '\\n') &&
                         ui_activate(state, kind, item))
                        return 1;
        }
}
''')
replace_function('src/ui_terminal.c','miku_ui_key','''
/* X10 鼠标每个字段在原始值上加 32；读取不完整时丢弃该事件。 */
static int ui_x10_mouse(void)
{
        int button = ui_read_raw(20);
        int x = ui_read_raw(20);
        int y = ui_read_raw(20);

        if (button == UI_NONE || x == UI_NONE || y == UI_NONE)
                return UI_NONE;
        g_ui_mx = x - 32;
        miku_g_ui_my = y - 32;
        button -= 32;
        if (button & 64)
                return button & 1 ? UI_WHEEL_DOWN : UI_WHEEL_UP;
        return UI_CLICK;
}

/* SGR 鼠标序列已经包含终止字节，正文仅含按钮和坐标。 */
static int ui_sgr_mouse(const char *sequence)
{
        int button;
        int x;
        int y;

        if (sscanf(sequence, "<%d;%d;%d", &button, &x, &y) != 3)
                return UI_NONE;
        g_ui_mx = x;
        miku_g_ui_my = y;
        if (button & 64)
                return button & 1 ? UI_WHEEL_DOWN : UI_WHEEL_UP;
        return UI_CLICK;
}

/*
 * CSI 终止字节为 0x40..0x7e；多余正文被丢弃但仍消费完整序列。
 * 每字节等待上限 20ms，保持旧键盘和鼠标协议兼容。
 */
static int ui_csi_sequence(int first)
{
        char sequence[24];
        int length = 0;
        int last;

        if (first == UI_NONE)
                return UI_NONE;
        if (first == 'M')
                return ui_x10_mouse();
        switch (first) {
        case 'A': return UI_UP;
        case 'B': return UI_DOWN;
        case 'C': return UI_RIGHT;
        case 'D': return UI_LEFT;
        default: break;
        }
        if (first >= 0x40 && first <= 0x7e)
                return UI_NONE;
        sequence[length++] = (char)first;
        for (;;) {
                last = ui_read_raw(20);
                if (last == UI_NONE)
                        return UI_NONE;
                if (last >= 0x40 && last <= 0x7e)
                        break;
                if (length < (int)sizeof(sequence) - 1)
                        sequence[length++] = (char)last;
        }
        sequence[length] = '\\0';
        if (sequence[0] == '<' && (last == 'M' || last == 'm'))
                return ui_sgr_mouse(sequence);
        return UI_NONE;
}

/*
 * 把输入字节映射为 UI 键值；UTF-8 高位字节加 UI_TEXT_BYTE 保留原值。
 * timeout_ms<0 表示等待首字节，0 为轮询。只允许界面主线程调用。
 * 未进入原始模式或无输入返回 UI_NONE，单独 Esc 返回 27。
 */
int miku_ui_key(int timeout_ms)
{
        int key;

        if (!g_ui_raw)
                return UI_NONE;
        key = ui_read_raw(timeout_ms);
        if (key == 0x1b) {
                if (ui_read_raw(20) != '[')
                        return 27;
                return ui_csi_sequence(ui_read_raw(20));
        }
        if (key >= 0x80)
                return UI_TEXT_BYTE + key;
        if (key == 8)
                return 127;
        return key;
}
''')
