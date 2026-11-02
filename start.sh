#!/bin/sh

# ========================================
# 彩色ASCII视频转换器启动脚本（C17 版）
# 功能：检测系统（Termux / macOS / Linux 发行版）→ 装缺的依赖 → 编译 → 运行 miku
# 作者: LinusTrevian（你猜我在线不）+ Claude Opus 5.5 Medium + GPT 4
#
# 用 POSIX sh 写：Termux 只有 /bin/sh，没有 /bin/bash 和 /usr/bin/env
#
# 用法：
#   ./start.sh                          # 用当前目录的 miku.mp4 转成 ascii.mp4
#   ./start.sh 输入.mp4 输出.mp4 320    # 指定文件与 ASCII 宽度
#   ./start.sh --play 输入.mp4 160      # 转完直接在终端播放（省略输出名会用临时文件）
#   ./start.sh --play-only 输入.mp4     # 不转码，直接播放
# 其余参数原样交给 miku，见 ./miku（不带参数）的提示
# ========================================

set -u

RED='\033[0;31m'       # 红色：错误
GREEN='\033[0;32m'     # 绿色：普通状态
YELLOW='\033[1;33m'    # 黄色：警告
BLUE='\033[0;34m'      # 蓝色：成功
NC='\033[0m'           # 重置颜色

print_info() { echo -e "${NC}[${GREEN} OK ${NC}] $1"; }
print_success() { echo -e "${NC}[${BLUE} SUCCESS ${NC}] $1"; }
print_warning() { echo -e "${NC}[${YELLOW} WARNING ${NC}] $1"; }
print_error() { echo -e "${NC}[${RED} ERROR ${NC}] $1"; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

# 检测系统：Termux 与 macOS 必须排在 Linux 发行版判断之前
detect_os() {
    if [ -n "${TERMUX_VERSION:-}" ] || [ -d /data/data/com.termux/files/usr ]; then
        OS=termux
        OS_NAME="Termux (Android)"
        return
    fi
    case "$(uname -s)" in
        Darwin)
            OS=macos
            OS_NAME="macOS $(sw_vers -productVersion 2>/dev/null)"
            return
            ;;
        Linux) : ;;
        *)
            OS=unknown
            OS_NAME=$(uname -s)
            return
            ;;
    esac
    if [ -f /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        OS=$ID
        OS_NAME=$NAME
    elif command -v lsb_release >/dev/null 2>&1; then
        OS=$(lsb_release -si | tr '[:upper:]' '[:lower:]')
        OS_NAME=$(lsb_release -sd)
    else
        OS=unknown
        OS_NAME=$(uname -s)
    fi
}

# 需要提权的命令前缀（Termux 与 root 都不用 sudo）
sudo_prefix() {
    if [ "$OS" = termux ] || [ "$(id -u)" = 0 ]; then
        echo ""
    elif command -v sudo >/dev/null 2>&1; then
        echo "sudo"
    else
        echo ""
    fi
}

# 安装依赖；返回非 0 表示这个系统没法自动装
install_pkgs() {
    local sudo
    sudo=$(sudo_prefix)
    case "$OS" in
        termux)
            # Termux 不做整机升级，只装缺的包
            pkg install -y "$@" >/dev/null 2>&1 || return 1
            ;;
        macos)
            command -v brew >/dev/null 2>&1 || return 1
            brew install "$@" >/dev/null 2>&1 || return 1
            ;;
        ubuntu|debian|linuxmint|pop|raspbian)
            $sudo apt-get update -y >/dev/null 2>&1
            $sudo apt-get install -y "$@" || return 1
            ;;
        fedora)
            $sudo dnf install -y "$@" || return 1
            ;;
        centos|rhel|rocky|almalinux)
            if command -v dnf >/dev/null 2>&1; then
                $sudo dnf install -y "$@" || return 1
            else
                $sudo yum install -y "$@" || return 1
            fi
            ;;
        arch|manjaro|endeavouros|artix)
            $sudo pacman -Sy --noconfirm "$@" || return 1
            ;;
        *)
            return 1
            ;;
    esac
    return 0
}

# 找一个可用的 C 编译器
find_cc() {
    for c in gcc cc clang; do
        if command -v "$c" >/dev/null 2>&1; then
            echo "$c"
            return 0
        fi
    done
    return 1
}

ensure_deps() {
    # 用字符串而不是数组：Android 的 /bin/sh 是老 mksh，dash/ash 也对数组支持不一
    missing=""
    if ! find_cc >/dev/null; then
        missing="$missing gcc"
    fi
    command -v ffmpeg >/dev/null 2>&1 || missing="$missing ffmpeg"
    command -v ffprobe >/dev/null 2>&1 || missing="$missing ffmpeg"
    # 播放与 --play 的声音都用它；ffplay 也算
    if ! command -v mpv >/dev/null 2>&1 && ! command -v ffplay >/dev/null 2>&1; then
        missing="$missing mpv"
    fi
    # 界面里的文件选择器：fzf 最好用，dialog 次之；两个都没有就把 fzf 装上
    if ! command -v fzf >/dev/null 2>&1 && ! command -v dialog >/dev/null 2>&1; then
        missing="$missing fzf"
    fi
    missing=$(echo "$missing" | tr ' ' '\n' | sort -u | tr '\n' ' ')
    missing=${missing# }

    if [ -z "$missing" ]; then
        print_success "依赖已满足"
        return 0
    fi

    print_info "缺少依赖: $missing，尝试安装..."
    # shellcheck disable=SC2086
    install_pkgs $missing

    if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
        print_error "ffmpeg / ffprobe 仍不可用，请手动安装后重试"
        print_info "Termux: pkg install ffmpeg  |  Debian: apt install ffmpeg  |  macOS: brew install ffmpeg"
        return 1
    fi
    if ! find_cc >/dev/null; then
        print_error "找不到 C 编译器，请手动安装 gcc 或 clang"
        return 1
    fi
    print_success "依赖安装完成"
    return 0
}

build_miku() {
    local cc
    cc=$(find_cc) || return 1
    print_info "编译: $cc -O3 -std=c17 -Wall -Wextra -o miku miku.c -lm"
    if "$cc" -O3 -march=native -std=c17 -Wall -Wextra -o miku miku.c -lm 2>/dev/null; then
        print_success "编译成功（已针对本机 CPU 优化）"
        return 0
    fi
    print_warning "-march=native 不可用，去掉后重试"
    if "$cc" -O3 -std=c17 -Wall -Wextra -o miku miku.c -lm; then
        print_success "编译成功"
        return 0
    fi
    print_error "编译失败，请检查 miku.c 与编译器的报错"
    return 1
}

main() {
    detect_os
    print_info "检测到系统: $OS_NAME"

    if [ ! -f miku.c ]; then
        print_error "当前目录没有 miku.c"
        exit 1
    fi

    ensure_deps || exit 1
    build_miku || exit 1

    # 有参数就原样交给 miku；没参数时用默认样片
    if [ $# -gt 0 ]; then
        exec ./miku "$@"
    fi

    # 没有参数：直接进终端界面（界面里能选文件、选风格、选比例与帧率）
    exec ./miku
}

main "$@"
