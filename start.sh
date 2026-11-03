#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
#
# ========================================
# 彩色 ASCII 视频转换器启动脚本（C17 版）
#
# 流程：检测系统 → 检查依赖（缺的才装）→ 用 make 编译 → 运行 build/miku
#
# 用 POSIX sh 编写：Termux 只有 /bin/sh，没有 bash；Debian 的 /bin/sh 是 dash，
# 所以颜色输出一律用 printf，不用 echo -e。
#
# 用法：
#   ./start.sh                          进入终端界面（选文件、风格、比例、帧率）
#   ./start.sh 输入.mp4 输出.mp4 320    指定文件与 ASCII 宽度
#   ./start.sh --play 输入.mp4 160      转完直接播放
#   ./start.sh --play-only 输入.mp4     不转码，直接播放
#   ./start.sh --deps                   只列出依赖与检测结果，不安装、不编译
# 其余参数原样交给 build/miku，参数说明见 ./build/miku（不带参数运行时的提示）。
#
# 环境变量：
#   MIKU_NO_INSTALL=1   不自动安装依赖，缺什么只提示（适合 CI 或已手动装好的机器）
#
# 依赖清单（同时见 Documentation/build.rst）：
#   必需  C 编译器（cc / gcc / clang 之一）、make、ffmpeg 与 ffprobe（建议含 libx264）
#   可选  mpv 或 ffplay：播放声音；fzf 或 dialog：界面里的文件选择器（都没有则用内置列表）
#   系统  macOS 读内存用 sysctl，Linux 读 /proc/meminfo；其余为标准 POSIX 工具
# ========================================

set -u

C_RED='\033[0;31m'
C_GREEN='\033[0;32m'
C_YELLOW='\033[1;33m'
C_BLUE='\033[0;34m'
C_NC='\033[0m'

info()    { printf '%b[%b OK %b] %s\n' "$C_NC" "$C_GREEN" "$C_NC" "$1"; }
success() { printf '%b[%b SUCCESS %b] %s\n' "$C_NC" "$C_BLUE" "$C_NC" "$1"; }
warn()    { printf '%b[%b WARNING %b] %s\n' "$C_NC" "$C_YELLOW" "$C_NC" "$1"; }
error()   { printf '%b[%b ERROR %b] %s\n' "$C_NC" "$C_RED" "$C_NC" "$1" >&2; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR" || exit 1

OS=unknown
OS_NAME=$(uname -s)
OS_LIKE=""
JOBS=2

have() {
        command -v "$1" >/dev/null 2>&1
}

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
                Linux) ;;
                *)
                        OS=unknown
                        OS_NAME=$(uname -s)
                        return
                        ;;
        esac
        if [ -f /etc/os-release ]; then
                # shellcheck disable=SC1091
                . /etc/os-release
                OS=${ID:-unknown}
                OS_LIKE=${ID_LIKE:-}
                OS_NAME=${PRETTY_NAME:-${NAME:-Linux}}
        elif have lsb_release; then
                OS=$(lsb_release -si | tr '[:upper:]' '[:lower:]')
                OS_NAME=$(lsb_release -sd)
        fi
}

# 包管理器按 ID 与 ID_LIKE 判断，衍生发行版（Mint、Manjaro、Rocky 等）也能识别
pkg_manager() {
        case "$OS" in
                termux) echo pkg; return ;;
                macos) echo brew; return ;;
        esac
        case " $OS $OS_LIKE " in
                *" fedora "*|*" rhel "*|*" centos "*)
                        if have dnf; then echo dnf; else echo yum; fi
                        ;;
                *" debian "*|*" ubuntu "*) echo apt ;;
                *" arch "*) echo pacman ;;
                *) echo none ;;
        esac
}

# 需要提权的命令前缀（Termux 与 root 都不用 sudo）
sudo_prefix() {
        if [ "$OS" = termux ] || [ "$(id -u)" = 0 ]; then
                echo ""
        elif have sudo; then
                echo "sudo"
        else
                echo ""
        fi
}

# 依赖命令 -> 包名。C 编译器的包名因系统而异；macOS 的 cc 由 Xcode 命令行工具提供
pkg_name() {
        case "$1" in
                cc)
                        case "$OS" in
                                termux) echo clang ;;
                                macos) echo "" ;;
                                *) echo gcc ;;
                        esac
                        ;;
                *) echo "$1" ;;
        esac
}

find_cc() {
        for c in cc gcc clang; do
                if have "$c"; then
                        echo "$c"
                        return 0
                fi
        done
        return 1
}

# 列出缺失的依赖（空格分隔的命令名）。mpv/ffplay、fzf/dialog 任一存在即可
collect_missing() {
        missing=""
        find_cc >/dev/null || missing="$missing cc"
        have make || missing="$missing make"
        have ffmpeg || missing="$missing ffmpeg"
        have ffprobe || missing="$missing ffprobe"
        have mpv || have ffplay || missing="$missing mpv"
        have fzf || have dialog || missing="$missing fzf"
        echo "${missing# }"
}

# 把缺失的命令映射成去重后的包名列表
packages_for() {
        pkgs=""
        for cmd in $1; do
                name=$(pkg_name "$cmd")
                [ -n "$name" ] || continue
                case " $pkgs " in
                        *" $name "*) ;;
                        *) pkgs="$pkgs $name" ;;
                esac
        done
        echo "${pkgs# }"
}

