from pathlib import Path
import re
from edit_helpers import function_span, TOKEN

# 底层工具不依赖编码器模块，字体查表也回到字体模块内。
p=Path('src/encoder.c');encoder=p.read_text();a,b=function_span(encoder,'miku_temp_dir');temp=encoder[a:b];p.write_text(encoder[:a]+encoder[b:])
p=Path('src/util.c');util=p.read_text();a,b=function_span(util,'miku_build_gamma_lut');gamma=util[a:b];util=util[:a]+util[b:];util=util.replace('int miku_gamma_lut[256];','').replace('#include "encoder.h"','');util+='\n'+temp+'\n';util+='''
/* 返回闭区间 [minimum,maximum] 内的值；调用方保证 minimum<=maximum。 */
int miku_clamp_int(int value, int minimum, int maximum)
{
        if (value < minimum)
                return minimum;
        if (value > maximum)
                return maximum;
        return value;
}
''';p.write_text(util)
p=Path('src/glyph.c');s=p.read_text();s+='\n/* 亮度表只在主线程开始渲染前初始化，之后各渲染器只读。 */\nint miku_gamma_lut[256];\n\n'+gamma.replace('v < 0 ? 0 : (v > 255 ? 255 : v)', 'miku_clamp_int(v, 0, 255)')+'\n';p.write_text(s)
p=Path('include/util.h');s=p.read_text().replace('extern int miku_gamma_lut[256];\nvoid miku_build_gamma_lut(void);\n','');s=s.replace('#endif /* MIKU_UTIL_H */','const char *miku_temp_dir(void);\nint miku_clamp_int(int value, int minimum, int maximum);\n\n#endif /* MIKU_UTIL_H */');p.write_text(s)
p=Path('include/encoder.h');s=p.read_text().replace('const char *miku_temp_dir(void);\n','');p.write_text(s)
p=Path('include/glyph.h');s=p.read_text().replace('#endif /* MIKU_GLYPH_H */','extern int miku_gamma_lut[256];\nvoid miku_build_gamma_lut(void);\n\n#endif /* MIKU_GLYPH_H */');p.write_text(s)
for file in ['src/render.c','src/terminal_frame.c']:
 p=Path(file);s=p.read_text().replace('#include "util.h"','#include "util.h"\n#include "glyph.h"');p.write_text(s)
for file in ['src/ui.c','src/ui_progress.c','src/layout.c']:
 p=Path(file);s=p.read_text();s=re.sub(r'cols = cols < MIN_ASCII_WIDTH\s*\? MIN_ASCII_WIDTH\s*:\s*\(cols > MAX_ASCII_WIDTH \? MAX_ASCII_WIDTH\s*: cols\);', 'cols = miku_clamp_int(cols, MIN_ASCII_WIDTH, MAX_ASCII_WIDTH);',s)
 s=re.sub(r'cell_w = cell_w < MIN_CELL_WIDTH\s*\? MIN_CELL_WIDTH\s*:\s*\(cell_w > MAX_CELL_WIDTH \? MAX_CELL_WIDTH : cell_w\);','cell_w = miku_clamp_int(cell_w, MIN_CELL_WIDTH, MAX_CELL_WIDTH);',s)
 s=s.replace('frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);','frac = miku_dmax(0, miku_dmin(frac, 1));').replace('bar_w = bar_w > 40 ? 40 : (bar_w < 8 ? 8 : bar_w);','bar_w = miku_clamp_int(bar_w, 8, 40);').replace('fps = fps < 1 ? 1 : (fps > 60 ? 60 : fps);','fps = miku_clamp_int(fps, 1, 60);').replace('Q[st->quality < 0 ? 0 : (st->quality > 4 ? 0 : st->quality)]', 'Q[(unsigned int)st->quality < 5 ? st->quality : 0]')
 if file.endswith('layout.c'):s=s.replace('#include "layout.h"','#include "layout.h"\n#include "util.h"')
 p.write_text(s)
p=Path('src/video.c');s=p.read_text();s=re.sub(r'fps = \(fps_want > 0 && fps_want < src_rounded\)\s*\? fps_want\s*:\s*\(src_rounded > 0 \? src_rounded : 24\);','fps = src_rounded > 0 ? src_rounded : 24;\n                if (fps_want > 0 && fps_want < src_rounded)\n                        fps = fps_want;',s);p.write_text(s)
p=Path('src/ui_picker.c');s=p.read_text().replace('color = focused ? UI_BOLD : (entry->is_dir ? UI_ACCENT : UI_DIM);','color = entry->is_dir ? UI_ACCENT : UI_DIM;\n        if (focused)\n                color = UI_BOLD;');p.write_text(s)
p=Path('src/ui_home.c');s=p.read_text();s=s.replace('const int total = ui_logo_width();','static const char *halves[4] = {" ", "▄", "▀", "█"};\n        const int total = ui_logo_width();');s=re.sub(r'\(t && b\) \? "█"\s*:\s*\(t \? "▀" : \(b \? "▄" : " "\)\)', 'halves[(t << 1) | b]',s);p.write_text(s)
# 简化已转换的返回常量和状态判断，不改变数值约定。
for p in Path('src').glob('*.c'):
 s=p.read_text().replace('return (0) ? 0 : -EIO;', 'return -EIO;').replace('return (1) ? 0 : -EIO;', 'return 0;')
 # 只替换代码标识符，不触碰日志字符串和注释。
 s=TOKEN.sub(lambda m:m[0],s) if False else s
 rename={'Q':'quality_names','L':'layout'}
 token=re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\b[A-Za-z_]\w*\b',re.S)
 s=token.sub(lambda m:rename.get(m[0],m[0]),s)
 p.write_text(s)
# 各模块显式包含实际使用的平台头文件，避免所有模块复制全部系统接口。
includes={
'audio':'errno fcntl math signal stdio stdlib string sys/socket sys/un sys/wait unistd',
'encoder':'dirent errno math stdio stdlib string sys/wait unistd',
'font_data':'stdint', 'layout':'math',
'playback':'errno math signal stdio stdlib string sys/select termios time unistd',
'ui':'math stdio stdlib string time unistd',
'ui_config':'stdarg stdio stdlib string',
'ui_directory':'dirent errno stdio stdlib string sys/stat unistd',
'ui_home':'stdio stdlib string',
'ui_progress':'errno fcntl math stdio stdlib string sys/stat unistd',
'ui_terminal':'signal stdio string sys/select termios unistd',
'ui_text':'stdio stdlib string',
'util':'errno math stdarg stdio stdlib string sys/wait time unistd',
}
for module,items in includes.items():
 p=Path('src',module+'.c');s=p.read_text();s=re.sub(r'^#include <[^>]+>\n','',s,flags=re.M);end=s.rfind('#include "');end=s.index('\n',end)+1;s=s[:end]+'\n'+''.join('#include <'+name+'.h>\n' for name in items.split())+s[end:];p.write_text(s)
