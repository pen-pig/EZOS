# -*- coding: utf-8 -*-
"""test_fatpath.py - FAT 后端的跨目录路径守护（对齐 exFAT）。

与 test_path.py 同源的缺陷：FAT 后端的 fat_find_entry(cwd, name) 只认单层名，
"SD/a" 整个被当成文件名——读/删永远找不到，而**写却会成功**，在根目录里造出
一个短名里带斜杠的垃圾目录项（ls 看得见、任何合法路径都读不到、还占簇）。
现在 FAT 与 exFAT 一样先拆"父目录簇 + 末段名"逐级走目录，父目录缺失即失败。

这里把同一套断言在 FAT16 上再跑一遍（FAT12/16 的根目录是**根目录区**、簇号
用 0 表示，跟 FAT32 的簇 2 不是一回事，所以路径起点必须走
root_dir_cluster_value()，这条测试就是守这个差异的）。

端口 4517(QMP) / 4518(serial)。
"""
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4517
SERIAL_PORT = 4518
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
WORK = os.path.join(HERE, "fatpath_disk.vhd").replace("\\", "/")

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


def flat(s):
    """大小写/空白无关化：FAT 短名一律大写（write SD/a -> 目录项 'A'），
    按原样断言小写会假红。"""
    return " ".join(s.lower().split())


def run_cmd(qmp, serial, cmd, settle=3.0):
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

    def check(name, cmd, expect, negate=False, settle=4.0):
        out = run_cmd(qmp, serial, cmd, settle)
        hit = expect.lower() in flat(out)
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

        # 先把数据盘格式化成 FAT16（fs_matrix 已证明 format 本身可用）
        check("format fat16", "format fat16", "disk formatted as", settle=10.0)
        check("df reports fat16", "df", "fat16", settle=4.0)

        # ---- 跨目录路径 ----
        check("mkdir SD", "mkdir SD", "")
        check("write into subdir", "write SD/a hello", "File written successfully.")
        check("read back via path", "cat SD/a", "hello")
        check("ls <dir> lists that dir", "ls SD", "a")
        # 关键：根目录里不许出现含斜杠的垃圾项
        check("root has no slash-named garbage", "ls", "SD/a", negate=True)
        # 父目录不存在 -> 必须失败，且不能偷偷建在别处
        check("write with missing parent fails", "write XD/b junk", "Failed to write file.")
        check("missing parent left nothing behind", "ls", "XD", negate=True)
        check("mkdir with missing parent fails", "mkdir XD/sub", "")
        check("still nothing named XD", "ls", "XD", negate=True)
        check("read missing file via path", "cat SD/nope", "File not found or read error.")
        check("delete via path", "rm SD/a", "File deleted.")
        check("subdir now empty", "ls SD", "a", negate=True)
        check("rmdir empty subdir", "rmdir SD", "Directory removed.")
        check("subdir gone from root", "ls", "SD", negate=True)

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
