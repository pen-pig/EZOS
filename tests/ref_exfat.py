# -*- coding: utf-8 -*-
"""ref_exfat.py - 宿主机侧独立 exFAT 只读实现（对拍基线）。

**为什么要单独写一份**：kernel/exfat.c 是我们要检验的对象，不能拿它自己的
逻辑去验证它自己（自证清白是最常见的假绿）。这个模块只按 exFAT 规范解析
磁盘镜像，与内核代码零共享，回答一个问题——

    **内核写出来的卷，别人（Windows / 独立实现）读得出来吗？**

因此这里的每个字段偏移都来自 spec（不是来自 exfat.c），并且额外做两项
内核自己不会做的交叉检查：
  * entry set 的 SetChecksum 重算比对（偏移 2-3 跳过）；
  * 0xC0 里的 NameHash 重算比对（字符先转**大写**再循环右移累加）。
这两项只要内核算错一个字节，盘上的目录项就处于"自相矛盾"状态——内核自己
读得出来，宿主（Windows）可能直接判卷损坏。

只读：本模块不写盘。格式化/建文件请走 temp/gen_diskimg.py。
"""
import struct

EOC_MIN = 0xFFFFFFF8          # FAT 项 >= 该值即链尾（含 0xFFFFFFFF）
MAX_CHAIN = 4096              # 防御坏盘里的环形 FAT 链


class ExfatError(Exception):
    pass


def _u16(b, off):
    return struct.unpack_from('<H', b, off)[0]


def _u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def _u64(b, off):
    return struct.unpack_from('<Q', b, off)[0]


def set_checksum(entries):
    """entry set 校验和：跳过首个条目自身的 2-3 字节。"""
    s = 0
    for i, byte in enumerate(entries):
        if i == 2 or i == 3:
            continue
        s = (((s << 15) | (s >> 1)) + byte) & 0xFFFF
    return s


def name_hash(name):
    """exFAT NameHash：大写化后 16 位循环右移 1 位累加。

    注意是转**大写**（不是小写）——小写版会让 "README.TXT" 的哈希与宿主
    不一致，宿主按哈希做目录查找时就把这个文件当成不存在。"""
    h = 0
    for ch in name:
        c = ord(ch)
        if 'a' <= ch <= 'z':
            c -= 32
        h = ((h << 15) | (h >> 1)) & 0xFFFF
        h = (h + c) & 0xFFFF
    return h


