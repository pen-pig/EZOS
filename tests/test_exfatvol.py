# -*- coding: utf-8 -*-
"""test_exfatvol.py - exFAT 大卷：位图跨簇、元数据成链、位图与 FAT 必须一致。

为什么要它：exFAT 的 Allocation Bitmap 是个**普通文件**（0x81 条目），长度
ceil(cluster_count/8) 字节。512B/簇时一簇只覆盖 4096 个簇 = 2MB，所以卷一大，
位图本身就跨好几个簇，必须像普通文件那样在 FAT 里**成链**。

这里历史上出过三个只在卷变大时才暴露的 bug，而"内核自己写自己读"永远绿：

  1. 每读一个 FAT 项都是一次真读盘 -> 大卷上 `df` 要 20 秒以上（小卷根本看不出）；
  2. exfat_bitmap_set 按"位图首簇 + 字节偏移"算，跨簇时写到簇外去了；
  3. 格式化只初始化 FAT 第一个扇区，且把每个元数据簇都写成 EOC -> 位图被截断成
     一簇，后面的簇在 FAT 里是 EOC、位图里却没有对应字节。

本测试用宿主机独立实现（tests/ref_exfat.py，按 spec、与 kernel/exfat.c 零共享）
做三件事：量卷覆盖、验位图/FAT 自洽、并**先把盘填到越过第一个位图簇**，再让内核
分配新簇——只有这样才能真正走到"位图跨簇"那条路径。

端口 4631(QMP) / 4632(serial)。
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import math
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe  # noqa: E402

QEMU = qemu_exe()
IMG = image_path()
WORK = os.path.join(HERE, "exfatvol_disk.img").replace("\\", "/")
QMP_PORT, SER_PORT = alloc_port(2)

# 16MB：约 32000 个 512B 簇 -> 位图 4000 字节 = 8 簇（跨簇路径必然被走到）。
# 再小（比如 2MB 以下）位图只有一簇，这个测试就退化成普通的读写测试了。
MB = int(sys.argv[1]) if len(sys.argv) > 1 else 16
VOL_BYTES = MB * 1024 * 1024

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa
import ref_exfat  # noqa: E402
from ref_exfat import _u32, _u64  # noqa: E402

# 宿主机要预填多少簇。目的不是测大文件，而是把已用簇推过第一个位图簇能表示
# 的范围：512B/簇的位图，一簇只有 4096 位 = 4096 个簇。簇 c 落在位图第
# (c-2)/8 字节，所以要用到位图第二簇，必须分配到 c >= 4098。取 4300 留余量。
PREFILL_CLUSTERS = 4300


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def flat(s):
    return " ".join(s.split())


class Guest(object):
    """一次 QEMU 会话。每用例独立起，盘在两次会话之间由宿主机改写。"""

    def __init__(self):
        self.proc = subprocess.Popen(
            [QEMU, "-icount", "shift=auto", "-vga", "std",
             "-drive", "format=raw,file=" + IMG,
             "-drive", "format=raw,file=" + WORK,
             "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
             "-serial", "tcp:127.0.0.1:%d,server,nowait" % SER_PORT],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        self.serial = SerialReader(SER_PORT)
        self.qmp = Qmp(QMP_PORT)

    def boot_wait(self):
        wait_for(self.serial, "TASK: preemptive", 150)
        wait_for(self.serial, "[/] >", 120)
        time.sleep(1.0)

    def run(self, cmd, settle=2.5, quiet=1.5, timeout=180.0):
        """键入一行，先等 settle 秒下限，再等串口静默 quiet 秒。

        格式化这类命令在慢盘上会远超预期时长，后面的命令被敲进去就会和它
        交错执行，症状是内核 panic（看起来像内存破坏，极具误导性）。"""
        before = self.serial.size()
        self.qmp.type_line(cmd)
        t0 = time.time()
        last = -1
        stable = 0.0
        while time.time() - t0 < timeout:
            time.sleep(0.3)
            if time.time() - t0 < settle:
                continue
            n = self.serial.size()
            if n == last:
                stable += 0.3
                if stable >= quiet:
                    break
            else:
                stable = 0.0
                last = n
        return flat(self.serial.tail_from(before))

    def close(self):
        try:
            self.qmp.quit()
        except Exception:
            pass
        try:
            self.serial.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=15)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 20)


def meta(fs, tag):
    """找根目录里的特殊次级条目 0x81/0x82，返回 (首簇, 数据长度)。"""
    for s in fs.dir_slots(fs.root):
        if s[0] == tag:
            return _u32(s, 20), _u64(s, 24)
    return None


def bmp_blob(fs, first, length):
    """按 FAT 链把位图整条读出来（不是"首簇 + 偏移"那种假设连续的做法）。"""
    out = b""
    for c in fs.chain(first):
        out += fs.cluster(c)
        if len(out) >= length:
            break
    return out[:length]


def bmp_fat_crosscheck(fs):
    """位图位 <-> FAT 项必须一一对应。

    这是独立于任何实现的第三视角：不看内核怎么写，只看盘上两个元数据是不是
    在讲同一件事。位图被截断、写错簇、漏置位，这里都会立刻红。"""
    m = meta(fs, 0x81)
    if m is None:
        return ["no 0x81 allocation bitmap entry"]
    first, length = m
    blob = bmp_blob(fs, first, length)
    need = (fs.cluster_count + 7) // 8
    problems = []
    if length != need:
        problems.append("bitmap DataLength=%d, ceil(cluster_count/8)=%d"
                        % (length, need))
    # 位图自身必须成链：长度够几个簇，链就得有几个簇
    want_clusters = int(math.ceil(length / float(fs.cluster_size)))
    got = fs.chain(first)
    if len(got) != want_clusters:
        problems.append("bitmap chain has %d clusters, needs %d"
                        % (len(got), want_clusters))
    if want_clusters >= 2 and len(got) >= 2 and got[1] != got[0] + 1:
        problems.append("bitmap clusters not chained: %s" % (got[:4],))
    if len(blob) < need:
        problems.append("bitmap blob only %d bytes (truncated)" % len(blob))
        return problems

    def bit(c):
        i = c - 2
        return (blob[i // 8] >> (i % 8)) & 1

    bad = 0
    firstbad = None
    for c in range(2, fs.cluster_count + 2):
        allocated = fs.fat(c) != 0
        if bool(bit(c)) != allocated:
            bad += 1
            if firstbad is None:
                firstbad = c
    if bad:
        problems.append("bitmap/FAT disagree on %d clusters (first: %d)"
                        % (bad, firstbad))
    return problems


def main():
    if not os.path.isfile(IMG):
        print("MISSING os-image.bin - run ninja first")
        return 2
    # 每次都从一块空盘开始：格式化的结果必须可重现。
    with open(WORK, "wb") as f:
        f.truncate(VOL_BYTES)

    results = []
    names = [("F%02d.TXT" % i, "CONTENT%02d" % i) for i in range(10)]

    # ---------------- 第一轮：内核格式化 + 读写 ----------------
    try:
        g = Guest()
    except Exception as e:
        print("boot failed: %s" % e)
        kill_all_qemu()
        return 2
    try:
        g.boot_wait()
        out = g.run("format exfat", 8.0)
        rec(results, "format exfat on %dMB disk" % MB,
            "formatted as exfat" in out.lower(), "| %s" % out[-70:])

        out = g.run("ls", 2.5)
        rec(results, "ls after format (no error)",
            "error" not in out.lower(), "| %s" % out[-70:])

        for n, v in names:
            g.run("write %s %s" % (n, v), 1.2)
        out = g.run("ls", 3.0)
        listed = sum(1 for n, _ in names if n.lower() in out.lower())
        rec(results, "ls shows all 10 files", listed == 10, "| %d/10" % listed)

        ok_read = 0
        for n, v in names:
            if v in g.run("cat " + n, 1.5):
                ok_read += 1
        rec(results, "cat reads back all 10 files", ok_read == 10,
            "| %d/10" % ok_read)

        for n, _ in names[:5]:
            g.run("rm " + n, 1.2)
        ok_read = 0
        for n, v in names[5:]:
            if v in g.run("cat " + n, 1.5):
                ok_read += 1
        rec(results, "remaining 5 files intact after rm", ok_read == 5,
            "| %d/5" % ok_read)

    finally:
        g.close()

    # ---------------- 宿主机侧：几何 + 位图/FAT 自洽 ----------------
    try:
        fs = ref_exfat.Exfat(WORK)
    except Exception as e:
        rec(results, "host ref_exfat parses the image", False, "| %s" % e)
        return 1
    try:
        dev = os.path.getsize(WORK) // 512
        vol_end = fs.part_off // 512 + fs.heap_off + fs.cluster_count * fs.spc
        rec(results, "volume fits inside the device", vol_end <= dev,
            "| ends at sector %d of %d" % (vol_end, dev))
        cover = 100.0 * (fs.cluster_count * fs.cluster_size) / VOL_BYTES
        rec(results, "exFAT covers >=90%% of the %dMB volume" % MB,
            cover >= 90.0, "| %.1f%% (%d clusters x %d B)"
            % (cover, fs.cluster_count, fs.cluster_size))

        m = meta(fs, 0x81)
        rec(results, "root has a 0x81 allocation bitmap entry", m is not None)
        if m:
            bmp_clusters = int(math.ceil(m[1] / float(fs.cluster_size)))
            rec(results, "bitmap spans more than one cluster "
                         "(the case that used to corrupt)", bmp_clusters >= 2,
                "| %d bytes = %d clusters" % (m[1], bmp_clusters))

        problems = bmp_fat_crosscheck(fs)
        rec(results, "bitmap bits agree with FAT after kernel writes",
            not problems,
            "| %s" % ("; ".join(problems[:3]) if problems else "0 problems"))

        rec(results, "ref_exfat.audit_root_meta() clean",
            not fs.audit_root_meta())
        probs = fs.audit()
        rec(results, "ref_exfat.audit() clean", not probs,
            "| %s" % ("; ".join(probs[:3]) if probs else "0 problems"))

        want = {n: v.encode() for n, v in names[5:]}
        match = 0
        for n, v in want.items():
            try:
                if fs.read_path("/" + n).rstrip(b"\x00").strip() == v:
                    match += 1
            except Exception:
                pass
        rec(results, "host ref reads the same 5 files", match == len(want),
            "| %d/%d" % (match, len(want)))

        # 后面要拿宿主机侧的几何去对内核 `df` 的输出
        fs_cluster_count = fs.cluster_count
    except Exception as e:
        # 审计自己炸了也算失败，别让异常把整轮结果吞掉
        rec(results, "host-side audit raised nothing", False, "| %s" % e)
        fs_cluster_count = 0

    # ---------------- 宿主机把盘填过第一个位图簇 ----------------
    try:
        rw = ref_exfat.ExfatRW(WORK)
    except Exception as e:
        rec(results, "host writer opens the image", False, "| %s" % e)
        return 1
    try:
        # 把"前 4300 个簇"在宿主机侧标记成已用（FAT=EOC + 位图置 1），不建真
        # 文件。为什么不用真文件：要让下一个被分配的簇落进位图第二簇，需要
        # 用到簇号 >= 4098，而参考实现的链长上限 MAX_CHAIN=4096，造一个 4200
        # 簇的文件反而会让 audit() 读链时被截断、报出假故障。
        # "已用但没有文件引用"在真盘上也很常见（碎片/异常掉电后的泄漏簇），
        # 而且这正是要考的那条路径：内核分配时必须往位图的**第二个簇**里写。
        marked = 0
        c = 2
        while c <= PREFILL_CLUSTERS and c < rw.cluster_count + 2:
            if rw.fat(c) == 0 and not rw.bmp_get(c):
                rw.set_fat(c, 0xFFFFFFFF)
                rw.bmp_set(c, 1)
                marked += 1
            c += 1
        rw.flush()
        rec(results, "host marks %d clusters used to push past "
                     "bitmap cluster #1" % PREFILL_CLUSTERS, marked > 4000,
            "| marked %d clusters" % marked)
        # 关键前提：下一个被分配的簇，其位图字节必须落在**第二个**位图簇里。
        rec(results, "next allocation lands in bitmap cluster #2",
            (PREFILL_CLUSTERS + 1 - 2) // 8 >= rw.cluster_size,
            "| byte index %d >= %d"
            % ((PREFILL_CLUSTERS + 1 - 2) // 8, rw.cluster_size))
    except Exception as e:
        rec(results, "host writer prefills the disk", False, "| %s" % e)
        return 1

    # ---------------- 第二轮：内核在"别人的盘"上再分配 ----------------
    try:
        g = Guest()
    except Exception as e:
        print("second boot failed: %s" % e)
        kill_all_qemu()
        return 2
    try:
        g.boot_wait()
        out = g.run("ls", 3.0)
        rec(results, "kernel still lists the volume after host prefill",
            "error" not in out.lower(), "| %s" % out[-70:])
        ok_seen = sum(1 for n, _ in names[5:] if n.lower() in out.lower())
        rec(results, "the 5 files are still there", ok_seen == 5,
            "| %d/5" % ok_seen)

        # df：两个断言都在真量东西——总量必须覆盖整块盘（卷写死小了这里立刻
        # 红），耗时必须短（大卷上曾经因为"每读一个 FAT 项都真读一次盘"要
        # 20 秒以上，小卷完全看不出来）。
        t0 = time.time()
        out = g.run("df", 1.0, quiet=2.0)
        dt = time.time() - t0
        m = re.search(r"(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%\s+/\s+\((\d+)\s+clusters",
                      out)
        want_kb = MB * 1024
        if not m:
            rec(results, "df prints a usable line", False, "| %s" % out[-80:])
        else:
            total_kb = int(m.group(1))
            rec(results, "df total covers the whole %dMB disk" % MB,
                0.85 * want_kb <= total_kb <= want_kb,
                "| %d KB of ~%d KB" % (total_kb, want_kb))
            rec(results, "df cluster count matches the host-side geometry",
                abs(int(m.group(5)) - fs_cluster_count) <= 2,
                "| guest=%s host=%d" % (m.group(5), fs_cluster_count))
            # 宿主机预填的那 4300 个簇，内核的空闲统计必须看得到
            used_kb = int(m.group(2))
            rec(results, "df sees the host-prefilled clusters as used",
                used_kb >= 1500, "| used %d KB (~%d clusters)"
                % (used_kb, PREFILL_CLUSTERS // 2))
        rec(results, "df finishes in <15s (was >20s before the FAT cache)",
            dt < 15.0, "| %.1fs" % dt)

        out = g.run("write NEW.TXT hello", 2.5)
        rec(results, "write NEW.TXT succeeds after host prefill",
            "error" not in out.lower(), "| %s" % out[-70:])
        out = g.run("cat NEW.TXT", 1.5)
        rec(results, "cat NEW.TXT reads back", "hello" in out.lower(),
            "| %s" % out[-70:])
        ok_read = 0
        for n, v in names[5:]:
            if v in g.run("cat " + n, 1.5):
                ok_read += 1
        rec(results, "pre-existing 5 files still readable", ok_read == 5,
            "| %d/5" % ok_read)
    finally:
        g.close()

    # ---------------- 宿主机复核：位图写对簇了吗 ----------------
    try:
        fs = ref_exfat.Exfat(WORK)
    except Exception as e:
        rec(results, "host re-parses after kernel 2nd pass", False, "| %s" % e)
        return 1
    try:
        try:
            ent = fs.lookup(fs.root, "NEW.TXT")
            new_cluster = ent["first_cluster"]
        except Exception as e:
            new_cluster = 0
            rec(results, "host finds NEW.TXT", False, "| %s" % e)
        if new_cluster:
            rec(results, "NEW.TXT allocated past bitmap cluster #1",
                (new_cluster - 2) // 8 >= fs.cluster_size,
                "| cluster %d -> bitmap byte %d" % (new_cluster,
                                                    (new_cluster - 2) // 8))
        problems = bmp_fat_crosscheck(fs)
        rec(results, "bitmap bits still agree with FAT after 2nd pass",
            not problems,
            "| %s" % ("; ".join(problems[:3]) if problems else "0 problems"))
        probs = fs.audit()
        rec(results, "ref_exfat.audit() clean after 2nd pass", not probs,
            "| %s" % ("; ".join(probs[:3]) if probs else "0 problems"))
    except Exception as e:
        rec(results, "host-side re-audit raised nothing", False, "| %s" % e)

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
