#!/usr/bin/env bash
# 交叉编译 Windows 单文件 EXE
# 首选 mingw-w64；没有时退回 zig 自带工具链（zig 需另装）
set -e
cd "$(dirname "$0")/.."
mkdir -p build dist

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    x86_64-w64-mingw32-windres src/win/app.rc -O coff -I src/win -o build/app.res.o
    x86_64-w64-mingw32-gcc -O2 -Isrc -municode -mwindows -static -static-libgcc \
        -o "dist/隐渊.exe" \
        src/core.c src/win/gui.c build/app.res.o \
        -lcomctl32 -lcomdlg32 -lshell32 -luuid -lmsimg32
    x86_64-w64-mingw32-strip "dist/隐渊.exe"
elif [ -n "$ZIG" ] || command -v zig >/dev/null 2>&1; then
    ZIG="${ZIG:-zig}"
    test -f build/app.res.o || { echo "缺少 build/app.res.o（资源对象需先用 mingw 的 windres 生成）"; exit 1; }
    "$ZIG" cc -target x86_64-windows-gnu -O2 -Isrc -municode -static -static-libgcc \
        -Wl,--subsystem,windows -o "dist/隐渊.exe" \
        src/core.c src/win/gui.c build/app.res.o \
        -lgdi32 -luser32 -lkernel32 -lcomctl32 -lcomdlg32 -lshell32 -luuid -lmsimg32 -ladvapi32
else
    echo "没有可用的交叉工具链（mingw-w64 或 zig）"; exit 1
fi
ls -l "dist/隐渊.exe"
