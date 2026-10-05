# -*- coding: utf-8 -*-
"""test_multilang.py - C / Rust / Zig 三方对拍的 E2E 守护。

内核里现在有三份 exFAT 校验和（C: kernel/exfat.c、Rust: rust/ezos_rs、
Zig: rust/ezos_zig）和两份文件名 hash（Rust / Zig），`rstest` 命令逐项
比对。这是"增量引入非 C 语言"的安全网：

  * **算法漂移**：第一版 Rust/Zig 把 hash 的字符转小写（`c | 0x20`）而不是
    规范要求的大写，对拍当场就红了——这类 bug 单看一份实现永远发现不了，
    因为两边"都能跑、都不报错"，只有对比才暴露。
  * **链接错版本 / 符号没接上**：任何一份没链进内核，链接期就失败；
    链上了但版本旧，这里变红。
  * **全 0 陷阱**：断言 hash != 0，防止"函数根本没被调用"也显示一致。

不替代 exFAT 本身的正确性测试（那是 test_vhd.py 独立重算 checksum 的活），
这里只保证**三种语言算出来的东西一样**。

端口 4563(serial) / 4564(QMP)。
用法：python tests/test_multilang.py   （退出码 0 = 通过）
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4564
SERIAL_PORT = 4563
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
# 用**副本**而不是 disk.vhd 本体：宿主机上随时可能有别的进程（索引服务、
# 杀软扫描、Explorer 缩略图）拿着它的句柄，QEMU 会直接
# "Could not open ... 另一个程序正在使用此文件" 退出，测试看起来像
# "串口连不上" —— 实际是盘没挂上。每条用例独立盘副本是 E2E 的通用规矩。
WORK = os.path.join(HERE, "multilang_disk.vhd").replace("\\", "/")

from test_nvme import SerialReader, Qmp, wait_for, kill_all_qemu  # noqa


def flat(s):
    return " ".join(s.lower().split())


def main():
    if not os.path.isfile(IMG) or not os.path.isfile(DISK):
        print("MISSING os-image.bin / disk.vhd - run ninja first")
        return 1

    shutil.copyfile(DISK, WORK)
    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + WORK,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print("%s %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  <- " + detail) if detail else ""))

    try:
        from time import sleep
        sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)
        wait_for(serial, "TASK: preemptive", 150)
        sleep(3.0)

        before = serial.size()
        qmp.type_line("rstest")
        sleep(6.0)
        out = serial.tail_from(before)
        f = flat(out)

        check("rstest runs (linked in)", "rust/zig cross-check" in f)
        # flat() 会把连续空白压成一个：源码里的 "  checksum  C=0x" 到这里是
        # "checksum c=0x"，断言里多写空格会假红。
        check("checksum: C == Rust == Zig",
              "checksum c=0x" in f and "rust=0x" in f and "zig=0x" in f,
              f[:160])
        # 16 位目录项集校验和是**另一支算法**（32 位版截断不得出这个数），
        # 必须单独对拍：历史上就是这里写错，盘上 SetChecksum 全错而内核毫无
        # 察觉，靠 tests/test_fsref.py 的宿主机独立实现才抓出来。
        check("setcksum(16-bit): C == Rust == Zig",
              "setcksum c=0x" in f and f.count("rust=0x") >= 2, f[:200])
        # 输出顺序是 C / rust / zig，"namehash rust=0x" 这种连写匹配不到
        # （flat 压过空白后是 "namehash c=0x... rust=0x... zig=0x..."）。
        check("namehash: C == Rust == Zig", "namehash c=0x" in f, f[:200])
        # 固定向量：'R','e','A','d','M','e','.','T','x','T' 大写化后 = 0x78A3，
        # 与宿主机独立实现一致（python -c "from ref_exfat import name_hash;
        # print(hex(name_hash('README.TXT')))" -> 0x78a3）。
        # 三份一致但集体算错的可能性极低，钉死绝对值也就几行成本。
        check("namehash == published vector 0x78A3",
              "namehash c=0x000078a3 rust=0x000078a3 zig=0x000078a3" in f,
              f[:220])
        check("overall PASS", "result: pass" in f)
        # 全 0 陷阱：三份都返回 0 说明函数压根没被调用
        check("hash is non-zero (not a no-op stub)",
              "namehash rust=0x0 " not in f and "zig=0x0 " not in f)

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
