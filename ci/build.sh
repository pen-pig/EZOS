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
# **排查通道**：GitHub 的 job log 要仓库管理员权限才能下载（403），CI 红了
# 我们看不到原文。所以关键错误必须同时走两路：stderr 尾部输出 + `::error::`
# 注解（注解通过 check-runs API 公开可读）。同理工具链版本走 `::notice::`。
#
# 用法: bash ci/build.sh
# 产物: os-image.bin
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
LD="${LD:-ld}"
ASM="${ASM:-nasm}"
PYTHON="${PYTHON:-python3}"
ZIG="${ZIG:-zig}"

ann_err()  { echo "::error::$1"  >&2; }
ann_note() { echo "::notice::$1" >&2; }
ann_warn() { echo "::warning::$1" >&2; }

# 把失败日志的尾部原样发到 ::error:: 注解。别只 grep 关键字——第一次就是靠
# "undefined reference" 去 grep 的，结果 CI 上是别的原因，一条注解都没捞到。
ann_log() {
    tail -n 30 "$1" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -n 8 \
        | while IFS= read -r l; do ann_err "$2: ${l:0:180}"; done
}

# ERR trap：$LINENO 直接指出是哪一行炸的，省掉"靠猜"这一步。
trap 'st=$?; \
      echo "[ci] FAILED at line $LINENO (exit $st)" >&2; \
      ann_err "[ci] build.sh failed at line $LINENO (exit $st)"' ERR

# 工具链体检：CI 上装的和开发机上用的必须是同一套（stable + rust-src），
# 否则就是"本地绿、CI 红"的经典死循环。版本打到注解里，方便远程核对。
rustc_v="$(rustc --version 2>/dev/null | head -1 || echo 'rustc: MISSING')"
cargo_v="$(cargo --version 2>/dev/null | head -1 || echo 'cargo: MISSING')"
zig_v="$("$ZIG" version 2>/dev/null | head -1 || echo 'zig: MISSING')"
echo "[ci] rustc  : $rustc_v"
echo "[ci] cargo  : $cargo_v"
echo "[ci] zig    : $zig_v"
echo "[ci] gcc    : $("$CC" --version 2>/dev/null | head -1 || echo MISSING)"
ann_note "rustc=$rustc_v"
ann_note "cargo=$cargo_v"
ann_note "zig=$zig_v"
# -Z build-std 要 rust-src；没装的话 cargo 会在开编前就退出，症状是"20 秒速红"。
sysroot="$(rustc --print sysroot 2>/dev/null || echo '')"
if [ -n "$sysroot" ] && [ -f "$sysroot/lib/rustlib/src/rust/Cargo.toml" ]; then
    ann_note "rust-src=OK ($sysroot/lib/rustlib/src/rust)"
else
    ann_note "rust-src=MISSING (sysroot=${sysroot:-none})"
    # 这条 notice 曾经是**误报**：cargo 是强制步骤（失败就 exit 1），CI 全绿
    # 说明 rust-src 其实在，只是这个探测路径没命中。所以别只打印 MISSING，
    # 把真实布局和已装组件一并打出来，省得下次对着一行 MISSING 瞎猜。
    ann_note "rustlib layout: $(ls "$sysroot/lib/rustlib" 2>/dev/null | tr '\n' ' ')"
    ann_note "installed src components: $(rustup component list --installed \
              2>/dev/null | grep -i src | tr '\n' ' ')"
fi

# ---- 编译内核 C 源（自动收集，主/dev 分支通用） ----
# **-O2**：以前用 -Os 是被窗口逼的——镜像 0x10000 和主栈 0x90000 抢同一块
# 512KB，Ubuntu gcc 13 的 -O2 产物 579KB 直接撞穿 496KB 上限，只能降档硬压。
# 现在主栈搬到 1MB..2MB，不再和镜像抢低 1MB，窗口扩到 576KB（镜像
# 0x10000..0xA0000；0xA0000 是 VGA aperture，不是可用 RAM），于是回到 -O2。
# -fno-align-* 只删掉函数/循环/跳转的对齐填充（本机实测 -22.7KB），不动优化
# 档位——它和 -Os 的区别：-Os 改的是优化策略，这只是不凑 16 字节边界。
# 不加的话 CI 的 gcc13 产物 591KB 会超出 576KB 窗口约 1.4KB。
CFLAGS="-m32 -ffreestanding -O2 -Wall -Wextra \
        -fno-align-functions -fno-align-loops -fno-align-jumps \
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
#
# 注：-Zjson-target-spec **不能删**。cargo 1.96 实测
#   "`.json` target specs require -Zjson-target-spec to be added to the cargo
#    invocation"，自定义 target 仍是 -Z 开关。
RUST_LIB="rust/ezos_rs/target/i686-ezos/release/libezos_rs.a"
ZIG_OBJ="rust/ezos_zig/ezos_zig.o"

