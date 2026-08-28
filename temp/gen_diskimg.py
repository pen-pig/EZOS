# -*- coding: utf-8 -*-
"""脚本C：重写 gen_diskimg.py —— 完全对齐内核 exfat_format 布局生成合法 16MB exFAT 镜像。
内核 exfat_format 参数（D:\\MyOS\\src\\kernel\\exfat.c）：
  fat_offset=25, fat_length=1, cluster_heap_offset=26, cluster_count=100,
  root_dir_cluster=2, volume_length=32767, partition_start=1,
  bps_shift=9 (512B/扇区), spc_shift=0 (1扇区/簇)
布局（绝对扇区）：
  LBA0: MBR(类型0x07, 起始1, 长32767)
  LBA1: VBR；LBA12: Boot Checksum；LBA13: VBR备份；LBA24: Boot Checksum备份
  LBA26: FAT(扇区偏移1+25)；LBA27: 簇2根目录；LBA28: 簇3位图；LBA29: 簇4 upcase
"""
import io, struct, sys

DISK_SIZE = 16 * 1024 * 1024
PART_START = 1
PART_SECTORS = 32767
FAT_OFFSET = 25
FAT_LENGTH = 1
CLUSTER_HEAP_OFFSET = 26
CLUSTER_COUNT = 100
ROOT_DIR_CLUSTER = 2
VOLUME_LENGTH = 32767


def checksum(data):
    """exFAT 校验和：逐字节循环右移1位 + 字节"""
    chk = 0
    for b in data:
        chk = ((chk >> 1) | (chk << 31)) & 0xFFFFFFFF
        chk = (chk + b) & 0xFFFFFFFF
    return chk


def make_mbr():
    mbr = bytearray(512)
    mbr[446] = 0x00
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00
    mbr[450] = 0x07
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF
    struct.pack_into('<I', mbr, 454, PART_START)
    struct.pack_into('<I', mbr, 458, PART_SECTORS)
    mbr[510] = 0x55; mbr[511] = 0xAA
    return bytes(mbr)


def make_vbr():
    vbr = bytearray(512)
    vbr[0] = 0xEB; vbr[1] = 0x76; vbr[2] = 0x90
    vbr[3:11] = b'EXFAT   '
    struct.pack_into('<Q', vbr, 0x40, PART_START)          # PartitionOffset
    struct.pack_into('<Q', vbr, 0x48, VOLUME_LENGTH)       # VolumeLength
    struct.pack_into('<I', vbr, 0x50, FAT_OFFSET)          # FatOffset
    struct.pack_into('<I', vbr, 0x54, FAT_LENGTH)          # FatLength
    struct.pack_into('<I', vbr, 0x58, CLUSTER_HEAP_OFFSET) # ClusterHeapOffset
    struct.pack_into('<I', vbr, 0x5C, CLUSTER_COUNT)       # ClusterCount
    struct.pack_into('<I', vbr, 0x60, ROOT_DIR_CLUSTER)    # RootDirectoryCluster
    struct.pack_into('<I', vbr, 0x64, 0x12345678)          # VolumeSerialNumber
    struct.pack_into('<H', vbr, 0x68, 0x0000)              # VolumeFlags
    vbr[0x6A] = 0; vbr[0x6B] = 0
    vbr[0x6C] = 0xFF                                        # PercentInUse
    vbr[0x6D] = 0
    vbr[0x6E] = 9                                           # BytesPerSectorShift
    vbr[0x6F] = 0                                           # SectorsPerClusterShift
    vbr[510] = 0x55; vbr[511] = 0xAA
    return bytes(vbr)


def make_boot_checksum(vbr):
    """sector 11：VolumeChecksum(偏移0) = VBR[0..10]+VBR[90..109] 共31字节校验；
    BootChecksum(偏移508) = 前11扇区(0-10) 共5632字节校验，其中 0x170-0x173 置0"""
    chk_sector = bytearray(512)
    vchk_buf = bytearray(31)
    vchk_buf[0:11] = vbr[0:11]
    vchk_buf[11:31] = vbr[90:110]
    struct.pack_into('<I', chk_sector, 0, checksum(bytes(vchk_buf)))

    # BootChecksum：前11个扇区
    boot_region = bytearray()
    # 读回本函数调用前已写入 img 的前11扇区由外部完成，这里由外部传入
    return bytes(chk_sector)


def make_fat():
    fat = bytearray(512)
    struct.pack_into('<I', fat, 0, 0xFFFFFFF8)   # FAT[0]
    struct.pack_into('<I', fat, 4, 0xFFFFFFFF)   # FAT[1]
    struct.pack_into('<I', fat, 8, 0xFFFFFFFF)   # 簇2 根目录链尾
    struct.pack_into('<I', fat, 12, 0xFFFFFFFF)  # 簇3 位图链尾
    struct.pack_into('<I', fat, 16, 0xFFFFFFFF)  # 簇4 upcase 链尾
    return bytes(fat)


def make_rootdir():
    root = bytearray(512)
    # 0x83 Volume Label
    root[0] = 0x83
    root[1] = 0x02
    root[4] = 0x00; root[5] = 0x00
    # 0x81 Allocation Bitmap
    root[32] = 0x81
    root[33] = 0x00
    struct.pack_into('<I', root, 32 + 0x14, 3)    # FirstCluster
    struct.pack_into('<Q', root, 32 + 0x18, 13)   # DataLength (100簇/8=12.5->13)
    # 0x82 Up-case Table
    root[64] = 0x82
    struct.pack_into('<I', root, 64 + 0x14, 4)    # FirstCluster
    struct.pack_into('<Q', root, 64 + 0x18, 124)  # DataLength
    # EntrySetChecksum（16位循环右移1位 + 字节，条目字节2-3置0）
    esc = 0
    for i in range(96):
        eb = 0 if (i % 32 == 2 or i % 32 == 3) else root[i]
        esc = (uint16_ror1(esc) + eb) & 0xFFFF
    root[2] = esc & 0xFF
    root[3] = (esc >> 8) & 0xFF
    return bytes(root)


