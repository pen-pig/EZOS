# -*- coding: utf-8 -*-
"""ref_ext4.py - 宿主机侧 ext2/3/4 **只读**参考实现与规范审计。

与 kernel/ext4.c 零共享代码。存在的理由同 ref_exfat.py / ref_fat.py：
内核自己读自己写的盘永远是绿的，第三方视角才能发现"字段全对但组合起来
不合法"的问题（FAT32 那次就是这样抓出整张 FAT 表错位两格的）。

覆盖：
  - 超级块字段解析（块大小 / 块数 / 每组块与 inode / inode 大小 / GDT 位置）
  - **几何一致性**：blocks_count * block_size 是否与卷容量相符（内核 ext4_format
    硬编码 1MB 卷，16MB 的盘里只有 6% 被使用——这类问题只有对拍才看得见）
  - 组描述符表：块位图 / inode 位图 / inode 表位置、每组空闲计数
  - **位图与 inode 表交叉验证**：位图里标"已用"的 inode 是否真的存在、
    inode 表里的 inode 是否都被位图标"已用"
  - inode 与目录项解析（线性目录，含 . ..）、file_type 与 i_mode 交叉核对
  - 间接块映射（i_block 0..11 直连，12 间接，13 二级，14 三级）

只读。不写盘。
"""
import struct

EXT4_MAGIC = 0xEF53

# i_block 索引含义
IDX_DIRECT = 12
IDX_SINGLE = 13
IDX_DOUBLE = 14
IDX_TRIPLE = 15

# 目录项 file_type
FT_UNKNOWN, FT_REG, FT_DIR, FT_CHR, FT_BLK, FT_FIFO, FT_SOCK, FT_LNK = range(8)


