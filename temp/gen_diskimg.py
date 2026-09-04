# -*- coding: utf-8 -*-
"""gen_diskimg.py - EZOS 数据盘镜像生成器（exFAT / FAT12 / FAT16 / FAT32）

用法:
    python gen_diskimg.py <path> [exfat|fat12|fat16|fat32]

默认 exFAT，布局与内核 exfat_format 完全一致（build_and_test.bat 兼容）。
FAT 镜像为标准 MBR + 单主分区 FAT 卷（分区类型 0x01/0x06/0x0B），含两个测试文件：
    README.TXT               8.3 短名条目
    LONG FILE NAME TEST.TXT  LFN 长名条目（验证驱动 LFN 解析）
生成后按内核 fat_probe 相同规则自检 BPB 合法性。
"""
import struct, sys

# ---------- exFAT 参数（与 kernel/exfat.c exfat_format 对齐） ----------
EXFAT_DISK_SIZE = 16 * 1024 * 1024
EXFAT_PART_SECTORS = 32767
EXFAT_FAT_OFFSET = 25
EXFAT_FAT_LENGTH = 1
EXFAT_CLUSTER_HEAP_OFFSET = 26
EXFAT_CLUSTER_COUNT = 100
EXFAT_ROOT_DIR_CLUSTER = 2
EXFAT_VOLUME_LENGTH = 32767

# ---------- FAT 参数 ----------
PART_START = 1          # 所有镜像分区起始 LBA（与 exFAT 镜像一致）
FAT_DISK_SIZE = 16 * 1024 * 1024        # FAT12 / FAT16
FAT32_DISK_SIZE = 64 * 1024 * 1024      # FAT32 需 >= 65525 簇
FAT_PART_TYPE = {12: 0x01, 16: 0x06, 32: 0x0B}

LFN_NAME = 'LONG FILE NAME TEST.TXT'
LFN_ALIAS = b'LONGFI~1TXT'
README_NAME = b'README  TXT'


def exfat_checksum(data):
    """exFAT 校验和：逐字节循环右移1位 + 字节"""
    chk = 0
    for b in data:
        chk = ((chk >> 1) | (chk << 31)) & 0xFFFFFFFF
        chk = (chk + b) & 0xFFFFFFFF
    return chk


def uint16_ror1(v):
    return ((v >> 1) | (v << 15)) & 0xFFFF


def make_mbr(part_type, part_len):
    mbr = bytearray(512)
    mbr[446] = 0x00
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00
    mbr[450] = part_type
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF
    struct.pack_into('<I', mbr, 454, PART_START)
    struct.pack_into('<I', mbr, 458, part_len)
    mbr[510] = 0x55; mbr[511] = 0xAA
    return bytes(mbr)


# ==================== exFAT ====================

def make_exfat_vbr():
    vbr = bytearray(512)
    vbr[0] = 0xEB; vbr[1] = 0x76; vbr[2] = 0x90
    vbr[3:11] = b'EXFAT   '
    struct.pack_into('<Q', vbr, 0x40, PART_START)
    struct.pack_into('<Q', vbr, 0x48, EXFAT_VOLUME_LENGTH)
    struct.pack_into('<I', vbr, 0x50, EXFAT_FAT_OFFSET)
    struct.pack_into('<I', vbr, 0x54, EXFAT_FAT_LENGTH)
    struct.pack_into('<I', vbr, 0x58, EXFAT_CLUSTER_HEAP_OFFSET)
    struct.pack_into('<I', vbr, 0x5C, EXFAT_CLUSTER_COUNT)
    struct.pack_into('<I', vbr, 0x60, EXFAT_ROOT_DIR_CLUSTER)
    struct.pack_into('<I', vbr, 0x64, 0x12345678)
    struct.pack_into('<H', vbr, 0x68, 0x0001)   # FileSystemRevision 1.00
    struct.pack_into('<H', vbr, 0x6A, 0x0000)   # VolumeFlags
    vbr[0x6C] = 9       # BytesPerSectorShift (512 = 1<<9, spec offset)
    vbr[0x6D] = 0       # SectorsPerClusterShift (1 sector/cluster)
    vbr[0x6E] = 1       # NumberOfFats
    vbr[0x6F] = 0x80    # DriveSelect
    vbr[0x70] = 0xFF    # PercentInUse (unknown)
    vbr[510] = 0x55; vbr[511] = 0xAA
    return bytes(vbr)


