公共头文件
==========

include/ 中的头文件与 src/ 中的源文件一一对应；只有需要被多个模块共享的类型和常量放在 types.h 与 constants.h。

- types.h：跨模块的数据结构，例如 ``struct profile``（输出规格，``fps`` 为实际输出帧率）、``struct video_info``（源视频信息，``fps_num``/``fps_den`` 为有理帧率）。
- constants.h：数值常量（ASCII 宽度范围、字体尺寸、编码预算、颜色与高光防漂白软膝参数）、UI 键值（``UI_CLICK`` 等）、界面配置项枚举与已有视频列表常量。``UI_IGNORE`` 是内部哨兵，只存在于输入层，不会传给界面。
- attrs.h：编译器属性宏（如 ``MIKU_PRINTF``），供变参格式化函数声明使用。
- 其余头文件：声明对应 .c 文件导出的函数（包括 ``ui_library.h``、``ui_settings.h``），函数注释写在头文件中，格式见 development/coding-style.rst。
