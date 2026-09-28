#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_image.py —— 把 kernel_raw.bin 组装成可引导镜像（单一事实来源）。

以前 kernel.bin 被 objcopy 写死 --pad-to 507904（992 扇区），boot.asm 里也
写死 KERNEL_SECTORS equ 992。两边都得手工同步，且镜像永远占满 496KB——
哪怕真实内核只有 470KB，也要多读 50 个扇区、U 盘镜像多半兆。

现在改成动态：本脚本按 kernel_raw.bin 的真实大小算出扇区数，把它写进引导
扇区的保留字段（偏移 0x1FC，LE16），boot.asm 运行时从内存读这个数。

契约（改任一处都要同步另外两处）：
  1) boot/boot.asm   引导扇区尾部 `times 508-($-$$) db 0` + `kernel_sectors: dw 0`
                     预留了 0x1FC/0x1FD 两个字节；boot 从 [kernel_sectors] 读
                     扇区数，读到 0（未打补丁的裸 boot.bin）或 >MAX 时兜底成 MAX。
  2) 本脚本          计算扇区数（向上取整、保证「引导扇区 + 内核」总数为偶数）
                     并写回 0x1FC。
  3) uefi/main.c     UEFI 路径没有引导扇区，改用 EFI 文件的真实大小加载。

MAX_SECTORS = 992（496KB）不能动：内核链接在 0x10000，主栈 esp=0x90000，
中间只有 512KB，留给栈 16KB。动态化只是"用满 496KB"，不抬高这条线。

用法：
  python tools/make_image.py boot/boot.bin kernel_raw.bin kernel.bin os-image.bin
"""

import os
import sys

SECTOR = 512
MAX_SECTORS = 992          # 496KB；linker.ld 的 ASSERT(. - 0x10000 <= 0x7C000) 同款上限
SECTOR_FIELD = 0x1FC       # 引导扇区内保留字段（= 0x7DFC，boot.asm 的 kernel_sectors）
BOOT_SIG = 0xAA55


def die(msg):
    sys.stderr.write("make_image: " + msg + "\n")
    raise SystemExit(1)


def read_file(path):
    if not os.path.isfile(path):
        die("缺少输入文件 %s" % path)
    with open(path, "rb") as f:
        return f.read()


def main():
    argv = sys.argv[1:]
    if len(argv) != 4:
        die("用法: make_image.py <boot.bin> <kernel_raw.bin> <kernel.bin> <os-image.bin>")
    boot_in, raw_in, kernel_out, image_out = argv

    boot = read_file(boot_in)
    if len(boot) != SECTOR:
        die("%s 必须正好 %d 字节，实际 %d" % (boot_in, SECTOR, len(boot)))
    if boot[0x1FE] != 0x55 or boot[0x1FF] != 0xAA:
        die("%s 结尾不是 0x55 0xAA，不是合法引导扇区" % boot_in)
    if boot[SECTOR_FIELD] != 0 or boot[SECTOR_FIELD + 1] != 0:
        die("%s 偏移 0x%03X 处不是 0 —— boot.asm 的代码/数据已越过保留字段，"
            "先去 boot.asm 瘦身" % (boot_in, SECTOR_FIELD))

    raw = read_file(raw_in)
    if len(raw) == 0:
        die("%s 是空的" % raw_in)

    # 扇区数：向上取整。
    # 再保证「整个镜像」的扇区数是偶数（= 1 个引导扇区 + 内核扇区数）。
    # H1c 踩过的坑：993 扇区（奇数）的镜像写 U 盘，SeaBIOS 认得出设备但报
    # could not read the boot disk；pad 到 1024（偶数）立刻正常。动态化之后
    # 内核扇区数是偶数反而会让总数变奇数，这里必须按总数判奇偶。
    # （tools/make_usb_boot.py 还会再 pad 到 1024，这里是为了 dd 直写的人。）
    sectors = (len(raw) + SECTOR - 1) // SECTOR
    if sectors > MAX_SECTORS:
        die("内核 %d 字节 = %d 扇区，超过上限 %d 扇区（%d 字节）。\n"
            "  上限来自 0x10000(内核) ~ 0x90000(主栈) 之间只有 512KB、留 16KB 给栈；\n"
            "  真要突破得先挪主栈，不是改这里。"
            % (len(raw), sectors, MAX_SECTORS, MAX_SECTORS * SECTOR))
    if (sectors + 1) % 2 and sectors < MAX_SECTORS:
        sectors += 1

    total = sectors * SECTOR
    pad = total - len(raw)

    # 打补丁：把扇区数写进引导扇区保留字段（LE16）
    patched = bytearray(boot)
    patched[SECTOR_FIELD] = sectors & 0xFF
    patched[SECTOR_FIELD + 1] = (sectors >> 8) & 0xFF

    with open(kernel_out, "wb") as f:
        f.write(raw)
        if pad:
            f.write(b"\x00" * pad)

    # os-image.bin = 打过补丁的引导扇区 + kernel.bin（两者内容完全一致，
    # 只是多一个引导扇区；UEFI 路径直接用 kernel.bin）
    with open(image_out, "wb") as fo:
        fo.write(patched)
        fo.write(raw)
        if pad:
            fo.write(b"\x00" * pad)

    sys.stdout.write("make_image: kernel %d B -> %d sectors (%d B), "
                     "pad %d B, os-image %d B (max %d B)\n"
                     % (len(raw), sectors, total, pad, total + SECTOR,
                        (MAX_SECTORS + 1) * SECTOR))


if __name__ == "__main__":
    main()
