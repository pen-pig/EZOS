# -*- coding: utf-8 -*-
"""probe_ext4.py - 最小复现：format ext4 -> write -> ls -> cat，全量打印输出。"""
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
WORK = os.path.join(HERE, "probe_ext4.img").replace("\\", "/")
QMP_PORT, SER_PORT = 4611, 4612
VOL = 16 * 1024 * 1024

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa


def main():
    with open(WORK, "wb") as f:
        f.truncate(VOL)
    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + WORK,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SER_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    serial = SerialReader(SER_PORT)
    qmp = Qmp(QMP_PORT)
    try:
        wait_for(serial, "TASK: preemptive", 150)
        time.sleep(3.0)

        def run(cmd, settle=3.0):
            before = serial.size()
            qmp.type_line(cmd)
            time.sleep(settle)
            t = serial.tail_from(before)
            print("--- %r ---" % cmd)
            print(repr(t))
            return " ".join(t.split())

        run("format ext4", 10.0)
        run("df", 3.0)
        run("ls", 3.0)
        run("write A.TXT HELLO", 4.0)
        run("ls", 3.0)
        run("cat A.TXT", 3.0)
        run("write B.TXT WORLD", 4.0)
        run("ls", 3.0)
    finally:
        try:
            qmp.quit()
        except Exception:
            pass
        serial.close()
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 15)


if __name__ == "__main__":
    main()