def make_exfat_fat():
    fat = bytearray(512)
    struct.pack_into('<I', fat, 0, 0xFFFFFFF8)   # 簇 0: 介质描述符
    struct.pack_into('<I', fat, 4, 0xFFFFFFFF)   # 簇 1: 保留
    for c in range(2, 7):                        # 簇 2-6: 根目录/位图/大写表/两文件 EOC
        struct.pack_into('<I', fat, c * 4, 0xFFFFFFFF)
    return bytes(fat)


def make_exfat_upcase():
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


def exfat_name_hash(name):
    """exFAT NameHash: 字符大写化后 16 位循环右移 1 位累加（对齐 kernel/exfat.c）"""
    h = 0
    for ch in name:
        c = ord(ch)
        if 'a' <= ch <= 'z':
            c -= 32
        h = uint16_ror1(h)
        h = (h + c) & 0xFFFF
    return h


def make_exfat_entry_set(name, first_cluster, size, is_dir=False):
    """构造 spec 对齐的 exFAT entry set（0x85 + 0xC0 + N*0xC1）。
    字段偏移与 kernel/exfat.c exfat_parse_entry_set / exfat_write_entry_set 一致:
      0x85: +1 SecondaryCount, +2 SetChecksum, +4 FileAttributes
      0xC0: +3 NameLength, +4 NameHash, +8 ValidDataLength,
            +0x14 FirstCluster, +0x18 DataLength
      0xC1: +2 起 UTF-16 文件名（每条目 15 字符，不足补 0）"""
    name_len = len(name)
    name_entries = (name_len + 14) // 15
    sec_count = 1 + name_entries
    total = (1 + sec_count) * 32
    s = bytearray(total)

    s[0] = 0x85
    s[1] = sec_count
    s[4] = 0x10 if is_dir else 0x20            # FileAttributes

    c0 = 32
    s[c0] = 0xC0
    s[c0 + 1] = 0x00                           # FAT 链有效（FAT 已写 EOC）
    s[c0 + 3] = name_len                       # NameLength
    struct.pack_into('<H', s, c0 + 4, exfat_name_hash(name))
    struct.pack_into('<Q', s, c0 + 8, size)    # ValidDataLength
    struct.pack_into('<I', s, c0 + 0x14, first_cluster)
    struct.pack_into('<Q', s, c0 + 0x18, size)  # DataLength

    for n in range(name_entries):
        c1 = 64 + n * 32
        s[c1] = 0xC1
        for i in range(15):
            idx = n * 15 + i
            ch = ord(name[idx]) if idx < name_len else 0
            struct.pack_into('<H', s, c1 + 2 + i * 2, ch)

    # SetChecksum: 仅跳过首个条目的字节 2-3（校验和字段本身）
    chk = 0
    for i in range(total):
        if i == 2 or i == 3:
            continue
        chk = (uint16_ror1(chk) + s[i]) & 0xFFFF
    struct.pack_into('<H', s, 2, chk)
    return bytes(s)


def gen_exfat(path):
    img = bytearray(EXFAT_DISK_SIZE)
    vbr = make_exfat_vbr()
    img[0:512] = make_mbr(0x07, EXFAT_PART_SECTORS)
    img[512:1024] = vbr

    boot_region = bytearray(img[0:11 * 512])
    boot_region[0x170:0x174] = b'\x00\x00\x00\x00'
    chk_sector = bytearray(512)
    vchk_buf = bytearray(31)
    vchk_buf[0:11] = vbr[0:11]
    vchk_buf[11:31] = vbr[90:110]
    struct.pack_into('<I', chk_sector, 0, exfat_checksum(bytes(vchk_buf)))
    struct.pack_into('<I', chk_sector, 508, exfat_checksum(bytes(boot_region)))
    img[12 * 512:13 * 512] = chk_sector
    img[13 * 512:14 * 512] = vbr
    img[24 * 512:25 * 512] = chk_sector

    img[26 * 512:27 * 512] = make_exfat_fat()

    readme = b"Welcome to EZOS!\nThis is the exFAT data disk.\n"
    lfn_data = b"Long filename (LFN) test file on exFAT.\n"
    img[30 * 512:30 * 512 + len(readme)] = readme        # 簇 5: README.TXT
    img[31 * 512:31 * 512 + len(lfn_data)] = lfn_data    # 簇 6: LFN 长名文件

    # 根目录（簇 2）: 0x83 卷标 + 0x81 位图 + 0x82 大写表 + 两个文件 entry set
    root = bytearray(512)
    root[0] = 0x83; root[1] = 0x02
    root[32] = 0x81; root[33] = 0x00
    struct.pack_into('<I', root, 52, 3)      # 0x81 FirstCluster@+0x14（簇 3 位图）
    struct.pack_into('<Q', root, 56, 13)     # 0x81 DataLength@+0x18
    root[64] = 0x82
    struct.pack_into('<I', root, 84, 4)      # 0x82 FirstCluster@+0x14（簇 4 大写表）
    struct.pack_into('<Q', root, 88, 124)    # 0x82 DataLength@+0x18
    off = 96
    for es in (make_exfat_entry_set('README.TXT', 5, len(readme)),
               make_exfat_entry_set(LFN_NAME, 6, len(lfn_data))):
        root[off:off + len(es)] = es
        off += len(es)
    img[27 * 512:28 * 512] = bytes(root)

    # 位图（簇 3）: 簇 2-6 已用（bit0-4）
    img[28 * 512:29 * 512] = bytes([0x1F]) + bytes(511)
    img[29 * 512:30 * 512] = make_exfat_upcase()

    with open(path, 'wb') as f:
        f.write(img)
    print("OK %s: %d bytes exFAT (spec-aligned entry sets, files: README.TXT + '%s')"
          % (path, len(img), LFN_NAME))


