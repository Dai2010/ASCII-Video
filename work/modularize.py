import json
import pathlib
import re
import shutil

root = pathlib.Path('.')
src = pathlib.Path('miku.c').read_bytes()
shutil.copy2('miku.c', 'work/miku-before-modules.c')
shutil.copy2('tests/playback.c', 'work/playback-before-modules.c')
nodes = json.load(open('work/source-ast.json'))
funcs = [n for n in nodes if n['kind'] == 'FunctionDecl']
types = [n for n in nodes if n['kind'] == 'TypedefDecl']
vars_ = [n for n in nodes if n['kind'] == 'VarDecl']
ranges = [(0, 195, 'settings'), (196, 674, 'font_data'),
          (675, 747, 'util'), (748, 1194, 'encoder'),
          (1195, 1357, 'glyph'), (1358, 1556, 'video'),
          (1557, 1676, 'render'), (1677, 1683, 'util'),
          (1684, 1720, 'layout'), (1721, 2041, 'convert'),
          (2042, 2056, 'terminal'), (2057, 2114, 'playback'),
          (2115, 2128, 'util'), (2129, 2141, 'terminal'),
          (2142, 2147, 'util'), (2148, 2354, 'audio'),
          (2355, 2399, 'strbuf'), (2400, 2650, 'playback'),
          (2651, 2735, 'ui_terminal'), (2736, 2865, 'ui_text'),
          (2866, 3000, 'ui_terminal'), (3001, 3158, 'ui_home'),
          (3159, 3676, 'ui_picker'), (3677, 3981, 'ui_config'),
          (3982, 4325, 'ui_progress'), (4326, 4478, 'ui'),
          (4479, 9999, 'main')]

def line_of(n):
        return src[:n['range']['begin']['offset']].count(b'\n') + 1

def owner(n):
        line = line_of(n)
        return next(m for a, b, m in ranges if a <= line <= b)

def extract(n):
        r = n['range']
        a = r['begin']['offset']
        b = r['end']['offset'] + r['end']['tokLen']
        if n['kind'] != 'FunctionDecl':
                b = src.index(b';', b) + 1
        return src[a:b].decode()

# 所有需要跨模块共享的类型集中定义，消除匿名结构体 typedef。
type_names = {'Tier':'tier', 'EncoderCand':'encoder_candidate',
              'Profile':'profile', 'GlyphAtlas':'glyph_atlas',
              'VideoInfo':'video_info', 'Layout':'layout',
              'ConvertHooks':'convert_hooks', 'ConvertOutcome':'convert_outcome',
              'StrBuf':'strbuf', 'DirEntry':'directory_entry',
              'Config':'ui_config', 'Progress':'ui_progress'}
rename = {old:'struct '+new for old,new in type_names.items()}
rename.update({n['name']:n['name'].lower() for n in vars_ if n['name'].isupper()})
# 宏仍为大写，函数和变量保持小写加下划线。
lex = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\b[A-Za-z_]\w*\b', re.S)
def rewrite(s):
        return lex.sub(lambda m: rename.get(m[0], m[0]), s)

# 原始宏含颜色、字体尺寸和界面键值；这里仅存值，不存实现或可变状态。
lines = src.decode().splitlines()
macros = []
for i,line in enumerate(lines):
        if line.startswith('#define ') and not line.startswith(('#define _POSIX', '#define _DEFAULT')):
                macros.append(line)
# 宏中的 ENCODER_COUNT 原本依赖 sizeof 私有数组，改成候选表的明确元素数。
macros = [re.sub(r'#define ENCODER_COUNT .*', '#define ENCODER_COUNT 9', x) for x in macros]
macros = [re.sub(r'#define UI_LYRIC_COUNT .*', '#define UI_LYRIC_COUNT 10', x) for x in macros]
# 歌词数由实际声明的初始化项核实。
lyric = next(n for n in vars_ if n['name'] == 'UI_LYRICS')
count = len(next(x for x in lyric['inner'] if x['kind']=='InitListExpr')['inner'])
macros = [re.sub(r'#define UI_LYRIC_COUNT .*', f'#define UI_LYRIC_COUNT {count}', x) for x in macros]

