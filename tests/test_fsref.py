# -*- coding: utf-8 -*-
"""test_fsref.py - 用宿主机独立 exFAT 实现与内核逐字节对拍。

内核自己读自己写的盘，永远是绿的——它俩共享同一套（可能错的）理解。这个测试
引入第三方视角：`tests/ref_exfat.py` 只按 exFAT 规范解析镜像，与 kernel/exfat.c
零共享代码。两个方向各跑一个独立 QEMU：

  A. **内核写 -> 宿主机读**（端口 4565/4566）
     内核 format + write/mkdir，退出后用参考实现把卷重新解析一遍：文件名字、
     目录层级、**文件内容逐字节**、跨簇文件的 FAT 链形态、每个 entry set 的
     SetChecksum 与 NameHash 是否与规范自洽。
     这条覆盖"我们写出去的盘，Windows 读得出来吗"。

  B. **宿主机写 -> 内核读**（端口 4567/4568）
     `temp/gen_diskimg.py` 按规范造的盘（含 23 字符长名文件，跨多个 0xC1 项）
     挂给内核，用 ls / cat 对拍宿主机侧解析出的同一份内容。
     这条覆盖"别人写的盘，我们读得出来吗"。

B 方向的期望值不写死在断言里，而是在内核跑之前先用参考实现读出真实内容，
再拿去和内核 cat 的输出比——避免"两边都错但错得一样"以外的另一种假绿：
期望值过时。
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
WORK_A = os.path.join(HERE, "fsref_a.vhd").replace("\\", "/")
WORK_B = os.path.join(HERE, "fsref_b.vhd").replace("\\", "/")

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa
from ref_exfat import Exfat, ExfatError  # noqa

# 600 字节：> 一簇（512），必然走多簇 FAT 链而不是 NoFatChain 单簇路径。
# 不含空格（write 按空格切参数），大小写混合以便抓出"该大写却小写"的哈希。
# 先拼 200 段再截断——直接 range(100) 只有 500 字节，恰好塞进一簇，多簇
# 路径就没被测到（测试自己骗自己，比没有测试更糟）。
PAYLOAD = ("".join("EzOs%d" % (i % 10) for i in range(200)))[:600]
assert len(PAYLOAD) == 600 and " " not in PAYLOAD


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
    """[(cmd, settle)] -> {cmd: 该条命令之后新增的串口输出}"""
    out = {}
    for cmd, settle in cmds:
        before = serial.size()
        qmp.type_line(cmd)
        time.sleep(settle)
        out[cmd] = serial.tail_from(before)
    return out


def flat(s):
    return " ".join(s.split())


def case_a(results):
    """内核写 -> 宿主机独立解析对拍。"""
    def rec(name, ok, detail=""):
        results.append((name, ok))
        print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))

    shutil.copyfile(DISK, WORK_A)
    proc, serial, qmp = open_guest(WORK_A, 4565, 4566)
    try:
        out = guest_cmds(qmp, serial, [
            ("format exfat", 12.0),
            ("write R1 HELLO", 4.0),
            ("mkdir SUB", 4.0),
            ("write SUB/IN1 INNERDATA", 4.0),
            ("write BIG " + PAYLOAD, 35.0),
            ("ls", 5.0),
            ("cat R1", 5.0),
        ])
        rec("kernel: format exfat",
            "disk formatted as" in out["format exfat"].lower())
        rec("kernel: write short file", "file written successfully"
            in out["write R1 HELLO"].lower())
        rec("kernel: write into subdir", "file written successfully"
            in out["write SUB/IN1 INNERDATA"].lower())
        rec("kernel: write multi-cluster file", "file written successfully"
            in out["write BIG " + PAYLOAD].lower())
        rec("kernel: cat sees its own write", "HELLO" in out["cat R1"],
            "| got %r" % flat(out["cat R1"])[:60])
    finally:
        close_guest(proc, serial, qmp, 4565)

    # ---- 宿主机侧独立解析 ----
    try:
        fs = Exfat(WORK_A)
    except ExfatError as exc:
        rec("host: volume parses", False, "| %s" % exc)
        return
    rec("host: volume parses", True,
        "| cluster=%dB root=%d" % (fs.cluster_size, fs.root))

    root = fs.parse_dir(fs.root)
    names = sorted(e['name'].upper() for e in root
                   if not e['is_dir'] or e['name'].upper() != 'SUB')
    rec("host: sees all three objects",
        {'R1', 'BIG', 'SUB'} <= set(e['name'].upper() for e in root),
        "| root=%s" % [e['name'] for e in root])

    e1 = fs.lookup(fs.root, 'R1')
    rec("host: short file content",
        e1 is not None and fs.read(e1).rstrip() == b'HELLO',
        "| %r" % (fs.read(e1) if e1 else None))

    sub = fs.lookup(fs.root, 'SUB')
    rec("host: SUB is a directory", sub is not None and sub['is_dir'])
    if sub is not None and sub['is_dir']:
        inner = fs.lookup(sub['first_cluster'], 'IN1')
        rec("host: subdir file content",
            inner is not None and fs.read(inner).rstrip() == b'INNERDATA',
            "| %r" % (fs.read(inner) if inner else None))

    big = fs.lookup(fs.root, 'BIG')
    ok_big = False
    detail = "| not found"
    if big is not None:
        nclus = len(fs.chain(big['first_cluster']))
        data = fs.read(big)
        ok_big = (data == PAYLOAD.encode('ascii')
                  and len(PAYLOAD) > fs.cluster_size
                  and nclus >= 2
                  and not big['no_fat_chain'])
        detail = "| size=%d clusters=%d no_fat_chain=%d match=%s" % (
            len(data), nclus, big['no_fat_chain'], data == PAYLOAD.encode('ascii'))
    # 逐字节：这是本测试的核心断言
    rec("host: multi-cluster file byte-exact", ok_big, detail)

    problems = fs.audit()
    rec("host: entry sets self-consistent (checksum+namehash)",
        not problems, "| %s" % problems[:3])


def case_b(results):
    """宿主机造的盘 -> 内核读，期望值由参考实现现取。"""
    def rec(name, ok, detail=""):
        results.append((name, ok))
        print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))

    shutil.copyfile(DISK, WORK_B)
    try:
        fs = Exfat(WORK_B)
    except ExfatError as exc:
        rec("host: fixture disk parses", False, "| %s" % exc)
        return
    root = fs.parse_dir(fs.root)
    files = [e for e in root if not e['is_dir'] and e['size'] > 0]
    if not files:
        rec("host: fixture has files", False, "| %s" % [e['name'] for e in root])
        return
    rec("host: fixture has files", True, "| %s" % [e['name'] for e in root])
    rec("host: fixture self-consistent", not fs.audit(), "| %s" % fs.audit()[:3])

    target = files[0]
    want = fs.read(target).decode('latin1')
    first_line = want.splitlines()[0].strip() if want.splitlines() else ""

    proc, serial, qmp = open_guest(WORK_B, 4567, 4568)
    try:
        out = guest_cmds(qmp, serial, [
            ("ls", 5.0),
            ("cat " + target['name'], 6.0),
        ])
        ls_out = flat(out["ls"])
        rec("kernel: ls lists host-written file",
            target['name'].upper() in ls_out.upper(),
            "| %r" % ls_out[:80])
        cat_out = flat(out["cat " + target['name']])
        rec("kernel: cat matches host-read content",
            first_line and first_line.replace(" ", "") in cat_out.replace(" ", ""),
            "| want %r got %r" % (first_line[:40], cat_out[:60]))
        # 长名文件（跨多个 0xC1 条目）至少要在 ls 里出现
        longs = [e['name'] for e in files if len(e['name']) > 15]
        if longs:
            rec("kernel: ls shows multi-entry long name",
                longs[0].upper() in ls_out.upper(), "| %r" % longs[0])
    finally:
        close_guest(proc, serial, qmp, 4567)


def main():
    results = []
    case_a(results)
    case_b(results)
    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    for n in bad:
        print("  FAILED: " + n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
