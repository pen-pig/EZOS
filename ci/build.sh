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
# **链接清单必须和 build.ninja 的 kernel_raw.bin 一致**（见下面两处注释）：
# 少任何一个产物都是"编译全绿、链接期才炸"，报错看起来很像代码有问题。
# 这里历史上漏过两个：boot/task_switch.o（task_irq_trampoline / switch_to）
# 和 Rust/Zig 产物（*_rs / *_zig）——CI 因此连续红了很多次。
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
PYTHON="${PYTHON:-python3}"
ZIG="${ZIG:-zig}"

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
# task_switch.asm 提供 task_irq_trampoline / switch_to（kernel/task.c 引用）。
# 少了它是 "undefined reference to `task_irq_trampoline'"——链接期才炸。
echo "[ci] ASM boot/task_switch.asm (elf32)"
"$ASM" -f elf32 boot/task_switch.asm -o boot/task_switch.o

# ---- 非 C 增量（Rust / Zig） ----
# kernel/rust_bridge.h 里 EZ_EXFAT_IMPL 默认 1 = Rust：exFAT 的卷校验和 /
# 名字哈希 / entry set 校验和的**生产路径**走 *_rs，shell 的 `rstest` 还要
# *_zig 做三路对拍。这两个产物不是"可选加速"，缺了就是链接期 undefined
# reference。所以 CI 必须真的编译它们，而不是退回 C 实现——退回就等于在测
# 一份跟发布版不同的内核（生产路径那份反而没人验过）。
RUST_LIB="rust/ezos_rs/target/i686-ezos/release/libezos_rs.a"
ZIG_OBJ="rust/ezos_zig/ezos_zig.o"

echo "[ci] CARGO libezos_rs.a"
( cd rust/ezos_rs && RUSTC_BOOTSTRAP=1 cargo build --release \
    -Z build-std=core,compiler_builtins -Zjson-target-spec \
    --target ../i686-ezos.json )
if [ ! -f "$RUST_LIB" ]; then
    echo "[ci] FATAL: $RUST_LIB 没生成（rust nightly + rust-src 装了吗？）" >&2
    exit 1
fi

echo "[ci] ZIG ezos_zig.o"
( cd rust/ezos_zig && "$ZIG" build-obj -target x86-freestanding \
    -O ReleaseSafe ezos_zig.zig )
if [ ! -f "$ZIG_OBJ" ]; then
    echo "[ci] FATAL: $ZIG_OBJ 没生成（zig 0.15.1 在 PATH 里吗？）" >&2
    exit 1
fi

# ---- 链接 + padding + 组镜像 ----
# 顺序与 build.ninja 一致：启动/切换 .o → 内核 .o → Zig → Rust 静态库
# （静态库必须放最后，否则归档里的符号可能解析不到）。
echo "[ci] LD kernel_raw.bin"
"$LD" $LDFLAGS -o kernel_raw.bin boot/kernel_entry.o boot/task_switch.o \
    $OBJS "$ZIG_OBJ" "$RUST_LIB"
echo "[ci] ASSEMBLE kernel.bin + os-image.bin (dynamic size)"
"$PYTHON" tools/make_image.py boot/boot.bin kernel_raw.bin kernel.bin os-image.bin

echo "[ci] BUILD OK: os-image.bin ($(stat -c%s os-image.bin) bytes)"
