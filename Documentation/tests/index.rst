测试与检查
==========

``make test`` 会编译并运行下列测试，每个测试都有超时保护（需要 timeout 或 gtimeout）。

- tests/playback.c：播放 IPC 的回归测试。直接包含 audio.c 复用实现，不启动音频设备。
- tests/render.c：五种字形尺寸与字符、色块渲染的基线校验，以及跨帧稳定、终端帧字形稳定、UTF-8 截断边界与高光防漂白断言。
- tests/buffer.c：strbuf 的容量边界测试。
- tests/video.c 与 tests/video.py：用真实 MP4、MKV 和裸 H.264 验证帧数。
  覆盖视频流帧数、容器时长回退和未知总帧数，依赖 ffmpeg、ffprobe
  以及 libx264。
- tests/style.py：用编译器语法树检查函数规模与嵌套深度，不把字符串误判为控制语句。由 ``make format-check`` 调用。
- tests/terminal.py：在独立伪终端中验证退出与回显恢复，不改写用户终端。

运行 ``make line`` 可只读检查 ``src/``、``include/``、``tests/`` 和
``start.sh`` 的缩进，输出 ``文件:行号:原因``。发现问题时返回非零状态。
C 按 Tab=8 检查，并允许合法的续行、注释对齐；Python 按每层 8 个空格检查，
不把括号续行和多行字符串当作代码块。Shell 检查普通行的空格单位，
不解析完整语法；反斜杠续行允许额外对齐。Makefile、文档、配置、历史工作文件
与构建产物不纳入检查。
也可用 ``make line LINE_FILES='src/render.c tests/style.py'`` 指定文件。
检查器自身的回归测试可用 ``.venv/bin/python -B tests/test_line.py`` 运行。

字体数据与画面渲染的改动应当跑 tests/render.c；界面输入相关的改动建议在伪终端中实际操作一遍，例如用 Python 的 pty 模块发送 SGR 鼠标序列（``ESC [ < 0 ; x ; y M`` 按下，``m`` 松开），确认每次点击只触发一次。
