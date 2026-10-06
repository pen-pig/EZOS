# -*- coding: utf-8 -*-
"""test_path.py - 路径解析守护测试（跨目录相对路径 / 系统卷 cwd）。

覆盖两条曾经真实存在的缺陷：

  A. 数据盘侧：exFAT 后端只认单层名，`write SD/f2 hi` 会在**当前目录**里
     造出一个名字就叫 "SD/f2" 的目录项——ls 看得见、任何合法路径都读不到、
     还占簇，是静默的数据损坏。现在必须：父目录存在才写进去，不存在则
     直接失败（fail closed），根目录列表里不许出现含 '/' 的项。
  B. 系统卷侧：`cd /system` 之后 `cat version` 必须能读到，不能只有
     绝对路径 `/system/version` 才行（cwd 已下沉到 fs 层）。

端口 4515(QMP) / 4516(serial)。断言文本均已由探针核实。
"""
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))

QEMU = qemu_exe()
QMP_PORT = 4515
SERIAL_PORT = 4516
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
WORK = os.path.join(HERE, "path_disk.vhd").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', ':': 'shift-semicolon'}

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa


def boot():
    shutil.copyfile(DISK, WORK)
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + WORK,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_cmd(qmp, serial, cmd, settle=3.0):
    """敲一条命令，返回它产生的新输出。"""
    before = serial.size()
    qmp.type_line(cmd)
    time.sleep(settle)
    return serial.tail_from(before)


def main():
    proc = boot()
    time.sleep(1.0)
    serial = SerialReader(SERIAL_PORT)
    qmp = Qmp(QMP_PORT)
    results = []

    def check(name, cmd, expect, negate=False, settle=3.0):
        out = run_cmd(qmp, serial, cmd, settle)
        hit = expect in out
        ok = (not hit) if negate else hit
        results.append((name, ok))
        print("%s %s\n    cmd=%r expect%s %r\n    got: %s"
              % ("PASS" if ok else "FAIL", name, cmd,
                 " NOT" if negate else "", expect,
                 " | ".join(l.strip() for l in out.splitlines()[:4])))
        return out

    try:
        wait_for(serial, "TASK: preemptive", 150)
        time.sleep(3.0)

        # ---- A. exFAT 跨目录路径 ----
        check("mkdir SD", "mkdir SD", "")
        check("write into subdir", "write SD/a hello", "File written successfully.")
        check("read back via path", "cat SD/a", "hello")
        check("ls <dir> lists that dir", "ls SD", "a")
        # 关键：根目录里不许出现含斜杠的垃圾项
        check("root has no slash-named garbage", "ls", "SD/a", negate=True)
        # 父目录不存在 -> 必须失败，且不能偷偷建在别处
        check("write with missing parent fails", "write XD/b junk", "Failed to write file.")
        check("missing parent left nothing behind", "ls", "XD", negate=True)
        check("read missing file via path", "cat SD/nope", "File not found or read error.")
        check("delete via path", "rm SD/a", "File deleted.")
        check("subdir now empty", "ls SD", "a", negate=True)
        check("rmdir empty subdir", "rmdir SD", "Directory removed.")
        check("subdir gone from root", "ls", "SD", negate=True)

        # ---- B. 系统卷 cwd 下的相对路径 ----
        check("cd /system", "cd /system", "")
        check("relative cat inside /system", "cat version",
              "EZOS built-in system volume")
        check("relative write inside /system rejected", "write v2 hi",
              "Failed to write file.")
        check("leave system volume", "cd ..", "")
        check("back at root", "pwd", "/")

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

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    for n in bad:
        print("  FAILED: " + n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
