# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
"""用真实 MP4、MKV 和裸视频验证总帧数；临时媒体自动清理。"""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
        """覆盖流帧数、容器时长回退，以及总时长仍未知的情况。"""
        binary = str(Path(sys.argv[1]).resolve())
        with tempfile.TemporaryDirectory(prefix="miku-video-") as directory:
                for extension in ("mp4", "mkv", "h264"):
                        path = Path(directory) / f"其他 video's.{extension}"
                        subprocess.run([
                                "ffmpeg", "-nostdin", "-v", "error",
                                "-f", "lavfi", "-i",
                                "color=size=160x90:rate=25:duration=2",
                                "-c:v", "libx264", "-threads", "1", str(path),
                        ], check=True, timeout=10)
                        expected = "0" if extension == "h264" else "50"
                        subprocess.run([
                                binary, str(path), expected,
                        ], check=True, timeout=5)
        print("video metadata: PASS")


if __name__ == "__main__":
        main()
