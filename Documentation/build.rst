构建与运行
==========

依赖
----

依赖分三类。缺失的必需项会导致启动失败，可选项只影响体验。

必需：

- C 编译器：cc、gcc 或 clang 之一。
- make。
- ffmpeg 与 ffprobe。建议编译时带 libx264；没有时退回 mpeg4 等编码器，画质与体积会不同。

可选：

- mpv 或 ffplay：播放时的声音。都没有时可以无声播放，转换不受影响。
- fzf 或 dialog：界面里的文件选择器。都没有时使用内置的单列列表。

系统工具：macOS 通过 sysctl 读取内存，Linux 读取 /proc/meminfo；其余只用标准 POSIX 工具与 /bin/sh。

运行 ``./start.sh --deps`` 可以只查看当前机器的依赖状态，不安装、不编译。

start.sh
--------

start.sh 用 POSIX sh 编写（Termux 只有 /bin/sh），流程为：检测系统 → 检查依赖 → 用 make 编译 → 运行 build/miku。缺失的依赖会通过对应系统的包管理器安装：Debian 系用 apt，Fedora 系用 dnf 或 yum，Arch 系用 pacman，macOS 用 brew，Termux 用 pkg。

常用调用：

::

    ./start.sh                          进入终端界面
    ./start.sh in.mp4 out.mp4 320       指定输入、输出与 ASCII 宽度
    ./start.sh --play in.mp4 160        转换后直接播放
    ./start.sh --deps                   只列出依赖状态

环境变量 ``MIKU_NO_INSTALL=1`` 时不自动安装，只提示缺失项，适合 CI 或已手动装好依赖的机器。
编译失败时，完整日志写入 build/make.log。

make
----

::

    make                       编译 build/miku
    make test                  编译并运行 tests/ 中的单元测试
    make format-check          clang-format 与 tests/style.py 检查
    make clean                 删除 BUILD_DIR 中的目标文件与可执行文件
    make help                  查看目标与变量

可覆盖的变量：

- ``CC``：编译器，默认 cc。
- ``OPT``：优化与调试选项，默认 ``-O2 -g``。需要改优化级别时用 ``OPT``，不要用 ``CFLAGS``，否则会丢掉 Makefile 里的警告设置。
- ``WERROR``：默认 ``-Werror``。在未知编译器上遇到新警告时，可设为空。
- ``BUILD_DIR``：产物目录，默认 build。
- ``PYTHON``：tests/style.py 使用的解释器。

构建变体
--------

不同的优化或调试设置通过 ``BUILD_DIR`` 与 ``OPT`` 区分，所有变体共用同一套源文件：

::

    make BUILD_DIR=build/debug OPT="-O0 -g3"
    make BUILD_DIR=build/release OPT="-O3 -march=native"

build/ 只需要保留一套默认产物。过去的 build/analysis、build/baseline、build/debug、build/manual、build/release 是同一套源码用不同参数生成的重复目标文件，已经删除；需要时用上面的方式重新生成即可。.gitignore 已忽略 /build/，媒体文件（测试输出）也不应放在 build/ 中。
