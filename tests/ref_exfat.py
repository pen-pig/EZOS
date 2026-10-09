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

只读解析在 Exfat 里；ExfatRW 另外提供最小**写入**路径（建文件/建目录/删文件，
含位图与 FAT 的真实维护），用来跑"宿主机先建 -> 内核读/改/删 -> 宿主机复检"
的双向互操作对拍（tests/test_hostfs.py）。两处代码都不看 exfat.c。
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

    def meta(self, tag):
        """根目录里的元数据条目（0x81 位图 / 0x82 大写表）-> (首簇, 数据长度)。

        **唯一事实来源**：所有工具（test_exfatvol / host_probe / fsck 测试）
        都必须走这里，不许自己 ``_u32(slot, 20)``。

        原因：规范里主条目**没有** FirstCluster / DataLength，这两个字段在
        紧跟的 ``0xC0`` Stream Extension 的 +0x14 / +0x18；主条目 +0x14 是
        保留字段（全 0）。EZOS 早先把它们写在主条目里，于是任何按规范写的
        卷（Windows 建的）在这里都读到 0 —— 位图校验会整片静默失效。
        现在 format 已按规范写，读也必须按规范读。
        """
        slots = self.dir_slots(self.root)
        for i, s in enumerate(slots):
            if s[0] != tag:
                continue
            st = slots[i + 1] if i + 1 < len(slots) and slots[i + 1][0] == 0xC0 else s
            return _u32(st, 20), _u64(st, 24)
        return None

    def flags_of(self, tag):
        """元数据条目紧跟的 0xC0 的 GeneralSecondaryFlags（bit0=AllocationPossible）。

        没有 0xC0 时返回 None——那是"非规范卷"，调用方自行判定是否接受。"""
        slots = self.dir_slots(self.root)
        for i, s in enumerate(slots):
            if s[0] != tag:
                continue
            if i + 1 < len(slots) and slots[i + 1][0] == 0xC0:
                return slots[i + 1][1]
            return None
        return None

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
    def audit_root_meta(self, strict=False):
        """根目录三个特殊条目——**Windows 挂载必查**，错一项就弹"需要格式化"。

        规范（exFAT spec）的布局是：每个 critical 主条目后面紧跟一个 ``0xC0``
        Stream Extension，``FirstCluster`` / ``DataLength`` / ``AllocationPossible``
        **都在 0xC0 里**——主条目自身没有这几个字段。EZOS 早先把它们直接写
        在主条目里、不写 0xC0，于是别人（Windows / 别的工具）建的规范卷，
        内核按主条目 +0x14 取位图簇号只取到保留字段 0，位图同步全落到簇 0：
        内核不看位图所以毫无察觉，宿主一挂载就看到 FAT 与位图互相矛盾。

          * ``0x83`` Volume Label：SecondaryCount=1，紧跟 0xC0；
          * ``0x81`` Allocation Bitmap：其 0xC0 的 flags bit0
            （AllocationPossible）必须为 1，bit1（NoFatChain）位图跨簇故为 0；
          * ``0x82`` Up-case Table：其 0xC0 的 flags bit0 必须为 0。

        ``strict=True`` 时额外要求 0x81/0x82 **必须**带 0xC0（新写出的盘该满足）。
        默认不要求，因为仓库里还有历史盘是 inline 形态，读它们不该报红。

        内核只把 ``0x85`` 当文件条目解析，绕过了这一段——所以这类错误内核
        自己永远发现不了，只能靠这里或者真的挂一次盘。"""
        problems = []
        slots = self.dir_slots(self.root)
        idx = {}
        for i, s in enumerate(slots):
            if s[0] in (0x81, 0x82, 0x83) and s[0] not in idx:
                idx[s[0]] = (i, s)
        if 0x83 not in idx:
            return ["root: no 0x83 volume label entry"]
        i83, s83 = idx[0x83]
        if s83[1] != 1:
            problems.append("0x83 volume label SecondaryCount=%d, spec requires 1"
                            % s83[1])
        nxt = slots[i83 + 1][0] if i83 + 1 < len(slots) else -1
        if nxt != 0xC0:
            problems.append("0x83 not followed by 0xC0 stream entry (found 0x%02X)"
                            % nxt)

        def _check(t, alloc_possible):
            """t=0x81/0x82；alloc_possible 是该条目应有的 AllocationPossible"""
            if t not in idx:
                problems.append("root: no 0x%02X entry" % t)
                return
            i, s = idx[t]
            nx = slots[i + 1][0] if i + 1 < len(slots) else -1
            if nx == 0xC0:
                st = slots[i + 1]
                ap = st[1] & 0x01
                fc = _u32(st, 0x14)
                if not ap and alloc_possible:
                    problems.append("0x%02X stream: AllocationPossible=0 "
                                    "(spec requires 1)" % t)
                if ap and not alloc_possible:
                    problems.append("0x%02X stream: AllocationPossible=1 "
                                    "(spec requires 0)" % t)
            else:
                if strict:
                    problems.append(
                        "0x%02X not followed by 0xC0 stream entry (found 0x%02X)"
                        % (t, nx))
                ap = s[1] & 0x01
                fc = _u32(s, 0x14)
                if not ap and alloc_possible:
                    problems.append("0x%02X AllocationPossible=0 "
                                    "(spec requires 1)" % t)
                if ap and not alloc_possible:
                    problems.append("0x%02X AllocationPossible=1 "
                                    "(spec requires 0)" % t)
            if not (2 <= fc < self.cluster_count + 2):
                problems.append("0x%02X FirstCluster=%d out of range" % (t, fc))

        _check(0x81, True)
        _check(0x82, False)
        return problems

    def audit(self, dir_cluster=None):
        """递归检查：SetChecksum / NameHash 是否自洽，簇链是否可读。

        返回问题列表（空 = 盘上所有目录项都与规范自洽）。"""
        if dir_cluster is None:
            dir_cluster = self.root
        problems = []
        if dir_cluster == self.root:
            problems.extend(self.audit_root_meta())
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


