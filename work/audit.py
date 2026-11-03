import json
import pathlib
import subprocess

FLAGS = ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Iinclude', '-std=c17']

def walk(node):
        yield node
        for child in node.get('inner', []):
                yield from walk(child)

def nesting(node, level=0, chain=False):
        kind = node.get('kind')
        level += kind in ('ForStmt', 'WhileStmt', 'DoStmt', 'SwitchStmt') or (
                kind == 'IfStmt' and not chain
        )
        children = node.get('inner', [])
        values = [level]
        for index, child in enumerate(children):
                same_chain = (kind == 'IfStmt' and node.get('hasElse') and
                              index == len(children) - 1 and
                              child.get('kind') == 'IfStmt')
                values.append(nesting(child, level, same_chain))
        return max(values)

for path in sorted(pathlib.Path('src').glob('*.c')):
        raw = path.read_bytes()
        result = subprocess.run(['clang', *FLAGS, '-Xclang', '-ast-dump=json', '-fsyntax-only', str(path)], capture_output=True, check=True)
        ast = json.loads(result.stdout)
        for fn in ast['inner']:
                if fn.get('kind') != 'FunctionDecl' or 'includedFrom' in fn.get('loc', {}):
                        continue
                body = next((c for c in fn.get('inner', []) if c.get('kind') == 'CompoundStmt'), None)
                if not body:
                        continue
                begin = fn['range']['begin']
                a = begin.get('offset', begin.get('expansionLoc', {}).get('offset'))
                if a is None:
                        continue
                b = body['range']['end']['offset']
                lines = raw[a:b].count(b'\n') + 1
                depth = nesting(body)
                locals_ = sum(n.get('kind') == 'VarDecl' for n in walk(body))
                args = sum(n.get('kind') == 'ParmVarDecl' for n in fn.get('inner', []))
                if lines > 80 or depth > 3 or args > 5 or locals_ > 10:
                        print(f'{path}:{fn["loc"].get("line", 0)} {fn["name"]}: lines={lines}, depth={depth}, locals={locals_}, args={args}')
