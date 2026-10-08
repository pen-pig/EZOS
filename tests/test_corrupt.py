# -*- coding: utf-8 -*-
"""test_corrupt.py - 盘上元数据被改坏时，内核必须"报错"而不是"卡死"

为什么要有这个测试
------------------
FAT / NTFS 的目录与 runlist 遍历代码，原来都是 `for(;;)` / `while(1)` 加一个
来自**磁盘**的结束标志：

  * FAT 目录块：`while (1) { if (!fat_dir_block(dir, idx)) break; }`
    —— FAT 链一旦成环（FAT[c] == c）就永远退不出；而且 fat_dir_block 是
    O(idx)，16MB 卷就是几亿次 FAT 读，等同挂死。
  * NTFS run list：`for(;;) { c1 = *run & 7; if (!c1) break; }`
    —— 结束字节被改坏就一路扫过记录边界读相邻内存。
  * NTFS 索引条目：`while (1) { if (flags & 2) return 0; pos += e_len; }`
    —— 末条目标志没了就一路走出 INDX 块。

三处的共同点是：**内核不打任何输出、不 panic，只是安静地转**。用户看到的是
"机器死了"，没有任何线索——比直接 panic 难查得多。这类缺陷靠功能测试永远
发现不了：好盘上每条路径都提前 break，测试全绿。

所以本测试**主动把盘改坏**，再断言内核仍然活着。

做法
----
1. 对每个可写 FS，先格式化并放几个文件，存下"黄金镜像"；
2. 用固定种子的**单 bit 翻转**把黄金镜像改坏若干处；
3. 用坏盘重新开机，断言：
     * 引导到 shell 提示符（挂载/自检阶段没卡住）
     * `ls` 能在超时内返回新的提示符（目录遍历没死循环）
     * 日志里没有 PANIC / #GP / #PF

关键设计点
----------
* **单 bit 翻转，不是随机覆盖字节**。整字节写 0/0xFF 太粗暴，多半在挂载阶段
  就被"不是这个文件系统"挡掉，根本到不了目录遍历。位翻转能穿过挂载，
  打进真正的解析路径。
* **固定种子** -> 可复现；改一行代码再跑，坏的是同一批字节。
* **只读串口日志，不用 OCR**。OCR 在这里既慢又不稳，而我们要判定的只是
  "命令有没有跑完"这种结构信号。
* **黄金镜像必须先自测通过**，否则"坏盘通过"可能只是因为好盘本来就跑不动。

判据：命令跑完的标志（探针实测）
--------------------------------
shell 每敲一个字符会重画整行，所以串口上"提示符个数"会暴涨，**不能数提示符**。
实测序列是：

    [/] > l[/] > ls\r\n/:\r\nbin/  ...  \r\n[/] > 

即：回车之前**没有** `\r\n`，回车之后才有。所以判据是
`新增输出里含 \r\n 且（去掉尾部空白后）以 "> " 结尾`。
卡死时要么连 `\r\n` 都没有，要么有头无尾，两种情况都会一路等到超时 -> 判红。

端口 4587（4585/4586 被 test_mouse.py 占用，端口必须每脚本唯一）。
用法：python tests/test_corrupt.py   （退出码 0 = 全通过）
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import (alloc_port, image_path,
                       disk_path, log_path, kill_stale_qemu)
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe  # noqa: E402

import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU = qemu_exe()
PORT = alloc_port()
PROMPT = b"[/] > "          # 引导结束标志
BOOT_TIMEOUT = 120.0        # 坏盘上首次挂载可能很慢，给足
CMD_TIMEOUT = 45.0

IMG = image_path()
SRC = disk_path()
LOG = log_path(os.path.join(HERE, "corrupt_serial.log").replace("\\", "/"))
DISK = os.path.join(HERE, "corrupt_disk.img").replace("\\", "/")
DISK_BYTES = 64 * 1024 * 1024   # FAT32 需要 >= 65525 簇，16MB 盘会被正确拒绝

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add'}


class Qmp(object):
    def __init__(self, port):
        for _ in range(60):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("QMP connect failed")
        self.sock.settimeout(20)
        self.f = self.sock.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        try:
            line = self.f.readline()
        except socket.timeout:
            raise RuntimeError("QMP read timed out")
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line) if line.strip() else None

    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = self._read()
            if r is None:
                continue
            if "return" in r or "error" in r:
                return r

    def hmc(self, c):
        return self.cmd("human-monitor-command", **{"command-line": c})

    def type_line(self, s):
        for ch in s:
            key = KEYMAP.get(ch)
            if key is None:
                key = ('shift-' + ch.lower()) if ('A' <= ch <= 'Z') else ch.lower()
            r = self.hmc("sendkey " + key)
            if "error" in r:
                raise RuntimeError("sendkey %r failed: %r" % (key, r))
            time.sleep(0.05)
        self.hmc("sendkey ret")


def read_log():
    """串口原文（bytes）。用 latin1 解码即可，别用 utf-8：盘上文件名可能是
    任意字节，utf-8 解码会抛异常，把"盘坏了"误报成"测试崩了"。"""
    try:
        with open(LOG, "rb") as f:
            return f.read()
    except OSError:
        return b""


def flat(b):
    return " ".join(b.decode("latin1", "replace").lower().split())


def wait_for(needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_log():
            return True
        time.sleep(0.5)
    return False


def run_cmd(qmp, cmd, timeout=CMD_TIMEOUT):
    """打一行命令，等到"这条命令真的跑完"。返回 (跑完了?, 新增输出)。

    见文件头"判据"一节：不能数提示符，只能看 `\r\n` + 结尾是 "> "。
    固定 sleep 也判不准——坏盘上目录遍历可能要走完整条（伪造的）链才放弃，
    快的时候不到一秒，慢的时候几十秒。
    """
    n0 = len(read_log())
    qmp.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        new = read_log()[n0:]
        # 注意是 rstrip() 之后判 b">"：提示符本身以 "> " 结尾，rstrip 会把那个
        # 空格吃掉，直接 endswith(b"> ") 永远为假（第一次跑全红就是栽在这里）。
        if b"\r\n" in new and new.rstrip().endswith(b">"):
            return True, new
        time.sleep(0.5)
    return False, read_log()[n0:]


def port_in_use(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
        s.close()
        return True
    except OSError:
        return False


def wait_port_free(port, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not port_in_use(port):
            return True
        time.sleep(0.3)
    return False


def launch(disk):
    """起一台 QEMU。每个用例一台，绝不复用（坏盘可能把内核拖进怪状态）。"""
    if port_in_use(PORT):
        kill_stale_qemu("qemu-system-x86_64.exe")
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        raise RuntimeError("PORT %d occupied" % PORT)
    try:
        os.remove(LOG)
    except OSError:
        pass
    p = subprocess.Popen([
        QEMU, "-vga", "std", "-display", "none", "-m", "128",
        "-drive", "format=raw,file=" + IMG,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-serial", "file:" + LOG,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = Qmp(PORT)
    if not wait_for(PROMPT, BOOT_TIMEOUT):
        p.kill()
        p.wait(timeout=15)
        raise RuntimeError("boot did not reach prompt")
    return p, qmp


def shutdown(p):
    p.kill()
    try:
        p.wait(timeout=15)
    except Exception:
        pass


def fresh_disk():
    shutil.copyfile(SRC, DISK)
    if os.path.getsize(DISK) < DISK_BYTES:
        with open(DISK, "r+b") as f:
            f.truncate(DISK_BYTES)


def gold_path(name):
    return os.path.join(HERE, "corrupt_gold_%s.img" % name).replace("\\", "/")


def corrupt(dst, gold, seed, nflips=6, span=96 * 1024, start=4096):
    """按固定种子做单 bit 翻转。只在卷头部动手，那里才是元数据。

    start=4096：**跳过前 4KB**。那里是引导扇区 / 超级块，翻坏了卷直接挂不
    上，fs_init 会回退到别的 FS，于是后面所有命令都跑在一个**没被破坏**的
    卷上——曾经 86 条"全绿"就是这么来的，一条都没碰到坏数据。
    从 4KB 往后翻，卷还挂得上，破坏才落在真正被遍历的元数据（FAT 表、
    GDT、inode 表、目录块）上。超级块的不可信输入由定向用例单独覆盖。
    """
    shutil.copyfile(gold, dst)
    size = os.path.getsize(dst)
    lo = min(start, size - 1)
    hi = min(span, size)
    if hi <= lo:
        hi = size
    rnd = random.Random(seed)
    hits = []
    with open(dst, "r+b") as f:
        for _ in range(nflips):
            off = rnd.randrange(lo, hi)
            f.seek(off)
            b = f.read(1)
            if not b:
                continue
            f.seek(off)
            f.write(bytes([b[0] ^ (1 << rnd.randrange(8))]))
            hits.append(off)
    return hits


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def vol_base(path):
    """卷起点（字节）。disk.vhd 带 MBR，分区 LBA 在偏移 454。"""
    with open(path, "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        return 0
    return _u32(mbr, 454) * 512


def corrupt_fat32_cycle(dst):
    """把 FAT32 根目录的 FAT 表项改成"指向自己"。

    这是最典型的坏链，也是 fat.c 老代码死循环的直接原因：FAT[c] == c 时链的
    结束标志永远不会出现。随机位翻转几乎碰不出这个形态（要正好把表项改成
    自己的簇号），所以必须**定向**造一个，否则测试对这类缺陷是瞎的。
    """
    base = vol_base(dst)
    with open(dst, "r+b") as f:
        f.seek(base)
        bpb = f.read(512)
        bps = _u16(bpb, 11)
        reserved = _u16(bpb, 14)
        root = _u32(bpb, 44)
        if bps < 512 or (bps & (bps - 1)) or reserved == 0 or root < 2:
            return None
        off = base + reserved * bps + root * 4
        f.seek(off)
        old = f.read(4)
        if len(old) != 4:
            return None
        f.seek(off)
        f.write(struct.pack("<I", root & 0x0FFFFFFF))
        return off, _u32(old, 0), root


def corrupt_exfat_cycle(dst):
    """同上，针对 exFAT（FatOffset@0x50、根簇@0x60、扇区移位@0x6C）。"""
    base = vol_base(dst)
    with open(dst, "r+b") as f:
        f.seek(base)
        vbr = f.read(512)
        fat_off = _u32(vbr, 0x50)
        root = _u32(vbr, 0x60)
        bps = 1 << vbr[0x6C]
        if bps < 512 or fat_off == 0 or root < 2:
            return None
        off = base + fat_off * bps + root * 4
        f.seek(off)
        old = f.read(4)
        if len(old) != 4:
            return None
        f.seek(off)
        f.write(struct.pack("<I", root))
        return off, _u32(old, 0), root


def corrupt_ext4_blkcount(dst):
    """把 ext4 超级块的 s_blocks_count_lo 改成天文数字。

    不加闸的话 e4_groups = blocks_total / blocks_per_group 会算到几十万，
    分配/扫描遍历块组的循环就是几十万次块读，ls 直接挂死；而且
    `blocks_total - first_data_blk` 还能 uint64 下溢。挂载时按 ata_capacity
    定上界 + 组数卡 65536，就是为这个场景加的。"""
    base = vol_base(dst)
    sb_off = base + 1024
    with open(dst, "r+b") as f:
        f.seek(sb_off)
        sb = f.read(1024)
        if len(sb) < 1024 or _u16(sb, 0x38) != 0xEF53:
            return None                      # 不是 ext4 超级块
        off = sb_off + 0x4                   # s_blocks_count_lo
        f.seek(off)
        old = f.read(4)
        if len(old) != 4:
            return None
        f.seek(off)
        f.write(struct.pack("<I", 0xFFFFFFFF))
        return off, _u32(old, 0), 0xFFFFFFFF


def corrupt_ext4_depth(dst):
    """把块组 0 的 inode 表里每个 inode 的 extent 头写成 entries/depth=0xFFFF。

    depth 是递归层数：不加闸会递归 65535 层，16KB 内核栈一帧也撑不住。
    entries 是循环上界：不加闸时 hdr+12+i*12 读到缓冲外几百 KB。
    inode 表块号从块组描述符的 bg_inode_table_lo（GDT 项偏移 0x8）取，
    不猜位置——ext4 的元数据布局随 blksize/特性变，猜必错。"""
    base = vol_base(dst)
    sb_off = base + 1024
    with open(dst, "r+b") as f:
        f.seek(sb_off)
        sb = f.read(1024)
        if len(sb) < 1024 or _u16(sb, 0x38) != 0xEF53:
            return None
        logb = _u32(sb, 0x18)
        if logb > 2:
            return None
        blksize = 1024 << logb          # s_log_block_size
        ino_size = _u16(sb, 0x58) or 128  # s_inode_size
        # GDT 起始块：1KB 块时 superblock 独占块 0，GDT 从块 2 起；否则块 1
        gdt_blk = 2 if blksize == 1024 else 1
        f.seek(base + gdt_blk * blksize)
        gd = f.read(32)
        if len(gd) < 32:
            return None
        ino_tbl = _u32(gd, 0x8)         # bg_inode_table_lo
        if ino_tbl == 0:
            return None
        off = base + ino_tbl * blksize
        f.seek(off)
        blk = bytearray(f.read(blksize))
        if len(blk) < blksize:
            return None
        # 先造一个"自环"的 extent 叶子块：块 L 的 header 里唯一的 index
        # 指向块 L 自己。这样 e4_free_extent_node 每递归一层都读同一块，
        # depth 从 0xFFFF 一路减到底 —— 没有深度闸就是 65535 层栈帧。
        # 用一个远离元数据的块（1KB 块号 1000，16MB 卷里是数据区）。
        LEAF = 1000
        if (LEAF + 1) * blksize > os.path.getsize(dst) - base:
            return None
        node = bytearray(blksize)
        struct.pack_into("<HHHH", node, 0, 0xF30A, 0xFFFF, 0xFFFF, 0xFFFF)
        struct.pack_into("<II", node, 12, 0, LEAF)   # ei_block=0, ei_leaf=LEAF
        f.seek(base + LEAF * blksize)
        f.write(bytes(node))

        n = 0
        # inode 表通常跨好几个块（ipg=1024 x 128B = 128KB），A.TXT 的 inode
        # 一般不在第一块里。只改第一块的话改到的全是保留 inode，一个文件
        # inode 都没碰到 —— 破坏"造出来了"但等于没造（n 会算成 0）。
        ipg = _u32(sb, 0x28)
        nblk = (ipg * ino_size + blksize - 1) // blksize
        if nblk < 1:
            nblk = 1
        if nblk > 8:
            nblk = 8                    # 前 8 块足够覆盖前几十个 inode
        for b in range(nblk):
            f.seek(base + (ino_tbl + b) * blksize)
            blk = bytearray(f.read(blksize))
            if len(blk) < blksize:
                break
            for i in range(blksize // ino_size):
                # **目录 inode 必须留着**：连目录的 extent 头都改坏的话，
                # rm 先就找不到文件，直接报 failed to delete，根本走不到
                # 释放路径 —— 递归爆栈这条就测不到（拆了闸也绿）。
                mode = struct.unpack_from("<H", blk, i * ino_size)[0]
                if (mode & 0xF000) == 0x4000:      # 目录
                    continue
                ib = i * ino_size + 0x28    # i_block
                if ib + 24 > blksize:
                    break
                # header: magic + entries=0xFFFF + max + depth=0xFFFF
                struct.pack_into("<HHHH", blk, ib, 0xF30A, 0xFFFF, 0xFFFF,
                                 0xFFFF)
                # 第一个 index 指向自环叶子块
                struct.pack_into("<II", blk, ib + 12, 0, LEAF)
                # 强制走 extent 释放路径：小文件（A.TXT 只有一个块）默认是
                # legacy 直接块，i_flags 里没有 EXTENTS_FL，e4_free_inode_blocks
                # 会走 else 分支，压根不进 e4_free_extent_node —— 递归这条
                # 就永远测不到。
                fo = i * ino_size + 0x20            # i_flags
                struct.pack_into("<I", blk, fo,
                                 struct.unpack_from("<I", blk, fo)[0] |
                                 0x00080000)        # EXT4_EXTENTS_FL
                n += 1
            f.seek(base + (ino_tbl + b) * blksize)
            f.write(bytes(blk))
        if n == 0:
            return None
        return base + ino_tbl * blksize, 0, n


# 定向损坏：
# - exfat/fat32 根目录是 FAT 簇链，自环才有意义；
# - ext4 的两个是纯磁盘字段当循环边界用的典型（块数 / extent 头）。
CYCLE = {"exfat": corrupt_exfat_cycle, "fat32": corrupt_fat32_cycle}
DIRECTED = {
    "ext4": [("超级块块数=天文数字", corrupt_ext4_blkcount),
             ("extent 头 depth/entries=0xFFFF", corrupt_ext4_depth)],
}

FS_LIST = ["exfat", "fat32", "ext4", "ntfs", "f2fs", "refs"]
SEEDS = [1, 2, 3]
# 调试用：EZOS_CORRUPT_FS=ext4 只跑 ext4（全量 30 分钟，改一个 FS 的
# 定向用例时不该等全套）。
_only = _os_ezos.environ.get("EZOS_CORRUPT_FS")
if _only:
    FS_LIST = [s for s in _only.split(",") if s]


def make_gold(name):
    """格式化一个好盘并放几个文件，存成 <disk>.gold。返回 (ok, 说明)。"""
    fresh_disk()
    p, qmp = launch(DISK)
    try:
        # **必须先切到数据盘再 format**。format 作用于"当前 FS drive"，
        # 而启动时 FS 挂在内置系统卷（drive 0）上——不切盘的话 format 格的
        # 是系统卷，数据盘从头到尾是原始副本，后面所有"坏盘"用例都在一个
        # 根本没被格式化的盘上跑。这个测试曾经 86 条全绿就是这么来的。
        done, out = run_cmd(qmp, "setdrive 1")
        if not done or "drive set to 1" not in flat(out).lower():
            return False, "setdrive 1 失败：%r" % flat(out)[:160]
        done, out = run_cmd(qmp, "format " + name, 180.0)
        if not done:
            return False, "format 卡住（180s 无结果）"
        if "disk formatted as" not in flat(out):
            return False, "format 失败：%r" % flat(out)[:120]
        # 用 write 而不是 touch：空文件的 cat 一步就返回，读路径根本没走到，
        # 坏盘上的簇链/extent 解析就测不出来。
        run_cmd(qmp, "write A.TXT hello")
        run_cmd(qmp, "mkdir SUB")
        run_cmd(qmp, "cd SUB")
        run_cmd(qmp, "touch C.TXT")
        run_cmd(qmp, "cd ..")
        done, out = run_cmd(qmp, "ls")
        # 黄金盘自检：好盘必须能列出 A.TXT 和 SUB，否则后面"坏盘能返回
        # 提示符"没有意义——那只是因为它本来就跑不动。
        if not done:
            return False, "好盘 ls 就卡住了"
        if "a.txt" not in flat(out) or "sub" not in flat(out):
            return False, "好盘 ls 列不出文件：%r" % flat(out)[:160]
    finally:
        shutdown(p)
    shutil.copyfile(DISK, gold_path(name))
    return True, "ok"


def probe(tag, where, extra=()):
    """用当前 DISK 起一台机器，断言"引导没卡住 + 命令能跑完 + 没有 panic"。

    extra 用来给定向用例加命令：随机翻位那批只跑读，但有些缺陷只在写/删
    路径上（比如 ext4 释放 extent 树的递归），不加 rm 就永远绿——
    **不加命令的定向用例是假绿**，反向拆闸验证时才发现（拆了闸它照样绿）。"""
    out = []
    try:
        p, qmp = launch(DISK)
    except RuntimeError as e:
        # 引导阶段就没出提示符 = 挂载/自检时卡住，这正是要抓的挂死
        out.append(("%s: 引导没挂死" % tag, False))
        print("  !! %s 引导未达提示符：%s（改动位置 %s）" % (tag, e, where))
        return out
    try:
        # **必须先切到数据盘**。启动时 FS 默认挂在内置系统卷（drive 0）上，
        # 不切的话 ls/cat/df 全跑在一个**根本没被破坏**的卷上——
        # 曾经"86 条全绿"就是这么来的：一条都没碰到坏盘。
        # 前置条件失败必须中止，否则后面每条都是毫无意义的绿
        # （rmdir 那次 11 条假绿就是这么踩的）。
        # 大小写别硬匹配：内核打印的是 "fs drive set to 1 (exfat)"。
        done, out0 = run_cmd(qmp, "setdrive 1")
        low = flat(out0).lower()
        if not done or "drive set to 1" not in low:
            out.append(("%s: 能切到数据盘 drive 1" % tag, False))
            print("  !! %s setdrive 1 失败：%r（后续用例无意义，中止）"
                  % (tag, flat(out0)[:160]))
            return out
        # 挂载失败时 fs_init 会回退到别的 FS，命令跑在回退卷上——那也是
        # "没挂死"，但**没碰到坏数据**，所以单独标出来，别让它混进
        # "遍历路径安全"的结论里。
        if "(exfat)" in low and not tag.startswith("exfat"):
            print("  ~~ %s：卷没挂上（回退到 exfat），只验证了挂载阶段不挂死"
                  % tag)
        # ls 覆盖目录遍历，cat 覆盖读路径（簇链/extent/NAT），
        # df 覆盖全卷扫描（FAT 链 / 位图）——三条路径分属不同代码，
        # 只测 ls 会漏掉后两条。
        for cmd in ["ls", "cat A.TXT", "df"] + list(extra):
            alive, _ = run_cmd(qmp, cmd)
            out.append(("%s: %s 未挂死" % (tag, cmd), alive))
            if not alive:
                print("  !! %s %s 超时未跑完（改动位置 %s）" % (tag, cmd, where))
        log = flat(read_log())
        # 注意别用裸 "panic"：启动横幅里就有
        # "ISR: 32 CPU exception gates installed (panic screen on fault)"，
        # 裸匹配会让**每一条**都假红（第一次跑 18 条全红就是这么来的）。
        bad = [k for k in ("kernel panic", "exception: #", "page fault")
               if k in log]
        out.append(("%s: 无 panic/异常" % tag, not bad))
        if bad:
            print("  !! %s 日志命中 %s" % (tag, bad))
    finally:
        shutdown(p)
    return out


def main():
    for path in (IMG, SRC):
        if not os.path.isfile(path):
            print("MISSING %s - run ninja first" % path)
            return 2

    results = []
    for name in FS_LIST:
        ok, info = make_gold(name)
        results.append(("%s: 黄金盘可用" % name, ok))
        if not ok:
            print("  !! %s 黄金盘失败：%s —— 跳过该 FS 的坏盘用例" % (name, info))
            continue

        # 干净盘重启：验证"格式化好的卷重新挂载"这条路径本身是通的。
        # 没有这条的话，万一 fs_init 根本挂不上某个 FS，所有坏盘用例都会
        # 以"回退到别的 FS"的方式通过，看起来全绿其实一条都没测到。
        shutil.copyfile(gold_path(name), DISK)
        results.extend(probe("%s 干净盘重启" % name, []))

        for seed in SEEDS:
            hits = corrupt(DISK, gold_path(name), seed)
            results.extend(probe("%s seed=%d" % (name, seed), hits))

        # 定向用例一律**从黄金盘重新复制**再改。踩过：原来直接在上一轮
        # 翻过位的盘上叠加，而那盘经常已经坏到挂载失败了，于是破坏根本
        # 没碰到目标路径——拆掉内核里的闸它也照样绿，是彻头彻尾的假绿。
        if name in CYCLE:
            shutil.copyfile(gold_path(name), DISK)
            info = CYCLE[name](DISK)
            if info is None:
                results.append(("%s: 能造出根目录链自环" % name, False))
                print("  !! %s 读不出 BPB/根簇，定向自环没造出来" % name)
            else:
                results.extend(probe("%s 根目录链自环" % name, [info[0]]))

        for label, fn in DIRECTED.get(name, []):
            shutil.copyfile(gold_path(name), DISK)
            info = fn(DISK)
            if info is None:
                results.append(("%s: 能造出%s" % (name, label), False))
                print("  !! %s 定向损坏没造出来：%s" % (name, label))
            else:
                results.extend(probe("%s %s" % (name, label), [info[0]],
                                    # rm 走释放路径（ext4 释放 extent 树是递归，
                                    # depth 没闸就爆栈）；write 走分配路径
                                    # （遍历块组找空闲块，组数没闸就是几十万
                                    # 次块读）。少了这两条，定向用例拆了内核
                                    # 的闸照样绿——那就等于没测。
                                    extra=("rm A.TXT", "write B.TXT hello")))

    npass = sum(1 for _, ok in results if ok)
    for label, ok in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", label))
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