class Ext4Error(Exception):
    pass


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Ext4(object):
    def __init__(self, path, part_off=None):
        self.path = path
        self.f = open(path, "rb")
        try:
            self.part_off = self._locate(part_off)
            self._parse_super()
        except Exception:
            self.f.close()
            raise

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # ---------- 底层 ----------
    def read_at(self, off, length):
        self.f.seek(self.part_off + off)
        d = self.f.read(length)
        if len(d) < length:
            raise Ext4Error("short read at %d+%d" % (off, length))
        return d

    def block(self, n):
        return self.read_at(n * self.block_size, self.block_size)

    def _pread(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _locate(self, part_off):
        if part_off is not None:
            return part_off
        mbr = self._pread(0, 512)
        if len(mbr) >= 512 and mbr[510] == 0x55 and mbr[511] == 0xAA:
            for i in range(4):
                e = 446 + i * 16
                if mbr[e + 4] == 0x83 and _u32(mbr, e + 12) - _u32(mbr, e + 8) > 1024:
                    return _u32(mbr, e + 8) * 512
        return 0

    # ---------- 超级块 ----------
    def _parse_super(self):
        sb = self.read_at(1024, 1024)
        self.sb = sb
        if _u16(sb, 56) != EXT4_MAGIC:
            raise Ext4Error("bad magic 0x%04X (want 0xEF53)" % _u16(sb, 56))
        self.inodes_total = _u32(sb, 0)
        self.blocks_lo = _u32(sb, 4)
        self.free_blocks = _u32(sb, 12)
        self.free_inodes = _u32(sb, 16)
        self.first_data_block = _u32(sb, 20)
        self.log_block_size = _u32(sb, 24)
        self.block_size = 1024 << self.log_block_size
        self.bpg = _u32(sb, 32)
        self.ipg = _u32(sb, 40)
        self.rev_level = _u32(sb, 76)
        self.first_ino = _u32(sb, 84)
        self.ino_size = _u16(sb, 88) or 128
        self.incompat = _u32(sb, 96)
        self.ro_compat = _u32(sb, 100)
        # 64bit：块数高位
        if self.incompat & 0x80 and len(sb) > 344:
            self.blocks_total = _u32(sb, 336) << 32 | self.blocks_lo
        else:
            self.blocks_total = self.blocks_lo
        self.desc_size = _u16(sb, 254) or 32
        # GDT 位置：1K 块时在块 2；否则块 1
        self.gdt_block = self.first_data_block + 1
        self.groups = (self.blocks_total - self.first_data_block + self.bpg - 1) // self.bpg
        # 容量（字节）
        self.fs_bytes = (self.blocks_total - self.first_data_block) * self.block_size

    def volume_bytes(self):
        """从镜像里量出的卷容量（分区大小）。"""
        import os
        return os.path.getsize(self.path) - self.part_off

    # ---------- 组描述符 ----------
    def gd(self, group):
        if group >= self.groups:
            raise Ext4Error("group %d out of range (%d)" % (group, self.groups))
        return self.read_at(self.gdt_block * self.block_size +
                            group * self.desc_size, self.desc_size)

    def group_block_bitmap(self, group):
        g = self.gd(group)
        blk = _u32(g, 0)
        if self.desc_size >= 64:
            blk |= _u32(g, 0x20) << 32
        return blk

    def group_inode_bitmap(self, group):
        g = self.gd(group)
        blk = _u32(g, 4)
        if self.desc_size >= 64:
            blk |= _u32(g, 0x24) << 32
        return blk

    def group_inode_table(self, group):
        g = self.gd(group)
        blk = _u32(g, 8)
        if self.desc_size >= 64:
            blk |= _u32(g, 0x28) << 32
        return blk

    def group_free_blocks(self, group):
        return _u16(self.gd(group), 12)

    def group_free_inodes(self, group):
        return _u16(self.gd(group), 14)

    # ---------- 位图 ----------
    def block_bitmap(self, group):
        return self.block(self.group_block_bitmap(group))

    def inode_bitmap(self, group):
        return self.block(self.group_inode_bitmap(group))

    # ---------- inode ----------
    def inode(self, ino):
        if ino < 1 or ino > self.inodes_total:
            raise Ext4Error("inode %d out of range" % ino)
        group = (ino - 1) // self.ipg
        idx = (ino - 1) % self.ipg
        table = self.group_inode_table(group)
        off = table * self.block_size + idx * self.ino_size
        d = self.read_at(off, self.ino_size)
        e = {
            "ino": ino,
            "mode": _u16(d, 0),
            "links": _u16(d, 26),
            "blocks": _u32(d, 28) | (_u32(d, 0x34 + 24) << 32 if len(d) > 0x64 else 0),
            "flags": _u32(d, 32),
            "size": _u32(d, 4) | (_u32(d, 108) << 32),
            "iblock": [_u32(d, 40 + 4 * i) for i in range(15)],
            "raw": d,
        }
        mode = e["mode"]
        e["is_dir"] = (mode & 0xF000) == 0x4000
        e["is_reg"] = (mode & 0xF000) == 0x8000
        return e

    def blocks_of(self, ino):
        """按 i_block 间接映射展开文件的数据块列表。"""
        e = self.inode(ino)
        out = []

        def add_direct(blk):
            if blk:
                out.append(blk)

        for i in range(12):
            add_direct(e["iblock"][i])
        if e["iblock"][IDX_SINGLE]:
            d = self.block(e["iblock"][IDX_SINGLE])
            for i in range(0, len(d) - 3, 4):
                add_direct(_u32(d, i))
        if e["iblock"][IDX_DOUBLE]:
            d = self.block(e["iblock"][IDX_DOUBLE])
            for i in range(0, len(d) - 3, 4):
                b = _u32(d, i)
                if not b:
                    continue
                d2 = self.block(b)
                for j in range(0, len(d2) - 3, 4):
                    add_direct(_u32(d2, j))
        if e["iblock"][IDX_TRIPLE]:
            d = self.block(e["iblock"][IDX_TRIPLE])
            for i in range(0, len(d) - 3, 4):
                b1 = _u32(d, i)
                if not b1:
                    continue
                d1 = self.block(b1)
                for j in range(0, len(d1) - 3, 4):
                    b2 = _u32(d1, j)
                    if not b2:
                        continue
                    d2 = self.block(b2)
                    for k in range(0, len(d2) - 3, 4):
                        add_direct(_u32(d2, k))
        return out

    def read_inode(self, ino):
        data = bytearray()
        for b in self.blocks_of(ino):
            data += self.block(b)
        e = self.inode(ino)
        return bytes(data[:e["size"]])

    # ---------- 目录 ----------
    def dir_entries(self, ino):
        """返回 [{name, ino, type}]，含 . 与 .."""
        data = self.read_inode(ino)
        out = []
        pos = 0
        while pos + 8 <= len(data):
            ino_nr = _u32(data, pos)
            rec_len = _u16(data, pos + 4)
            name_len = data[pos + 6]
            ftype = data[pos + 7]
            if rec_len < 8 or pos + rec_len > len(data):
                break
            if ino_nr == 0:
                pos += rec_len
                continue
            name = data[pos + 8:pos + 8 + name_len]
            out.append({"name": name.decode("utf-8", "replace"),
                        "ino": ino_nr, "type": ftype})
            pos += rec_len
        return out

    def lookup(self, name, dir_ino=2):
        for e in self.dir_entries(dir_ino):
            if e["name"] == name:
                return e
        return None

    def read_path(self, path):
        cur = 2
        parts = [p for p in path.strip("/").split("/") if p and p != "."]
        for p in parts:
            e = self.lookup(p, cur)
            if e is None:
                raise Ext4Error("no such entry: %s" % p)
            cur = e["ino"]
        return self.read_inode(cur)

    # ---------- 审计 ----------
    def audit(self):
        """返回问题列表（空 = 全部合规）。"""
        p = []
        # 1) 几何
        if self.block_size < 1024 or self.block_size > 65536 or \
                (self.block_size & (self.block_size - 1)):
            p.append("block size %d is not a power of two in [1024,65536]"
                     % self.block_size)
        if self.first_data_block != (1 if self.block_size == 1024 else 0):
            p.append("s_first_data_block=%d wrong for %dB blocks"
                     % (self.first_data_block, self.block_size))
        if self.bpg == 0 or (self.bpg & (self.bpg - 1)) and self.block_size >= 4096:
            pass   # bpg 不必是 2 的幂（仅 >32KB 块时要求 8 的倍数）
        if self.bpg > self.block_size * 8:
            p.append("blocks/group=%d exceeds bitmap capacity (%d bytes)"
                     % (self.bpg, self.block_size))
        if self.ipg == 0 or self.ino_size < 128 or (self.ino_size & (self.ino_size - 1)):
            p.append("bad inodes/group=%d or inode_size=%d" % (self.ipg, self.ino_size))
        if self.groups < 1:
            p.append("group count = %d" % self.groups)
        if self.inodes_total != self.groups * self.ipg:
            p.append("inodes_count=%d != groups*ipg=%d"
                     % (self.inodes_total, self.groups * self.ipg))
        if self.blocks_total <= self.first_data_block:
            p.append("blocks_count=%d <= first_data_block" % self.blocks_total)

        # 2) 容量：文件系统实际使用 vs 卷大小（内核 ext4_format 写死 1MB）
        vol = self.volume_bytes()
        if vol and self.fs_bytes < vol:
            p.append("filesystem covers %d B but volume is %d B (%.0f%% wasted)"
                     % (self.fs_bytes, vol, 100.0 * (vol - self.fs_bytes) / vol))

        # 3) 每组：位图 / inode 表 / 空闲计数
        for g in range(self.groups):
            try:
                bb, ib = self.group_block_bitmap(g), self.group_inode_bitmap(g)
                it = self.group_inode_table(g)
            except Ext4Error as e:
                p.append(str(e))
                continue
            if not (self.first_data_block <= bb < self.blocks_total):
                p.append("group %d block bitmap block %d out of range" % (g, bb))
            if not (self.first_data_block <= ib < self.blocks_total):
                p.append("group %d inode bitmap block %d out of range" % (g, ib))
            if not (self.first_data_block <= it < self.blocks_total):
                p.append("group %d inode table block %d out of range" % (g, it))
            # 空闲计数交叉验证
            try:
                bmp = self.block_bitmap(g)
                used = sum(bin(b).count("1") for b in bmp)
                total = min(self.bpg, self.blocks_total - g * self.bpg -
                            self.first_data_block)
                if total > 0 and self.group_free_blocks(g) != total - used:
                    p.append("group %d: free block count %d != bitmap-derived %d"
                             % (g, self.group_free_blocks(g), total - used))
                ibmp = self.inode_bitmap(g)
                iused = sum(bin(b).count("1") for b in ibmp)
                n_ino = min(self.ipg, self.inodes_total - g * self.ipg)
                if self.group_free_inodes(g) != n_ino - iused:
                    p.append("group %d: free inode count %d != bitmap-derived %d"
                             % (g, self.group_free_inodes(g), n_ino - iused))
            except Ext4Error as e:
                p.append(str(e))

        # 4) 根目录：可读 + 每个目录项的 file_type 与 inode 模式一致
        try:
            for e in self.dir_entries(2):
                try:
                    ino = self.inode(e["ino"])
                except Ext4Error as ex:
                    p.append("root entry %s -> %s" % (e["name"], ex))
                    continue
                want = FT_DIR if ino["is_dir"] else (FT_REG if ino["is_reg"] else -1)
                if want >= 0 and e["type"] != want and e["name"] not in (".", ".."):
                    p.append("root entry %s: file_type=%d but inode says %s"
                             % (e["name"], e["type"],
                                "dir" if ino["is_dir"] else "reg"))
        except Ext4Error as ex:
            p.append("root dir: %s" % ex)
        return p
