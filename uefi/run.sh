#!/usr/bin/env bash
# 在 OVMF(i386) 下启动并抓取串口标记，用于验证 BOOTIA32.EFI + 跳转内核
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU="/d/MyOS/tools/qemu-portable-20241220/qemu-system-i386.exe"
FD="/d/MyOS/tools/qemu-portable-20241220/share/edk2-i386-code.fd"
ESP="$SRC_DIR/esp"
LOG="$SRC_DIR/serial.log"
: "${QEMU:?}" "${FD:?}"
WFD=$(cygpath -w "$FD"); WESP=$(cygpath -w "$ESP"); WLOG=$(cygpath -w "$LOG")

# 把仓库根 kernel.bin 拷进 ESP（本步不提交：.gitignore 含 kernel.bin）
KERNEL_BIN="$SRC_DIR/../kernel.bin"
if [ -f "$KERNEL_BIN" ]; then
  cp -f "$KERNEL_BIN" "$ESP/kernel.bin" || true
  echo "copied kernel.bin ($(wc -c < "$KERNEL_BIN") bytes) -> esp/kernel.bin"
else
  echo "WARN: $KERNEL_BIN 不存在，内核将无法加载"
fi

rm -f "$LOG"
"$QEMU" -machine pc -m 256 -vga std \
  -drive if=pflash,format=raw,readonly=on,file="$WFD" \
  -drive if=none,format=raw,file=fat:rw:"$WESP",id=esp \
  -usb -device usb-storage,drive=esp \
  -serial file:"$WLOG" -net none -nographic &
QPID=$!
sleep 18
kill -9 "$QPID" 2>/dev/null || true
echo "=== EZEFI 标记行校验 ==="
grep -a "EZEFI: hello" "$LOG" && echo "[OK] 已抓到应用启动标记" || echo "[FAIL] 未抓到应用启动标记"
grep -a "EZEFI:gop " "$LOG" && echo "[OK] 已抓到 GOP 行" || echo "[FAIL] 未抓到 GOP 行"
grep -a "EZEFI:gop write@0x5000 ok" "$LOG" && echo "[OK] 已写入 0x5000" || echo "[FAIL] 未写入 0x5000"
grep -a "EZEFI:kernel size=507904 dst=0x10000 ok" "$LOG" && echo "[OK] 内核已载入 0x10000" || echo "[FAIL] 内核未载入"
grep -a "EZEFI:uefi magic@0x5010 ok" "$LOG" && echo "[OK] UEFI 魔数已写" || echo "[FAIL] UEFI 魔数未写"
grep -a "EZEFI:ebs ok" "$LOG" && echo "[OK] ExitBootServices 成功" || echo "[FAIL] EBS 未成功"
echo "=== 跳转后串口内容（前 50 行，含 OVMF 固件 + 内核启动日志）==="
head -n 50 "$LOG"
