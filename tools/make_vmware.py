# -*- coding: utf-8 -*-
"""make_vmware.py - 产出可直接用 VMware Workstation/Player 打开的虚拟机。

为什么需要它：QEMU 的 SeaBIOS 太"干净"——PCI 拓扑、USB 控制器、ACPI 表、
中断路由都理想化，真机上出问题的地方它多半不会复现。VMware 用的是
PhoenixBIOS + 更接近真机的芯片组，是介于 QEMU 和真机之间的第二道验证。

用法：
    python tools/make_vmware.py                 # 默认 usb-image.bin -> vmware/
    python tools/make_vmware.py --image X.bin

产物：vmware/ezos-usb.vmdk（磁盘）+ vmware/ezos.vmx（虚拟机配置）
【重要】VMware 侧请用 **legacy BIOS** 启动（firmware = "bios"）：
  我们的混合镜像 legacy 与 UEFI 双通道都能引导，但 VMware 自带的 EFI 是
  64 位，加载不了 BOOTIA32.EFI，走 UEFI 会直接 "No bootable device"。
"""
import argparse
import os

import sys as _sys
_sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ezos_env as _ezos  # noqa: E402
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU_IMG = _ezos.qemu_img_exe()

VMX = """.encoding = "UTF-8"
# 【必填】virtualHW.version 决定硬件/固件兼容性。VMware 26 缺这一行会直接
# 报 "Cannot read the virtual machine configuration file"，别删。参考值：
# Workstation 17=20、17.5=21、25/26=22（可用宿主机的 vmx 对照）。
virtualHW.version = "22"
# 【必填】config.version = "8" 与上面 virtualHW 配套。缺这一行时 VMware 26
# 报 "Cannot read the virtual machine configuration file"，加上就能正常启动。
config.version = "8"
displayName = "EZOS"
guestOS = "other-32"
memsize = "256"
numvcpus = "1"
# 固定 uuid.bios，避免每次重新生成 vmx 都被当成另一台新机器（宿主机会问"是否移动或复制"）。
uuid.bios = "56 4d 0f 96 11 01 ec 21-23 2c f1 83 14 80 32 68"

# legacy BIOS。别改成 efi：VMware 的 EFI 是 64 位，BOOTIA32.EFI 加载不了。
firmware = "bios"

# IDE 而不是 SCSI/SATA：内核目前只有 ATA(IDE) 与 AHCI 驱动，IDE 最稳。
ide0:0.present = "TRUE"
ide0:0.fileName = "{disk}"
ide0:0.deviceType = "disk"
scsi0.present = "FALSE"
sata0.present = "FALSE"
floppy0.present = "FALSE"

# 串口 -> 文件：内核的 COM1 诊断全落到这里，E2E 也是按行抓的。
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "ezos-serial.log"
serial0.yieldOnMsrWrite = "TRUE"

# 想试 U 盘（USB MSC）时把下面的 usb.present 打开，并在虚拟机里接上设备
usb.present = "FALSE"

bios.bootDelay = "3000"
logging = "FALSE"
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image",
                    default=os.path.join(ROOT, "usb-image.bin"),
                    help="raw 镜像（默认 usb-image.bin，双通道混合镜像）")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "vmware"))
    args = ap.parse_args()

    img = os.path.abspath(args.image)
    if not os.path.isfile(img):
        print("MISSING image: %s" % img)
        return 1
    if not os.path.isdir(args.out_dir):
        os.makedirs(args.out_dir)

    vmdk = os.path.join(args.out_dir, "ezos-usb.vmdk")
    if os.path.exists(vmdk):
        os.remove(vmdk)
    # monolithicSparse：36MB 镜像里绝大部分是 FAT32 空白，稀疏后只有 1MB 出头。
    # 用 raw 直通也行，但 sparse 更适合分发。
    rc = subprocess.call([QEMU_IMG, "convert", "-f", "raw", "-O", "vmdk",
                          img, vmdk])
    if rc != 0 or not os.path.isfile(vmdk):
        print("qemu-img convert failed (rc=%d)" % rc)
        return 1

    # 行尾必须 CRLF：宿主是 Windows，VMware 的 vmx 解析器对纯 LF 不友好。
    # 之前用 newline="\n" 写出 LF 文件，叠加缺 virtualHW.version 一起触发
    # "Internal error. Remove from library?"。
    with open(os.path.join(args.out_dir, "ezos.vmx"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write(VMX.format(disk="ezos-usb.vmdk"))

    print("OK %s (%d bytes)" % (vmdk, os.path.getsize(vmdk)))
    print("OK %s" % os.path.join(args.out_dir, "ezos.vmx"))
    print("")
    print("用 VMware 打开 ezos.vmx -> 开机。串口日志: vmware/ezos-serial.log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
