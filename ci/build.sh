#!/usr/bin/env bash
# ============================================================
# ci/build.sh - EZOS CI 构建脚本（系统 gcc -m32 + nasm + ld）
#
# 与 worker/local 的 i686-elf-tools 方案等价：
#   - 用系统 gcc -m32 -ffreestanding 编译（Ubuntu 的 gcc-multilib）
#   - 64 位除法符号由 kernel/div64.c（__udivdi3/__umoddi3）提供
#   - padding 尺寸从 boot/boot.asm 的 KERNEL_SECTORS 自动推导，
#     避免 main(512扇区/256KB) 与 dev(768扇区/384KB) 写死不一致
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

# ---- 从 boot.asm 推导 padding（KERNEL_SECTORS * 512） ----
KERNEL_SECTORS="$(grep -oiE 'KERNEL_SECTORS[[:space:]]+equ[[:space:]]+[0-9]+' boot/boot.asm | grep -oE '[0-9]+' | head -1)"
if [ -z "${KERNEL_SECTORS}" ]; then
    echo "[ci] ERROR: cannot find KERNEL_SECTORS in boot/boot.asm" >&2
    exit 1
fi
PAD_BYTES=$((KERNEL_SECTORS * 512))
echo "[ci] KERNEL_SECTORS=${KERNEL_SECTORS}  =>  pad-to ${PAD_BYTES} bytes"

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
echo "[ci] OBJCOPY pad to ${PAD_BYTES}"
"$OBJCOPY" -I binary -O binary --pad-to "${PAD_BYTES}" kernel_raw.bin kernel.bin
echo "[ci] ASSEMBLE os-image.bin"
cat boot/boot.bin kernel.bin > os-image.bin

echo "[ci] BUILD OK: os-image.bin ($(stat -c%s os-image.bin) bytes)"
