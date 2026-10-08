# -*- coding: utf-8 -*-
"""test_hostfs.py - 宿主机与内核**双向互操作**对拍（exFAT）。

与 test_fsref.py 的分工：fsref 是"内核写的盘宿主读得懂吗 / 宿主建的盘内核读得懂吗"
（单向、一次性）；这里更进一步——**同一张卷上你来我往**：

  A. 宿主建 -> 内核读（端口 4571/4572）
     宿主机写入器（tests/ref_exfat.py ExfatRW，第二个独立实现）在真实盘上建：
     普通文件、子目录 + 子目录内文件、22 字符长名（跨 2 个 0xC1 条目）、
     1100 字节 3 簇 FAT 链文件。内核 ls / cat 逐个比对。

  B. 宿主建 -> 内核**追加** -> 宿主复检（端口 4573/4574）
     关键在"追加"：内核往别人建的卷里写新文件、建新目录，宿主侧要验证
     **原有文件一个字节都没变**，且位图 / FAT / entry set 校验和仍然自洽。
     真实场景就是 Windows 往 VHD 里拖文件、EZOS 再往里写东西。

  C. 宿主建 -> 内核**删除** -> 宿主复检（端口 4575/4576）
     删除不仅要"条目消失"，还要簇真的还回位图与 FAT（否则盘会慢性漏空间），
     同时相邻文件不能被误伤。

所有期望值都由宿主机侧现算，不写死。
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))

QEMU = qemu_exe()
IMG = image_path()
DISK = disk_path()
WORK_A = os.path.join(HERE, "hostfs_a.vhd").replace("\\", "/")
WORK_B = os.path.join(HERE, "hostfs_b.vhd").replace("\\", "/")
WORK_C = os.path.join(HERE, "hostfs_c.vhd").replace("\\", "/")

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa
from ref_exfat import Exfat, ExfatRW, ExfatError  # noqa

# 1100 字节 = 3 簇（簇 512B），必须打印字符：cat 把非打印字符替换成 '.'
BIG_A = ("".join("Hx%dQ" % (i % 10) for i in range(400)))[:1100]
assert len(BIG_A) == 1100
BIG_B = ("".join("PrE%dZ" % (i % 10) for i in range(400)))[:900]
assert len(BIG_B) == 900


def boot(disk, qmp_port, ser_port):
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + disk,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % ser_port,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def open_guest(disk, qmp_port, ser_port):
    proc = boot(disk, qmp_port, ser_port)
    time.sleep(1.0)
    serial = SerialReader(ser_port)
    qmp = Qmp(qmp_port)
    wait_for(serial, "TASK: preemptive", 150)
    time.sleep(3.0)
    return proc, serial, qmp


def close_guest(proc, serial, qmp, qmp_port):
    try:
        qmp.quit()
    except Exception:
        pass
    serial.close()
    try:
        proc.wait(timeout=10)
    except Exception:
        kill_all_qemu()
    wait_port_free(qmp_port, 15)


def guest_cmds(qmp, serial, cmds):
    out = {}
    for cmd, settle in cmds:
        before = serial.size()
        qmp.type_line(cmd)
        time.sleep(settle)
        out[cmd] = serial.tail_from(before)
    return out


def flat(s):
    return " ".join(s.split())


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def case_a(results):
    """宿主建 -> 内核读。"""
    shutil.copyfile(DISK, WORK_A)
    w = ExfatRW(WORK_A)
    w.create(w.root, "HOST.TXT", b"HOSTWROTE123")
    w.create(w.root, "HOSTDIR", b"", is_dir=True)
    sub = [e for e in w.parse_dir(w.root) if e['name'] == 'HOSTDIR'][0]
    w.create(sub['first_cluster'], "DEEP.TXT", b"DEEPDATA")
    w.create(w.root, "MULTIENTRYFILENAME.TXT", b"LONGNAMEOK")
    w.create(w.root, "BIGHOST.BIN", BIG_A.encode('ascii'))
    # 显式构造"entry set 跨簇边界"的形状：exFAT 里 entry set 就是目录字节流里
    # 连续的一段，Windows 不会为它避开簇边界。内核以前逐簇解析目录，这种条目
    # **ls 看得到、cat/ls/cd 全部进不去**（看得见摸不着）。先用填充文件把下一个
    # 空闲槽顶到簇尾，再建目标文件，并硬性确认它真的跨了界——不确认的话这个
    # 用例可能悄悄退化成普通用例（测试骗自己比没测试更糟）。
    spc_slots = w.cluster_size // 32

    def next_free_slot():
        for i, (c, off) in enumerate(w.slot_offsets(w.root)):
            if w._slot_byte(c, off) == 0x00:
                return i
        return len(w.slot_offsets(w.root))

    guard = 0
    while next_free_slot() % spc_slots != spc_slots - 2 and guard < 12:
        w.create(w.root, "FILL%02d" % guard, b"filler")
        guard += 1
    start_slot = next_free_slot()
    w.create(w.root, "STRADDLE.TXT", b"ACROSSBOUNDARY")
    w.flush()
    rec(results, "host: produced entry set straddling a cluster boundary",
        start_slot // spc_slots != (start_slot + 2) // spc_slots,
        "| start slot %d, %d slots/cluster" % (start_slot, spc_slots))

    fs = Exfat(WORK_A)
    rec(results, "host: writer output self-consistent",
        not fs.audit(), "| %s" % fs.audit()[:3])

    proc, serial, qmp = open_guest(WORK_A, 4571, 4572)
    try:
        out = guest_cmds(qmp, serial, [
            ("ls", 5.0),
            ("cat HOST.TXT", 5.0),
            ("cat HOSTDIR/DEEP.TXT", 5.0),
            ("cat MULTIENTRYFILENAME.TXT", 5.0),
            ("cat STRADDLE.TXT", 5.0),
            ("cat BIGHOST.BIN", 8.0),
            ("ls HOSTDIR", 5.0),
        ])
    finally:
        close_guest(proc, serial, qmp, 4571)

    ls = flat(out["ls"]).upper()
    rec(results, "kernel: ls sees host file", "HOST.TXT" in ls,
        "| %r" % flat(out["ls"])[:90])
    rec(results, "kernel: ls sees host dir", "HOSTDIR" in ls)
    rec(results, "kernel: ls sees 22-char name", "MULTIENTRYFILENAME.TXT" in ls)
    rec(results, "kernel: ls sees 3-cluster file", "BIGHOST.BIN" in ls)
    rec(results, "kernel: cat host short file",
        "HOSTWROTE123" in flat(out["cat HOST.TXT"]),
        "| %r" % flat(out["cat HOST.TXT"])[:60])
    rec(results, "kernel: cat host subdir file",
        "DEEPDATA" in flat(out["cat HOSTDIR/DEEP.TXT"]),
        "| %r" % flat(out["cat HOSTDIR/DEEP.TXT"])[:60])
    rec(results, "kernel: cat host long-name file",
        "LONGNAMEOK" in flat(out["cat MULTIENTRYFILENAME.TXT"]),
        "| %r" % flat(out["cat MULTIENTRYFILENAME.TXT"])[:60])
    got_big = flat(out["cat BIGHOST.BIN"]).replace(" ", "")
    # 跨簇 entry set：这一条以前必挂（"看得见、进不去"）
    rec(results, "kernel: cat file whose entry set straddles clusters",
        "ACROSSBOUNDARY" in flat(out["cat STRADDLE.TXT"]),
        "| %r" % flat(out["cat STRADDLE.TXT"])[:70])
    rec(results, "kernel: cat host multi-cluster file byte-exact",
        BIG_A in got_big, "| len=%d" % len(got_big))
    rec(results, "kernel: ls HOSTDIR lists DEEP.TXT",
        "DEEP.TXT" in flat(out["ls HOSTDIR"]).upper(),
        "| %r" % flat(out["ls HOSTDIR"])[:60])


def case_b(results):
    """宿主建 -> 内核追加 -> 宿主复检：原有内容必须一个字节不变。"""
    shutil.copyfile(DISK, WORK_B)
    w = ExfatRW(WORK_B)
    w.create(w.root, "PRE.TXT", BIG_B.encode('ascii'))
    pre_clusters = w.chain(w.lookup(w.root, "PRE.TXT")['first_cluster'])
    w.flush()
    before = Exfat(WORK_B)
    pre_before = before.read(before.lookup(before.root, "PRE.TXT"))
    rec(results, "host: fixture written", pre_before == BIG_B.encode('ascii'),
        "| %d bytes" % len(pre_before))

    proc, serial, qmp = open_guest(WORK_B, 4573, 4574)
    try:
        out = guest_cmds(qmp, serial, [
            ("write NEWK KERNELDATA", 6.0),
            ("mkdir KDIR", 5.0),
            ("write KDIR/K2 XYZ", 6.0),
            ("ls", 5.0),
        ])
    finally:
        close_guest(proc, serial, qmp, 4573)

    rec(results, "kernel: wrote into host-created volume",
        "file written successfully" in out["write NEWK KERNELDATA"].lower(),
        "| %r" % flat(out["write NEWK KERNELDATA"])[:70])

    fs = Exfat(WORK_B)
    rec(results, "host: volume still parses", True)

    pre = fs.lookup(fs.root, "PRE.TXT")
    rec(results, "host: pre-existing file byte-identical after kernel wrote",
        pre is not None and fs.read(pre) == BIG_B.encode('ascii'),
        "| %s" % ("missing" if pre is None else "%d bytes" % pre['size']))
    rec(results, "host: pre-existing file kept its clusters",
        pre is not None and fs.chain(pre['first_cluster']) == pre_clusters,
        "| %s" % (fs.chain(pre['first_cluster']) if pre else None))

    newk = fs.lookup(fs.root, "NEWK")
    rec(results, "host: sees kernel-created file",
        newk is not None and fs.read(newk).rstrip() == b"KERNELDATA",
        "| %r" % (fs.read(newk)[:20] if newk else None))

    kdir = fs.lookup(fs.root, "KDIR")
    rec(results, "host: sees kernel-created dir", kdir is not None and kdir['is_dir'])
    if kdir is not None and kdir['is_dir']:
        k2 = fs.lookup(kdir['first_cluster'], "K2")
        rec(results, "host: sees file inside kernel-created dir",
            k2 is not None and fs.read(k2).rstrip() == b"XYZ",
            "| %r" % (fs.read(k2)[:20] if k2 else None))

    problems = fs.audit()
    rec(results, "host: whole volume self-consistent after kernel write",
        not problems, "| %s" % problems[:3])
    rec(results, "host: root meta still mountable",
        not fs.audit_root_meta(), "| %s" % fs.audit_root_meta()[:2])


def case_c(results):
    """宿主建 -> 内核删 -> 宿主复检：条目消失 + 簇真的还回去。"""
    shutil.copyfile(DISK, WORK_C)
    w = ExfatRW(WORK_C)
    w.create(w.root, "KEEP.TXT", b"KEEPTHIS")
    w.create(w.root, "DELME.BIN", BIG_B.encode('ascii'))
    w.flush()
    fs0 = Exfat(WORK_C)
    doomed = fs0.lookup(fs0.root, "DELME.BIN")
    clusters = fs0.chain(doomed['first_cluster'])
    rec(results, "host: doomed file spans multiple clusters", len(clusters) >= 2,
        "| %s" % clusters)

    proc, serial, qmp = open_guest(WORK_C, 4575, 4576)
    try:
        out = guest_cmds(qmp, serial, [
            ("rm DELME.BIN", 6.0),
            ("ls", 5.0),
            ("cat KEEP.TXT", 5.0),
        ])
    finally:
        close_guest(proc, serial, qmp, 4575)

    rec(results, "kernel: rm succeeded",
        ("deleted" in out["rm DELME.BIN"].lower()
         or "success" in out["rm DELME.BIN"].lower()),
        "| %r" % flat(out["rm DELME.BIN"])[:70])
    rec(results, "kernel: ls no longer lists deleted file",
        "DELME.BIN" not in flat(out["ls"]).upper(),
        "| %r" % flat(out["ls"])[:90])

    fs = Exfat(WORK_C)
    rec(results, "host: deleted entry gone",
        fs.lookup(fs.root, "DELME.BIN") is None)

    rw = ExfatRW(WORK_C)
    still = [c for c in clusters if rw.bmp_get(c) or rw.fat(c) != 0]
    rec(results, "host: deleted file's clusters returned to bitmap+FAT",
        not still, "| still marked used: %s" % still)

    keep = fs.lookup(fs.root, "KEEP.TXT")
    rec(results, "host: neighbour file unharmed",
        keep is not None and fs.read(keep).rstrip() == b"KEEPTHIS",
        "| %r" % (fs.read(keep)[:20] if keep else None))
    rec(results, "host: volume self-consistent after delete",
        not fs.audit(), "| %s" % fs.audit()[:3])


def main():
    results = []
    case_a(results)
    case_b(results)
    case_c(results)
    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    for n in bad:
        print("  FAILED: " + n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
