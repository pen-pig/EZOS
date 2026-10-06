# -*- coding: utf-8 -*-
"""ref_fat.py - 宿主机侧 FAT12/FAT16/FAT32 **只读**参考实现。

与 ref_exfat.py 同样的定位：只按规范解析，与 kernel/fat.c 零共享代码。
存在的理由是内核自己读自己写的盘永远是绿的——两边共享同一套可能错的
理解。FAT12/16/32 的"正确写法"恰好是 Windows 一眼就能验的（插上就认），
所以这条线的第三方视角最硬。

已实现：
  - MBR 分区表定位（无分区表时整盘当卷）
  - BPB 逐字段解析与 **audit_bpb() 规范体检**（媒体描述符 / 簇大小 / FAT 数 /
    根目录项数 / total16 与 total32 的一致性 / 引导签名 / FAT32 扩展 BPB /
    FSInfo 与备份引导扇区）
  - FAT12 的 12 位偏移（最容易写错的地方）与 FAT16/32
  - 目录项与 LFN（长文件名）合并，跨簇目录按整条链拼接
  - audit()：FAT 链越界/成环、目录项首簇越界、文件大小超链长、
    8.3 名非法字符、LFN checksum 不符

只读：不写盘。造盘请用 temp/gen_diskimg.py 或 tests 里的写入器。
"""
import struct

# 目录项属性位
ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F

# 8.3 名字里不允许出现的字符（规范：这些必须转成下划线）
INVALID_83 = set('"*+,/:;<=>?[\\]|')


