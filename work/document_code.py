import json
import pathlib
import re
import subprocess
import unicodedata

FLAGS = ['-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-Iinclude','-std=c17']
DESC = {
'miku_has_audio_stream':'探测第一条音轨是否存在。只检查可见流，不启动音频播放。',
'miku_start_audio':'优先启动 mpv 的纯音频子进程，缺失时回退 ffplay。成功只表示 fork 完成，播放器的后续错误通过 IPC 可用性体现。',
'miku_stop_audio':'终止并等待本模块启动的音频子进程。重复调用不会误杀其他进程，不取得外部进程的所有权。',
'audio_ipc_open':'按需连接本次播放的 Unix 套接字。已经连接时直接复用，失败不阻塞等待播放器启动。',
'audio_parse_position':'仅接受当前请求编号的数值位置。事件、null 和非有限数不能用来校时，避免时钟错误回到零。',
'miku_audio_time_pos':'非阻塞读取完整 JSON 行并推进一个在途查询。最多每 50ms 发送一次，半行保留到下次调用；超时连接会被丢弃重建。',
'miku_audio_ipc_close':'关闭 IPC 并清空未完成请求、半行缓存及查询时间。音频子进程的终止另由 stop_audio 负责。',
'miku_stamp_bt709':'用无损流复制补写 BT.709 色彩标签。临时文件完成后才替换输出；失败清理临时文件并保留原输出。',
'miku_total_memory_mb':'读取可用的物理内存信息，Linux 优先使用 /proc/meminfo，其他平台尝试现有系统接口。探测失败返回未知值。',
'miku_cpu_cores':'优先读取正数 MIKU_CORES 覆盖值，否则询问在线处理器数，失败保守使用一个核心。',
'encoder_listed_by_name':'检查 ffmpeg 的编码器列表是否包含完整的编码器名称。列出不等于能编码，实际可用性还需短片探测。',
'miku_encoder_listed':'按候选索引查询并缓存 ffmpeg 是否列出编码器。缓存只表示注册情况，不替代实际编码探测。',
'hw_bitrate_kbps':'按像素率估计硬件编码目标码率，并夹紧到预设的最低和最高 kbps。',
'x264_args':'读取 MIKU_PRESET/MIKU_CRF 覆盖项，未设置时使用项目默认值；拼接软件编码器参数并返回拥有的字符串。',
'miku_encoder_args':'为指定尺寸生成候选编码器的参数。x264 使用质量参数，其他有固定参数者直接复制，否则根据像素率估算码率。',
'miku_cand_input_prefix':'返回编码器必须置于输入之前的参数；VAAPI 按本机设备扫描结果提供前缀，返回值均为借用。',
'count_decodable_frames':'解码探测文件并读取实际帧数。不能仅凭编码器退出码认定输出有效，损坏或空结果返回负计数。',
'miku_encoder_works':'实际编码少量黑帧再解码计数。两个步骤均成功才接受候选，所有探测命令、临时文件和路径均在返回前释放。',
'miku_pick_encoder':'按候选表顺序返回首个实际可用编码器。MIKU_NO_HW 非零时跳过硬件项；返回静态表内借用指针。',
'profile_cpu':'由每秒像素率、软件或硬件系数估计每秒视频消耗的 CPU 秒数，供自动档位预算判断。',
'miku_output_tail_ok':'抽查输出末尾两秒的可解码帧数，至少需要一秒输出帧数。用来发现部分硬件编码器中途漏帧。',
'miku_parse_custom_size':'解析 WxH 或 WxH@FPS。宽高必须为支持范围内的偶数，省略帧率时写入零，表示跟随源帧率。',
'miku_choose_profile':'解析显式尺寸或档位，或执行自动预算选择；所有结果都必须匹配实际可用的编码器，输出帧率不超过有效源帧率。',
'miku_compute_layout':'计算画布内居中的字符网格。字符高度为宽度两倍，网格超出高度时反推列数，黑边偏移按偶数对齐。',
'play_restore_term':'恢复播放前保存的 termios、颜色和光标。原始模式标志避免重复恢复覆盖下一阶段的终端状态。',
'play_on_signal':'信号处理器只置停止标志，不执行内存、I/O 或等待子进程等不安全操作。实际清理由播放主循环完成。',
'play_raw_mode':'保存当前终端属性并关闭规范输入及回显。仅在设置成功后标记原始模式，失败时不伪造可恢复状态。',
'play_poll_key':'非阻塞读取单个播放按键。终端未进入原始模式或没有可读字节时返回零。',
'miku_terminal_columns':'优先从标准输出读取终端列数，失败再尝试标准输入；两者都不可用时返回零，布局方自行选择默认值。',
'miku_terminal_rows':'优先从标准输出读取终端行数，失败再尝试标准输入；返回零表示当前无法获得尺寸。',
'miku_ui_status':'在配置状态对象中格式化提示，不直接输出终端。结果按状态栏容量截断，由下一次整屏重绘显示。',
'miku_ui_derive_output':'从输入路径派生带 ASCII 后缀的输出名。只移除文件名部分的扩展名，避免把目录中的点误当成扩展名。',
'ui_style_name':'返回画面风格的静态显示名。未知非色块值沿用字符风格，不分配字符串。',
'ui_fps_name':'返回帧率显示文本。预设项使用静态文本，其他正值格式化到本函数的静态缓冲。',
'ui_quality_name':'将质量索引映射为静态显示名，无效索引回退自动档位。',
'miku_ui_edit_field':'在调用方路径数组内直接编辑。退格按 UTF-8 字符回退，容量不足时拒绝继续追加；Esc 结束编辑但保留已输入内容。',
'miku_ui_cfg_count':'返回当前页面可聚焦项目数，转换页和播放页使用不同项目映射。',
'miku_ui_cfg_index':'把可见槽位映射为配置项枚举。slot 必须小于当前页面项目数，映射表为静态只读数据。',
'ui_cfg_value':'把单个配置项格式化为显示文本，路径可用占位提示代替空值；结果写入调用方数组。',
'miku_ui_draw_config':'生成配置页标题、项目、状态栏和帮助提示。构建完成后一次提交屏幕，不在绘制过程中改变配置。',
'miku_ui_cfg_adjust':'按方向切换当前配置项；枚举项循环选择，帧率和宽度保持既有可选范围。',
'ui_is_video':'按扩展名识别可在选择器中展示的视频文件，ASCII 字母大小写不敏感；不代替媒体内容探测。',
'ui_cmp_entry':'目录优先，其余按名称排序。接收 qsort 提供的两个有效记录地址，不修改或保留记录。',
'miku_ui_load_dir':'扫描子目录和视频文件，生成排序后的目录快照。成功时把列表所有权交给调用方，由调用方 free；失败释放已分配列表。',
'miku_ui_parent_dir':'在原路径数组中退回父目录，根目录保持根目录。调用方保证数组有 NUL 结尾且至少可容纳当前目录表示。',
'miku_ui_join':'把目录和名称连接到独立输出数组，根目录只保留一个分隔符。输入输出不得重叠，容量不足时沿用有界截断行为。',
'miku_ui_read_result':'读取外部选择器的首条结果，支持 NUL 或换行终止；拒绝超出调用方数组容量的记录。',
'miku_ui_accept_path':'重新检查选择结果是否为可读普通文件；可解析时规范化为绝对路径，防止工具成功但文件不可用。',
'ui_logo_width':'累计四个字母的字符宽度并加入三个字母间隔，返回欢迎页图标的显示列数。',
'ui_draw_logo':'把双行位图压为终端半块字符，保持四个字母居中；只追加输出缓冲，不操作输入或焦点。',
'miku_ui_home':'维护欢迎菜单焦点，支持方向键、滚轮、数字键和点击。返回转换、播放或退出菜单索引，歌词每次进入时选择一次。',
'miku_ui_draw_note':'显示简短等待提示和可选副标题。仅在界面主线程使用，临时输出缓冲在提交后释放。',
'ui_draw_progress':'根据当前进度快照绘制百分比、剩余时间和规格。总帧数未知时不伪造百分比或剩余时间。',
'ui_progress_cb':'同步接收进度快照并更新界面状态，最多每 150ms 重绘一次，避免终端刷新挤占编码时间。',
'ui_abort_cb':'轮询取消按键并同步界面停止标志。取消状态一旦设置即保持到本次转换结束。',
'ui_log_open':'保存标准错误描述符后重定向到本次转换日志，避免 ffmpeg 错误覆盖界面。saved 由配对关闭函数恢复。',
'ui_log_close':'刷新并恢复之前保存的标准错误描述符，负描述符表示未取得资源，可直接返回。',
'ui_log_tail':'读取日志末尾的有限片段并选取最后一个非空行。无法读取时返回空字符串，由调用方提供通用错误提示。',
'miku_ui_convert':'把配置翻译为转换请求，安装同步进度和取消回调，并在执行后恢复日志描述符。回调上下文位于本函数栈上，转换返回前始终有效。',
'miku_ui_play':'退出界面备用屏后同步播放，再重新进入配置界面。帧率不超过源帧率，播放宽度按配置或终端尺寸决定。',
'ui_draw_done':'显示最终输出路径、体积、耗时和可执行的下一步动作，不改变转换结果。',
'miku_ui_wait_done':'显示完成页并等待返回、播放或退出动作。返回动作索引，调用方负责实际执行，避免绘制函数启动播放器。',
'ui_on_signal':'信号处理器只设置界面停止标志，所有终端恢复和资源释放交给普通执行路径。',
'ui_raw_mode':'保存当前终端属性后关闭规范输入和回显；仅成功设置时才标记需要恢复，所有输入从同一个界面线程读取。',
'miku_ui_leave':'关闭鼠标上报、退出备用屏、恢复光标和折行，再还原 termios。幂等调用不覆盖已交还给 shell 的终端属性。',
'miku_ui_enter':'进入备用屏并启用鼠标输入，安装只置标志的退出信号处理器。需与 ui_leave 成对使用，只允许主线程调用。',
'ui_read_raw':'等待并读取一个原始输入字节。负超时表示阻塞，零表示轮询，正值单位毫秒；超时或读失败返回 UI_NONE。',
'miku_ui_hit_index':'查找精确命中行，未命中时再容忍上下各一行，便于触屏选择。返回槽位索引或 -1。',
'miku_ui_hit_reset':'每次重绘前清空点击行表，旧屏幕的命中信息不再参与后续输入解释。',
'miku_ui_hit_add':'登记当前屏幕一条可点击行，达到固定容量后不再追加，防止命中表越界。',
'ui_utf8_len':'根据 UTF-8 首字节估计编码长度。调用方保证字符串有效且完整，非法首字节按单字节处理。',
'ui_codepoint':'合并一个完整 UTF-8 编码的位段。len 必须与首字节一致，输入至少包含 len 字节，不执行编码合法性校验。',
'ui_cell_width':'按既有宽字符区间估算显示列数：控制符零列，常见中日韩字符及部分符号两列，其余一列。',
'miku_ui_text_width':'累计有效 UTF-8 文本的显示列数，供居中和截断使用；不把 ANSI 控制序列视为可解析的文本格式。',
'miku_ui_add_text':'从有效 UTF-8 文本开头按显示列宽追加，不拆分字符。max_w<0 表示不限制，失败记录在缓冲的 error 字段。',
'miku_ui_add_text_tail':'宽度不足时从完整字符边界保留文本尾部，优先显示长路径的文件名；沿用缓冲的粘性错误处理。',
'miku_ui_center_col':'计算非负居中左边距，内容宽于终端时从最左列开始，调用方仍负责文本截断。',
'miku_ui_pad':'追加指定数量空格；n<=0 不追加，分配错误保存在缓冲中并由屏幕提交函数处理。',
'miku_ui_row':'追加定位到行首并清除该行的 ANSI 序列，row 使用终端的一基行号；不直接写终端。',
'miku_ui_clear_screen':'清空可见屏幕并把光标移到起点，用于模式切换；终端生命周期由界面模块负责。',
'miku_dmin':'返回两个浮点数中较小的一个，供布局与颜色计算复用，无资源和全局状态。',
'miku_dmax':'返回两个浮点数中较大的一个，供布局与颜色计算复用，无资源和全局状态。',
'miku_xasprintf':'两次格式化生成精确长度的字符串，避免固定大小临时数组。成功返回拥有的堆字符串，调用方 free；格式化或分配失败返回 NULL。',
'miku_shell_quote':'为一个完整 shell 参数添加单引号，并转义参数内部的单引号。返回拥有的堆字符串，不能再放进另一层单引号中使用。',
'miku_exit_ok':'解释 system/pclose/wait 风格的原始退出状态。只有正常退出且退出码为零才返回 true，不能传入已解码退出码。',
'miku_command_exists':'通过 shell 的 command -v 检查固定工具名。name 必须是程序内确定的命令名，不能直接传入未验证的用户文本。',
'miku_now_seconds':'读取单调时钟并换算为秒。只用于持续时间和播放调度，不代表日历时间，不受系统时间调整影响。',
'miku_sleep_until':'等待到单调时钟指定时刻。被信号打断后重新计算剩余时间，已过期的截止时刻直接返回。',
'miku_temp_video_path':'按临时目录、进程号和当前秒数生成临时视频路径，只分配路径字符串，不创建文件。调用方负责文件和字符串的清理。',
'miku_temp_dir':'借用 TMPDIR，其次 TMP，未设置时使用 /tmp。调用方不能 free 返回值，也不能在使用期间改写相应环境变量。',
'ratio_dims':'按画面比例计算偶数尺寸，避免不支持 yuv420p 的奇数尺寸；源比例仅在 ratio=0 时使用。',
'miku_choose_profile_ex':'在基础档位选择上应用原画、比例和指定帧率。任何尺寸或帧率改变都重新探测编码器，避免把旧组合的探测结果直接复用。',
'parse_rate':'解析正有理数或小数帧率，小数转换为分母 1000 的近似分数；成功时同步写出分子分母。',
}
DESC['miku_build_gamma_lut'] = '在开始渲染前建立 256 项亮度 gamma 表。表由字体模块持有，初始化后渲染器只读；同一进程的初始化必须串行。'
DESC.update({
'ui_start':'校验输入及输出配置后执行转换或直接播放，转换完成后把后续动作交给完成页；所有对象均由配置页持有。',
'miku_tui_main':'交互入口分别保存转换和播放配置。终端进入和离开成对管理，信号或退出菜单都经同一正常收尾路径。',
'miku_ui_pick_with_dialog':'调用外部 dialog 接管终端并读取结果。工具返回后重新进入备用屏，结果必须再次验证为可读普通文件。',
})
PARAM = {
'input':'借用的输入媒体路径，需以 NUL 结尾。','output':'输出媒体路径或输出位置，生命周期覆盖本次调用。',
'path':'借用的文件系统路径，不转移字符串所有权。','ipc_sock':'本次音频进程的 Unix 套接字路径，允许空串表示不启用 IPC。',
'line':'可修改的单行解析缓冲，调用期间有效。','quality':'质量档位或自定义尺寸字符串。','src_fps':'源帧率的整数估计，非正表示未知。',
'fps':'帧率，单位为帧/秒。','fps_want':'用户要求的帧率，零表示跟随源。','w':'宽度像素值或接收宽度的输出地址。','h':'高度像素值或接收高度的输出地址。',
'c':'借用的编码器候选，只读。','idx':'候选编码器索引，必须处于候选表范围内。','codec':'固定的编码器名称。',
'p':'借用的规格对象，非 const 指针用于接收计算结果。','vi':'已探测的源视频信息，只读。','vp':'已探测的源视频信息，只读。',
'info':'源视频信息，非 const 指针用于接收探测结果。','ratio':'画面比例索引，范围为 0..RATIO_COUNT-1。',
's':'有效的 NUL 结尾文本或待解析字段。','fmt':'printf 格式串，变参类型必须与占位符一致。','format':'printf 格式串，变参类型必须与占位符一致。',
'buf':'调用方持有的可修改缓冲。','buffer':'调用方持有的输出缓冲，失败状态保存在 error 中。','sb':'调用方持有的输出缓冲，失败状态保存在 error 中。',
'cap':'输出数组容量，单位字节，包含结尾 NUL 空间。','capacity':'输出数组容量，单位字节，包含结尾 NUL 空间。',
'st':'调用方持有的界面配置，非 const 指针允许更新。','state':'调用方持有的当前界面状态。','kind':'页面种类，0 表示转换，1 表示播放。',
'which':'需要编辑的配置项枚举。','title':'借用的 UTF-8 页面标题。','item':'配置项枚举。','dir':'目录路径或调整方向，具体单位见函数说明。',
'slot':'当前页面中的零基焦点槽位。','q':'质量档位索引。','style':'STYLE_CHAR 或 STYLE_BLOCK。',
'start':'起始文件或目录路径，NULL/空串表示当前目录。','out':'调用方提供的结果地址，不转移其所有权。','count':'接收实际目录记录数的输出地址。',
'pa':'第一个待比较目录记录地址。','pb':'第二个待比较目录记录地址。','name':'固定名称或单个文件名，不转移所有权。',
'cols':'宽度，单位为终端显示列或字符网格列。','rows':'高度，单位为字符网格行。','row':'一基终端行号或零基采样行，见具体函数说明。',
'width':'宽度，单位为显示列或像素，见函数说明。','max_w':'允许的最大显示列数，负数表示不限。',
'len':'当前字符的 UTF-8 字节数。','cp':'已经解码的 Unicode 码点。','n':'数量或帧率显示值，见函数说明。',
'ud':'调用方回调上下文，仅借用，不释放。','pg':'当前进度状态，借用。','update':'只在同步回调期间有效的进度快照，不得保存指针。',
'res':'调用方提供的实际转换结果。','saved':'保存/恢复标准错误所用的文件描述符。','dst':'调用方持有的输出字符数组。',
'elapsed':'已耗用时间，单位秒。','msg':'借用的 UTF-8 提示正文。','sub':'可选副标题，允许 NULL。',
'sig':'收到的信号编号；处理器只置停止标志。','timeout_ms':'首字节等待毫秒数，负数阻塞、零轮询。',
'status':'system/pclose/wait 返回的原始状态值。','a':'第一个数值。','b':'第二个数值。',
'base_h':'计算比例时使用的基准高度，单位像素。','source':'借用的源视频信息。',
'num':'输出帧率分子地址。','den':'输出帧率分母地址。','req_cols':'请求的字符列数，必须为正。',
'video_w':'源视频宽度，必须为正。','video_h':'源视频高度，必须为正。','layout':'调用方持有的布局输出对象。',
}
PARAM.update({
'bottom':'按值传入的格子下半部颜色。',
'program':'用于帮助文本的程序名，借用 argv[0]。',
'options':'调用方提供的命令行或播放配置，路径字段均为借用。',
'argc':'运行时提供的参数个数。','argv':'运行时持有的 NUL 结尾参数数组，不释放。',
'profile':'输出规格，非 const 时由本函数填写，只读编码器指针借用静态表。',
'tier':'档位表索引，范围为 0..TIER_COUNT-1。','source_fps':'源帧率整数估计，非正表示未知。',
'hooks':'可选同步回调，NULL 时输出命令行提示。','job':'当前转换任务，独占其中管道和缓冲，阶段间共享。',
'quoted_input':'已完成 shell 转义的输入路径，不能再次加外层单引号。',
'quoted_output':'已完成 shell 转义的输出路径，不能再次加外层单引号。',
'sample_rows':'解码采样行数，色块风格为网格行数的两倍。',
'request':'借用的转换请求，路径和回调在本次执行期间必须有效。',
'ret':'先前阶段的零成功或负 errno，用于决定是否回退。',
'area':'以源字体坐标表示的正面积采样矩形，只读。',
'glyph':'零基字形索引，范围为 0..FONT_CHAR_COUNT-1。',
'position':'采样位置或截止时刻，坐标系/时间单位见函数说明。',
'limit':'源采样方向的总像素数，必须为正。',
'x':'横向坐标或其插值邻点和权重。','y':'纵向坐标或其插值邻点和权重。',
'cw':'目标字符格宽度，单位像素。','ch':'目标字符格高度，单位像素。',
'samples':'本次建图独占的采样、墨量及排序临时状态。',
'gsize':'单个灰度字形的字节数，等于格子宽乘高。',
'norm':'由暗到亮排列的归一化墨量数组，只读。','target':'目标归一化亮度，范围 0..1。',
'atlas':'字形图集，创建成功后 masks 所有权交给调用方。',
'cell_w':'格子宽度，单位像素。','cell_h':'格子高度，单位像素。',
'pixel':'至少三个字节的 RGB24 采样点，仅借用。','color':'按值传入的 RGB 通道。',
'alpha':'字形覆盖率，范围 0..255。','grid':'借用的网格几何与 RGB24 输入输出。',
'mask':'字形灰度覆盖率数组，至少容纳一个完整格子。',
'background':'非零表示背景色，零表示前景色。','previous':'上一次成功输出的颜色缓存地址。',
'colors':'当前行的前景和背景颜色缓存，调用方持有。',
'frame':'借用的终端采样帧和网格尺寸，不转移图集所有权。',
'play':'单次播放状态，拥有本次解码资源。','wanted':'请求列数，非正时跟随终端。',
'with_audio':'非零时尝试播放原音轨，失败时允许无声播放。',
'entry':'借用的目录记录，仅在目录快照释放前有效。','index':'零基记录索引，必须在当前列表内。',
'columns':'可用终端显示列数。','key':'已解码的 UI 键值或普通输入字节。',
'result':'外部选择器结果路径或调用方结果对象，见接口说明。',
'chosen':'调用方持有的所选结果数组。','text':'借用的 NUL 结尾文本，UTF-8 要求见接口说明。',
'lyric':'借用的 UTF-8 欢迎页文本，只在本次绘制期间使用。','focus':'当前菜单焦点的零基索引。',
'length':'接收/更新字符串字节长度的地址，不含结尾 NUL。','top':'图标首行的一基终端行号。',
'first':'CSI 序列的第一个正文字节。','sequence':'以 NUL 结尾的鼠标序列正文，已去掉终止字节。',
'avail':'当前行可用的终端显示列数。','minimum':'夹紧区间下界，包含端点。',
'maximum':'夹紧区间上界，包含端点。','value':'待限制到区间内的整数。',
'deadline':'单调时钟截止秒数，必须与 now_seconds 使用同一时钟。',
'metadata':'本次探测的中间元数据，由调用方持有。',
'extra':'在当前长度之后还需保留的字节数，包含调用方所需的 NUL。',
})

