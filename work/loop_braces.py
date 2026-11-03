import json
from pathlib import Path
import subprocess

controls = {'IfStmt','ForStmt','WhileStmt','DoStmt','SwitchStmt'}

def walk(node):
        yield node
        for child in node.get('inner',[]):
                yield from walk(child)

for path in Path('src').glob('*.c'):
        raw=path.read_bytes()
        result=subprocess.run(['clang','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-Iinclude','-std=c17','-Xclang','-ast-dump=json','-fsyntax-only',str(path)],capture_output=True,check=True)
        ast=json.loads(result.stdout)
        edits=[]
        for fn in ast['inner']:
                if fn.get('kind')!='FunctionDecl' or 'includedFrom' in fn.get('loc',{}):continue
                for node in walk(fn):
                        if node.get('kind') not in ('ForStmt','WhileStmt','DoStmt'):continue
                        children=node.get('inner',[])
                        body=children[0] if node['kind']=='DoStmt' else children[-1]
                        if body.get('kind') not in controls:continue
                        begin=body['range']['begin'];end=body['range']['end']
                        if 'offset' not in begin or 'offset' not in end:continue
                        a=begin['offset'];b=end['offset']+end['tokLen']
                        if raw[b:b+1]==b';':b+=1
                        edits.extend([(a,b'{\n'),(b,b'\n}')])
        for at,text in sorted(edits,reverse=True):raw=raw[:at]+text+raw[at:]
        path.write_bytes(raw)
p=Path('.clang-format');s=p.read_text().replace('RemoveBracesLLVM: true','RemoveBracesLLVM: false');p.write_text(s)