enum_match = re.search(r'enum \{ CFG_INPUT.*?\};', src.decode(), re.S)[0]
modules = {}
for n in funcs + vars_:
        mod = owner(n)
        if mod == 'settings':
                mod = 'encoder'
        modules.setdefault(mod, []).append(n)
# 只导出其他编译单元真正引用的符号。
names = {n['name']:mod for mod,ns in modules.items() for n in ns}
refs = {}
for mod,ns in modules.items():
        text = '\n'.join(extract(n) for n in ns)
        tokens = set(re.findall(r'\b[A-Za-z_]\w*\b', re.sub(r'/\*.*?\*/|"(?:\\.|[^"\\])*"', '', text, flags=re.S)))
        refs[mod] = {name for name in tokens if name in names and names[name] != mod}
export = set().union(*refs.values())
for n in funcs:
        if n['name'] in export:
                rename[n['name']] = 'miku_' + n['name']
for n in vars_:
        if n['name'] in export:
                rename[n['name']] = 'miku_' + n['name'].lower()

preamble = '/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n'
def header(name, body):
        guard = 'MIKU_' + name.upper() + '_H'
        pathlib.Path('include', name+'.h').write_text(preamble+f'\n#ifndef {guard}\n#define {guard}\n\n'+body+f'\n#endif /* {guard} */\n')
pathlib.Path('src').mkdir(exist_ok=True)
pathlib.Path('include').mkdir(exist_ok=True)
header('constants', '\n'.join(rewrite(x) for x in macros)+'\n\n'+enum_match+'\n')
body = '#include <stddef.h>\n#include <stdint.h>\n#include "constants.h"\n\n'
for n in types:
        old = n['name']
        declaration = extract(n)
        declaration = declaration.replace('typedef struct {', 'struct '+type_names[old]+' {', 1)
        declaration = re.sub(r'\}\s*'+old+r'\s*;', '};', declaration)
        body += rewrite(declaration)+'\n\n'
header('types', body)
# 平台接口由各源文件自行包含；功能宏由构建选项统一设置。
stdlib = '\n'.join(lines[48:68])+'\n'
for mod,ns in modules.items():
        ns.sort(key=lambda n:n['range']['begin']['offset'])
        hbody = '#include "types.h"\n#include <signal.h>\n#include <sys/types.h>\n\n'
        chunks = []
        for n in ns:
                text = extract(n)
                if n['name'] in export:
                        text = re.sub(r'^static\s+', '', text)
                        if n['kind'] == 'FunctionDecl':
                                hbody += rewrite(text[:text.index('{')].strip())+';\n'
                        else:
                                declaration = text.split('=',1)[0].strip().rstrip(';')
                                hbody += 'extern '+rewrite(declaration)+';\n'
                chunks.append(rewrite(text))
        header(mod, hbody)
        deps = sorted({names[n] for n in refs[mod]})
        include = '#include "'+mod+'.h"\n'+''.join('#include "'+d+'.h"\n' for d in deps)
        # Font license provenance must travel with the bitmap data.
        note = ''
        if mod == 'font_data':
                note = '/*\n * 字体数据来自原项目内嵌的 DejaVu Sans Mono 位图。\n * 许可为 Bitstream Vera；保留原字体来源，不重新生成字形。\n * 共 95 个 ASCII 字符，每字形 32 行，每行 16 位。\n */\n'
        pathlib.Path('src',mod+'.c').write_text(preamble+'\n'+include+'\n'+stdlib+'\n'+note+'\n\n'.join(chunks)+'\n')
# 保存拆分清单，为后续接口契约和注释核查提供依据。
json.dump({'export':sorted(export),'owners':names,'renames':rename},open('work/modules.json','w'),ensure_ascii=False,indent=2)
