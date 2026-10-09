#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_image.py —— 把 stage2 + kernel_raw.bin 组装成可引导镜像。

镜像布局（boot/layout.inc 是唯一事实来源，本脚本从那里解析常量）：

    LBA 0               引导扇区（boot/boot.bin）
    LBA 1 .. 4          stage2（boot/stage2.bin，固定 STAGE2_SECTORS 个扇区）
    LBA 5 .. 5+N-1      内核

引导链：BIOS 读 LBA0 -> 引导扇区把 stage2 读进低内存并跳过去 -> stage2 开
进保护模式把内核分批搬到 KERNEL_DST（19MB）-> 跳内核。

为什么多一层 stage2：BIOS 的 INT13h AH=42h 用 seg:off 寻址（20 位），目的地址
只能落在低 1MB，而 0xA0000 起是 VGA aperture（不是平坦 RAM）——"引导扇区直接读
内核"的上限被硬件钉死在 0x10000..0xA0000 = 576KB。stage2 只让 BIOS 写低地址的
bounce 缓冲，再由 32 位地址前缀搬到高地址，于是上限变成"19-22MB 这段空档有多
大"（3MB），不再由 BIOS 决定。

历史：这个文件以前把 MAX_SECTORS 抄成字面常量（992/1152），tests/test_image.py
又抄了一份 —— 两边一走偏就是"本地全绿、只有 CI 红"。现在都从 layout.inc 解析。

契约（改任一处都要同步另外两处）：
  1) boot/boot.asm + boot/stage2.asm   从 layout.inc 取常量；引导扇区尾部
     `times 508-($-$$) db 0` + `kernel_sectors: dw 0` 预留了 0x1FC/0x1FD，
     本脚本把内核扇区数写进去，stage2 运行时读它（读到 0 或超上限时兜底）。
  2) 本脚本                            计算扇区数并写回 0x1FC。
  3) uefi/main.c                       UEFI 路径没有引导扇区，改用 EFI 文件的
     真实大小，并 AllocatePages 到 KERNEL_DST。

用法：
  python tools/make_image.py boot/boot.bin boot/stage2.bin kernel_raw.bin \
                             kernel.bin os-image.bin