class Exfat(object):
    """按 spec 解析一个含 exFAT 分区的裸镜像（支持带 VHD footer 的 disk.vhd）。"""

    def __init__(self, path):
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[-512:-504] == b'conectix':
            raw = raw[:-512]          # 固定 VHD：数据从偏移 0，尾部是 footer
        self.raw = raw
        self.part_off = self._locate_volume()
        vbr = self.raw[self.part_off:self.part_off + 512]   # 几何参数还没读，先按 512
        if vbr[3:11] != b'EXFAT   ':
            raise ExfatError('no exFAT OEM name at volume start')
        self.bps = 1 << vbr[0x6C]
        self.spc = 1 << vbr[0x6D]
        self.fat_off = _u32(vbr, 0x50)
        self.fat_len = _u32(vbr, 0x54)
        self.heap_off = _u32(vbr, 0x58)
        self.cluster_count = _u32(vbr, 0x5C)
        self.root = _u32(vbr, 0x60)
        self.cluster_size = self.bps * self.spc
        if self.bps < 512 or self.cluster_size == 0 or self.root < 2:
            raise ExfatError('implausible geometry bps=%d spc=%d root=%d'
                             % (self.bps, self.spc, self.root))

    def _locate_volume(self):
        """分区起点：MBR 分区表项 1 的 LBA start；MBR 没了就退化为 0/512 探测。

        内核 exfat_format 只动卷内，不写 MBR，所以正常情况下 MBR 一直有效；
        兜底是为了让人为构造的裸卷也能解析。"""
        if self.raw[510:512] == b'\x55\xAA' and len(self.raw) >= 512:
            lba = _u32(self.raw, 454)
            if 0 < lba * 512 < len(self.raw):
                off = lba * 512
                if self.raw[off + 3:off + 11] == b'EXFAT   ':
                    return off
        for off in (0, 512, 1048576):
            if len(self.raw) > off + 512 and \
                    self.raw[off + 3:off + 11] == b'EXFAT   ':
                return off
        raise ExfatError('cannot locate exFAT volume')

    # ---------- 原始访问 ----------
    def sector(self, n):
        off = self.part_off + n * self.bps
        return self.raw[off:off + self.bps]

    def cluster(self, c):
        if c < 2:
            raise ExfatError('cluster %d out of range' % c)
        off = self.part_off + (self.heap_off + (c - 2) * self.spc) * self.bps
        return self.raw[off:off + self.cluster_size]

    def fat(self, c):
        off = self.part_off + self.fat_off * self.bps + c * 4
        if off + 4 > len(self.raw):
            raise ExfatError('FAT entry %d beyond image' % c)
        return _u32(self.raw, off)

    def chain(self, first, limit=MAX_CHAIN):
        out = []
        seen = set()
        c = first
        while 2 <= c < self.cluster_count + 2 and len(out) < limit:
            if c in seen:
                raise ExfatError('FAT chain loops at cluster %d' % c)
            seen.add(c)
            out.append(c)
            nxt = self.fat(c)
            if nxt == 0 or nxt >= EOC_MIN:
                break
            c = nxt
        return out

    # ---------- 目录 ----------
    def dir_slots(self, dir_cluster):
        """目录簇链拼起来的槽序列 -> [(全局槽号, 32 字节)]"""
        slots = []
        for c in self.chain(dir_cluster):
            data = self.cluster(c)
            for off in range(0, len(data), 32):
                slots.append(data[off:off + 32])
        return slots

    def parse_dir(self, dir_cluster):
        """扫描目录，返回条目列表（跳过空闲/已删除槽，遇到 0x00 终结符停止）。"""
        slots = self.dir_slots(dir_cluster)
        entries = []
        i = 0
        while i < len(slots):
            t = slots[i][0]
            if t == 0x00:
                break                      # 终结符：其后不再有活条目
            if (t & 0x80) == 0:
                i += 1                     # 已删除 / 空闲槽
                continue
            if t != 0x85:
                i += 1                     # 非主条目（0x81 位图 / 0x82 大写表等）
                continue
            e = self._parse_set(slots, i)
            if e is None:
                i += 1
                continue
            e['dir_cluster'] = dir_cluster
            entries.append(e)
            i += 1 + e['secondary_count']
        return entries

    def _parse_set(self, slots, i):
        """解析 0x85 + secondary_count 个次级项（0xC0 stream + N*0xC1 name）。"""
        primary = slots[i]
        count = primary[1]
        if i + count >= len(slots) + 1 or count < 2:
            return None
        secs = slots[i + 1:i + 1 + count]
        stream = None
        name_u = []
        for s in secs:
            if s[0] == 0xC0 and stream is None:
                stream = s
            elif s[0] == 0xC1:
                name_u.extend(struct.unpack_from('<15H', s, 2))
        if stream is None:
            return None
        raw = b''.join([primary] + secs)
        nlen = stream[3]
        name = ''.join(chr(c) for c in name_u[:nlen])
        flags = stream[1]
        return {
            'name': name,
            'is_dir': bool(_u16(primary, 4) & 0x10),
            'attr': _u16(primary, 4),
            'size': _u64(stream, 0x18),
            'valid_size': _u64(stream, 0x08),
            'first_cluster': _u32(stream, 0x14),
            'no_fat_chain': bool(flags & 0x02),
            'name_len': nlen,
            'hash_stored': _u16(stream, 4),
            'hash_calc': name_hash(name),
            'chk_stored': _u16(primary, 2),
            'chk_calc': set_checksum(raw),
            'secondary_count': count,
        }

    def lookup(self, dir_cluster, name):
        want = name.upper()
        for e in self.parse_dir(dir_cluster):
            if e['name'].upper() == want:
                return e
        return None

    def resolve(self, path):
        """跨目录解析（支持 '..'），返回条目或 None。"""
        parts = [p for p in path.replace('\\', '/').split('/') if p and p != '.']
        cur = self.root
        entry = None
        for idx, part in enumerate(parts):
            if part == '..':
                continue                    # 参考实现不做父目录回溯，够用
            entry = self.lookup(cur, part)
            if entry is None:
                return None
            if idx == len(parts) - 1:
                return entry
            if not entry['is_dir']:
                return None
            cur = entry['first_cluster']
        return None

    # ---------- 文件 ----------
    def read(self, entry):
        """按 DataLength 读文件内容；NoFatChain 时按连续簇，否则沿 FAT 链。"""
        size = entry['size']
        if size == 0:
            return b''
        if entry['no_fat_chain']:
            need = (size + self.cluster_size - 1) // self.cluster_size
            clusters = list(range(entry['first_cluster'],
                                  entry['first_cluster'] + need))
        else:
            clusters = self.chain(entry['first_cluster'])
        data = b''.join(self.cluster(c) for c in clusters)
        if len(data) < size:
            raise ExfatError('file %s truncated: %d bytes for size %d'
                             % (entry['name'], len(data), size))
        return data[:size]

    def read_path(self, path):
        e = self.resolve(path)
        if e is None:
            return None
        return self.read(e)

    # ---------- 体检 ----------
    def audit(self, dir_cluster=None):
        """递归检查：SetChecksum / NameHash 是否自洽，簇链是否可读。

        返回问题列表（空 = 盘上所有目录项都与规范自洽）。"""
        if dir_cluster is None:
            dir_cluster = self.root
        problems = []
        for e in self.parse_dir(dir_cluster):
            if e['chk_stored'] != e['chk_calc']:
                problems.append('%s: set checksum %04X != recomputed %04X'
                                % (e['name'], e['chk_stored'], e['chk_calc']))
            if e['hash_stored'] != e['hash_calc']:
                problems.append('%s: name hash %04X != recomputed %04X'
                                % (e['name'], e['hash_stored'], e['hash_calc']))
            try:
                if not e['is_dir'] and e['size'] > 0:
                    self.read(e)
            except ExfatError as exc:
                problems.append('%s: %s' % (e['name'], exc))
            if e['is_dir']:
                problems.extend(self.audit(e['first_cluster']))
        return problems