install_pkgs() {
        mgr=$(pkg_manager)
        sudo=$(sudo_prefix)
        case "$mgr" in
                pkg)
                        pkg install -y "$@"
                        ;;
                brew)
                        have brew || { error "未找到 Homebrew，请先安装: https://brew.sh"; return 1; }
                        brew install "$@"
                        ;;
                apt)
                        $sudo apt-get update -y >/dev/null 2>&1
                        $sudo apt-get install -y "$@"
                        ;;
                dnf)
                        $sudo dnf install -y "$@"
                        ;;
                yum)
                        $sudo yum install -y "$@"
                        ;;
                pacman)
                        $sudo pacman -Sy --needed --noconfirm "$@"
                        ;;
                *)
                        return 1
                        ;;
        esac
}

# 给用户看的手动安装命令（自动安装失败或系统不支持时使用）
install_hint() {
        case "$OS" in
                termux) echo "pkg install clang make ffmpeg mpv fzf" ;;
                macos) echo "xcode-select --install；brew install make ffmpeg mpv fzf" ;;
                *)
                        case "$(pkg_manager)" in
                                apt) echo "sudo apt install gcc make ffmpeg mpv fzf" ;;
                                dnf|yum) echo "sudo dnf install gcc make ffmpeg mpv fzf（Fedora 的 ffmpeg 需先启用 RPM Fusion）" ;;
                                pacman) echo "sudo pacman -S gcc make ffmpeg mpv fzf" ;;
                                *) echo "请用发行版的包管理器安装 gcc、make、ffmpeg、mpv、fzf" ;;
                        esac
                        ;;
        esac
}

# --deps：逐项列出依赖状态，不做任何安装
print_deps() {
        printf '依赖清单（%s）\n' "$OS_NAME"
        if find_cc >/dev/null; then printf '  [必需] C 编译器      已安装 (%s)\n' "$(find_cc)"; else printf '  [必需] C 编译器      缺失（cc / gcc / clang）\n'; fi
        if have make; then printf '  [必需] make          已安装\n'; else printf '  [必需] make          缺失\n'; fi
        if have ffmpeg; then printf '  [必需] ffmpeg        已安装\n'; else printf '  [必需] ffmpeg        缺失\n'; fi
        if have ffprobe; then printf '  [必需] ffprobe       已安装\n'; else printf '  [必需] ffprobe       缺失\n'; fi
        if have ffmpeg && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264; then
                printf '  [建议] libx264       可用\n'
        else
                printf '  [建议] libx264       不可用（将退回其他编码器，体积与画质会有差异）\n'
        fi
        if have mpv; then printf '  [可选] 播放声音      mpv\n';
        elif have ffplay; then printf '  [可选] 播放声音      ffplay\n';
        else printf '  [可选] 播放声音      缺失（无声播放）\n'; fi
        if have fzf; then printf '  [可选] 文件选择器    fzf\n';
        elif have dialog; then printf '  [可选] 文件选择器    dialog\n';
        else printf '  [可选] 文件选择器    缺失（使用内置列表）\n'; fi
}

ensure_deps() {
        missing=$(collect_missing)
        if [ -z "$missing" ]; then
                success "依赖已满足"
        elif [ "${MIKU_NO_INSTALL:-0}" = 1 ]; then
                warn "缺少依赖: $missing（MIKU_NO_INSTALL=1，不自动安装）"
        else
                pkgs=$(packages_for "$missing")
                info "缺少依赖: $missing，尝试安装: ${pkgs:-（无可用包名）}"
                if [ -n "$pkgs" ]; then
                        # shellcheck disable=SC2086
                        install_pkgs $pkgs || warn "自动安装未成功，可手动执行: $(install_hint)"
                fi
        fi

        # 必需项仍缺失就直接失败；可选项只提醒
        if ! find_cc >/dev/null; then
                error "找不到 C 编译器（cc / gcc / clang）"
                info "手动安装: $(install_hint)"
                return 1
        fi
        if ! have make; then
                error "找不到 make"
                info "手动安装: $(install_hint)"
                return 1
        fi
        if ! have ffmpeg || ! have ffprobe; then
                error "ffmpeg / ffprobe 不可用"
                info "手动安装: $(install_hint)"
                return 1
        fi
        if ! have mpv && ! have ffplay; then
                warn "没有 mpv / ffplay，转换不受影响，但播放时没有声音"
        fi
        success "必需依赖就绪"
        return 0
}

# 用 Makefile 编译；参数依次为 CC 与 OPT。日志写入 build/make.log
run_make() {
        make -j"$JOBS" CC="$1" OPT="$2" WERROR= >build/make.log 2>&1
}

build_miku() {
        cc=$(find_cc) || return 1
        [ -f Makefile ] || { error "当前目录缺少 Makefile"; return 1; }
        mkdir -p build || return 1
        JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null) || JOBS=2
        [ -n "$JOBS" ] || JOBS=2
        info "编译 src/ 中的模块（make，$JOBS 个并行任务）→ build/miku"
        if run_make "$cc" "-O3 -march=native"; then
                success "编译成功（已针对本机 CPU 优化）"
                return 0
        fi
        warn "-march=native 不可用，清理后改用通用 -O3 重新编译"
        make clean >/dev/null 2>&1
        if run_make "$cc" "-O3"; then
                success "编译成功"
                return 0
        fi
        error "编译失败，最后几行日志："
        tail -n 20 build/make.log >&2
        error "完整日志见 build/make.log"
        return 1
}

main() {
        detect_os
        if [ "${1:-}" = "--deps" ]; then
                print_deps
                exit 0
        fi
        info "检测到系统: $OS_NAME"

        ensure_deps || exit 1
        build_miku || exit 1

        # 参数原样交给 miku；没有参数时 miku 进入终端界面
        exec ./build/miku "$@"
}

main "$@"