RET = {
'miku_start_audio':'返回：0 表示已创建子进程；无播放器为 -ENOENT，fork 失败为负 errno。',
'miku_stop_audio':'返回：无；音频子进程已请求终止并等待回收。',
'audio_ipc_open':'返回：1 已连接，0 尚不可连接；不改变音频播放状态。',
'audio_parse_position':'返回：当前估计音频秒数，无有效回复返回 -1。',
'miku_audio_time_pos':'返回：有效音频位置，单位秒；尚无完整有效回复返回 -1。',
'miku_stamp_bt709':'返回：0 已补色标，失败返回 -EIO；原输出在替换前保持有效。',
'miku_total_memory_mb':'返回：物理内存 MiB 数，0 表示探测失败。',
'miku_cpu_cores':'返回：至少为 1 的可用核心数。',
'count_decodable_frames':'返回：实际帧数，失败返回负值。',
'miku_parse_custom_size':'返回：0 成功并写出宽高帧率，-EINVAL 表示尺寸或帧率不合法。',
'miku_choose_profile':'返回：0 已选定，-ENODEV 表示档位无效或没有可用组合。',
'miku_choose_profile_ex':'返回：0 已选定，-ENODEV 表示不能满足请求。',
'miku_probe_video':'返回：0 获得有效视频信息，失败返回 -EIO。',
'miku_ui_convert':'返回：0 转换成功，失败或用户取消返回 -EIO；取消标志由回调保存。',
'miku_ui_load_dir':'返回：0 成功且 out/count 已填写，失败返回 -EIO。',
'miku_ui_pick_file':'返回：0 选定，-ECANCELED 用户取消，-ENOENT 所选文件不可读。',
'miku_ui_pick_with_fzf':'返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。',
'miku_ui_pick_with_dialog':'返回：1 选定，0 工具不可用，-1 取消，-2 文件失效。',
'miku_ui_home':'返回：0 转换，1 播放，2 退出。',
'miku_ui_wait_done':'返回：0 返回配置，1 播放结果，2 退出程序。',
'miku_ui_cfg_index':'返回：CFG_* 配置项枚举。',
'miku_ui_cfg_count':'返回：当前页面可聚焦的项目数。',
'miku_ui_key':'返回：UI_*、普通字节或编码后的 UTF-8 字节；无输入为 UI_NONE。',
'miku_terminal_columns':'返回：正列数，无法探测返回 0。',
'miku_terminal_rows':'返回：正行数，无法探测返回 0。',
'miku_ui_read_result':'返回：1 已读到完整结果，0 结果缺失、过长或读取失败。',
'miku_ui_accept_path':'返回：1 普通文件可读，0 不可用。',
'ui_style_name':'返回：借用的静态 UTF-8 名称，不得释放。',
'ui_quality_name':'返回：借用的静态 UTF-8 名称，不得释放。',
'ui_fps_name':'返回：借用的静态文本或静态缓冲，下一次调用可能覆盖内容。',
'profile_cpu':'返回：估计的 CPU 秒/视频秒。',
'hw_bitrate_kbps':'返回：夹紧后的码率，单位 kbps。',
'ui_utf8_len':'返回：估计的编码字节数，范围 1..4。',
'ui_cell_width':'返回：0、1 或 2 显示列。',
'parse_rate':'返回：1 成功，0 无法解析正帧率。',
'miku_now_seconds':'返回：单调时钟秒数，只适合比较同一运行环境的时刻。',
'miku_temp_video_path':'返回：拥有的堆路径，调用方 free；失败返回 NULL。',
'miku_temp_dir':'返回：借用的环境变量或静态字符串，不得 free。',
}