def uint16_ror1(v):
    return ((v >> 1) | (v << 15)) & 0xFFFF


def make_bitmap():
    bmp = bytearray(512)
    bmp[0] = 0x0F  # 簇2,3,4,5 已用
    return bytes(bmp)


def make_upcase():
    up = bytearray(512)
    pos = 16
    for c in range(128):
        uc = c
        if ord('a') <= c <= ord('z'):
            uc = c - 32
        if uc != c:
            struct.pack_into('<H', up, pos, c)
            struct.pack_into('<H', up, pos + 2, uc)
            pos += 4
    struct.pack_into('<H', up, pos, 0xFFFF)
    struct.pack_into('<H', up, pos + 2, 0xFFFF)
    pos += 4
    chk = 0
    for i in range(512):
        chk = ((chk << 31) | (chk >> 1)) & 0xFFFFFFFF
        chk = (chk + up[i]) & 0xFFFFFFFF
    struct.pack_into('<I', up, 0, chk)
    return bytes(up)


def make_diskimg(path):
    img = bytearray(DISK_SIZE)
    mbr = make_mbr()
    vbr = make_vbr()

    # 扇区0: MBR
    img[0:512] = mbr
    # 扇区1: VBR
    img[512:1024] = vbr

    # 前11扇区 (0-10) 用于 BootChecksum
    boot_region = bytearray(img[0:11 * 512])
    # 规范：扇区0 偏移 0x170-0x173 置0（第一引导代码校验位）
    boot_region[0x170:0x174] = b'\x00\x00\x00\x00'

    chk_sector = bytearray(512)
    # VolumeChecksum @0
    vchk_buf = bytearray(31)
    vchk_buf[0:11] = vbr[0:11]
    vchk_buf[11:31] = vbr[90:110]
    struct.pack_into('<I', chk_sector, 0, checksum(bytes(vchk_buf)))
    # BootChecksum @508
    struct.pack_into('<I', chk_sector, 508, checksum(bytes(boot_region)))

    # 扇区12: Boot Checksum；扇区13: VBR备份；扇区24: Boot Checksum备份
    img[12 * 512:13 * 512] = chk_sector
    img[13 * 512:14 * 512] = vbr
    img[24 * 512:25 * 512] = chk_sector

    # 扇区26: FAT（1+25）
    img[26 * 512:27 * 512] = make_fat()
    # 扇区27: 根目录簇2（1+26+0）
    img[27 * 512:28 * 512] = make_rootdir()
    # 扇区28: 位图簇3
    img[28 * 512:29 * 512] = make_bitmap()
    # 扇区29: upcase 簇4
    img[29 * 512:30 * 512] = make_upcase()

    # 写入 README.TXT 到簇5（扇区30），并在根目录追加文件条目
    readme = b"Welcome to EZOS!\nThis is the exFAT data disk.\n"
    img[30 * 512:30 * 512 + len(readme)] = readme

    # 根目录重建：label/bitmap/upcase + README.TXT (0xC0 流扩展 + 0xC1 文件名)
    root = bytearray(512)
    root[0] = 0x83; root[1] = 0x02
    root[4] = 0x00; root[5] = 0x00
    # 0x81 bitmap
    root[32] = 0x81; root[33] = 0x00
    struct.pack_into('<I', root, 52, 3)
    struct.pack_into('<Q', root, 56, 13)
    # 0x82 upcase
    root[64] = 0x82
    struct.pack_into('<I', root, 84, 4)
    struct.pack_into('<Q', root, 88, 124)
    # README.TXT: 文件目录条目 0x85 + 流扩展 0xC0 + 文件名 0xC1
    # 0x85 文件目录条目（含 0xC0 流扩展 + 0xC1 文件名，共3条）
    root[96] = 0x85       # File directory entry
    root[97] = 0x03       # SecondaryCount = 2 (0xC0 + 0xC1)
    root[98] = 0x00       # SetChecksum (先0，最后算)
    root[99] = 0x00
    # 属性：0x20 档案
    root[104] = 0x20
    # 创建时间（简化置0）
    root[128] = 0xC0     # Stream extension
    root[129] = 0x00
    root[130] = 0x00
    # 一般标志：0x01 = 分配可能
    struct.pack_into('<Q', root, 136, len(readme))   # DataLength
    struct.pack_into('<I', root, 144, 5)             # FirstCluster = 5
    struct.pack_into('<Q', root, 152, len(readme))   # ValidDataLength
    # 0xC1 文件名条目
    root[160] = 0xC1
    root[161] = 0x00
    root[162] = 0x00
    name = 'README.TXT'
    for i, ch in enumerate(name):
        struct.pack_into('<H', root, 164 + i * 2, ord(ch))
    # 条目集校验和：3 条目录条目共 96 字节
    esc = 0
    for i in range(96, 96 + 96):
        off = i % 32
        eb = 0 if (off == 2 or off == 3) else root[i]
        esc = (uint16_ror1(esc) + eb) & 0xFFFF
    root[98] = esc & 0xFF
    root[99] = (esc >> 8) & 0xFF

    img[27 * 512:28 * 512] = bytes(root)

    with open(path, 'wb') as f:
        f.write(img)
    print("OK disk.img generated: %d bytes (exFAT aligned with kernel exfat_format)" % len(img))


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r"D:\MyOS\disk.img"
    make_diskimg(path)
