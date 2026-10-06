# -*- coding: utf-8 -*-
"""test_heap.py - 内核堆护栏（尾部 canary / 越界检测）的 E2E 守护。

为什么单独一个文件：kmalloc 原来只有**块头魔数**，用户区写穿时会一路踩进
下一个块的头或别人的数据，要等受害者被 kfree 才可能暴露——那时现场早没了。
现在每个已分配块在用户区末尾（4 字节对齐处）多一个尾部 canary，写穿 1 字节
就会踩中，kfree 立刻停机并标注 "heap overflow (tail canary smashed)"；
kmalloc_audit() 还能在不改动堆的前提下全池体检。

这里验证三件事：
  1) kmalloc self-test 里"越界 1 字节"必须被 audit 抓到（抓不到 = canary
     没真的装在用户区末尾，比如偷懒放到了块尾的 padding 里，那样 size 刚好
     对齐时根本没有检测能力）；
  2) 现场复原后 audit 归零——否则自检自己会把堆留成坏状态；
  3) kmtest / pmmtest 整体 PASS，且堆占用没有失控（canary 让每个块多 4~16B，
     池只有 384KB，这里顺便盯住开销）。

反例（改动内核后这里应变红）：
  - 把 km_tail_of 改成 "块尾 -4"（用 padding）-> 第 1 条 FAIL
  - 删掉 km_arm 里的 canary 装配      -> 第 1 条 FAIL
  - 拆分块时忘记把请求大小存进 next 槽 -> kfree 报 corrupt request size

端口 4561(serial) / 4562(QMP)。
用法：python tests/test_heap.py   （退出码 0 = 通过）
"""
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

QEMU = qemu_exe()
QMP_PORT = 4562
SERIAL_PORT = 4561
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")

from test_nvme import SerialReader, Qmp, wait_for, kill_all_qemu  # noqa


def flat(s):
    return " ".join(s.lower().split())


def main():
    if not os.path.isfile(IMG) or not os.path.isfile(DISK):
        print("MISSING os-image.bin / disk.vhd - run ninja first")
        return 1

    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + DISK,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print("%s %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  <- " + detail) if detail else ""))

    def run(cmd, settle=4.0):
        before = serial.size()
        qmp.type_line(cmd)
        time.sleep(settle)
        return serial.tail_from(before)

    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)
        wait_for(serial, "TASK: preemptive", 150)
        time.sleep(3.0)

        out = run("kmtest", 6.0)
        f = flat(out)
        check("kmalloc self-test runs", "kmalloc self-test" in f)
        check("tail canary catches 1-byte overflow",
              "tail canary detects 1-byte overflow: yes [ok]" in f,
              "canary is not armed right after the user area")
        check("audit clean after repair",
              "audit clean after repair: yes [ok]" in f)
        check("kmtest overall PASS", "result: pass" in f)
        check("no heap panic during self-test",
              "tail canary smashed" not in f and "header smashed" not in f)

        # 池大小与占用盯梢：canary 让每块多 4~16B，池只有 384KB。kmtest 首行
        # 会打印 "before: total 384KB used ..B largest ..KB"，这里只卡住
        # "池没被改小"和"自检前后基线一致（merge OK）"两条，具体数字人看。
        check("heap pool unchanged (384KB)", "total 384kb" in f)
        check("free returns heap to baseline", "merge ok" in f)

        out = run("pmmtest", 6.0)
        check("pmm self-test PASS", "result: pass" in flat(out))

        ok = all(v for _, v in results)
        print("  => %d/%d %s" % (sum(1 for _, v in results if v),
                                 len(results), "PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        try:
            serial.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()


if __name__ == "__main__":
    sys.exit(main())
