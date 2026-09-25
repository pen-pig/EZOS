#!/usr/bin/env bash
# 在 OVMF(i386) 下启动并抓取串口标记，用于验证 BOOTIA32.EFI
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU="/d/MyOS/tools/qemu-portable-20241220/qemu-system-i386.exe"
FD="/d/MyOS/tools/qemu-portable-20241220/share/edk2-i386-code.fd"
ESP="$SRC_DIR/esp"
LOG="$SRC_DIR/serial.log"
: "${QEMU:?}" "${FD:?}"
WFD=$(cygpath -w "$FD"); WESP=$(cygpath -w "$ESP"); WLOG=$(cygpath -w "$LOG")
rm -f "$LOG"
"$QEMU" -machine pc -m 256 -vga std \
  -drive if=pflash,format=raw,readonly=on,file="$WFD" \
  -drive if=none,format=raw,file=fat:rw:"$WESP",id=esp \
  -usb -device usb-storage,drive=esp \
  -serial file:"$WLOG" -net none -nographic &
QPID=$!
sleep 13
kill -9 "$QPID" 2>/dev/null || true
echo "=== serial.log 中的标记行 ==="
grep -a "EZEFI: hello" "$LOG" && echo "[OK] 已抓到应用启动标记" || echo "[FAIL] 未抓到应用启动标记"
grep -a "EZEFI:gop " "$LOG" && echo "[OK] 已抓到 GOP 行" || echo "[FAIL] 未抓到 GOP 行"
grep -a "EZEFI:gop write@0x5000 ok" "$LOG" && echo "[OK] 已写入 0x5000" || echo "[FAIL] 未写入 0x5000"
