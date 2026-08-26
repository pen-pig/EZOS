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

OBJS="boot/kernel_entry.o kernel/kernel.o kernel/tty.o kernel/idt.o kernel/isr.o kernel/keyboard.o kernel/ata.o kernel/shell.o kernel/shell_extra.o kernel/exfat.o kernel/gfx.o kernel/gui.o kernel/mouse.o kernel/gfxwin.o kernel/desktop.o kernel/games.o"
SRCS="kernel/kernel.c kernel/tty.c kernel/idt.c kernel/isr.c kernel/keyboard.c kernel/ata.c kernel/shell.c kernel/shell_extra.c kernel/exfat.c kernel/gfx.c kernel/gui.c kernel/mouse.c kernel/gfxwin.c kernel/desktop.c kernel/games.c"

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

echo "[5/6] 填充内核到 256KB..."
"$OBJCOPY" -I binary -O binary --pad-to 262144 kernel_raw.bin kernel.bin

echo "[6/6] 生成系统镜像..."
cat boot/boot.bin kernel.bin > os-image.bin

echo ""
echo "构建成功！已生成 os-image.bin"

if [[ "$MODE" == "build-only" || "$MODE" == "no-run" ]]; then
    echo "本次构建未启动 QEMU（使用了 $MODE 模式）。"
    exit 0
fi

echo "正在重建 disk.img（exFAT 布局）..."
"$PYTHON" "$(dirname "$0")/temp/gen_diskimg.py" disk.img

echo "正在启动 QEMU..."
"$QEMU" -vga std -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
