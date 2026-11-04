# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
"""只读检查八列缩进；C 使用 Tab，Python 与 Shell 使用空格。"""

import io
from pathlib import Path
import shutil
import subprocess
import sys
import tokenize
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
STYLE = (
        "{BasedOnStyle: InheritParentConfig, IndentWidth: 8, "
        "TabWidth: 8, UseTab: Always}"
)
IGNORED_TOKENS = {tokenize.COMMENT, tokenize.NL, tokenize.ENDMARKER}


def describe(prefix):
        """同时显示真实空白字符和按 Tab=8 展开后的列数。"""
        width = len(prefix.expandtabs(8))
        return f"{prefix!r}（{width} 列）"


def c_replacement(raw, replacement):
        """仅提取行首空白的修改，忽略换行布局和行内排版建议。"""
        offset = int(replacement.attrib["offset"])
        end = offset + int(replacement.attrib["length"])
        old = raw[offset:end]
        new = (replacement.text or "").encode("utf-8")
        if old.strip() or new.strip():
                return None
        start = raw.rfind(b"\n", 0, end) + 1
        actual = raw[start:end]
        if actual.strip(b" \t"):
                return None
        if b"\n" in new:
                expected = new.rsplit(b"\n", 1)[1]
        elif b"\n" in old:
                return None
        else:
                expected = raw[start:offset] + new
        if actual == expected:
                return None
        number = raw.count(b"\n", 0, start) + 1
        return number, actual.decode(), expected.decode()


def check_c(path):
        """借用现有 clang-format 识别 C 语法，允许续行和注释合法对齐。"""
        raw = path.read_bytes()
        result = subprocess.run([
                "clang-format", f"--style={STYLE}",
                f"--assume-filename={ROOT / 'src' / path.name}",
                "--output-replacements-xml",
        ], input=raw, capture_output=True, check=True, timeout=15)
        replacements = ET.fromstring(result.stdout)
        if replacements.attrib.get("incomplete_format") == "true":
                raise ValueError(f"{path}: clang-format 无法完整解析文件")
        issues = set()
        for replacement in replacements:
                issue = c_replacement(raw, replacement)
                if issue is not None:
                        issues.add(issue)
        return [
                (number, f"实际缩进 {describe(actual)}，应为 {describe(want)}")
                for number, actual, want in sorted(issues)
        ]


def check_python(path):
        """按语法缩进层级检查语句，括号续行与多行字符串不当作新层级。"""
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        level = 0
        statement = True
        seen = set()
        issues = []
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
                if token.type == tokenize.INDENT:
                        level += 1
                        continue
                if token.type == tokenize.DEDENT:
                        level -= 1
                        continue
                if token.type == tokenize.NEWLINE:
                        statement = True
                        continue
                if token.type in IGNORED_TOKENS:
                        continue
                number = token.start[0]
                prefix = lines[number - 1]
                prefix = prefix[:len(prefix) - len(prefix.lstrip(" \t"))]
                if number not in seen and "\t" in prefix:
                        issues.append((number, "Python 缩进禁止使用 Tab"))
                elif statement and prefix != " " * (level * 8):
                        issues.append((number, f"第 {level} 层应缩进 "
                                       f"{level * 8} 个空格，"
                                       f"实际 {len(prefix)}"))
                seen.add(number)
                statement = False
        return issues


def check_shell(path):
        """检查普通 Shell 行的空白单位，反斜杠续行允许额外空格对齐。"""
        issues = []
        continued = False
        for number, line in enumerate(path.read_text().splitlines(), 1):
                content = line.lstrip(" \t")
                prefix = line[:len(line) - len(content)]
                if content and not content.startswith("#"):
                        if "\t" in prefix:
                                issues.append((number,
                                               "Shell 缩进禁止使用 Tab"))
                        elif not continued and len(prefix) % 8:
                                issues.append((number, "Shell 缩进应为 8 个空格"
                                               f"的整数倍，实际 {len(prefix)}"))
                continued = line.rstrip().endswith("\\")
        return issues


def default_paths():
        """只检查正式源码和测试，排除 work、build、虚拟环境与文档。"""
        paths = [ROOT / "start.sh"]
        for directory in ("src", "include", "tests"):
                paths.extend(path for path in (ROOT / directory).rglob("*")
                             if path.suffix in {".c", ".h", ".py", ".sh"})
        return sorted(paths)


def main():
        """发现缩进问题返回 1，工具、文件或语法错误返回 2；不写文件。"""
        paths = [Path(name) for name in sys.argv[1:]] or default_paths()
        checks = {".c": check_c, ".h": check_c, ".py": check_python,
                  ".sh": check_shell}
        if any(path.suffix in {".c", ".h"} for path in paths):
                if shutil.which("clang-format") is None:
                        print("缺少 clang-format；请安装后再运行 make line。",
                              file=sys.stderr)
                        return 2
        count = 0
        for path in paths:
                try:
                        if path.suffix not in checks:
                                raise ValueError(f"不支持的文件类型：{path}")
                        issues = checks[path.suffix](path)
                except (OSError, ValueError, SyntaxError, tokenize.TokenError,
                        subprocess.SubprocessError, ET.ParseError) as error:
                        print(f"{path}: 检查失败：{error}", file=sys.stderr)
                        if isinstance(error, subprocess.CalledProcessError):
                                print(error.stderr.decode(), file=sys.stderr)
                        return 2
                display = path.relative_to(ROOT) if path.is_relative_to(ROOT) \
                        else path
                for number, reason in issues:
                        print(f"{display}:{number}: {reason}")
                count += len(issues)
        print(f"缩进检查：{len(paths)} 个文件，{count} 处问题")
        return int(count > 0)


if __name__ == "__main__":
        sys.exit(main())
