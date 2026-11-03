# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
"""使用编译器语法树检查函数规模和嵌套，不把字符串当成控制语句。"""

import json
from pathlib import Path
import subprocess
import sys


CONTROLS = {"ForStmt", "WhileStmt", "DoStmt", "SwitchStmt"}
FLAGS = [
        "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-Iinclude",
        "-std=c17", "-Xclang", "-ast-dump=json", "-fsyntax-only",
]


def walk(node):
        """枚举显式及隐式子节点；部分占位节点没有 kind。"""
        yield node
        for child in node.get("inner", []):
                yield from walk(child)


def nesting(node, level=0, chain=False):
        """else-if 是同级分支，其余控制结构才增加一层嵌套。"""
        kind = node.get("kind")
        level += kind in CONTROLS or (kind == "IfStmt" and not chain)
        children = node.get("inner", [])
        values = [level]
        for index, child in enumerate(children):
                same_chain = (
                        kind == "IfStmt" and node.get("hasElse") and
                        index == len(children) - 1 and
                        child.get("kind") == "IfStmt"
                )
                values.append(nesting(child, level, same_chain))
        return max(values)


def functions(path):
        """只返回当前编译单元的函数定义，排除头文件和系统宏。"""
        result = subprocess.run(
                ["clang", *FLAGS, str(path)], capture_output=True, check=True,
        )
        for node in json.loads(result.stdout)["inner"]:
                if node.get("kind") != "FunctionDecl":
                        continue
                if "includedFrom" in node.get("loc", {}):
                        continue
                body = next((child for child in node.get("inner", [])
                             if child.get("kind") == "CompoundStmt"), None)
                if body is not None:
                        yield node, body


def check_function(raw, function, body):
        """返回违规指标；函数长度从签名算起，不计函数头说明。"""
        begin = function["range"]["begin"]
        start = begin.get("offset", begin.get("expansionLoc", {}).get("offset"))
        end = body["range"]["end"]["offset"]
        assert start is not None, function["name"]
        metrics = {
                "lines": (raw[start:end].count(b"\n") + 1, 80),
                "nesting": (nesting(body), 3),
                "parameters": (sum(child.get("kind") == "ParmVarDecl"
                                   for child in function.get("inner", [])), 5),
                "locals": (sum(node.get("kind") == "VarDecl"
                               for node in walk(body)), 10),
                "labels": (sum(node.get("kind") == "LabelStmt"
                               for node in walk(body)), 6),
        }
        return {name: value for name, (value, limit) in metrics.items()
                if value > limit}


def main():
        """所有源函数必须通过；任何语法错误也会直接让检查失败。"""
        failed = False
        count = 0
        for path in sorted(Path("src").glob("*.c")):
                raw = path.read_bytes()
                for function, body in functions(path):
                        count += 1
                        violations = check_function(raw, function, body)
                        if violations:
                                failed = True
                                print(path, function["name"], violations)
        print(f"function style: {count} checked")
        return int(failed)


if __name__ == "__main__":
        sys.exit(main())
