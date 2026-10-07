# -*- coding: utf-8 -*-
"""test_ext4vol.py - ext4 卷必须吃满整块盘（多块组）并与宿主机参考实现对拍。

为什么要它：ext4_format 早期把卷大小写死成 1024 块 = 1MB。在 16MB 的数据盘
上格式化完只剩 6% 可用，剩下 94% 的扇区永远摸不到——而"内核自己读自己写的
盘"永远是绿的，所以这里必须用**宿主机独立实现**（tests/ref_ext4.py，只读、
按 spec 解析、与 kernel/ext4.c 零共享）来量：文件系统到底覆盖了卷的百分之几。

顺带把"块组边界"这条也钉死：组边界是 first_data_block + g*bpg（Linux
ext4_group_first_block_no 的定义），含 1KB 块时的 +1 偏移。按 g*bpg 算会差
一个块，内核自己完全看不出来，但位图/空闲计数在宿主机侧立刻对不上。

端口 4601(QMP) / 4602(serial)。
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
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe  # noqa: E402

QEMU = qemu_exe()
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
WORK = os.path.join(HERE, "ext4vol_disk.img").replace("\\", "/")
QMP_PORT, SER_PORT = 4601, 4602

# 16MB：数据盘的常规尺寸，正好跨 2 个块组（1KB 块 x 8192 块/组 = 8MB/组）
VOL_BYTES = 16 * 1024 * 1024

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa
import ref_ext4  # noqa: E402


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def flat(s):
    return " ".join(s.split())


def main():
    if not os.path.isfile(IMG):
        print("MISSING os-image.bin - run ninja first")
        return 2
    # 每次都从头造一块空盘：格式化的结果必须是**可重现**的，
    # 拿一块被历史测试写脏的盘来测等于什么都没测。
    with open(WORK, "wb") as f:
        f.truncate(VOL_BYTES)

    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + WORK,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SER_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        serial = SerialReader(SER_PORT)
        qmp = Qmp(QMP_PORT)
    except Exception as e:
        print("boot failed: %s" % e)
        kill_all_qemu()
        return 2
    results = []
    try:
        wait_for(serial, "TASK: preemptive", 150)
        time.sleep(3.0)

        def run(cmd, settle=2.5):
            before = serial.size()
            qmp.type_line(cmd)
            time.sleep(settle)
            return flat(serial.tail_from(before))

        # 1) 格式化
        out = run("format ext4", 8.0)
        rec(results, "format ext4 on 16MB disk",
            "Disk formatted as ext4" in out, "| %s" % out[-70:])

        # 2) 根目录可读
        out = run("ls", 2.5)
        rec(results, "ls after format (no error)",
            "error" not in out.lower(), "| %s" % out[-70:])

        # 3) 写文件并读回（跨组也要能写：这里写到 20 个文件）
        names = []
        for i in range(20):
            n = "F%02D.TXT" % i if False else ("F%02d.TXT" % i)
            names.append((n, "CONTENT%02d" % i))
        for n, v in names:
            run("write %s %s" % (n, v), 1.2)
        out = run("ls", 3.0)
        listed = sum(1 for n, _ in names if n.lower() in out.lower())
        rec(results, "ls shows all 20 files", listed == 20,
            "| %d/20" % listed)

        ok_read = 0
        for n, v in names:
            out = run("cat " + n, 1.5)
            if v in out:
                ok_read += 1
        rec(results, "cat reads back all 20 files", ok_read == 20,
            "| %d/20" % ok_read)

        # 4) 删一半再读：释放路径必须把块还回**正确的组**
        for n, _ in names[:10]:
            run("rm " + n, 1.2)
        out = run("ls", 3.0)
        gone = sum(1 for n, _ in names[:10] if n.lower() not in out.lower())
        rec(results, "rm removes 10 files", gone == 10, "| %d/10" % gone)
        ok_read = 0
        for n, v in names[10:]:
            out = run("cat " + n, 1.5)
            if v in out:
                ok_read += 1
        rec(results, "remaining 10 files intact after rm", ok_read == 10,
            "| %d/10" % ok_read)
    finally:
        try:
            qmp.quit()
        except Exception:
            pass
        try:
            serial.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 20)

    # ---------- 宿主机侧独立审计 ----------
    try:
        fs = ref_ext4.Ext4(WORK)
    except Exception as e:
        rec(results, "host ref_ext4 parses the image", False, "| %s" % e)
        return 1
    try:
        vol = fs.volume_bytes()
        cover = 100.0 * fs.fs_bytes / vol if vol else 0.0
        rec(results, "ext4 covers >=95%% of the 16MB volume "
                     "(was 6%% with hardcoded 1MB)", cover >= 95.0,
            "| %d B of %d B = %.1f%%" % (fs.fs_bytes, vol, cover))

        rec(results, "uses more than one block group", fs.groups >= 2,
            "| groups=%d blocks=%d" % (fs.groups, fs.blocks_total))

        problems = fs.audit()
        rec(results, "ref_ext4.audit() clean", not problems,
            "| %s" % ("; ".join(problems[:3]) if problems else "0 problems"))

        # 内核写的目录项，宿主机必须能独立读出来（双向对拍）
        got = {}
        try:
            for ent in fs.dir_entries(2):
                name = ent["name"]
                if name in (".", ".."):
                    continue
                ino = ent["ino"]
                if ino:
                    got[name] = fs.read_inode(ino).rstrip(b"\x00")
        except Exception as e:
            rec(results, "host reads root dir", False, "| %s" % e)
            got = {}
        want = {n: v.encode() for n, v in names[10:]}
        match = sum(1 for n, v in want.items()
                    if got.get(n, b"").strip() == v)
        rec(results, "host ref reads the same 10 files",
            match == len(want), "| %d/%d" % (match, len(want)))
    finally:
        fs.close()

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
