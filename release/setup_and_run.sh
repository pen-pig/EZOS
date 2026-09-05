#!/usr/bin/env bash
# ============================================================
#  EZOS - Linux/macOS 一键启动脚本（QEMU）
#  要求: os-image.bin 与本脚本同目录
#  未装 QEMU 时 Debian/Ubuntu 系尝试 apt 自动安装
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

[ -f "os-image.bin" ] || {
    echo "[ERROR] os-image.bin not found next to this script."
    echo "        Please download the full EZOS release zip."
    exit 1
}

QEMU="${QEMU:-qemu-system-x86_64}"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "[INFO] ${QEMU} not found."
    if command -v apt-get >/dev/null 2>&1; then
        echo "[INFO] installing qemu-system-x86 via apt..."
        sudo apt-get update
        sudo apt-get install -y qemu-system-x86
    else
        echo "[ERROR] Please install QEMU manually (https://www.qemu.org/download/) and retry."
        exit 1
    fi
fi

if [ ! -f "disk.img" ]; then
    echo "[INFO] disk.img missing, regenerating with gen_diskimg.py..."
    (command -v python3 >/dev/null 2>&1 && python3 gen_diskimg.py disk.img) \
        || python gen_diskimg.py disk.img
fi

echo "[INFO] Booting EZOS in QEMU..."
"$QEMU" -vga std -m 128 -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