class ExfatRW(Exfat):
    """宿主机侧 exFAT 写入器：**第二个实现**，与 kernel/exfat.c 零共享。

    存在的理由和只读部分一样——内核自己写自己读永远绿。有了它才能问出那两个
    只有"另一方的实现"才答得上来、且真实互操作（Windows 拖文件进 VHD）必然
    遇到的问题：

      * 别人建的卷，内核往里**追加**时会不会踩坏原有条目 / 位图 / FAT？
      * 内核删掉别人建的文件后，簇和位图有没有真的还回去？
    """

    def __init__(self, path):
        super(ExfatRW, self).__init__(path)
        with open(path, 'rb') as f:
            orig = f.read()
        self.path = path
        self.footer = orig[-512:] if orig[-512:-504] == b'conectix' else b''
        self.raw = bytearray(self.raw)
        m = self.meta(0x81)
        if m is None:
            raise ExfatError('no 0x81 allocation bitmap - refusing to write')
        self.bmp_cluster, self.bmp_len = m

    # ---------- 原始写 ----------
    def _cl_off(self, c):
        return self.part_off + (self.heap_off + (c - 2) * self.spc) * self.bps

    def write_cluster(self, c, data):
        if c < 2 or c >= self.cluster_count + 2:
            raise ExfatError('cluster %d out of range' % c)
        if len(data) > self.cluster_size:
            raise ExfatError('cluster payload %d > %d' % (len(data), self.cluster_size))
        data = bytes(data) + b'\x00' * (self.cluster_size - len(data))
        off = self._cl_off(c)
        self.raw[off:off + self.cluster_size] = data

    def set_fat(self, c, v):
        off = self.part_off + self.fat_off * self.bps + c * 4
        struct.pack_into('<I', self.raw, off, v & 0xFFFFFFFF)

    def bmp_get(self, c):
        idx = c - 2
        off = self._cl_off(self.bmp_cluster) + idx // 8
        return (self.raw[off] >> (idx % 8)) & 1

    def bmp_set(self, c, v):
        idx = c - 2
        off = self._cl_off(self.bmp_cluster) + idx // 8
        if v:
            self.raw[off] |= (1 << (idx % 8))
        else:
            self.raw[off] &= ((1 << (idx % 8)) ^ 0xFF) & 0xFF

    def alloc(self, n=1):
        """按位图找 n 个空闲簇，置位并串成 FAT 链（尾簇 EOC）。"""
        out = []
        for c in range(2, self.cluster_count + 2):
            if len(out) == n:
                break
            if self.fat(c) == 0 and not self.bmp_get(c):
                out.append(c)
        if len(out) < n:
            raise ExfatError('out of free clusters: need %d got %d' % (n, len(out)))
        for i, c in enumerate(out):
            self.bmp_set(c, 1)
            self.set_fat(c, out[i + 1] if i + 1 < len(out) else 0xFFFFFFFF)
        return out

    def free_chain(self, first):
        """回放 FAT 链，清 FAT 并把位图 bit 还回空闲。"""
        freed = []
        for c in self.chain(first):
            self.set_fat(c, 0)
            self.bmp_set(c, 0)
            freed.append(c)
        return freed

    # ---------- 目录槽 ----------
    def slot_offsets(self, dir_cluster):
        return [(c, off) for c in self.chain(dir_cluster)
                for off in range(0, self.cluster_size, 32)]

    def _slot_byte(self, c, off):
        return self.raw[self._cl_off(c) + off]

    def _extend_dir(self, dir_cluster):
        """目录槽不够时接一簇（内核读目录走 FAT 链，所以只需接链 + 清零）。"""
        chain = self.chain(dir_cluster)
        new = self.alloc(1)[0]
        self.set_fat(chain[-1], new)
        self.set_fat(new, 0xFFFFFFFF)
        self.write_cluster(new, b'')
        return new

    def free_run(self, dir_cluster, need):
        """找 need 个连续空闲槽；不够就扩目录再来。"""
        for _ in range(4):
            slots = self.slot_offsets(dir_cluster)
            run = 0
            for i, (c, off) in enumerate(slots):
                if (self._slot_byte(c, off) & 0x80) == 0:
                    run += 1
                    if run == need:
                        start = i - need + 1
                        # 末尾必须留一个 0x00 终结符，否则读者会读到未初始化区
                        if start + need < len(slots):
                            return slots[start:start + need]
                else:
                    run = 0
            self._extend_dir(dir_cluster)
        raise ExfatError('cannot find %d free directory slots' % need)

    def _write_run(self, dir_cluster, blob):
        slots = self.free_run(dir_cluster, len(blob) // 32)
        for i, (c, off) in enumerate(slots):
            base = self._cl_off(c) + off
            self.raw[base:base + 32] = blob[i * 32:(i + 1) * 32]

    # ---------- 建 / 删 ----------
    def _build_set(self, name, is_dir, first_cluster, size, no_fat_chain):
        nlen = len(name)
        n_name_entries = max(1, (nlen + 14) // 15)
        secondary = 1 + n_name_entries
        primary = bytearray(32)
        primary[0] = 0x85
        primary[1] = secondary
        struct.pack_into('<H', primary, 4, 0x10 if is_dir else 0x20)
        stream = bytearray(32)
        stream[0] = 0xC0
        stream[1] = 0x01 | (0x02 if (no_fat_chain and not is_dir and size > 0) else 0)
        stream[3] = nlen
        struct.pack_into('<H', stream, 4, name_hash(name))
        if not is_dir:
            struct.pack_into('<Q', stream, 8, size)      # ValidDataLength
        struct.pack_into('<I', stream, 20, first_cluster)
        struct.pack_into('<Q', stream, 24,
                         size if not is_dir else self.cluster_size)
        entries = [primary, stream]
        name_u = name.encode('utf-16-le')
        for i in range(n_name_entries):
            e = bytearray(32)
            e[0] = 0xC1
            chunk = name_u[i * 30:(i + 1) * 30]
            e[2:2 + len(chunk)] = chunk
            entries.append(e)
        blob = b''.join(bytes(x) for x in entries)
        struct.pack_into('<H', entries[0], 2, set_checksum(blob))
        return b''.join(bytes(x) for x in entries)

    def create(self, dir_cluster, name, data=b'', is_dir=False,
               force_fat_chain=False):
        """建文件或目录。返回 (first_cluster, [占用的簇])。"""
        data = bytes(data)
        size = len(data)
        if is_dir:
            cl = self.alloc(1)[0]
            self.write_cluster(cl, b'')
            blob = self._build_set(name, True, cl, 0, False)
            used = [cl]
        elif size == 0:
            blob = self._build_set(name, False, 0, 0, True)
            used = []
        else:
            need = (size + self.cluster_size - 1) // self.cluster_size
            clusters = self.alloc(need)
            for i, c in enumerate(clusters):
                self.write_cluster(c, data[i * self.cluster_size:
                                            (i + 1) * self.cluster_size])
            # 单簇且没有强制要求时按 NoFatChain 记；多簇一律走 FAT 链
            no_fat = (need == 1 and not force_fat_chain)
            if no_fat:
                self.set_fat(clusters[0], 0xFFFFFFFF)
            blob = self._build_set(name, False, clusters[0], size, no_fat)
            used = clusters
        self._write_run(dir_cluster, blob)
        return (used[0] if used else 0), used

    def delete(self, dir_cluster, name):
        """标记 entry set 为已删除（清 0x80）并归还簇链。返回释放的簇列表。"""
        slots = self.slot_offsets(dir_cluster)
        blob = bytes(b''.join(bytes(self.raw[self._cl_off(c) + o:
                                             self._cl_off(c) + o + 32])
                              for c, o in slots))
        slots_bytes = [blob[i * 32:(i + 1) * 32] for i in range(len(slots))]
        for i, s in enumerate(slots_bytes):
            if s[0] != 0x85:
                continue
            ent = self._parse_set(slots_bytes, i)
            if ent is None or ent['name'].upper() != name.upper():
                continue
            for k in range(ent['secondary_count'] + 1):
                c, off = slots[i + k]
                self.raw[self._cl_off(c) + off] &= 0x7F
            if ent['first_cluster'] >= 2:
                return self.free_chain(ent['first_cluster'])
            return []
        return None

    def flush(self, path=None):
        with open(path or self.path, 'r+b') as f:
            f.write(bytes(self.raw) + self.footer)
