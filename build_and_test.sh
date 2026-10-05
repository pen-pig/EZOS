#!/usr/bin/env bash
# ============================================================
# build_and_test.sh - EZOS (MyOS) Linux 构建脚本
#
# 用法:
#   ./build_and_test.sh                # 构建并启动 QEMU
#   ./build_and_test.sh build-only     # 只构建，不启动 QEMU
#   ./build_and_test.sh no-run         # 同 build-only
#   ./build_and_test.sh clean          # 清理构建产物
#
# 工具链解析（不写死路径）:
#   1. 环境变量覆盖: EZOS_CC / EZOS_LD / EZOS_ASM / EZOS_OBJCOPY / EZOS_QEMU / EZOS_PYTHON
#   2. EZOS_TOOLS=<工具链目录> 指定后自动加入 PATH
#   3. 默认从 PATH 中查找 i686-elf-gcc / nasm / qemu-system-x86_64 等
#
# 示例:
#   EZOS_TOOLS=/opt/i686-elf-tools ./build_and_test.sh build-only
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-run}"

# ---- 工具链解析 ----
CC="${EZOS_CC:-i686-elf-gcc}"
LD="${EZOS_LD:-i686-elf-ld}"
ASM="${EZOS_ASM:-nasm}"
OBJCOPY="${EZOS_OBJCOPY:-i686-elf-objcopy}"
QEMU="${EZOS_QEMU:-qemu-system-x86_64}"
PYTHON="${EZOS_PYTHON:-python3}"

# 若指定 EZOS_TOOLS 目录，则加入 PATH 供命令查找
if [[ -n "${EZOS_TOOLS:-}" ]]; then
    export PATH="$EZOS_TOOLS:$EZOS_TOOLS/bin:$PATH"
fi

for tool in "$CC" "$LD" "$ASM" "$OBJCOPY"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[错误] 未找到 $tool，请安装工具链、将其加入 PATH，或用 EZOS_TOOLS 指定目录" >&2
        exit 1
    fi
done

CFLAGS="-ffreestanding -O2 -Wall -Wextra -Ikernel"
LDFLAGS="-m elf_i386 -T linker.ld --oformat binary -e _start"

# 源文件自动收集（与 ci/build.sh 一致）：新增 kernel/*.c 无需改本脚本。
# 早期版本在这里硬编码 SRCS/OBJS 两份列表，新增模块时极易漏改 OBJS 一侧，
# 表现为链接期一堆 "undefined reference"（症状与代码错误难区分）。
# OBJS 也由 SRCS 推导，保证两侧永不脱节；kernel_entry.o 单独前置（入口）。
SRCS="$(find kernel -name '*.c' | sort)"
OBJS="boot/kernel_entry.o"
for src in $SRCS; do
    OBJS="$OBJS ${src%.c}.o"
done

clean() {
    echo "正在清理构建产物..."
    rm -f boot/boot.bin boot/kernel_entry.o kernel/*.o kernel_raw.bin kernel.bin os-image.bin
    echo "清理完成！"
}

if [[ "$MODE" == "clean" ]]; then
    clean
    exit 0
fi

echo "============================================"
echo "正在构建 My OS..."
echo "============================================"

echo "[1/6] 汇编引导扇区..."
"$ASM" -f bin boot/boot.asm -o boot/boot.bin

echo "[2/6] 汇编内核入口..."
"$ASM" -f elf32 boot/kernel_entry.asm -o boot/kernel_entry.o

echo "[3/6] 编译内核 C 文件..."
for src in $SRCS; do
    obj="${src%.c}.o"
    echo "  CC $src"
    "$CC" $CFLAGS -c "$src" -o "$obj"
done

echo "[4/6] 链接内核..."
"$LD" $LDFLAGS -o kernel_raw.bin $OBJS

# 镜像大小是动态的：tools/make_image.py 按 kernel_raw.bin 真实大小算扇区数，
# 写进引导扇区 0x1FC，boot.asm 运行时读它。不再从 boot.asm 解析 KERNEL_SECTORS
# （旧做法：boot.asm 写死 992、objcopy 写死 --pad-to 507904，两边手工同步）。
echo "[5/6] 生成镜像（动态大小）..."
"$PYTHON" "$(dirname "$0")/tools/make_image.py" \
    boot/boot.bin kernel_raw.bin kernel.bin os-image.bin
if [ $? -ne 0 ]; then echo "[错误] make_image.py 失败" >&2; exit 1; fi

echo ""
echo "构建成功！已生成 os-image.bin"

if [[ "$MODE" == "build-only" || "$MODE" == "no-run" ]]; then
    echo "本次构建未启动 QEMU（使用了 $MODE 模式）。"
    exit 0
fi

echo "正在重建 disk.vhd（exFAT 布局）..."
"$PYTHON" "$(dirname "$0")/temp/gen_diskimg.py" disk.vhd

echo "正在启动 QEMU..."
"$QEMU" -icount shift=auto -vga std -drive format=raw,file=os-image.bin -drive format=raw,file=disk.vhd
