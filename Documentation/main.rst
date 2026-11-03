miku 项目文档
=============

本文档部分内容由大语言模型（LLM）辅助撰写。若文档与源码不一致，以源码和 git 历史为准。

miku 是把视频转换为彩色字符画（ASCII 视频）的程序，提供命令行与终端界面两种入口。项目使用 C17 编写，解码与编码交给 ffmpeg 完成，程序本身只负责画面的字形采样、颜色计算与终端输出。

阅读顺序
--------

- build.rst：依赖清单、start.sh 与 make 的用法、构建变体。
- src/index.rst：每个源文件的职责与模块之间的数据流。
- include/index.rst：公共头文件与数据结构。
- tests/index.rst：单元测试与风格检查脚本。
- development/coding-style.rst：代码规范，遵循 Linux 内核代码风格。

目录结构
--------

::

    ASCII-Video/
    ├── start.sh          启动脚本：检查依赖、编译、运行
    ├── Makefile          构建规则与测试目标
    ├── src/              源文件（每个 .c 一个模块）
    ├── include/          头文件（与 src/ 一一对应）
    ├── tests/            单元测试与风格检查
    ├── Documentation/    本文档
    ├── README.MD         功能与版本说明（面向使用者）
    └── build/            编译产物（可删除，随时重新生成）

数据流
------

::

    输入视频 → video.c（ffprobe 探测）→ encoder.c（选分辨率、帧率、编码器）
            → convert.c（ffmpeg 解码为采样帧）→ glyph.c / render.c（字形与颜色）
            → ffmpeg（编码 + 合并音频）→ 输出视频

    播放路径：playback.c（解码采样帧、按时钟显示）→ terminal_frame.c（序列化）→ 终端
    音频由 audio.c 启动 mpv 或 ffplay，并通过 IPC 读取播放位置用于同步。
