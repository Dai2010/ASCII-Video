import json
import pathlib
import subprocess
import re
from edit_helpers import function_span

status_names = {
        'miku_stamp_bt709', 'miku_start_audio', 'miku_parse_custom_size',
        'miku_choose_profile', 'miku_choose_profile_ex', 'miku_probe_video',
        'miku_sb_reserve', 'miku_sb_addf', 'miku_ui_convert', 'miku_ui_load_dir',
}
flags = ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Iinclude', '-std=c17']

def visit(node, parent=None):
        yield node, parent
        for child in node.get('inner', []):
                yield from visit(child, node)

# 只改变已核对为真假值的跨模块接口；将调用方的真假判断显式保持下来。
for path in list(pathlib.Path('src').glob('*.c')) + [pathlib.Path('tests/playback.c')]:
        raw = path.read_bytes()
        result = subprocess.run(['clang', *flags, '-Xclang', '-ast-dump=json', '-fsyntax-only', str(path)], capture_output=True, check=True)
        ast = json.loads(result.stdout)
        inserts = []
        for fn in ast['inner']:
                if fn.get('kind') != 'FunctionDecl' or 'includedFrom' in fn.get('loc', {}):
                        continue
                for node, parent in visit(fn):
                        if node.get('kind') != 'CallExpr':
                                continue
                        callee = node.get('inner', [{}])[0]
                        names = [n.get('referencedDecl', {}).get('name') for n, _ in visit(callee)]
                        if not any(name in status_names for name in names):
                                continue
                        if parent and parent.get('kind') == 'CompoundStmt':
                                continue
                        r = node['range']
                        a = r['begin'].get('offset')
                        b = r['end'].get('offset')
                        if a is None or b is None:
                                raise ValueError('接口调用包含未处理的宏位置')
                        b += r['end']['tokLen']
                        inserts.extend([(a, '('), (b, ' == 0)')])
        for at, text in sorted(inserts, reverse=True):
                raw = raw[:at] + text.encode() + raw[at:]
        path.write_bytes(raw)

# 从已知 0/1 约定转为 0/负 errno；接口返回类型和调用方已同步。
for path in pathlib.Path('src').glob('*.c'):
        text = path.read_text()
        for name in status_names:
                try:
                        a, b = function_span(text, name)
                except ValueError:
                        continue
                piece = text[a:b]
                piece = re.sub(r'\breturn\s+([^;]+);', r'return (\1) ? 0 : -EIO;', piece)
                text = text[:a] + piece + text[b:]
        path.write_text(text)