MODULE = {
'audio':'音频进程与非阻塞 IPC；所有播放状态只由播放主线程访问。',
'cli':'命令行解析和执行分派；路径字符串借用 argv，临时路径由入口释放。',
'convert':'转换管线；同步回调、资源取得、帧处理及失败回退分层管理。',
'encoder':'编码器能力、成本和档位；探测缓存由主线程串行初始化。',
'font_data':'内嵌字形数据；只读位图与运行时的字体采样逻辑分开维护。',
'glyph':'字形采样与图集；建图得到的 masks 所有权交给调用方。',
'layout':'画布布局计算；不执行 I/O，不取得输入对象所有权。',
'playback':'终端播放调度；按单调时钟显示有效帧，统一恢复终端和回收资源。',
'render':'RGB24 渲染；输入输出内存借用，按格子边界写入调用方画布。',
'strbuf':'可复用输出缓冲；容量增长和首个错误状态均封装在缓冲对象内。',
'ui_directory':'目录扫描与路径处理；返回的列表由选择器持有并释放。',
'ui_terminal':'界面终端生命周期与按键协议；只由界面主线程访问状态。',
'ui_text':'UTF-8 显示列计算与输出；调用方必须提供有效完整的 UTF-8 文本。',
'util':'字符串、数值和单调时钟工具；所有权以各接口说明为准。',
'video':'媒体信息与比例规格；ffprobe 元数据与最终显示尺寸分开处理。',
}

