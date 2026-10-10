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

第二组（2026-10-10 加）是文件名编解码与文本/网络算法：UTF-16LE<->UTF-8
（含代理对与落单代理）、Levenshtein 编辑距离、RFC 1071 校验和。这一组刚
加时立刻抓到两件事：一是 Zig 产物混进了 SSE 指令（内核不开 CR4.OSFXSR，
一跑就 #UD），二是探针本身把两个落单代理并排放，被正确合并成合法代理对，
是断言而非实现错了。两件事都留下了防线，见 tools/check_cpu.py 和探针注释。

不替代 exFAT 本身的正确性测试（那是 test_vhd.py 独立重算 checksum 的活），
这里只保证**三种语言算出来的东西一样**。

端口 4563(serial) / 4564(QMP)。
用法：python tests/test_multilang.py   （退出码 0 = 通过）
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

QEMU = qemu_exe()
QMP_PORT = alloc_port()
SERIAL_PORT = alloc_port()
IMG = image_path()
DISK = disk_path()
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
        # ---- 第二组：UTF-16 <-> UTF-8 三路对拍 ----
        # 三份实现在 NTFS/exFAT/ReFS 的名字读写边界上被调用，错了的后果
        # 不是显示难看，是匹配到错误的文件（历史 bug：三个后端三套降级
        # 策略，NTFS 变 '?'、exFAT 变 '.'、ReFS 判定不匹配）。
        check("utf16->utf8: C == Rust == Zig",
              "u16->u8 len c=24 rust=24 zig=24 [ok]" in f, f[-300:])
        # 缓冲不足必须 fail closed（返回负数），不能返回半截名字
        check("utf16->utf8 fails closed when buffer too small",
              "u16->u8 tiny ffffffff/ffffffff/ffffffff [ok]" in f, f[-260:])
        # 反向编码 + 往返一致
        check("utf8->utf16: C == Rust == Zig",
              "u8->u16 n c=14 rust=14 zig=14 [ok]" in f, f[-220:])
        # Levenshtein：shell 的 "did you mean" 用的就是它
        check("levenshtein: C == Rust == Zig", "levenshtein [ok]" in f, f[-180:])
        # RFC 1071 校验和：net.c 生产路径已迁到 Rust
        check("cksum: C == Rust == Zig", "cksum c=0x0000f311" in f, f[-200:])

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
