import collections
import json
import pathlib
import re
import subprocess

FLAGS = ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Iinclude', '-std=c17']

def decl(typ, name):
        if '(*)' in typ:
                return typ.replace('(*)', '(*'+name+')')
        if '[' in typ:
                i = typ.index('[')
                return typ[:i].rstrip()+' '+name+typ[i:]
        return typ+' '+name

def unconst(typ):
        if '*' not in typ:
                return re.sub(r'\bconst\s*', '', typ)
        return re.sub(r'\*const\b', '*', typ)

def walk(n):
        yield n
        for c in n.get('inner', []):
                yield from walk(c)

for path in sorted(pathlib.Path('src').glob('*.c')):
        raw = path.read_bytes()
        ast = json.loads(subprocess.check_output(['clang', *FLAGS, '-Xclang', '-ast-dump=json', '-fsyntax-only', str(path)], stderr=subprocess.DEVNULL))
        functions = [n for n in ast['inner'] if n.get('kind', '')=='FunctionDecl' and 'includedFrom' not in n.get('loc',{}) and any(c['kind']=='CompoundStmt' for c in n.get('inner',[]))]
        patches=[]
        for fn in functions:
                body = next(c for c in fn['inner'] if c['kind']=='CompoundStmt')
                a=fn['range']['begin']['offset']; b=body['range']['end']['offset']+1
                text=raw[a:b].decode()
                # 保持短函数的局部结构；只拆分长函数或深嵌套函数。
                def depth(n,d=0):
                        inc=n.get('kind', '') in ('IfStmt','ForStmt','WhileStmt','DoStmt','SwitchStmt')
                        return max([d+inc]+[depth(c,d+inc) for c in n.get('inner',[])])
                if text.count('\n') < 65 and depth(body)<=3:
                        continue
                name=fn['name']; tag=name.removeprefix('miku_')+'_context'
                state={}; statics=[]; counts=collections.Counter(); fields=[]; edits=[]
                def span(n):
                        r=n['range']; return r['begin']['offset'],r['end']['offset']+r['end']['tokLen']
                def original(n):
                        x,y=span(n); return raw[x:y].decode()
                def variable(n,param=False):
                        old=n['name']; counts[old]+=1
                        field=old.lower()+(('_scope_'+str(counts[old])) if counts[old]>1 else '')
                        typ=n['type']['qualType']; storage=n.get('storageClass')
                        if storage=='static':
                                target=name+'_'+field
                                init=n.get('init')
                                expr=n.get('inner',[])[-1] if init else None
                                statics.append('static '+decl(typ,target)+((' = '+original(expr)) if expr else '')+';')
                                state[n['id']]=target
                        else:
                                state[n['id']]='ctx->'+field
                                fields.append((unconst(typ),field,n,param))
                for n in fn.get('inner',[]):
                        if n.get('kind', '')=='ParmVarDecl':variable(n,True)
                for n in walk(body):
                        if n.get('kind')=='VarDecl': variable(n)
                for n in walk(body):
                        if n.get('kind', '')=='DeclRefExpr' and n.get('referencedDecl',{}).get('id') in state:
                                x,y=span(n); edits.append((x,y,state[n['referencedDecl']['id']]))
                def rewrite(x,y):
                        data=raw[x:y].decode()
                        for e,f,v in sorted([p for p in edits if x<=p[0] and p[1]<=y],reverse=True):
                                # UTF-8 offsets cannot index Python strings.
                                buf=data.encode();data=(buf[:e-x]+v.encode()+buf[f-x:]).decode()
                        return data
                def expr(n):
                        x,y=span(n);return rewrite(x,y)
                def decl_stmt(n):
                        result=[]
                        for v in n.get('inner',[]):
                                if v['kind']!='VarDecl' or v.get('storageClass')=='static':continue
                                if 'init' not in v:continue
                                init=v['inner'][-1];typ=unconst(v['type']['qualType']);target=state[v['id']]
                                value=expr(init)
                                if '[' in typ:
                                        result.append('memcpy('+target+', ('+typ+'){'+value.strip('{}')+'}, sizeof('+target+'));')
                                elif value.startswith('{'):
                                        result.append(target+' = ('+typ+')'+value+';')
                                else:result.append(target+' = '+value+';')
                        return '\n'.join(result) or ';'
                helpers=[]; serial=0; ret=fn['type']['qualType'].split(' (')[0]
                phases=['prepare','configure','process','finish','release','complete','dispatch','resume','advance','finalize']
                def call(helper,root,loops,breaks):
                        code='ctx->flow = '+helper+'(ctx);\n'
                        code+='if (ctx->flow == MIKU_FLOW_RETURN)\n'
                        code+=('return'+(' ctx->result' if ret!='void' else '')+';\n') if root else 'return MIKU_FLOW_RETURN;\n'
                        if loops:code+='if (ctx->flow == MIKU_FLOW_CONTINUE)\ncontinue;\n'
                        elif not root:code+='if (ctx->flow == MIKU_FLOW_CONTINUE)\nreturn MIKU_FLOW_CONTINUE;\n'
                        if breaks:code+='if (ctx->flow == MIKU_FLOW_BREAK)\nbreak;\n'
                        elif not root:code+='if (ctx->flow == MIKU_FLOW_BREAK)\nreturn MIKU_FLOW_BREAK;\n'
                        if root and 'goto cleanup' in text:
                                code+='/* 跳到 out_cleanup，统一关闭管道并释放已取得资源。 */\nif (ctx->flow == MIKU_FLOW_CLEANUP)\ngoto out_cleanup;\n'
                        elif not root:code+='if (ctx->flow == MIKU_FLOW_CLEANUP)\nreturn MIKU_FLOW_CLEANUP;\n'
                        return code
                def make_helper(code):
                        nonlocal_dummy=None
                        global_unused=None
                        idx=len(helpers)
                        # 名称由原函数名和职责阶段组成，保证不同提取点不重名。
                        used=set(h[0] for h in helpers)
                        candidate=next((name+'_'+p for p in phases if name+'_'+p not in used),name+'_stage_'+str(idx))
                        helpers.append((candidate,code))
                        return candidate
                def render(n,root=False,loops=0,breaks=0,allow_extract=True):
                        kind=n.get('kind', '')
                        if kind=='DeclStmt':return decl_stmt(n)
                        if kind=='ReturnStmt' and not root:
                                children=n.get('inner',[])
                                return (('ctx->result = '+expr(children[0])+';\n') if children else '')+'return MIKU_FLOW_RETURN'
                        if kind=='BreakStmt' and not breaks:return 'return MIKU_FLOW_BREAK'
                        if kind=='ContinueStmt' and not loops:return 'return MIKU_FLOW_CONTINUE'
                        if kind=='GotoStmt':return 'return MIKU_FLOW_CLEANUP'
                        if kind=='LabelStmt':
                                return '/* 退出路径：关闭管道并释放本次操作持有的缓冲区。 */\nout_cleanup:\n'+render(n['inner'][0],root,loops,breaks)
                        if kind=='CompoundStmt':
                                pieces=[]
                                children=n.get('inner',[])
                                for c in children:
                                        piece=render(c,root,loops,breaks)
                                        if not piece.rstrip().endswith(('}', ';')):piece+=';'
                                        pieces.append(piece)
                                code='\n'.join(pieces)
                                if allow_extract and (code.count('\n')>38 or depth(n)>3):
                                        chunks=[];current=[];size=0
                                        for c,piece in zip(children,pieces):
                                                # 清理标签必须留在原函数中，不能跨函数跳转。
                                                if c['kind']=='LabelStmt':
                                                        if current:chunks.append(current);current=[];size=0
                                                        chunks.append([piece]);continue
                                                if size+piece.count('\n')>32 and current:
                                                        chunks.append(current);current=[];size=0
                                                current.append(piece);size+=piece.count('\n')+1
                                        if current:chunks.append(current)
                                        calls=[]
                                        for chunk in chunks:
                                                chunkcode='\n'.join(chunk)
                                                if 'out_cleanup:' in chunkcode:calls.append(chunkcode);continue
                                                # Helper 内的返回语句需改为流程状态。
                                                if root:
                                                        # 重新以 helper 语义渲染相同的语句。
                                                        indices=[pieces.index(p) for p in chunk]
                                                        chunkcode='\n'.join(render(children[i],False,0,0,False).rstrip(';')+';' if children[i]['kind'] in ('ReturnStmt','BreakStmt','ContinueStmt','GotoStmt') else render(children[i],False,0,0,False) for i in indices)
                                                helper=make_helper(chunkcode)
                                                calls.append(call(helper,root,loops,breaks))
                                        code='\n'.join(calls)
                                return '{\n'+code+'\n}'
                        x,y=span(n)
                        replacements=[]
                        childloops=loops+(kind in ('ForStmt','WhileStmt','DoStmt'))
                        childbreaks=breaks+(kind in ('ForStmt','WhileStmt','DoStmt','SwitchStmt'))
                        for c in n.get('inner',[]):
                                if 'range' not in c or 'offset' not in c['range']['begin']:continue
                                ca,cb=span(c)
                                if ca<x or cb>y:continue
                                if c.get('kind','').endswith('Stmt'):
                                        replacements.append((ca,cb,render(c,root,childloops,childbreaks)))
                        code=raw[x:y]
                        localedits=[p for p in edits if x<=p[0] and p[1]<=y and not any(ca<=p[0] and p[1]<=cb for ca,cb,_ in replacements)]
                        for ca,cb,value in sorted(replacements+localedits,reverse=True):code=code[:ca-x]+value.encode()+code[cb-x:]
                        return code.decode()
                # 根函数的所有语句最终在 helper 里执行；根函数仅负责栈状态与清理分派。
                generated=render(body,True,0,0)
                if not helpers:
                        continue
                # render 根函数时原返回仍须访问 ctx 字段。
                fields_text='\n'.join(decl(t,f)+';' for t,f,n,p in fields)
                if ret!='void':fields_text+='\n'+decl(ret,'result')+';'
                fields_text+='\nenum miku_flow flow;'
                definition='\n'.join(statics)+'\nstruct '+tag+' {\n'+fields_text+'\n};\n\n'
                for h,code in helpers:
                        definition+='static enum miku_flow '+h+'(struct '+tag+' *ctx)\n{\n'+code+'\nreturn MIKU_FLOW_NEXT;\n}\n\n'
                signature=raw[a:body['range']['begin']['offset']].decode()
                init='struct '+tag+' storage = {0};\nstruct '+tag+' *ctx = &storage;\n'
                for t,f,n,param in fields:
                        if param:init+='ctx->'+f+' = '+n['name']+';\n'
                generated=generated[:1]+'\n'+init+generated[1:]
                # 在根函数最后必须保留原返回；FLOW_NEXT 只在正常落入末尾时出现。
                definition+=signature+generated
                patches.append((a,b,definition))
        for a,b,value in sorted(patches,reverse=True):raw=raw[:a]+value.encode()+raw[b:]
        path.write_bytes(raw)
