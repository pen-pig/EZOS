#!/usr/bin/env bash
# 构建最小 32 位 UEFI 应用 BOOTIA32.EFI
# 工具链：i686-elf-gcc 编译(产出 ELF32 目标文件) + Dev-Cpp 的 ld 链接成 PE32(-mi386pe -subsystem 10)
# 用法：bash uefi/build.sh   （从仓库根 D:/MyOS/src 执行，或任意目录均可，路径为绝对/相对自适应）
set -e

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SRC_DIR/esp/EFI/BOOT"

# 工具链路径（已确认存在于本机）
I686_GCC="/d/MyOS/tools/i686-elf-tools-windows/bin/i686-elf-gcc"
MINGW_LD="/c/Program Files (x86)/Dev-Cpp/MinGW64/bin/ld.exe"

# 若 i686-elf-gcc 不在默认相对位置，尝试 PATH
if [ ! -x "$I686_GCC" ]; then
  I686_GCC="$(command -v i686-elf-gcc || true)"
fi
if [ -z "$I686_GCC" ] || [ ! -x "$I686_GCC" ]; then
  echo "ERROR: 找不到 i686-elf-gcc" >&2
  exit 1
fi
if [ ! -x "$MINGW_LD" ]; then
  echo "ERROR: 找不到 Dev-Cpp 的 ld.exe" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# i686-elf-gcc / ld 均为原生 Windows 程序，需要将路径转为 Windows 形态
WSRC="$(cygpath -w "$SRC_DIR")"
WOUT="$(cygpath -w "$OUT_DIR")"

# 1) 编译（-ffreestanding 不依赖 libc；-fno-pic 避免 GOT 重定位；-fshort-wchar 不使用）
"$I686_GCC" -c -ffreestanding -fno-builtin -fno-stack-protector -fno-ident \
  -fno-asynchronous-unwind-tables -fno-pic -mno-sse -mno-mmx \
  -O2 -Wall -Wextra \
  -I"$WSRC" "$WSRC\\main.c" -o "$WSRC\\main.o"

# 2) 链接为 PE32 EFI 应用（subsystem 10 = EFI Application）
"$MINGW_LD" -mi386pe --subsystem 10 --entry EfiMain --strip-all \
  "$WSRC\\main.o" -o "$WOUT\\BOOTIA32.EFI"

# 3) 移除 .comment 段（其 VMA 低于 image base 会导致 OVMF 加载器拒绝，报 Load Error）
OBJCOPY="/c/Program Files (x86)/Dev-Cpp/MinGW64/bin/objcopy.exe"
"$OBJCOPY" --remove-section .comment "$WOUT\\BOOTIA32.EFI"

echo "BUILD OK -> $OUT_DIR/BOOTIA32.EFI"
