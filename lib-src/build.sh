#!/usr/bin/env bash
# 本地复现构建 lib/drmid_hook.so
#
#   ./lib-src/build.sh              # 自动找 NDK
#   ANDROID_NDK=/path/to/ndk ./lib-src/build.sh
#
# 产物写到 lib/drmid_hook.so（与 release workflow 里的路径一致）。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="$ROOT/lib/drmid_hook.so"

# ---- 定位 NDK ----
find_ndk() {
    if [ -n "${ANDROID_NDK:-}" ] && [ -d "$ANDROID_NDK" ]; then
        echo "$ANDROID_NDK"; return 0
    fi
    if [ -n "${ANDROID_NDK_ROOT:-}" ] && [ -d "$ANDROID_NDK_ROOT" ]; then
        echo "$ANDROID_NDK_ROOT"; return 0
    fi
    if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        echo "$ANDROID_NDK_HOME"; return 0
    fi
    # macOS 常见路径
    for cand in "$HOME/Library/Android/sdk/ndk/"*; do
        [ -d "$cand" ] && { echo "$cand"; return 0; }
    done
    # Linux 常见路径
    for cand in "$HOME/Android/Sdk/ndk/"* /opt/android-ndk* /usr/lib/android-ndk \
                /usr/local/lib/android/sdk/ndk/* /opt/hostedtoolcache/Android-NDK/*; do
        [ -d "$cand" ] && { echo "$cand"; return 0; }
    done
    return 1
}

if ! NDK="$(find_ndk)"; then
    cat >&2 <<'EOF'
error: 找不到 Android NDK。

本仓库不提交预编译的 lib/drmid_hook.so（避免把二进制塞进 git），
需要在本地装 NDK 后构建，或直接下载 CI 产物：
  https://github.com/<owner>/Xmark-Tools/releases

自动安装（Linux，需 curl + unzip）：
  NDK_VER=r27c
  curl -fsSL -o /tmp/ndk.zip \
    "https://dl.google.com/android/repository/android-ndk-${NDK_VER}-linux.zip"
  sudo unzip -q /tmp/ndk.zip -d "$HOME/Android/Sdk/ndk"
  # 然后重跑本脚本，find_ndk 会命中 $HOME/Android/Sdk/ndk/*

或手动安装（任选其一）：
  # Android Studio → SDK Manager → SDK Tools → NDK (Side by side)
  # 或命令行：
  sdkmanager "ndk;26.1.10909125"
EOF
    exit 1
fi

# ---- 定位 host 工具链目录 ----
# NDK 只提供 <host>-x86_64 这一种布局，但 uname -s 的输出形式各平台不一致
# （Linux 是 linux / Darwin 是 darwin / Git-Bash 下可能被截断），逐个试。
pick_toolchain_dir() {
    local ndk="$1"
    for host in \
        "$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64" \
        linux-x86_64 darwin-x86_64 windows-x86_64 \
        linux-x86 darwin-x86; do
        if [ -d "$ndk/toolchains/llvm/prebuilt/$host/bin" ]; then
            echo "$ndk/toolchains/llvm/prebuilt/$host/bin"
            return 0
        fi
    done
    return 1
}

if ! TOOLCHAIN_BIN="$(pick_toolchain_dir "$NDK")"; then
    echo "error: NDK 存在但找不到 llvm 工具链目录：$NDK/toolchains/llvm/prebuilt/*/bin" >&2
    ls -1 "$NDK/toolchains/llvm/prebuilt/" 2>/dev/null >&2 || true
    exit 1
fi

CC="$TOOLCHAIN_BIN/aarch64-linux-android26-clang"
[ -x "$CC" ] || CC="$TOOLCHAIN_BIN/aarch64-linux-android26-clang.exe"
[ -x "$CC" ] || { echo "error: 找不到编译器 $CC" >&2; exit 1; }

echo "NDK      : $NDK"
echo "toolchain: $TOOLCHAIN_BIN"
echo "CC       : $CC"
echo "source   : $HERE/drmid_hook.c"
echo "output   : $OUT"

mkdir -p "$(dirname "$OUT")"

# -fvisibility=hidden : 只导出显式标了 visibility("default") 的符号
#                       (A64HookFunction / A64HookFunctionV / zn_module /
#                        JNI_OnLoad / drmid_*)，其余全部隐藏，
#                       避免污染被注入进程的全局符号表。
# -fno-stack-protector : trampoline 逻辑不需要，且 .plt 里少一个 __stack_chk_fail
# -O2                  : 常规优化
"$CC" \
    -shared -fPIC -O2 -fvisibility=hidden \
    -fno-stack-protector \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    "$HERE/drmid_hook.c" \
    -llog

echo
echo "构建完成:"
ls -la "$OUT"

# ---- 产物自检 ----
if command -v file >/dev/null 2>&1; then
    echo
    file "$OUT"
fi

# 确认关键导出符号都在（用 readelf，NDK 自带 llvm-readelf）
READELF="$TOOLCHAIN_BIN/llvm-readelf"
[ -x "$READELF" ] || READELF="$TOOLCHAIN_BIN/llvm-readelf.exe"
if [ -x "$READELF" ]; then
    echo
    echo "=== 导出符号 ==="
    # 注意：build.sh 用 -fvisibility=hidden，所以每个符号都必须
    # 在 .c 里显式标 visibility("default")。少标一个这里就会漏。
    missing=""
    for sym in A64HookFunction A64HookFunctionV zn_module JNI_OnLoad; do
        if "$READELF" --dyn-syms "$OUT" | grep -qE "[[:space:]]${sym}\$"; then
            echo "  ok   $sym"
        else
            echo "  MISS $sym" >&2
            missing="$missing $sym"
        fi
    done
    if [ -n "$missing" ]; then
        echo "error: 关键导出符号缺失:$missing" >&2
        echo "      检查 drmid_hook.c 是否漏了 __attribute__((visibility(\"default\")))" >&2
        exit 1
    fi
fi

# 确认是 aarch64
if command -v file >/dev/null 2>&1; then
    file "$OUT" | grep -q 'aarch64\|ARM aarch64' || {
        echo "error: 产物不是 aarch64" >&2; exit 1
    }
fi

echo
echo "OK"
