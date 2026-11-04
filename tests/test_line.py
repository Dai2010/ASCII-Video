# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
"""用故意写错的缩进验证检查器的行号、合法例外和只读行为。"""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import line


class IndentationTests(unittest.TestCase):
        """样本由临时目录持有，测试结束后自动清理。"""

        def setUp(self):
                """在项目内创建样本，使用同一套 clang-format 规则。"""
                temporary = tempfile.TemporaryDirectory(dir=line.ROOT / "work")
                self.addCleanup(temporary.cleanup)
                self.directory = Path(temporary.name)

        def check(self, suffix, text):
                """检查样本并确认其字节内容没有被重写。"""
                path = self.directory / f"中文 sample{suffix}"
                path.write_text(text, encoding="utf-8")
                before = path.read_bytes()
                checks = {".c": line.check_c, ".py": line.check_python,
                          ".sh": line.check_shell}
                issues = checks[suffix](path)
                self.assertEqual(path.read_bytes(), before)
                return [number for number, _ in issues]

        def test_c_four_spaces_and_unicode_offsets(self):
                """C 四空格缩进必须报告原文件第四行，中文不偏移行号。"""
                text = "/* 中文 */\nint main(void)\n{\n    return 0;\n}\n"
                self.assertEqual(self.check(".c", text), [4])

        def test_c_eight_spaces_are_not_tabs(self):
                """八个空格也不符合 C 必须使用 Tab 的规定。"""
                text = "int main(void)\n{\n        return 0;\n}\n"
                self.assertEqual(self.check(".c", text), [3])

        def test_c_missing_indent(self):
                """完全未缩进也必须被识别。"""
                text = "int main(void)\n{\nreturn 0;\n}\n"
                self.assertEqual(self.check(".c", text), [3])

        def test_c_wrong_tab_depth(self):
                """不能只检查是否用了 Tab，还要检查结构要求的层级。"""
                text = "int main(void)\n{\n\tif (1)\n\treturn 0;\n}\n"
                self.assertEqual(self.check(".c", text), [4])

        def test_c_comment_and_continuation(self):
                """注释星号与函数参数对齐允许在 Tab 后追加空格。"""
                text = (
                        "/*\n * 中文注释\n */\n"
                        "int main(void)\n{\n"
                        "\treturn function(first_argument,\n"
                        "\t\t\tsecond_argument, third_argument);\n}\n"
                )
                self.assertEqual(self.check(".c", text), [])

        def test_c_inline_spacing_is_not_indentation(self):
                """只检查缩进，不把行内运算符排版也报成缩进错误。"""
                text = "int main(void)\n{\n\treturn 1+2;\n}\n"
                self.assertEqual(self.check(".c", text), [])

        def test_python_four_spaces_every_statement(self):
                """四空格块内的每一条语句都给出原始行号。"""
                text = "def sample():\n    first()\n    second()\n"
                self.assertEqual(self.check(".py", text), [2, 3])

        def test_python_tabs(self):
                """Python 的 Tab 即使显示为八列也必须报错。"""
                self.assertEqual(self.check(".py", "if True:\n\tpass\n"),
                                 [2])

        def test_python_continuation_and_string(self):
                """括号续行和多行字符串内容不是新的代码块。"""
                text = (
                        "def sample():\n        values = [\n"
                        "    1,\n        ]\n        text = '''\n"
                        "    四个空格是字符串内容\n'''\n"
                )
                self.assertEqual(self.check(".py", text), [])

        def test_shell_four_spaces(self):
                """普通 Shell 行四空格缩进不合格。"""
                self.assertEqual(self.check(".sh", "if true; then\n"
                                           "    echo ok\nfi\n"), [2])

        def test_shell_tabs(self):
                """Shell 代码缩进不允许 Tab。"""
                self.assertEqual(self.check(".sh", "\techo ok\n"), [1])

        def test_shell_continuation(self):
                """Shell 的续行参数可以额外对齐。"""
                text = "        printf '%s' \\\n    value\n"
                self.assertEqual(self.check(".sh", text), [])

        def test_cli_line_number_and_failure_status(self):
                """命令行对违规返回 1，并输出路径和行号。"""
                path = self.directory / "bad.py"
                path.write_text("if True:\n    pass\n")
                result = subprocess.run([
                        sys.executable, "-B", str(line.ROOT / "tests/line.py"),
                        str(path),
                ], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 1)
                self.assertIn("bad.py:2:", result.stdout)

        def test_cli_missing_file(self):
                """无法读取文件时返回 2，不能被当作检查通过。"""
                result = subprocess.run([
                        sys.executable, "-B", str(line.ROOT / "tests/line.py"),
                        str(self.directory / "missing.py"),
                ], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2)
                self.assertIn("检查失败", result.stderr)


if __name__ == "__main__":
        unittest.main()
