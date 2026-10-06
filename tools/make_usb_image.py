#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_usb_image.py - 组装"真机也能起"的 U 盘镜像（legacy BIOS + UEFI 双通道）

为什么不是直接把 os-image.bin 写进 U 盘：os-image.bin 的 LBA0 是我们自己写的
引导扇区，**既没有 BPB 也没有分区表**。这在 QEMU/SeaBIOS 上无所谓，但真机上
两种固件都会因此拒绝它：

  * legacy BIOS：不少固件要求引导扇区里有合法 BPB（否则无法判定介质几何），
    并要求 MBR 里有一个**活动分区**才把 U 盘当 USB-HDD 启动；
  * UEFI：固件根本不看 LBA0 的代码，只扫分区表里 FAT 分区的
    \\EFI\\BOOT\\BOOTIA32.EFI（32 位）/BOOTX64.EFI（64 位）。没有分区表 =
    没有 EFI 分区 = 机器直接跳过这块盘。

所以这里产出一份混合镜像 usb-image.bin：

    LBA 0            mbr_usb.bin：BPB + 活动 EFI 分区表项 + 迷你引导代码
    LBA 1 .. N       内核（与 os-image.bin 里完全相同的字节）
    LBA 1024         真正的引导扇区（os-image.bin 的 LBA0 原样搬过来）
    LBA 2048 ..      EFI 系统分区（FAT32，含 EFI/BOOT/BOOTIA32.EFI + kernel.bin）

引导链：BIOS 读 LBA0 -> MBR 代码用 INT13h AH=42 把 LBA1024 那个引导扇区载入
0000:7C00 并跳过去 -> 引导扇区照旧从 LBA1 读内核（**boot/boot.asm 一行都不用改**：
内核在 USB 镜像里同样从 LBA1 开始）。UEFI 则直接走 EFI 分区里的加载器。

用法：
    python tools/make_usb_image.py                    # 默认输入输出路径
    python tools/make_usb_image.py --check usb-image.bin
