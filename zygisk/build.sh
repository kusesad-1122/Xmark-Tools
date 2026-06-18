#!/usr/bin/env bash
# XinmaskPlus CPU 伪装 Zygisk 模块 — 编译脚本 (署名: 苦涩or苳季)
# 依赖: Android NDK r27c (clang++ aarch64-linux-android24)
set -e
NDK="${NDK:-$HOME/android-ndk-r27c}"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++"

build() {  # $1=源文件 $2=输出
  "$CLANG" -std=c++17 -O2 -fPIC -shared -nostdlib++ -fno-exceptions -fno-rtti -s \
    -Wl,-soname,libzygisk_xinmask_cpu.so \
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
    -o "$2" "$1" -llog
  echo "built: $2"
}

build zygisk_cpuinfo.cpp     arm64-v8a.so

# 自检(可选):
# llvm-readelf -l arm64-v8a.so | grep LOAD        # 对齐应为 0x4000
# llvm-readelf -d arm64-v8a.so | grep -i soname   # 应有 SONAME
# llvm-readelf --dyn-syms arm64-v8a.so | grep zygisk_  # 两个入口