#!/usr/bin/env bash
# ============================================================
# ci/build.sh - EZOS CI 构建脚本（系统 gcc -m32 + nasm + ld）
#
# 与 worker/local 的 i686-elf-tools 方案等价：
#   - 用系统 gcc -m32 -ffreestanding 编译（Ubuntu 的 gcc-multilib）
#   - 64 位除法符号由 kernel/div64.c（__udivdi3/__umoddi3）提供
#   - 镜像大小由 tools/make_image.py 动态计算（kernel_raw.bin 真实大小取整到
#     扇区），扇区数写进引导扇区 0x1FC，boot.asm 运行时读取；不再需要
#     main/dev 各自写死 KERNEL_SECTORS
#
# 用法: bash ci/build.sh
# 产物: os-image.bin
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
LD="${LD:-ld}"
ASM="${ASM:-nasm}"
OBJCOPY="${OBJCOPY:-objcopy}"

# ---- 镜像大小动态化（单一事实来源：tools/make_image.py） ----

# ---- 编译内核 C 源（自动收集，主/dev 分支通用） ----
CFLAGS="-m32 -ffreestanding -O2 -Wall -Wextra \
        -fno-pie -fno-pic -fno-stack-protector \
        -fno-asynchronous-unwind-tables \
        -Ikernel -MMD"
LDFLAGS="-m elf_i386 -T linker.ld --oformat binary -e _start"

OBJS=""
for src in $(find kernel -name '*.c' | sort); do
    obj="${src%.c}.o"
    echo "[ci] CC ${src}"
    "$CC" $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done

# ---- 汇编 ----
echo "[ci] ASM boot/boot.asm (bin)"
"$ASM" -f bin boot/boot.asm -o boot/boot.bin
echo "[ci] ASM boot/kernel_entry.asm (elf32)"
"$ASM" -f elf32 boot/kernel_entry.asm -o boot/kernel_entry.o

# ---- 链接 + padding + 组镜像 ----
echo "[ci] LD kernel_raw.bin"
"$LD" $LDFLAGS -o kernel_raw.bin boot/kernel_entry.o $OBJS
PYTHON="${PYTHON:-python3}"
echo "[ci] ASSEMBLE kernel.bin + os-image.bin (dynamic size)"
"$PYTHON" tools/make_image.py boot/boot.bin kernel_raw.bin kernel.bin os-image.bin

echo "[ci] BUILD OK: os-image.bin ($(stat -c%s os-image.bin) bytes)"