"""
import argparse
import os
import struct
import sys

SECTOR = 512
VBR_LBA = 1024          # 引导扇区副本所在地（内核最多 992 扇区，1024 安全）
ESP_LBA = 2048          # EFI 系统分区起点（1MB 对齐，固件最喜欢）
ESP_SECTORS = 69632     # 34MB：FAT32 需要 >= 65525 簇（512B/簇）才名副其实
PART_TYPE_EFI = 0xEF

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def put16(b, off, v):
    struct.pack_into('<H', b, off, v)


def put32(b, off, v):
    struct.pack_into('<I', b, off, v)


# ==================== FAT32 ESP ====================

def fat32_params(total_sectors, spc=1, reserved=32, nfats=2):
    fat_size = 1
    for _ in range(32):
        clusters = (total_sectors - reserved - nfats * fat_size) // spc
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need <= fat_size:
            break
        fat_size = need
    first_data = reserved + nfats * fat_size
    clusters = (total_sectors - first_data) // spc
    return {'total': total_sectors, 'spc': spc, 'reserved': reserved,
            'nfats': nfats, 'fat_size': fat_size, 'clusters': clusters,
            'first_data': first_data, 'root_cluster': 2}


def fat32_boot_sector(p):
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'EZOSUSB '
    put16(bs, 11, SECTOR)
    bs[13] = p['spc']
    put16(bs, 14, p['reserved'])
    bs[16] = p['nfats']
    put16(bs, 17, 0)                       # root entry count (0 = FAT32)
    put16(bs, 19, 0)                       # total sectors 16
    bs[21] = 0xF8                          # media
    put16(bs, 22, 0)                       # FAT size 16 (0 = FAT32)
    put16(bs, 24, 63)                      # sectors per track
    put16(bs, 26, 16)                      # heads
    put32(bs, 28, 0)                       # hidden sectors
    put32(bs, 32, p['total'])              # total sectors 32
    put32(bs, 36, p['fat_size'])
    put16(bs, 40, 0)                       # ext flags
    put16(bs, 42, 0)                       # fs version
    put32(bs, 44, p['root_cluster'])       # 根目录首簇
    put16(bs, 48, 1)                       # FSInfo sector
    put16(bs, 50, 6)                       # backup boot sector
    bs[64] = 0x80                          # drive number
    bs[66] = 0x29                          # boot signature
    put32(bs, 67, 0x1BADB002)              # volume id
    bs[71:82] = b'EZOS ESP   '
    bs[82:90] = b'FAT32   '
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


def fat32_fsinfo(p, next_free):
    fs = bytearray(SECTOR)
    put32(fs, 0, 0x41615252)
    put32(fs, 484, 0x61417272)
    put32(fs, 488, p['clusters'] - (next_free - 2))
    put32(fs, 492, next_free)
    fs[508] = 0x00
    put32(fs, 508, 0xAA550000) if False else None
    fs[510] = 0x55
    fs[511] = 0xAA
    return bytes(fs)


def fat32_dirent(name83, attr, first_cluster, size):
    e = bytearray(32)
    n = name83.encode() if isinstance(name83, str) else name83
    e[0:11] = n.ljust(11, b' ')
    e[11] = attr
    e[12] = 0
    put16(e, 20, (first_cluster >> 16) & 0xFFFF)
    put16(e, 26, first_cluster & 0xFFFF)
    put32(e, 28, size)
    return bytes(e)


def build_esp(esp_sectors, kernel_data, efi_data):
    """造一个最小但合规的 FAT32 EFI 系统分区：

        \\EFI\\BOOT\\BOOTIA32.EFI     UEFI 加载器
        \\KERNEL.BIN                  UEFI 加载器要读的内核（uefi/main.c）

    目录用 8.3 短名（BOOTIA32 EFI / KERNEL BIN），不写 LFN——固件不认长名时
    短名才是保底。"""
    p = fat32_params(esp_sectors)
    if p['clusters'] < 65525:
        raise SystemExit("ESP too small for FAT32: %d clusters (need >= 65525)"
                         % p['clusters'])
    img = bytearray(esp_sectors * SECTOR)
    FAT32_DIR = 0x10 | 0x20        # 目录属性：DIRECTORY | ARCHIVE

    def wsec(lba, data):
        img[lba * SECTOR:lba * SECTOR + len(data)] = data

    def wclus(c, data):
        off = (p['first_data'] + (c - 2) * p['spc']) * SECTOR
        img[off:off + len(data)] = data

    bs = fat32_boot_sector(p)
    wsec(0, bs)
    wsec(6, bs)                    # backup boot sector

    # 簇分配：2=根目录 3=EFI 4=EFI/BOOT 5..= 文件数据
    def chain(data):
        n = (len(data) + SECTOR - 1) // SECTOR
        return n

    n_kernel = chain(kernel_data)
    n_efi = chain(efi_data)
    c_kernel = 5
    c_efi = c_kernel + n_kernel
    next_free = c_efi + n_efi

    fat = bytearray(p['fat_size'] * SECTOR)
    put32(fat, 0, 0x0FFFFFF8)      # 簇 0 保留
    put32(fat, 4, 0x0FFFFFFF)      # 簇 1 保留

    def set_fat(c, val):
        # c 是**簇号**，FAT 数组下标是 c-2。两者混用会让整张表错位 2 个簇：
        # 症状是链在首簇就断（固件只读到 512 字节就 EOF），而所有字段断言
        # 仍然全绿——因为它们根本不查链。
        assert c >= 2, "FAT slot for cluster 0/1 is reserved, got %d" % c
        put32(fat, (c - 2) * 4, val)

    for c in (2, 3, 4):            # 三个目录簇各自 EOC
        set_fat(c, 0x0FFFFFFF)
    for k in range(n_kernel):
        set_fat(c_kernel + k, c_kernel + k + 1 if k + 1 < n_kernel else 0x0FFFFFFF)
    for k in range(n_efi):
        set_fat(c_efi + k, c_efi + k + 1 if k + 1 < n_efi else 0x0FFFFFFF)
    for i in range(p['nfats']):
        wsec(p['reserved'] + i * p['fat_size'], bytes(fat))
    wsec(1, fat32_fsinfo(p, next_free))

    # 根目录：EFI 目录 + KERNEL.BIN
    root = bytearray(SECTOR)
    root[0:32] = fat32_dirent(b'EFI', FAT32_DIR, 3, 0)
    root[32:64] = fat32_dirent(b'KERNEL  BIN', 0x20, c_kernel, len(kernel_data))
    wclus(2, bytes(root))

    # 目录：. .. BOOT
    # `..` 必须是**父目录的簇**。根目录的簇是 root_cluster(=2)，不是 0——
    # 目录项的两个规范细节（都是只有真固件才会暴露的）：
    #  1. `..` 指向**父目录的簇**。根目录的簇是 root_cluster(=2)，写 0 会让
    #     固件在遍历目录时判该项无效。
    #  2. 属性字节是 DIRECTORY|ARCHIVE(0x30)，不是只有 0x10。规范要求目录
    #     同时置 ARCHIVE 位；实测 OVMF 只认 0x30（它自己用 0x30 建的目录能
    #     正常 ls 出来）。
    efi_dir = bytearray(SECTOR)
    efi_dir[0:32] = fat32_dirent(b'.', FAT32_DIR, 3, 0)
    efi_dir[32:64] = fat32_dirent(b'..', FAT32_DIR, p['root_cluster'], 0)
    efi_dir[64:96] = fat32_dirent(b'BOOT', FAT32_DIR, 4, 0)
    wclus(3, bytes(efi_dir))

    # EFI/BOOT 目录：. .. BOOTIA32.EFI
    boot_dir = bytearray(SECTOR)
    boot_dir[0:32] = fat32_dirent(b'.', FAT32_DIR, 4, 0)
    boot_dir[32:64] = fat32_dirent(b'..', FAT32_DIR, 3, 0)
    boot_dir[64:96] = fat32_dirent(b'BOOTIA32EFI', 0x20, c_efi, len(efi_data))
    wclus(4, bytes(boot_dir))

    # 文件数据
    for i in range(n_kernel):
        chunk = kernel_data[i * SECTOR:(i + 1) * SECTOR]
        wclus(c_kernel + i, chunk + b'\x00' * (SECTOR - len(chunk)))
    for i in range(n_efi):
        chunk = efi_data[i * SECTOR:(i + 1) * SECTOR]
        wclus(c_efi + i, chunk + b'\x00' * (SECTOR - len(chunk)))

    return bytes(img), p


# ==================== MBR ====================

DAP_SIG = b'\x10\x00\x01\x00\x00\x7c\x00\x00'   # len/resv/count/off/seg of our DAP


def patch_mbr(mbr, total_sectors, vbr_lba, esp_lba, esp_sectors):
    m = bytearray(mbr)
    put32(m, 0x20, total_sectors)                   # BPB TotalSectors32
    off = m.find(DAP_SIG)
    if off < 0:
        raise SystemExit("mbr_usb.bin: DAP not found - did boot/mbr_usb.asm change?")
    put32(m, off + 8, vbr_lba)                      # DAP LBA = 引导扇区所在扇区
    put32(m, 0x1C6, esp_lba)                        # 分区项 1 LBA 起点
    put32(m, 0x1CA, esp_sectors)                    # 分区项 1 扇区数
    cy = esp_lba // (16 * 63)
    m[0x1C3] = (esp_lba // 63) % 16                 # CHS end head
    m[0x1C4] = (esp_lba % 63) + 1 | ((cy >> 8) << 6)
    m[0x1C5] = cy & 0xFF
    return bytes(m)


def check_image(path):
    """只读自检：分区表 / BPB / 引导扇区副本 / 内核位置 / FAT32 卷"""
    size = os.path.getsize(path)
    assert size % SECTOR == 0, "size not sector aligned"
    total = size // SECTOR
    with open(path, 'rb') as f:
        mbr = f.read(SECTOR)
        f.seek(VBR_LBA * SECTOR)
        vbr = f.read(SECTOR)
        f.seek(ESP_LBA * SECTOR)
        esp = f.read(SECTOR)
    assert mbr[510] == 0x55 and mbr[511] == 0xAA, "MBR signature"
    assert mbr[0x1BE] in (0x00, 0x80), "partition 1 attribute byte"
    assert mbr[0x1C2] == PART_TYPE_EFI, "partition 1 type != 0xEF"
    lba = struct.unpack_from('<I', mbr, 0x1C6)[0]
    cnt = struct.unpack_from('<I', mbr, 0x1CA)[0]
    assert lba == ESP_LBA and cnt > 0, "partition 1 LBA/length"
    assert lba + cnt <= total, "partition exceeds image"
    assert struct.unpack_from('<I', mbr, 0x20)[0] == total, "BPB total sectors"
    assert vbr[510] == 0x55 and vbr[511] == 0xAA, "VBR signature (LBA %d)" % VBR_LBA
    ksectors = struct.unpack_from('<H', vbr, 0x1FC)[0]
    assert 1 <= ksectors <= 992, "kernel sector field %d" % ksectors
    assert esp[510] == 0x55 and esp[511] == 0xAA, "ESP boot sector signature"
    assert esp[82:90] == b'FAT32   ', "ESP is not FAT32"
    fat32_sz = struct.unpack_from('<I', esp, 36)[0]
    assert fat32_sz > 0 and struct.unpack_from('<H', esp, 22)[0] == 0, "ESP FAT32 fields"

    # 关键：用**独立实现**（tests/ref_fat.py，只按规范解析，与本生成器零共享）
    # 复查整个 FAT32 卷。字段断言查不出 FAT 链错误——链错位时上面全部照绿，
    # 而 UEFI 固件按链读文件只能拿到第一个簇，内核根本加载不起来。
    sys.path.insert(0, os.path.join(ROOT, 'tests'))
    from ref_fat import Fat
    problems = []
    with Fat(path) as v:
        problems.extend(v.audit())
        k = v.lookup('KERNEL.BIN')
        if k is None:
            problems.append('KERNEL.BIN not found in ESP root')
        else:
            need = (k['size'] + v.cluster_size - 1) // v.cluster_size
            have = len(v.chain(k['cluster']))
            if have < need:
                problems.append('KERNEL.BIN chain has %d clusters, %d needed for '
                                '%d bytes (UEFI would read a truncated kernel)'
                                % (have, need, k['size']))
            data = v.read(k)
            if len(data) != k['size']:
                problems.append('KERNEL.BIN read %d bytes, size field says %d'
                                % (len(data), k['size']))
            elif not any(data[:4096]):
                problems.append('KERNEL.BIN first 4KB are all zero')
            else:
                # 与源内核逐字节比对（KERNEL.BIN 是裸二进制，不是 ELF）
                src = os.path.join(ROOT, 'kernel.bin')
                if os.path.isfile(src):
                    with open(src, 'rb') as f:
                        if f.read() != data:
                            problems.append('KERNEL.BIN content != kernel.bin')
        e = v.lookup('EFI')
        if e is None:
            problems.append('EFI directory not found')
        else:
            b = v.lookup('BOOT', e['cluster'])
            if b is None:
                problems.append('EFI/BOOT not found')
            else:
                efi = v.lookup('BOOTIA32.EFI', b['cluster'])
                if efi is None:
                    problems.append('EFI/BOOT/BOOTIA32.EFI not found')
                else:
                    chain = v.chain(efi['cluster'])
                    need = (efi['size'] + v.cluster_size - 1) // v.cluster_size
                    if len(chain) < need:
                        problems.append('BOOTIA32.EFI chain %d < %d needed'
                                        % (len(chain), need))
                    if not v.read(efi).startswith(b'MZ'):
                        problems.append('BOOTIA32.EFI is not a PE image')
    assert not problems, 'FAT32 ESP audit failed:\n  ' + '\n  '.join(problems)

    return {'total': total, 'esp_lba': lba, 'esp_sectors': cnt,
            'kernel_sectors': ksectors}


def main():
    ap = argparse.ArgumentParser(description="build a hybrid (BIOS+UEFI) USB image")
    ap.add_argument("--os-image", default=os.path.join(ROOT, "os-image.bin"))
    ap.add_argument("--kernel", default=os.path.join(ROOT, "kernel.bin"))
    ap.add_argument("--mbr", default=os.path.join(ROOT, "boot", "mbr_usb.bin"))
    ap.add_argument("--efi", default=os.path.join(ROOT, "uefi", "esp", "EFI",
                                                  "BOOT", "BOOTIA32.EFI"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "usb-image.bin"))
    ap.add_argument("--check", metavar="FILE", help="only verify an existing image")
    args = ap.parse_args()

    if args.check:
        info = check_image(args.check)
        print("OK %s: %d sectors total, ESP at LBA %d (%d sectors), "
              "kernel %d sectors" % (args.check, info['total'], info['esp_lba'],
                                     info['esp_sectors'], info['kernel_sectors']))
        return 0

    for p in (args.os_image, args.kernel, args.mbr):
        if not os.path.isfile(p):
            print("MISSING %s (run ninja first)" % p)
            return 2
    os_image = open(args.os_image, 'rb').read()
    kernel = open(args.kernel, 'rb').read()
    mbr = open(args.mbr, 'rb').read()
    if len(mbr) != SECTOR or mbr[510] != 0x55 or mbr[511] != 0xAA:
        print("bad mbr_usb.bin (not a 512B boot sector)")
        return 2

    vbr = os_image[:SECTOR]
    kernel_img = os_image[SECTOR:]
    if len(kernel_img) != len(kernel):
        print("WARN: kernel.bin (%d) != os-image kernel (%d); using os-image copy"
              % (len(kernel), len(kernel_img)))
        kernel = kernel_img
    ksectors = struct.unpack_from('<H', vbr, 0x1FC)[0]
    if ksectors == 0 or ksectors > 992:
        print("kernel sector field at 0x1FC is %d - refusing to build" % ksectors)
        return 2

    if os.path.isfile(args.efi):
        efi = open(args.efi, 'rb').read()
        print("EFI loader: %s (%d bytes)" % (args.efi, len(efi)))
    else:
        efi = b''
        print("WARN: %s missing - image will be legacy-BIOS only "
              "(run uefi/build.sh to get UEFI support)" % args.efi)

    total = ESP_LBA + ESP_SECTORS
    img = bytearray(total * SECTOR)
    img[0:SECTOR] = patch_mbr(mbr, total, VBR_LBA, ESP_LBA, ESP_SECTORS)
    img[SECTOR:SECTOR + len(kernel_img)] = kernel_img          # 内核从 LBA1 起
    img[VBR_LBA * SECTOR:(VBR_LBA + 1) * SECTOR] = vbr         # 引导扇区副本
    esp_bytes, p = build_esp(ESP_SECTORS, kernel, efi)
    img[ESP_LBA * SECTOR:(ESP_LBA + ESP_SECTORS) * SECTOR] = esp_bytes

    with open(args.out, 'wb') as f:
        f.write(img)
    info = check_image(args.out)
    print("OK %s: %d bytes (%d sectors)" % (args.out, len(img), total))
    print("   legacy: MBR(LBA0) -> boot sector(LBA%d) -> kernel(LBA1..%d)"
          % (VBR_LBA, ksectors))
    print("   uefi  : ESP LBA%d, FAT32 %d clusters x %dB, %s"
          % (ESP_LBA, p['clusters'], p['spc'] * SECTOR,
             "EFI/BOOT/BOOTIA32.EFI + KERNEL.BIN" if efi else "no EFI loader"))
    return 0


if __name__ == '__main__':
    sys.exit(main())
