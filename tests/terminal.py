# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
"""在独立伪终端中验证退出和回显恢复，不改写用户终端。"""

import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time


def pump(master, seconds):
        """持续排空输出，防止测试自身的终端背压阻塞播放器。"""
        deadline = time.monotonic() + seconds
        output = bytearray()
        while time.monotonic() < deadline:
                if select.select([master], [], [], 0.02)[0]:
                        output.extend(os.read(master, 65536))
        return output


def run_case(binary, arguments, actions, label):
        """每个动作在有限等待后执行，所有退出路径回收测试进程组。"""
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, 100, 0, 0))
        before = termios.tcgetattr(slave)
        process = subprocess.Popen(
                [binary, *arguments], stdin=slave, stdout=slave, stderr=slave,
                start_new_session=True,
        )
        output = bytearray()
        try:
                for delay, action in actions:
                        output.extend(pump(master, delay))
                        if isinstance(action, bytes):
                                os.write(master, action)
                        elif action is not None:
                                os.kill(process.pid, action)
                deadline = time.monotonic() + 12
                while process.poll() is None and time.monotonic() < deadline:
                        output.extend(pump(master, 0.05))
                assert process.poll() is not None, f"{label}: timeout"
                assert process.returncode == 0, (
                        label, process.returncode, output[-1000:]
                )
                assert termios.tcgetattr(slave) == before, label
                print(f"{label}: exit=0, terminal restored")
                return output
        finally:
                if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=2)
                os.close(master)
                os.close(slave)


def main():
        """覆盖正常结束、按键、信号，以及两次界面与播放模式切换。"""
        binary = os.path.abspath(sys.argv[1])
        fixture = os.path.abspath(sys.argv[2])
        run_case(binary, [], [(0.2, b"q")], "TUI quit")
        run_case(binary, [], [(0.2, signal.SIGINT)], "TUI SIGINT")
        run_case(binary, [], [(0.2, signal.SIGTERM)], "TUI SIGTERM")
        playback = ["--play-only", fixture, "--mute"]
        run_case(binary, playback, [(0.7, b"q")], "playback quit")
        run_case(binary, playback, [(0.7, signal.SIGINT)], "playback SIGINT")
        run_case(binary, playback, [(0.7, signal.SIGTERM)], "playback SIGTERM")
        output = run_case(binary, playback, [], "muted EOF")
        assert output.count(b"\x1b[H") > 24, "no rendered frames"
        run_case(binary, ["--play-only", fixture], [], "audio EOF")
        run_case(binary, [], [
                (0.2, b"2"), (0.2, b"jjjj\r"), (1.5, b"q"),
                (0.5, b"\r"), (1.5, b"q"), (0.5, b"\x1b"), (0.2, b"q"),
        ], "TUI playback twice")


if __name__ == "__main__":
        main()