def width(text):
        return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in text)

def wrap(text, limit=72):
        out=[]
        while width(text)>limit:
                count=0
                end=0
                for i,c in enumerate(text):
                        count+=2 if unicodedata.east_asian_width(c) in 'WF' else 1
                        if count>limit:break
                        end=i+1
                # 英文单词尽量不在中间换行。
                if end<len(text) and text[end-1:end].isascii() and text[end:end+1].isalnum():
                        space=text.rfind(' ',0,end)
                        if space>limit//2:end=space
                out.append(text[:end].rstrip())
                text=text[end:].lstrip()
        return out+[text]

def offset(loc):
        return loc.get('offset', loc.get('expansionLoc',{}).get('offset'))

def previous_comment(raw, at):
        prefix=raw[:at].decode()
        comments=list(re.finditer(r'/\*.*?\*/',prefix,re.S))
        if comments and not prefix[comments[-1].end():].strip():
                match=comments[-1]
                body=re.sub(r'(?m)^\s*\* ?', '', match[0][2:-2]).strip()
                return len(prefix[:match.start()].encode()), body
        return at, ''

documents={}
for path in sorted(pathlib.Path('src').glob('*.c')):
        raw=path.read_bytes()
        result=subprocess.run(['clang',*FLAGS,'-Xclang','-ast-dump=json','-fsyntax-only',str(path)],capture_output=True,check=True)
        ast=json.loads(result.stdout)
        patches=[]
        for fn in ast['inner']:
                if fn.get('kind')!='FunctionDecl' or 'includedFrom' in fn.get('loc',{}):continue
                if not any(n.get('kind')=='CompoundStmt' for n in fn.get('inner',[])):continue
                at=offset(fn['range']['begin'])
                if at is None:continue
                name=fn['name'];begin,previous=previous_comment(raw,at)
                previous = previous.split('\n@')[0].strip()
                previous = re.sub(r'^'+re.escape(name)+r' - ', '', previous)
                description=DESC.get(name,previous)
                if not description:
                        raise ValueError('缺少函数说明: '+name)
                # 状态接口的旧 0/1 说明已过时，使用专门契约覆盖。
                if name in DESC:body=[name+' - '+description]
                else:body=[name+' - '+previous.splitlines()[0]]+previous.splitlines()[1:]
                body+=['']
                for parameter in fn.get('inner',[]):
                        if parameter.get('kind')!='ParmVarDecl':continue
                        param=parameter.get('name','unnamed')
                        text=PARAM.get(param)
                        if text is None:
                                # 手写长注释已经解释过这些辅助接口的对象和坐标。
                                text='参数含义、单位和有效范围见上面的具体约定。'
                        body.append('@'+param+': '+text)
                if any(n.get('kind')=='ParmVarDecl' for n in fn.get('inner',[])):body+=['']
                ret=RET.get(name)
                qual=fn['type']['qualType'].split(' (')[0]
                if ret is None and name in DESC:
                        if qual=='bool':ret='返回：true 满足所述条件，false 不满足；不转移输入对象所有权。'
                        elif qual=='void':ret='返回：无；只执行上述更新，不转移借用对象所有权。'
                        elif '*' in qual:ret='返回：拥有的字符串由调用方 free；借用指针的例外见上述说明，失败为 NULL。'
                        else:ret='返回：上述计算结果或状态；其单位和取值含义见说明。'
                if ret:body.append(ret)
                if path.stem.startswith('ui') or path.stem in ('audio','playback'):
                        body.append('线程约束：仅由界面/播放主线程串行调用；信号处理器只置标志。')
                elif path.stem=='encoder':body.append('线程约束：编码器探测及其静态缓存由主线程串行访问。')
                lines=[]
                for line in body:lines.extend(wrap(line) if line else [''])
                doc='/*\n'+''.join(' *'+(' '+line if line else '')+'\n' for line in lines)+' */\n'
                patches.append((begin,at,doc))
                documents[name]=doc
        for a,b,value in sorted(patches,reverse=True):raw=raw[:a]+value.encode()+raw[b:]
        # 单独说明模块职责；保留 SPDX 与版权行的位置。
        text=raw.decode()
        if path.stem in MODULE and MODULE[path.stem] not in text:
                at=text.index('\n\n')+2
                note='/*\n'+''.join(' * '+x+'\n' for x in wrap(MODULE[path.stem]))+' */\n\n'
                text=text[:at]+note+text[at:]
        path.write_text(text)
# 公开头文件保留完整契约，读接口时不必翻到实现才能知道资源归属。
for path in pathlib.Path('include').glob('*.h'):
        text=path.read_text()
        patches=[]
        for name,doc in documents.items():
                if not name.startswith('miku_'):continue
                match=re.search(r'(?m)^[\w \t*]+\b'+re.escape(name)+r'\([^;]*?\);',text)
                if match:patches.append((match.start(),doc))
        for at,doc in sorted(patches,reverse=True):text=text[:at]+doc+text[at:]
        path.write_text(text)
