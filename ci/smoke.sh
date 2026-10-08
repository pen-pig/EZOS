#!/usr/bin/env bash
# ============================================================
# ci/smoke.sh —— QEMU 冒烟测试（断言版）
#
# 旧版只判断「QEMU 12 秒内没崩溃」。这是个**弱断言**：内核卡在 shell
# 之前、25 个子系统自检全红、文件系统没挂上，它一样会绿——因为内核只要
# 不三重启，QEMU 就一直跑着。CI 连红十几次期间它一次都没红过，等于没守。
#
# 所以改成：把串口输出落成文件，然后对**内容**下断言。内核启动时会跑
# 一遍全子系统自检（SELFTEST: ...），并在起来后打印 shell 提示符
# `[/] > `——这两个信号足以证明「真的起来了」，而不是「没死」。
#
# 失败时把 serial 日志尾部发到 ::error:: 注解：本仓库的 job log 需要
# 管理员权限才能下载（403），注解是公开可读的，是唯一能远程看到原文的通道。
# ============================================================
set -uo pipefail

LOG=${LOG:-serial.log}
# 45s 而不是 30s：CI 机器忙的时候内核跑完 25 个子系统自检要十几秒，30s
# 曾经差一点点没跑到，于是"全部子系统自检通过"偶发红一次（5 次里 1 次，
# 本地连跑 5 次复现不出来）。多给 15 秒比加个会掩盖真问题的重试划算。
QEMU_TIMEOUT=${QEMU_TIMEOUT:-45}
IMG=${IMG:-os-image.bin}
DISK=${DISK:-disk.img}
# CI 上 qemu 在 PATH 里；本机（Windows）没有，用 EZOS_QEMU 指全路径。
QEMU=${EZOS_QEMU:-qemu-system-x86_64}

ann_err()  { echo "::error::$1" >&2; }
ann_note() { echo "::notice::$1" >&2; }

fails=0
check() {
    # check "<描述>" "<grep 模式>" <期望命中次数的最小值>
    local desc="$1" pat="$2" at_least="${3:-1}" n
    n=$(grep -c -e "$pat" "$LOG" 2>/dev/null || true)
    n=${n:-0}
    if [ "$n" -ge "$at_least" ]; then
        echo "  [ok]   $desc (n=$n)"
    else
        echo "  [FAIL] $desc (n=$n, want >=$at_least)"
        ann_err "smoke: $desc —— 串口日志里 '$pat' 只出现 $n 次（期望 >=$at_least）"
        fails=$((fails + 1))
    fi
}
anti() {
    # anti "<描述>" "<不该出现的 grep 模式>"
    local desc="$1" pat="$2" n
    n=$(grep -c -e "$pat" "$LOG" 2>/dev/null || true)
    n=${n:-0}
    if [ "$n" -eq 0 ]; then
        echo "  [ok]   $desc"
    else
        echo "  [FAIL] $desc —— 出现了 $n 次"
        ann_err "smoke: $desc —— 串口日志里出现了 $n 次 '$pat'"
        grep -n -e "$pat" "$LOG" | head -5 | while IFS= read -r l; do
            ann_err "smoke hit: ${l:0:180}"
        done
        fails=$((fails + 1))
    fi
}

echo "[smoke] booting $IMG + $DISK for ${QEMU_TIMEOUT}s, serial -> $LOG"
rm -f "$LOG"
set +e
timeout "$QEMU_TIMEOUT" "$QEMU" -machine pc -vga std -m 128 \
    -drive format=raw,file="$IMG" \
    -drive format=raw,file="$DISK" \
    -display none -no-reboot -serial "file:$LOG"
rc=$?
set -e
echo "[smoke] qemu rc=$rc (124 = 撑到超时，说明没三重启)"

if [ "$rc" -ne 124 ] && [ "$rc" -ne 0 ]; then
    ann_err "smoke: QEMU 异常退出 rc=$rc（0=干净退出，124=撑满超时，其余=崩溃/三重启）"
    fails=$((fails + 1))
fi

if [ ! -s "$LOG" ]; then
    ann_err "smoke: 串口日志为空——内核一行都没打出来（连引导扇区都没跑到）"
    echo "[smoke] FAIL: empty serial log"
    exit 1
fi

echo "[smoke] ---- assertions ----"
# 1) 真的进了内核（不是卡在 BIOS/引导扇区）
check "内核横幅" "EZOS Kernel .* loaded at 0x10000"
# 2) 引导扇区把镜像读进来了（这条是 INT13h 路径打出来的）
check "BIOS INT13h 读入内核镜像" "kernel image read by BIOS INT 13h"
# 3) 全子系统自检通过（内核启动时跑 25 个 selftest，最后一个汇总行）
check "全部子系统自检通过" "SELFTEST: all subsystem checks passed"
# 4) shell 起来了 —— 旧版冒烟最致命的盲区就在这里
check "shell 提示符就绪" "^\[/\] > "
# 5) 块设备/文件系统（E2E 依赖 drive1 上的卷）
check "ATA drive 0 存在" "ATA: drive 0 present"
check "ATA drive 1 存在" "ATA: drive 1 present"
check "文件系统就绪" "FS: filesystem ready"
# 6) 反向断言：任何一处自检/锁测试失败都必须红
anti  "没有自检失败"        "SELFTEST: .* FAIL"
anti  "没有锁测试失败"      "LOCKTEST: .* FAIL"
anti  "没有 panic"          "PANIC\|panic:\|Kernel panic"
anti  "没有通用保护/缺页异常" "#GP\|#PF\|General Protection"

echo "[smoke] ---- result ----"
if [ "$fails" -eq 0 ]; then
    ann_note "smoke: 全部断言通过（serial $(wc -c <"$LOG") bytes）"
    echo "[smoke] PASS: all assertions green"
    exit 0
fi

echo "[smoke] FAIL: $fails assertion(s) red —— 串口日志尾部："
# 自检行单独打一遍：红的大概率就是它，混在 tail 里容易被横幅/ASCII art 挤掉
grep -n -e "SELFTEST" "$LOG" | tail -8 | while IFS= read -r l; do
    ann_err "selftest: ${l:0:180}"
done
tail -n 40 "$LOG" | grep -v '^[[:space:]]*$' | tail -n 15 | while IFS= read -r l; do
    ann_err "serial tail: ${l:0:180}"
done
exit 1