class FatError(Exception):
    pass


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Fat(object):
    def __init__(self, path, part_off=None):
        self.path = path
        self.f = open(path, "rb")
        try:
            self.part_off = self._locate_volume(part_off)
            self._parse_bpb()
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

    # ---------- 底层读 ----------
    def sector(self, n):
        self.f.seek(self.part_off + n * 512)
        d = self.f.read(512)
        if len(d) < 512:
            raise FatError("short read at sector %d" % n)
        return d

    def read_at(self, off, length):
        """off 是卷相对字节偏移（不含分区起点）"""
        self.f.seek(self.part_off + off)
        d = self.f.read(length)
        if len(d) < length:
            raise FatError("short read at %d+%d" % (off, length))
        return d

    def write_at(self, off, data):
        self.f.seek(self.part_off + off)
        self.f.write(data)

    def flush(self):
        self.f.flush()

    # ---------- 分区定位 ----------
    def _locate_volume(self, part_off):
        mbr = self._pread(0, 512)
        if len(mbr) < 512 or mbr[510] != 0x55 or mbr[511] != 0xAA:
            return 0                      # 无有效签名：整盘当卷
        # 逐个分区项找 FAT
        for i in range(4):
            e = 446 + i * 16
            ptype = mbr[e + 4]
            if ptype == 0x00:
                continue
            start = _u32(mbr, e + 8)
            if ptype in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0xEF):
                # 先按 FAT 引导扇区特征判断
                try:
                    if self._looks_like_fat(start):
                        return start * 512
                except Exception:
                    continue
        return 0

    def _pread(self, abs_off, n):
        self.f.seek(abs_off)
        d = self.f.read(n)
        return d

    def _looks_like_fat(self, start_lba):
        v = self._pread(start_lba * 512, 512)
        if len(v) < 512 or v[510] != 0x55 or v[511] != 0xAA:
            return False
        bps = _u16(v, 0x0B)
        if bps not in (512, 1024, 2048, 4096):
            return False
        spc = v[0x0D]
        if spc == 0 or (spc & (spc - 1)) != 0:
            return False
        nfat = v[0x10]
        if nfat not in (1, 2):
            return False
        root_ent = _u16(v, 0x11)
        tot16 = _u16(v, 0x13)
        tot32 = _u32(v, 0x20)
        spf16 = _u16(v, 0x16)
        spf32 = _u32(v, 0x24) if root_ent == 0 else 0
        return (tot16 != 0 or tot32 != 0) and (spf16 != 0 or spf32 != 0)

    # ---------- BPB ----------
    def _parse_bpb(self):
        b = self.sector(0)
        self.bpb = b
        self.jump = b[0:3]
        self.oem = b[3:11].decode("latin1")
        self.bps = _u16(b, 0x0B)
        self.spc = b[0x0D]
        self.reserved = _u16(b, 0x0E)
        self.nfats = b[0x10]
        self.root_entries = _u16(b, 0x11)
        self.total16 = _u16(b, 0x13)
        self.media = b[0x15]
        self.spf16 = _u16(b, 0x16)
        self.spt = _u16(b, 0x18)
        self.heads = _u16(b, 0x1A)
        self.hidden = _u32(b, 0x1C)
        self.total32 = _u32(b, 0x20)
        self.drive_num = b[0x24] if self.root_entries else b[0x40]
        self.ext_sig = b[0x26] if self.root_entries else b[0x42]
        self.serial = _u32(b, 0x27) if self.root_entries else _u32(b, 0x43)
        self.label = b[0x2B:0x36].decode("latin1") if self.root_entries \
            else b[0x47:0x52].decode("latin1")
        self.fs_type = b[0x36:0x3E].decode("latin1").strip() \
            if self.root_entries else b[0x52:0x5A].decode("latin1").strip()

        # FAT32 扩展 BPB
        if self.root_entries == 0 and self.spf16 == 0:
            self.spf = _u32(b, 0x24)
            self.ext_flags = _u16(b, 0x28)
            self.fsver = _u16(b, 0x2A)
            self.root_cluster = _u32(b, 0x2C)
            self.fsinfo_sec = _u16(b, 0x30)
            self.backup_sec = _u16(b, 0x32)
        else:
            self.spf = self.spf16
            self.ext_flags = 0
            self.fsver = 0
            self.root_cluster = 0
            self.fsinfo_sec = 0
            self.backup_sec = 0

        self.total = self.total16 or self.total32
        if not self.total:
            raise FatError("BPB has no total sector count")

        self.cluster_size = self.bps * self.spc
        self.fat_off = self.reserved * self.bps
        self.root_dir_sectors = ((self.root_entries * 32) + self.bps - 1) // self.bps
        self.root_off = (self.reserved + self.nfats * self.spf) * self.bps
        self.data_off = self.root_off + self.root_dir_sectors * self.bps
        self.cluster_count = ((self.total - self.reserved
                               - self.nfats * self.spf
                               - self.root_dir_sectors)
                              // self.spc)
        # 决定 FAT 类型：按规范由 cluster_count 划分
        if self.cluster_count < 4085:
            self.fat_type = 12
        elif self.cluster_count < 65525:
            self.fat_type = 16
        else:
            self.fat_type = 32

    def fat_entry_bytes(self):
        return {12: 1.5, 16: 2, 32: 4}[self.fat_type]

    def root_dir_sectors_total(self):
        return self.root_dir_sectors

    def eoc(self):
        return 0xFFF8 if self.fat_type != 12 else 0xFFF

    def bad(self):
        return 0xFFF7 if self.fat_type != 12 else 0xFF7

    # ---------- FAT 表 ----------
    def next_cluster(self, c):
        if c < 2 or c >= self.cluster_count + 2:
            return None
        n = c - 2
        if self.fat_type == 12:
            off = self.fat_off + n + n // 2
            b = self.read_at(off, 2)
            v = b[0] | (b[1] << 8)
            v = (v >> 4) if (n % 2 == 0) else (v & 0xFFF)
        elif self.fat_type == 16:
            v = _u16(self.read_at(self.fat_off + n * 2, 2), 0)
        else:
            v = _u32(self.read_at(self.fat_off + n * 4, 4), 0) & 0x0FFFFFFF
        return v

    def set_fat(self, c, v):
        n = c - 2
        if self.fat_type == 12:
            off = self.fat_off + n + n // 2
            b = bytearray(self.read_at(off, 2))
            cur = b[0] | (b[1] << 8)
            if n % 2 == 0:
                cur = (cur & 0xF000) | (v & 0xFFF)
            else:
                cur = (cur & 0x000F) | ((v & 0xFFF) << 4)
            b[0] = cur & 0xFF
            b[1] = (cur >> 8) & 0xFF
            self.write_at(off, bytes(b))
        elif self.fat_type == 16:
            self.write_at(self.fat_off + n * 2, struct.pack("<H", v))
        else:
            self.write_at(self.fat_off + n * 4, struct.pack("<I", v & 0x0FFFFFFF))

    def chain(self, first):
        """返回簇链（不含 EOC）。有环或越界就抛 FatError——审计要的就是这个。"""
        out = []
        c = first
        seen = set()
        while c is not None and 2 <= c < self.cluster_count + 2:
            if c in seen:
                raise FatError("FAT chain loops at cluster %d" % c)
            seen.add(c)
            out.append(c)
            c = self.next_cluster(c)
        return out

    def read_cluster(self, c):
        return self.read_at(self.data_off + (c - 2) * self.cluster_size,
                            self.cluster_size)

    # ---------- 目录 ----------
    def root_slots(self):
        return self.read_at(self.root_off,
                            self.root_entries * 32) if self.root_entries else b""

    def dir_chain_slots(self, cluster):
        """目录字节流：按整条簇链拼接（目录可以跨簇）"""
        buf = bytearray()
        for c in self.chain(cluster):
            buf += self.read_cluster(c)
        return bytes(buf)

    def slots(self, dir_cluster):
        if dir_cluster is None or dir_cluster == 0:
            # FAT32 没有"固定根目录区"：root entry count 为 0，根目录是
            # root_cluster 起的普通簇链。漏了这一条会把整个根目录读成空的，
            # 而且**审计照样全绿**（空目录不违规）——所以必须显式分流。
            if self.fat_type == 32:
                return self.dir_chain_slots(self.root_cluster)
            return self.root_slots()
        return self.dir_chain_slots(dir_cluster)

    @staticmethod
    def lfn_checksum(name11):
        s = 0
        for b in name11:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def dir_slots(self, dir_cluster):
        """返回 [(文件内偏移, 32 字节 slot)]，展开 LFN 序列"""
        raw = self.slots(dir_cluster)
        result = []
        lfn_parts = {}
        lfn_sum = None
        pos = 0
        while pos + 32 <= len(raw):
            e = raw[pos:pos + 32]
            if e[0] == 0x00:
                break                      # 剩余槽全空
            if e[0] == 0xE5:
                lfn_parts.clear()
                lfn_sum = None
                pos += 32
                continue
            attr = e[0x0B]
            if attr == ATTR_LFN:
                order = e[0] & 0x3F
                chk = e[0x0D]
                if lfn_sum is None:
                    lfn_sum = chk
                elif chk != lfn_sum:
                    lfn_sum = None        # 校验和不一致：这套 LFN 作废
                part = e[1:11] + e[14:26] + e[28:32]
                lfn_parts[order] = part
                pos += 32
                continue
            # 普通项
            entry = {
                "raw": e,
                "offset": pos,
                "attr": attr,
                "is_dir": bool(attr & ATTR_DIRECTORY),
                "is_vol": bool(attr & ATTR_VOLUME_ID),
                # 目录项布局（32 字节）：20-21 = FstClusHI，26-27 = FstClusLO。
                # 这两个字段隔了 6 字节，**读反是最容易犯的错**（会得到
                # cluster<<16 的大数，越界检查立刻报 out of range）。
                "cluster": _u16(e, 0x1A) | (_u16(e, 0x14) << 16),
                "size": _u32(e, 0x1C),
            }
            name83 = e[0:11].decode("latin1")
            entry["name83"] = name83
            entry["name"] = self._merge_name(name83)
            # LFN 合并（倒序取出，按 UTF-16LE 解码）
            if lfn_parts and lfn_sum == self.lfn_checksum(e[0:11]):
                seq = []
                for k in sorted(lfn_parts.keys()):
                    seq.append(lfn_parts[k])
                blob = b"".join(seq)
                try:
                    txt = blob.decode("utf-16-le")
                    txt = txt.split("￿")[0]
                    if txt:
                        entry["name"] = txt
                except Exception:
                    pass
            lfn_parts.clear()
            lfn_sum = None
            result.append((pos, entry))
            pos += 32
        return result

    @staticmethod
    def _merge_name(name11):
        base = name11[:8].rstrip()
        ext = name11[8:11].rstrip()
        return base + ("." + ext if ext else "")

    def parse_dir(self, dir_cluster=None):
        return [e for _, e in self.dir_slots(dir_cluster)
                if not e["is_vol"] and e["raw"][0] not in (0x00, 0xE5)]

    def lookup(self, name, dir_cluster=None):
        for e in self.parse_dir(dir_cluster):
            if e["name"].upper() == name.upper():
                return e
        return None

    def read(self, entry):
        data = bytearray()
        n = (entry["size"] + self.cluster_size - 1) // self.cluster_size
        chain = self.chain(entry["cluster"])
        for i in range(min(n, len(chain))):
            data += self.read_cluster(chain[i])
        return bytes(data[:entry["size"]])

    def read_path(self, path):
        path = path.strip("/").replace("\\", "/")
        if not path:
            raise FatError("empty path")
        parts = [p for p in path.split("/") if p and p != "."]
        cur = None
        for p in parts[:-1]:
            e = self.lookup(p, cur)
            if e is None or not e["is_dir"]:
                raise FatError("no such dir: %s" % p)
            cur = e["cluster"]
        e = self.lookup(parts[-1], cur)
        if e is None:
            raise FatError("no such file: %s" % path)
        return self.read(e)

    def dir_of_path(self, path):
        path = path.strip("/").replace("\\", "/")
        cur = None
        for p in [x for x in path.split("/") if x]:
            e = self.lookup(p, cur)
            if e is None:
                raise FatError("no such entry: %s" % p)
            cur = e["cluster"]
        return cur

    # ---------- 审计 ----------
    def audit_bpb(self):
        """BPB 规范体检。返回问题列表（空 = 逐字段合规）。

        逐条按 FAT 规范核对，不只查"解析代码用到的那几个字段"——
        内核不校验的字段恰恰是 Windows 拒绝挂载的原因。
        """
        p = []
        # 引导跳转变量只要求 "jmp short + NOP"（EB xx 90）。xx 是补齐到 512
        # 字节的偏移，常见值 0x3C 但 0x58 之类同样合规——别把它当规范项。
        if not (self.jump[0] == 0xEB and self.jump[2] == 0x90):
            p.append("boot jump code %r != EB xx 90" % self.jump)
        if self.bps not in (512, 1024, 2048, 4096):
            p.append("bytes/sector=%d not in {512,1024,2048,4096}" % self.bps)
        if self.spc == 0 or (self.spc & (self.spc - 1)) != 0:
            p.append("sectors/cluster=%d is not a power of two" % self.spc)
        if self.spc > 128:
            p.append("sectors/cluster=%d > 128" % self.spc)
        if self.reserved < 1:
            p.append("reserved sectors=%d < 1" % self.reserved)
        if self.nfats not in (1, 2):
            p.append("num FATs=%d not 1 or 2" % self.nfats)
        if self.spf < 1:
            p.append("sectors/FAT=%d < 1" % self.spf)
        # total16 与 total32 必须恰好一个有效
        if self.total16 and self.total32:
            p.append("both total16=%d and total32=%d set (must be exactly one)"
                     % (self.total16, self.total32))
        expect_media = 0xF0 if self.fat_type == 12 else 0xF8
        if self.media != expect_media:
            p.append("media descriptor 0x%02X != 0x%02X for FAT%d"
                     % (self.media, expect_media, self.fat_type))
        if self.fat_type == 32:
            if self.root_entries != 0:
                p.append("FAT32 must have root entry count 0, got %d"
                         % self.root_entries)
            if self.spf16 != 0:
                p.append("FAT32 must have sectors/FAT16 = 0, got %d" % self.spf16)
            if self.fsver != 0:
                p.append("FAT32 FS version must be 0, got 0x%X" % self.fsver)
            if self.root_cluster < 2:
                p.append("FAT32 root cluster %d < 2" % self.root_cluster)
            # 备份引导扇区：规范要求它是主引导扇区的副本，Windows 会交叉核对
            if self.backup_sec and self.backup_sec < self.total:
                b = self.sector(self.backup_sec)
                if not (b[0] == 0xEB and b[2] == 0x90):
                    p.append("backup boot sector @%d has no jmp opcode"
                             % self.backup_sec)
                for name, off, width in (("bytes/sector", 0x0B, 2),
                                         ("sectors/cluster", 0x0D, 1),
                                         ("reserved", 0x0E, 2),
                                         ("num FATs", 0x10, 1),
                                         ("root entries", 0x11, 2),
                                         ("total32", 0x20, 4)):
                    a = (self.bpb[off:off + width])
                    c = (b[off:off + width])
                    if a != c:
                        p.append("backup boot sector @%d differs: %s %r != %r"
                                 % (self.backup_sec, name, c, a))
            else:
                p.append("FAT32 backup boot sector number invalid (%d)"
                         % self.backup_sec)
            # FSInfo
            if self.fsinfo_sec and self.fsinfo_sec < self.total:
                fi = self.sector(self.fsinfo_sec)
                if _u32(fi, 0) != 0x41615252:
                    p.append("FSInfo lead signature 0x%08X != 0x41615252"
                             % _u32(fi, 0))
                if _u32(fi, 0x1E4) != 0x61417272:
                    p.append("FSInfo struct signature 0x%08X != 0x61417272"
                             % _u32(fi, 0x1E4))
            else:
                p.append("FAT32 FSInfo sector number invalid (%d)"
                         % self.fsinfo_sec)
        else:
            if self.root_entries == 0:
                p.append("FAT%d must have non-zero root entry count"
                         % self.fat_type)
            if self.total32:
                p.append("FAT%d must have total32 = 0, got %d"
                         % (self.fat_type, self.total32))
            if self.spf16 == 0:
                p.append("sectors/FAT=%d must be non-zero" % self.spf16)
        # 引导签名
        sec0 = self.bpb
        if sec0[510] != 0x55 or sec0[511] != 0xAA:
            p.append("boot sector signature %02X%02X != 55AA"
                     % (sec0[510], sec0[511]))
        if self.ext_sig != 0x29:
            p.append("extended boot signature 0x%02X != 0x29" % self.ext_sig)
        # 几何一致性：簇不能跨卷尾
        used = (self.reserved + self.nfats * self.spf
                + self.root_dir_sectors + self.cluster_count * self.spc)
        if used > self.total:
            p.append("geometry needs %d sectors > volume %d" % (used, self.total))
        # FAT 容量必须够放 cluster_count+2 项
        need = int((self.cluster_count + 2) * self.fat_entry_bytes())
        if self.spf * self.bps < need:
            p.append("FAT area %d bytes < %d needed for %d clusters"
                     % (self.spf * self.bps, need, self.cluster_count))
        return p

    def audit(self):
        """递归检查目录与 FAT 链的一致性。返回问题列表。"""
        problems = list(self.audit_bpb())
        problems.extend(self._audit_dir(None, "/"))
        return problems

    def _audit_dir(self, cluster, path, depth=0, parent=None):
        problems = []
        if depth > 64:
            return ["%s: directory nesting deeper than 64 (loop?)" % path]
        if parent is None:
            parent = self.root_cluster if self.fat_type == 32 else 0
        here = self.root_cluster if (self.fat_type == 32 and cluster is None) \
            else (cluster or 0)
        try:
            slots = self.dir_slots(cluster)
        except FatError as e:
            return ["%s: %s" % (path, e)]
        for _, e in slots:
            if e["is_vol"] or e["raw"][0] in (0x00, 0xE5):
                continue
            sub = "%s/%s" % (path, e["name"])
            # "." / ".." 指向自己/父目录，递归下去必然无限循环；但它们本身
            # 的簇号是**规范项**：写错固件会在遍历目录时中断，症状是"目录读
            # 出来是空的"且不报错（OVMF 的 FatPkg 就会这样）。
            if e["name"] == ".":
                if e["cluster"] != here:
                    problems.append("%s: '.' points to cluster %d, must be %d"
                                    % (sub, e["cluster"], here))
                continue
            if e["name"] == "..":
                if e["cluster"] != parent:
                    problems.append("%s: '..' points to cluster %d, must be %d"
                                    % (sub, e["cluster"], parent))
                continue
            # 名字合法性（8.3 主名非法字符）
            for ch in e["name83"][:8] + e["name83"][8:11]:
                if ch in INVALID_83:
                    problems.append("%s: illegal char %r in 8.3 name"
                                    % (sub, ch))
                    break
            if e["cluster"] and not (2 <= e["cluster"] < self.cluster_count + 2):
                problems.append("%s: first cluster %d out of range"
                                % (sub, e["cluster"]))
                continue
            if e["is_dir"]:
                if e["cluster"] < 2:
                    problems.append("%s: directory with cluster < 2" % sub)
                    continue
                problems.extend(self._audit_dir(e["cluster"], sub, depth + 1, here))
            else:
                try:
                    ch = self.chain(e["cluster"]) if e["cluster"] else []
                except FatError as err:
                    problems.append("%s: %s" % (sub, err))
                    continue
                need = (e["size"] + self.cluster_size - 1) // self.cluster_size
                if len(ch) < need:
                    problems.append("%s: chain has %d clusters, %d needed for "
                                    "%d bytes" % (sub, len(ch), need, e["size"]))
        return problems