# ==================== FAT12/16/32 ====================

def build_fat_params(total_sectors, spc, want_type):
    """迭代求 FAT 尺寸（FAT 区大小影响数据区起点，进而影响簇数）"""
    nfats = 2
    if want_type == 32:
        reserved, root_entries = 32, 0
    else:
        reserved, root_entries = 1, 512
    fat_size = 1
    for _ in range(10):
        root_sectors = (root_entries * 32 + 511) // 512
        first_data = reserved + nfats * fat_size + root_sectors
        clusters = (total_sectors - first_data) // spc
        t = 12 if clusters < 4085 else (16 if clusters < 65525 else 32)
        if t != want_type:
            raise AssertionError("cluster count %d gives FAT%d, want FAT%d (spc=%d)"
                                 % (clusters, t, want_type, spc))
        if t == 12:
            need = ((clusters + 2) * 3 + 1) // 2       # 12 bit/项
        else:
            need = (clusters + 2) * (2 if t == 16 else 4)
        need = (need + 511) // 512
        if need <= fat_size:
            break
        fat_size = need
    # 用最终 fat_size 统一重算（FAT 允许比最小值大，避免参数互锁振荡）
    root_sectors = (root_entries * 32 + 511) // 512
    first_data = reserved + nfats * fat_size + root_sectors
    clusters = (total_sectors - first_data) // spc
    return {
        'total': total_sectors, 'spc': spc, 'reserved': reserved, 'nfats': nfats,
        'root_entries': root_entries, 'fat_size': fat_size, 'root_sectors': root_sectors,
        'first_data': first_data,
        'clusters': clusters, 'fat_type': want_type,
    }


def make_fat_bs(p):
    bs = bytearray(512)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', bs, 11, 512)                    # BytesPerSector
    bs[13] = p['spc']                                      # SectorsPerCluster
    struct.pack_into('<H', bs, 14, p['reserved'])          # ReservedSectors
    bs[16] = p['nfats']                                    # NumFATs
    struct.pack_into('<H', bs, 17, p['root_entries'])      # RootEntryCount
    tot16 = p['total'] if p['total'] < 0x10000 else 0
    struct.pack_into('<H', bs, 19, tot16)                  # TotSectors16
    bs[21] = 0xF8                                          # Media
    fat16sz = p['fat_size'] if p['fat_type'] != 32 else 0
    struct.pack_into('<H', bs, 22, fat16sz)                # FATSz16
    struct.pack_into('<H', bs, 24, 63)                     # SectorsPerTrack
    struct.pack_into('<H', bs, 26, 16)                     # NumHeads
    struct.pack_into('<I', bs, 28, PART_START)             # HiddenSectors
    struct.pack_into('<I', bs, 32, 0 if tot16 else p['total'])  # TotSectors32
    if p['fat_type'] == 32:
        struct.pack_into('<I', bs, 36, p['fat_size'])      # FATSz32
        struct.pack_into('<H', bs, 40, 0)                  # ExtFlags
        struct.pack_into('<H', bs, 42, 0)                  # FSVer
        struct.pack_into('<I', bs, 44, 2)                  # RootClus
        struct.pack_into('<H', bs, 48, 1)                  # FSInfo 扇区
        struct.pack_into('<H', bs, 50, 6)                  # BkBootSec
        bs[64] = 0x80                                      # DriveNumber
        bs[66] = 0x29                                      # BootSig
        struct.pack_into('<I', bs, 68, 0x1BADB002)         # VolumeID
        bs[72:83] = b'EZOS       '
        bs[83:91] = b'FAT32   '
    else:
        bs[36] = 0x80
        bs[38] = 0x29
        struct.pack_into('<I', bs, 40, 0x1BADB002)
        bs[43:54] = b'EZOS       '
        bs[54:62] = b'FAT%d   ' % p['fat_type']
    bs[510] = 0x55; bs[511] = 0xAA
    return bytes(bs)


