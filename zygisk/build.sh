#!/usr/bin/env bash
# XinmaskPlus CPU 伪装 Zygisk 模块 — 编译脚本 (署名: 苦涩or苳季)
# 依赖: Android NDK r27c (clang++ aarch64-linux-android24)
set -e
NDK="${NDK:-$HOME/android-ndk-r27c}"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++"

# Step 1: Generate cpuinfo_presets.h from cpuinfo_* text files
python3 gen_presets.py

build() {  # $1=源文件 $2=输出
  "$CLANG" -std=c++17 -O2 -fPIC -shared -nostdlib++ -fno-exceptions -fno-rtti -s \
    -Wl,-soname,libzygisk_xinmask_cpu.so \
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
    -o "$2" "$1" -llog
  echo "built: $2"
}

build zygisk_cpuinfo.cpp arm64-v8a.so