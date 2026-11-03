import json
import pathlib
import re
import subprocess

FLAGS = ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Iinclude', '-std=c17']

def position(loc):
        return loc.get('offset', loc.get('expansionLoc', {}).get('offset'))

def span(node):
        r = node['range']
        a = position(r['begin'])
        end = r['end']
        b = position(end)
        token = end.get('tokLen', end.get('expansionLoc', {}).get('tokLen', 1))
        if a is None or b is None:
                raise ValueError('声明缺少明确源码位置')
        return a, b + token

def declaration(typ, name):
        if '(*)' in typ:
                return typ.replace('(*)', '(*'+name+')')
        if '[' in typ:
                base, suffix = typ.split('[', 1)
                return base.rstrip()+' '+name+'['+suffix
        return typ+' '+name

def mutable_type(typ):
        if '*' not in typ:
                return re.sub(r'\bconst\s*', '', typ)
        return re.sub(r'\*const\b', '*', typ)

for path in list(pathlib.Path('src').glob('*.c')) + list(pathlib.Path('tests').glob('*.c')):
        raw = path.read_bytes()
        result = subprocess.run(['clang', *FLAGS, '-Xclang', '-ast-dump=json', '-fsyntax-only', str(path)], capture_output=True, check=True)
        ast = json.loads(result.stdout)
        groups = {}
        moved = {}
        replacements = []
        names = {}
        refs = []
        def walk(node, block=None, parent=None):
                kind = node.get('kind')
                if kind == 'CompoundStmt':
                        block = node
                        groups[id(block)] = {'node': block, 'decls': [], 'names': set()}
                        # 保留块开头原有的初始化时序，但拆成单项声明。
                        initial = True
                        for child in node.get('inner', []):
                                if child.get('kind') != 'DeclStmt':
                                        initial = False
                                child['_initial'] = initial
                if kind == 'DeclStmt' and block:
                        entries = [v for v in node.get('inner', []) if v.get('kind') == 'VarDecl']
                        if not entries:
                                return
                        group = groups[id(block)]
                        initial = node.get('_initial', False) and parent.get('kind') != 'ForStmt'
                        assigns = []
                        declarations = []
                        for variable in entries:
                                typ = variable['type']['qualType']
                                name = variable['name']
                                original_name = name
                                while name in group['names']:
                                        name += '_next'
                                group['names'].add(name)
                                if name != original_name:
                                        names[variable['id']] = name
                                storage = variable.get('storageClass')
                                initializer = variable.get('inner', [])[-1] if 'init' in variable else None
                                value = None
                                if initializer:
                                        a, b = span(initializer)
                                        value = raw[a:b].decode()
                                if initial or storage == 'static' or ('[' in typ and value is not None):
                                        text = (storage+' ' if storage else '')+declaration(typ, name)
                                        if value is not None:
                                                text += ' = '+value
                                        text += ';'
                                        if initial:
                                                declarations.append(text)
                                        else:
                                                group['decls'].append(text)
                                else:
                                        typ = mutable_type(typ)
                                        group['decls'].append(declaration(typ, name)+';')
                                        if value is not None:
                                                if value.lstrip().startswith('{'):
                                                        value = '('+typ+')'+value
                                                assigns.append(name+' = '+value+';')
                        a, b = span(node)
                        replacement = '\n'.join(declarations + assigns)
                        if parent.get('kind') == 'ForStmt' and not replacement:
                                replacement = ';'
                        replacements.append((a, b, replacement))
                        return
                if kind == 'DeclRefExpr':
                        refs.append(node)
                for child in node.get('inner', []):
                        walk(child, block, node)
        for fn in ast['inner']:
                if fn.get('kind') == 'FunctionDecl' and 'includedFrom' not in fn.get('loc', {}):
                        for node in fn.get('inner', []):
                                if node.get('kind') == 'CompoundStmt':
                                        walk(node)
        # 重复循环变量仅重命名其自身引用，不改其他作用域的同名变量。
        for node in refs:
                name = names.get(node.get('referencedDecl', {}).get('id'))
                if name:
                        a, b = span(node)
                        if not any(x <= a and b <= y for x, y, _ in replacements):
                                replacements.append((a, b, name))
        for group in groups.values():
                if group['decls']:
                        a, _ = span(group['node'])
                        replacements.append((a+1, a+1, '\n'+'\n'.join(group['decls'])+'\n'))
        for a,b,text in sorted(replacements, reverse=True):
                raw = raw[:a]+text.encode()+raw[b:]
        path.write_bytes(raw)