echo "[ci] CARGO libezos_rs.a"
if ! ( cd rust/ezos_rs && RUSTC_BOOTSTRAP=1 cargo build --release \
        -Z build-std=core,compiler_builtins -Zjson-target-spec \
        --target ../i686-ezos.json ) > ci-cargo.log 2>&1; then
    echo "[ci] FATAL: cargo build failed, tail:" >&2
    tail -40 ci-cargo.log >&2
    ann_log ci-cargo.log cargo
    exit 1
fi
if [ ! -f "$RUST_LIB" ]; then
    echo "[ci] FATAL: $RUST_LIB 没生成" >&2
    ann_err "cargo produced no $RUST_LIB"
    exit 1
fi

echo "[ci] ZIG ezos_zig.o"
if ! ( cd rust/ezos_zig && "$ZIG" build-obj -target x86-freestanding \
        -O ReleaseSafe ezos_zig.zig ) > ci-zig.log 2>&1; then
    echo "[ci] FATAL: zig build-obj failed, tail:" >&2
    tail -40 ci-zig.log >&2
    ann_log ci-zig.log zig
    exit 1
fi
if [ ! -f "$ZIG_OBJ" ]; then
    echo "[ci] FATAL: $ZIG_OBJ 没生成" >&2
    ann_err "zig produced no $ZIG_OBJ"
    exit 1
fi

# ---- 链接 + padding + 组镜像 ----
# 顺序与 build.ninja 一致：启动/切换 .o → 内核 .o → Zig → Rust 静态库
# （静态库必须放最后，否则归档里的符号可能解析不到）。
echo "[ci] LD kernel_raw.bin"
if ! "$LD" $LDFLAGS -o kernel_raw.bin boot/kernel_entry.o boot/task_switch.o \
        $OBJS "$ZIG_OBJ" "$RUST_LIB" > ci-ld.log 2>&1; then
    echo "[ci] FATAL: link failed, tail:" >&2
    tail -40 ci-ld.log >&2
    ann_log ci-ld.log ld
    # 镜像上限是 linker.ld 的 ASSERT(<=0x90000)。CI 用的 gcc 版本跟开发机不
    # 一样时，产物大小会漂移几个 KB——把 .o 体积打出来，一眼就能看出是"超
    # 容量"还是"缺符号"。
    tot=0
    for o in $OBJS; do
        s=$(stat -c%s "$o" 2>/dev/null || echo 0)
        tot=$((tot + s))
    done
    ann_note "sum of kernel .o bytes = $tot (limit 589824 for the linked image)"
    exit 1
fi
echo "[ci] ASSEMBLE kernel.bin + os-image.bin (dynamic size)"
"$PYTHON" tools/make_image.py boot/boot.bin kernel_raw.bin kernel.bin os-image.bin

SZ=$(stat -c%s os-image.bin)
echo "[ci] BUILD OK: os-image.bin ($SZ bytes)"
# 0x90000 = 589824 (576KB) 是 linker.ld 的硬上限；离顶越近，换个 gcc 就越
# 容易翻车。余量预警：贴着上限跑时换个编译器/加个功能就翻车（历史上 -O2
# 撞穿 496KB 上限，CI 连红十几次）。剩不到 64KB 就提前喊，别等撞穿了才发现。
RAW=$(stat -c%s kernel_raw.bin)
ann_note "kernel_raw=$RAW / 589824 bytes limit"
if [ "$RAW" -gt 524288 ]; then     # 589824 - 64KB
    # warning 而不是 error：还在限内就不该红，但得让人看见余量在缩水
    ann_warn "kernel_raw=$RAW：距 589824 上限只剩 $((589824 - RAW)) 字节"
fi
