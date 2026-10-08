# -*- coding: utf-8 -*-
"""test_f2fsvol.py - f2fs 卷必须吃满整块盘，且元数据区随卷大小伸缩。

为什么要它：f2fs_format 早期把卷大小写死成 4095 块（32767 扇区 ≈ 16MB）。
在更大的盘上格式化，多出来的扇区永远摸不到。现在卷块数由 ata_capacity()
实测推导，元数据段数（CP/SIT/NAT/SSA）按 main 段数迭代求不动点——
SIT/NAT/SSA 都是按 **main 相对段号** 索引的，段数涨了它们也得跟着涨，
否则 segno/55 会越界写穿到别的元数据区。

端口 4621(QMP) / 4622(serial)。

用法：python tests/test_f2fsvol.py [MB]     默认 64MB
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import os
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
WORK = os.path.join(HERE, "f2fsvol_disk.img").replace("\\", "/")
QMP_PORT, SER_PORT = alloc_port(2)

MB = int(sys.argv[1]) if len(sys.argv) > 1 else 64
VOL_BYTES = MB * 1024 * 1024

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def flat(s):
    return " ".join(s.split())


def main():
    if not os.path.isfile(IMG):
        print("MISSING os-image.bin - run ninja first")
        return 2
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
        # 等真正的 shell 提示符，别用固定 sleep：机器忙时 3 秒不一定够，
        # 第一条命令会被喂进还没就绪的 shell。
        wait_for(serial, "[/] >", 120)
        time.sleep(1.0)

        def run(cmd, settle=2.5, quiet=1.5, timeout=180.0):
            """键入一行命令，先等 settle 秒下限，再等串口静默 quiet 秒。

            只死等固定时长是不够的：格式化这类命令在慢盘/慢机器上会远超
            预期时长，后面的命令被敲进去就会和它交错执行，症状是内核
            panic（看起来像内存破坏，极具误导性）。"""
            before = serial.size()
            qmp.type_line(cmd)
            t0 = time.time()
            last = -1
            stable = 0.0
            while time.time() - t0 < timeout:
                time.sleep(0.3)
                if time.time() - t0 < settle:
                    continue
                n = serial.size()
                if n == last:
                    stable += 0.3
                    if stable >= quiet:
                        break
                else:
                    stable = 0.0
                    last = n
            return flat(serial.tail_from(before))

        out = run("format f2fs", 20.0)
        rec(results, "format f2fs on %dMB disk" % MB,
            "disk formatted as f2fs" in out.lower(), "| %s" % out[-70:])

        out = run("ls", 2.5)
        rec(results, "ls after format (no error)",
            "error" not in out.lower(), "| %s" % out[-70:])

        # 容量覆盖：df 的 1K-blocks 总数必须接近整盘（原来写死 16MB）
        out = run("df", 2.5)
        total_k = 0
        for tok in out.split():
            if tok.isdigit():
                total_k = int(tok)
                break
        want_k = VOL_BYTES // 1024
        cover = 100.0 * total_k / want_k if want_k else 0.0
        rec(results, "f2fs main area covers >=85%% of the %dMB volume" % MB,
            cover >= 85.0, "| %d KB of %d KB = %.1f%%" % (total_k, want_k, cover))

        names = [("F%02d.TXT" % i, "CONTENT%02d" % i) for i in range(20)]
        for n, v in names:
            run("write %s %s" % (n, v), 1.5)
        out = run("ls", 3.0)
        listed = sum(1 for n, _ in names if n.lower() in out.lower())
        rec(results, "ls shows all 20 files", listed == 20, "| %d/20" % listed)

        ok_read = 0
        for n, v in names:
            out = run("cat " + n, 1.8)
            if v in out:
                ok_read += 1
        rec(results, "cat reads back all 20 files", ok_read == 20,
            "| %d/20" % ok_read)

        for n, _ in names[:10]:
            run("rm " + n, 1.5)
        out = run("ls", 3.0)
        gone = sum(1 for n, _ in names[:10] if n.lower() not in out.lower())
        rec(results, "rm removes 10 files", gone == 10, "| %d/10" % gone)
        ok_read = 0
        for n, v in names[10:]:
            out = run("cat " + n, 1.8)
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

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
