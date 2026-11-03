from pathlib import Path
p=Path('include/types.h');s=p.read_text()
s=s.replace('int cols, rows;\t    /* 网格的字符列数、行数 */','int cols; /* 网格列数，单位字符格。 */\n        int rows; /* 网格行数，单位字符格。 */').replace('int cell_w, cell_h; /* 每个字符的像素大小 */','int cell_w; /* 格子宽度，单位像素。 */\n        int cell_h; /* 格子高度，单位像素。 */').replace('int off_x, off_y;   /* 网格左上角在画布中的位置（居中，取偶数） */','int off_x; /* 水平方向黑边偏移，单位像素，取偶数。 */\n        int off_y; /* 垂直方向黑边偏移，单位像素，取偶数。 */').replace('long long done, total, out_frames;', 'long long done; /* 已消费的源帧数。 */\n        long long total; /* 源总帧数，零表示未知。 */\n        long long out_frames; /* 已写入编码器的输出帧数。 */').replace('double elapsed, eta;', 'double elapsed; /* 从开始转换起经过的单调时钟秒数。 */\n        double eta; /* 预计剩余秒数，负数表示未知。 */')
# 每个共享结构先解释生命周期，再为字段注明用途与单位。
structures={
'tier':('预设质量档位，字符串和表项具有静态生命周期。', {'name':'静态档位名。','width':'输出宽度，单位像素。','height':'输出高度，单位像素。','fps':'默认输出帧率。','min_mem_mb':'选择此档位所需最低物理内存 MiB。'}),
'encoder_candidate':('编码器候选，所有字符串均借用静态表；NULL 表示无附加参数。', {'codec':'ffmpeg 编码器名称。','fixed':'固定编码参数，NULL 时按像素率估算。','pipe_filter':'追加到 RGB 转换之后的滤镜片段。','input_prefix':'必须放在输入前的参数，VAAPI 运行时另行解析。','hardware':'非零表示硬件编码。','vaapi':'非零表示必须先选择 VAAPI 渲染设备。'}),
'profile':('一次转换最终采用的规格；enc 借用静态候选，不能释放。', {'name':'供界面和日志显示的档位名称。','width':'实际输出宽度，单位像素。','height':'实际输出高度，单位像素。','fps':'实际输出帧率，不高于有效源帧率。','enc':'已通过真实短片探测的编码器候选。'}),
'glyph_atlas':('字形蒙版和亮度索引；masks 由建图分配，调用方最终 free。', {'cell_w':'单个字形宽度，单位像素。','cell_h':'单个字形高度，单位像素。','masks':'按墨量升序排列的连续灰度字形，归调用方所有。','chars':'与 masks 同序的 ASCII 字符，包含结尾 NUL。','lut':'输入亮度 0..255 到字形索引的映射。'}),
'video_info':('探测结果，不持有动态内存；帧率以正有理数保存。', {'width':'解码并应用旋转后的宽度，单位像素。','height':'解码并应用旋转后的高度，单位像素。','fps_num':'源帧率分子。','fps_den':'源帧率分母，探测成功时为正数。','total_frames':'源总帧数或根据时长的估计，零表示未知。'}),
'layout':('画布中的字符网格与偶数黑边偏移，不持有内存。',{}),
'progress_update':('同步回调的只读快照；回调返回后不得保存其地址。', {'done':'已经读取的源帧数。','total':'源总帧数，零表示未知。','output_frames':'已经写入的输出帧数。','elapsed':'已经过时间，单位秒。','eta':'预计剩余时间，负数表示未知。'}),
'convert_hooks':('回调和 ud 均由调用方持有；转换过程同步调用，不创建回调线程。', {'ud':'回调上下文，必须覆盖整个转换调用。','on_progress':'可选进度回调，update 仅在调用期间有效。','should_abort':'可选取消谓词，非零表示取消本次转换。'}),
'convert_outcome':('返回实际规格和源信息；编码器指针借用静态候选，不需单独释放。', {'prof':'最终成功或正在尝试的输出规格。','info':'已经探测的源媒体信息。'}),
'strbuf':('可复用字节缓冲；buf 唯一拥有，len<=cap，调用方最终 free。', {'buf':'分配的字节存储，可为 NULL。','len':'已使用字节，不含可选结尾 NUL。','cap':'已分配字节容量。','error':'首个负 errno；提交时必须检查，不输出半帧。'}),
'directory_entry':('单条目录快照，名称保存在记录内部，不依赖 readdir 的临时指针。', {'name':'目录项名称，NUL 结尾。','is_dir':'非零表示目录，目录优先排序。','size':'普通文件字节数，目录为零。'}),
'ui_config':('转换和播放页面各保存一份，切换页面不会混用输入文件。', {'input':'用户输入路径，UTF-8，NUL 结尾。','output':'输出路径，空串表示自动派生。','style':'STYLE_CHAR 或 STYLE_BLOCK。','ratio':'比例索引，零表示源比例。','fps':'指定帧率，零表示跟随源。','quality':'0 自动、1 原画、2 高、3 中、4 低。','cols_want':'播放列数，零表示跟随终端。','mute':'非零时不启动音轨。','focus':'当前页面的零基焦点槽位。','status':'状态栏提示，固定容量，NUL 结尾。'}),
'ui_progress':('一次转换的界面上下文，借用配置和结果，生命周期覆盖同步转换。', {'res':'借用的转换输出对象。','st':'借用的页面配置，只读。','width_cols':'本次转换采用的字符列数。','style':'本次转换画面风格。','last_draw':'上次提交进度屏幕的单调时钟秒数。','cancelled':'非零表示用户取消，禁止软件回退重试。'}),
}
import re
for name,(note,fields) in structures.items():
 pattern=r'struct '+name+r' \{(.*?)\n\};';match=re.search(pattern,s,re.S)
 if not match:raise ValueError(name)
 body=match[1];lines=body.splitlines();new=[]
 for line in lines:
  field=re.search(r'([A-Za-z_]\w*)(?:\[[^]]+\])?\s*;',line)
  if not field:
   field=re.search(r'\(\*([A-Za-z_]\w*)\)',line)
  if field and field[1] in fields:
   line=re.sub(r'\s*/\*.*?\*/','',line)
   line+=' /* '+fields[field[1]]+' */'
  new.append(line)
 replacement='/* '+note+' */\nstruct '+name+' {'+'\n'.join(new)+'\n};'
 s=s[:match.start()]+replacement+s[match.end():]
p.write_text(s)
