# ASCII-Video

高性能、高保真的终端彩色字符视频播放与转换工具（C17 编写，无庞大第三方图形库依赖）。

> **版本发布说明**：每个版本的详细发布说明与更新日志已独立归档至 [GitHub Releases](https://github.com/Kuimuztion/ASCII-Video/releases)。

脚本 `start.sh` 仅适用与RHEL、Debian、Arch系
其他发行版请自行安装或手动编译所需依赖


# 重构为C语言的原因：
> 作者不会写C++，最初版本是人工智能 `GPT 4` 接手。现改为C语言让其更好维护。
> 作者同样还是使用了人工智能 `Claude Opus 5.5 Medium` 来协助。并摘除了臃肿的OpenCV库。

# 使用方法

 - chmod +x start.sh
 - ./start.sh

  当你运行这个`Shell`脚本后, 它将会为您更新操作系统的所有包
然后自动拉取编译所需要的依赖包, 并自动编译`C`代码最后输出并运行启动.

你也可以直接用参数指定其他视频，不用再改脚本：

 - ./start.sh 输入.mp4 输出.mp4 [ASCII宽度]
 - 例如：./start.sh bad-1.mp4 bad-1-ascii.mp4 320

或者手动编译后单独运行转换程序：

 - make
 - ./build/miku bad-1.mp4 bad-1-ascii.mp4 320

参数说明：
 - ASCII宽度：每行字符数，80-640；省略该参数或写 `auto` 时按当前终端列数自动适配（不在终端里运行则用 320）
   - 320 -> 每个字符 8x16 像素（high 档下 320x90 个字符刚好铺满 2K）
   - 426 -> 每个字符 6x12 像素，426x120 个字符，字符更多更细（左右各留 2 像素黑边）
   - 数值越大画面越细腻，但单个字符越小、越不容易看清
 - 质量档位：`auto`（默认，自动选）/ `high` / `mid` / `low`，或直接给尺寸 `1920x1080[@30]`
   - 档位决定输出分辨率与默认帧率；默认帧率不超过源帧率，`WxH@FPS` 指定的帧率原样生效
   - 字符大小由「输出宽度 / ASCII宽度」自动决定，档位越低字符越小
 - 例：`./build/miku in.mp4 out.mp4 320 low`、`./build/miku in.mp4 out.mp4 auto 1920x1080@30`
 - 模式参数（位置随意，可以和别的参数混着写）：
   - 默认：只转码
   - `--play`：转完播放；**省略输出文件名**就用临时文件，播完/中断自动删除
   - `--play-only`：不转码，直接从源播放（最省 CPU，不写文件）
   - `--mute`：播放时不出声（默认有音轨就用 mpv 或 ffplay 放声音）
   - `--block`：色块风格（半块 ▀），不加就是字符风格
 - 不带任何参数运行 `./build/miku` 会打开终端界面（只在真终端里有效）
   - 例：`./build/miku --play in.mp4 160`、`./build/miku --play-only in.mp4 120 low`、`./build/miku --play --mute in.mp4 out.mp4`


感谢您收看此视觉盛宴 非常感谢您的支持
有提问可发送到我的邮箱 [3623241078@qq.com]
