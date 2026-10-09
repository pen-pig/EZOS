# -*- coding: utf-8 -*-
"""test_usbboot.py - U 盘混合镜像（legacy BIOS + UEFI）双通道启动验证

tools/make_usb_image.py 产出的 usb-image.bin 是为真机准备的：LBA0 是带 BPB 和
活动 EFI 分区表项的 MBR，LBA1 起是 stage2+内核，引导扇区副本与 ESP 的位置
由 make_usb_image.py 按内核上限动态算出，LBA2048 起是 FAT32 的 EFI
系统分区。这里在 QEMU 上把两条路都跑一遍：

  A. legacy：SeaBIOS 读 LBA0 -> MBR 代码加载引导扇区副本 -> 引导扇区
     从 LBA1 读内核（boot/boot.asm 没被改过，走的还是老路径）；
  B. UEFI：OVMF(i386) 扫分区表 -> 找到 ESP -> \\EFI\\BOOT\\BOOTIA32.EFI ->
     uefi/main.c 把 KERNEL.BIN 载入 0x1300000（KERNEL_DST）并跳转。

两条都只要"内核真的跑起来了"这一种断言：串口出现内核的启动标记。至于 UEFI
那条，额外断言 EZEFI 的 kernel size 行（证明它读的是镜像里那份内核）。

端口 4519(QMP/legacy) / 4520(serial/legacy)、4521(QMP/UEFI) / 4522(serial/UEFI)。
UEFI 用例用 -nographic + 串口文件，串口走 TCP 更实时，所以两边都用 TCP。
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

QEMU64 = qemu_exe()
QEMU32 = qemu32_exe()
OVMF = ovmf_fd()
USB_IMG = os.path.join(ROOT, "usb-image.bin")

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa
import make_usb_image  # noqa  (reuse its checker)


def run_case(name, exe, args, port_qmp, port_serial, markers, timeout=120):
    """启动一次 QEMU，等 markers 里每一条出现（串口），返回 (ok, 详情)。"""
    proc = subprocess.Popen(
        [exe] + args +
        ["-serial", "tcp:127.0.0.1:%d,server,nowait" % port_serial,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port_qmp],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    serial = SerialReader(port_serial)
    qmp = Qmp(port_qmp)
    hits = {}
    try:
        for m in markers:
            hits[m] = wait_for(serial, m, timeout)
    finally:
        try:
            qmp.quit()
        except Exception:
            pass
        serial.close()
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        wait_port_free(port_qmp, 15)
    ok = all(hits.values())
    print("%s %s" % ("PASS" if ok else "FAIL", name))
    for m in markers:
        print("    %s %r" % ("[ok]  " if hits[m] else "[miss]", m))
    return ok


def main():
    if not os.path.isfile(USB_IMG):
        print("MISSING usb-image.bin - run ninja first")
        return 2
    info = make_usb_image.check_image(USB_IMG)
    print("image: %d sectors, ESP @%d (%d sectors), kernel %d sectors"
          % (info['total'], info['esp_lba'], info['esp_sectors'],
             info['kernel_sectors']))
    results = []

    # A. legacy BIOS
    results.append(run_case(
        "legacy: SeaBIOS boots usb-image.bin",
        QEMU64,
        ["-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + USB_IMG.replace("\\", "/")],
        4519, 4520,
        ["TASK: preemptive"], 150))

    # B. UEFI (32-bit OVMF)
    results.append(run_case(
        "uefi: OVMF loads \\EFI\\BOOT\\BOOTIA32.EFI",
        QEMU32,
        ["-machine", "pc", "-m", "256",
         "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF,
         "-drive", "format=raw,file=" + USB_IMG.replace("\\", "/"),
         "-net", "none"],
        4521, 4522,
        ["EZEFI: hello", "EZEFI:kernel size=", "TASK: preemptive"], 150))

    allok = all(results)
    print("\nUSB-BOOT:", "PASS" if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