def make_fat_table(p, used_clusters):
    t = p['fat_type']
    fat = bytearray(p['fat_size'] * 512)
    eoc = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[t]

    def set_entry(i, val):
        if t == 12:
            off = i * 3 // 2
            if i % 2 == 0:
                fat[off] = val & 0xFF
                fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
            else:
                fat[off] = (fat[off] & 0x0F) | ((val & 0x0F) << 4)
                fat[off + 1] = (val >> 4) & 0xFF
        elif t == 16:
            struct.pack_into('<H', fat, i * 2, val)
        else:
            struct.pack_into('<I', fat, i * 4, val)

    set_entry(0, {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[t])
    set_entry(1, {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[t])
    for c in used_clusters:
        set_entry(c, eoc)
    return bytes(fat)


def lfn_checksum(sname11):
    s = 0
    for ch in sname11:
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


def make_lfn_entries(longname, sname11):
    """LFN 条目（物理顺序：最高序号带 0x40 在最前，紧邻 8.3 条目之前）"""
    utf16 = [ord(c) for c in longname] + [0]
    n = (len(utf16) + 12) // 13
    entries = []
    for seq in range(n, 0, -1):
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == n else 0)
        chars = utf16[(seq - 1) * 13: seq * 13]
        while len(chars) < 13:
            chars.append(0xFFFF)
        for k, ch in enumerate(chars):
            if k < 5:
                off = 1 + k * 2
            elif k < 11:
                off = 14 + (k - 5) * 2
            else:
                off = 28 + (k - 11) * 2
            struct.pack_into('<H', e, off, ch)
        e[11] = 0x0F
        e[13] = lfn_checksum(sname11)
        entries.append(bytes(e))
    return entries


def make_dirent(sname11, attr, first_clus, size):
    e = bytearray(32)
    e[0:11] = sname11
    e[11] = attr
    if first_clus >= 0x10000:
        struct.pack_into('<H', e, 20, first_clus >> 16)    # FAT32 簇号高 16 位
    struct.pack_into('<H', e, 26, first_clus & 0xFFFF)
    struct.pack_into('<I', e, 28, size)
    return bytes(e)


def verify_fat_bs(bs, want_type):
    """按 kernel/fat.c fat_probe 相同规则自检"""
    assert bs[510] == 0x55 and bs[511] == 0xAA, "missing 0x55AA"
    assert bs[0] in (0xEB, 0xE9), "missing jmp"
    assert bs[3:8] != b'EXFAT ' and bs[3:7] != b'NTFS', "OEM conflict"
    bps = bs[11] | (bs[12] << 8)
    assert bps == 512, "bps"
    spc = bs[13]
    assert spc and (spc & (spc - 1)) == 0, "spc not pow2"
    assert (bs[14] | (bs[15] << 8)) != 0, "reserved=0"
    assert 1 <= bs[16] <= 2, "nfats"
    root_entries = bs[17] | (bs[18] << 8)
    total = bs[19] | (bs[20] << 8)
    if total == 0:
        total = bs[32] | (bs[33] << 8) | (bs[34] << 16) | (bs[35] << 24)
    assert total != 0, "total=0"
    assert bs[21] >= 0xF0, "media"
    fat16sz = bs[22] | (bs[23] << 8)
    fat32sz = bs[36] | (bs[37] << 8) | (bs[38] << 16) | (bs[39] << 24)
    fat_size = fat16sz if fat16sz else fat32sz
    assert fat_size != 0, "fat_size=0"
    root_sectors = (root_entries * 32 + 511) // 512
    first_data = (bs[14] | (bs[15] << 8)) + bs[16] * fat_size + root_sectors
    assert first_data < total, "first_data >= total"
    clusters = (total - first_data) // spc
    assert clusters >= 2, "clusters < 2"
    t = 12 if clusters < 4085 else (16 if clusters < 65525 else 32)
    assert t == want_type, "type mismatch: %d != %d" % (t, want_type)
    if t == 32:
        assert root_entries == 0 and fat16sz == 0, "FAT32 fields"
        root_cluster = bs[44] | (bs[45] << 8) | (bs[46] << 16) | (bs[47] << 24)
        assert 2 <= root_cluster < clusters + 2, "root_cluster"
    return clusters


def gen_fat(path, fat_type):
    if fat_type == 32:
        disk_sectors = 131072
        spc = 1
    elif fat_type == 16:
        disk_sectors = 32768
        spc = 4
    else:
        disk_sectors = 32768
        spc = 16
    p = build_fat_params(disk_sectors - 1, spc, fat_type)
    img = bytearray(disk_sectors * 512)

    def wsec(lba, data):
        img[lba * 512:lba * 512 + len(data)] = data

    wsec(0, make_mbr(FAT_PART_TYPE[fat_type], p['total']))

    bs = make_fat_bs(p)
    clusters = verify_fat_bs(bs, fat_type)
    assert clusters == p['clusters'], "self-check cluster mismatch"
    wsec(PART_START, bs)
    if fat_type == 32:
        wsec(PART_START + 1, make_fsinfo(p))          # FSInfo
        wsec(PART_START + 6, bs)                      # 备份引导扇区

    # 文件数据簇
    if fat_type == 32:
        root_clus, c_readme, c_lfn = 2, 3, 4
    else:
        root_clus, c_readme, c_lfn = 0, 2, 3
    readme = ("Welcome to EZOS!\nThis is the FAT%d data disk.\n" % fat_type).encode()
    lfn_data = ("Long filename (LFN) test file on FAT%d.\n" % fat_type).encode()

    fat = make_fat_table(p, ([root_clus] if fat_type == 32 else []) + [c_readme, c_lfn])
    for i in range(p['nfats']):
        wsec(PART_START + p['reserved'] + i * p['fat_size'], fat)

    def data_sector(cluster):
        return PART_START + p['first_data'] + (cluster - 2) * p['spc']

    wsec(data_sector(c_readme), readme)
    wsec(data_sector(c_lfn), lfn_data)

    # 根目录
    root = bytearray(512)
    root[0:32] = make_dirent(README_NAME, 0x20, c_readme, len(readme))
    off = 32
    for e in make_lfn_entries(LFN_NAME, LFN_ALIAS):
        root[off:off + 32] = e
        off += 32
    root[off:off + 32] = make_dirent(LFN_ALIAS, 0x20, c_lfn, len(lfn_data))
    if fat_type == 32:
        wsec(data_sector(root_clus), bytes(root))
    else:
        wsec(PART_START + p['reserved'] + p['nfats'] * p['fat_size'], bytes(root))

    with open(path, 'wb') as f:
        f.write(img)
    print("OK %s: %d bytes FAT%d (%d clusters, %d B/cluster, FAT x%d sectors, "
          "files: README.TXT + '%s')"
          % (path, len(img), fat_type, p['clusters'], p['spc'] * 512,
             p['fat_size'], LFN_NAME))


def make_fsinfo(p):
    fs = bytearray(512)
    struct.pack_into('<I', fs, 0, 0x41615252)             # 'RRaA'
    struct.pack_into('<I', fs, 484, 0x61417272)           # 'rrAa'
    struct.pack_into('<I', fs, 488, p['clusters'] - 3)    # 空闲簇数
    struct.pack_into('<I', fs, 492, 5)                    # 下一空闲簇
    fs[510] = 0x55; fs[511] = 0xAA
    return bytes(fs)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "disk.img"
    fstype = (sys.argv[2] if len(sys.argv) > 2 else "exfat").lower()
    if fstype in ("exfat", "exf"):
        gen_exfat(path)
    elif fstype in ("fat12", "12"):
        gen_fat(path, 12)
    elif fstype in ("fat16", "16"):
        gen_fat(path, 16)
    elif fstype in ("fat32", "32"):
        gen_fat(path, 32)
    else:
        print("unknown fs type: %s (exfat|fat12|fat16|fat32)" % fstype)
        sys.exit(1)


if __name__ == '__main__':
    main()