"""

import os
import re
import sys

SECTOR = 512
SECTOR_FIELD = 0x1FC       # 引导扇区内保留字段（= 0x7DFC，boot.asm 的 kernel_sectors）
BOOT_SIG = 0xAA55

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT = os.path.join(os.path.dirname(HERE), "boot", "layout.inc")


def die(msg):
    sys.stderr.write("make_image: " + msg + "\n")
    raise SystemExit(1)


def parse_layout(path):
    """解析 nasm 的 `NAME equ expr` 常量（支持 0x 十六进制与简单四则运算）。"""
    if not os.path.isfile(path):
        die("缺少 %s（镜像布局的唯一事实来源）" % path)
    raw = {}
    order = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.split(";")[0].strip()
            m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\s+equ\s+(.+?)\s*$", line)
            if m:
                raw[m.group(1)] = m.group(2)
                order.append(m.group(1))
    vals = {}
    for name in order:
        expr = raw[name]
        # 先把已解析的常量替换成数字，剩下的交给受限 eval
        for k, v in vals.items():
            expr = re.sub(r"\b%s\b" % re.escape(k), str(v), expr)
        if not re.match(r"^[0-9xXa-fA-F+\-*/()\s<>&|]*$", expr):
            die("layout.inc 里 %s 的表达式看不懂：%s" % (name, raw[name]))
        try:
            vals[name] = int(eval(expr, {"__builtins__": {}}, {}))
        except Exception as e:  # noqa
            die("layout.inc 里 %s 求值失败（%s）：%s" % (name, e, raw[name]))
    return vals


def read_file(path):
    if not os.path.isfile(path):
        die("缺少输入文件 %s" % path)
    with open(path, "rb") as f:
        return f.read()


def main():
    argv = sys.argv[1:]
    if len(argv) != 5:
        die("用法: make_image.py <boot.bin> <stage2.bin> <kernel_raw.bin> "
            "<kernel.bin> <os-image.bin>")
    boot_in, stage2_in, raw_in, kernel_out, image_out = argv

    L = parse_layout(LAYOUT)
    for key in ("STAGE2_SECTORS", "KERNEL_LBA", "KERNEL_MAX_SECTORS"):
        if key not in L:
            die("layout.inc 缺少常量 %s" % key)

    boot = read_file(boot_in)
    if len(boot) != SECTOR:
        die("%s 必须正好 %d 字节，实际 %d" % (boot_in, SECTOR, len(boot)))
    if boot[0x1FE] != 0x55 or boot[0x1FF] != 0xAA:
        die("%s 结尾不是 0x55 0xAA，不是合法引导扇区" % boot_in)
    if boot[SECTOR_FIELD] != 0 or boot[SECTOR_FIELD + 1] != 0:
        die("%s 偏移 0x%03X 处不是 0 —— boot.asm 的代码/数据已越过保留字段，"
            "先去 boot.asm 瘦身" % (boot_in, SECTOR_FIELD))

    # stage2 必须是整扇区、且正好 STAGE2_SECTORS 个 —— 引导扇区里写死了这个数
    stage2 = read_file(stage2_in)
    want = L["STAGE2_SECTORS"] * SECTOR
    if len(stage2) != want:
        die("%s 必须正好 %d 字节（layout.inc STAGE2_SECTORS=%d），实际 %d。"
            " stage2.asm 末尾的 times 会自动补齐；超了要加大 STAGE2_SECTORS"
            % (stage2_in, want, L["STAGE2_SECTORS"], len(stage2)))

    raw = read_file(raw_in)
    if len(raw) == 0:
        die("%s 是空的" % raw_in)

    max_sectors = L["KERNEL_MAX_SECTORS"]
    sectors = (len(raw) + SECTOR - 1) // SECTOR
    if sectors > max_sectors:
        die("内核 %d 字节 = %d 扇区，超过上限 %d 扇区（%d 字节）。\n"
            "  上限来自镜像窗口 19MB..22MB（linker.ld 的 ASSERT 与\n"
            "  boot/layout.inc 的 KERNEL_MAX_BYTES）：16-19MB 是 GUI 背缓冲、\n"
            "  22MB 起是 .bss。真要扩得先挪 .bss 或 GUI 背缓冲。"
            % (len(raw), sectors, max_sectors, max_sectors * SECTOR))

    # 整个镜像（引导扇区 + stage2 + 内核）的扇区数必须是偶数。
    # H1c 踩过的坑：奇数扇区的镜像写 U 盘，SeaBIOS 认得出设备但报
    # could not read the boot disk；pad 成偶数立刻正常。
    total = L["KERNEL_LBA"] + sectors
    if (total % 2) and sectors < max_sectors:
        sectors += 1
        total += 1

    pad = sectors * SECTOR - len(raw)

    # 打补丁：把扇区数写进引导扇区保留字段（LE16）
    patched = bytearray(boot)
    patched[SECTOR_FIELD] = sectors & 0xFF
    patched[SECTOR_FIELD + 1] = (sectors >> 8) & 0xFF

    with open(kernel_out, "wb") as f:
        f.write(raw)
        if pad:
            f.write(b"\x00" * pad)

    # os-image.bin = 打过补丁的引导扇区 + stage2 + kernel.bin
    # （kernel.bin 与镜像里的内核字节完全一致，只是前面少了 boot+stage2；
    #   UEFI 路径直接用 kernel.bin）
    with open(image_out, "wb") as fo:
        fo.write(patched)
        fo.write(stage2)
        fo.write(raw)
        if pad:
            fo.write(b"\x00" * pad)

    sys.stdout.write("make_image: kernel %d B -> %d sectors (%d B), pad %d B, "
                     "os-image %d B (max %d B)\n"
                     % (len(raw), sectors, sectors * SECTOR, pad,
                        (total) * SECTOR, (max_sectors + L["KERNEL_LBA"]) * SECTOR))


if __name__ == "__main__":
    main()
