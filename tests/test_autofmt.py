# -*- coding: utf-8 -*-
"""test_autofmt.py - 自动格式化只能对"空盘"动手

背景：kernel.c 启动时如果数据盘上没有能识别的文件系统，会自动格成 exFAT
并放一个 README.TXT——对第一次启动的全新盘来说很方便。

问题在于 `fs_init()` 返回 -2 只表示"认不出来"，它分不出两种盘：

    (a) 盘是空的（刚擦除/全新）        -> 自动格式化没问题
    (b) 盘上装着一个 EZOS 不认识的 FS  -> 自动格式化 = 毁数据

(b) 不是假想：btrfs、XFS、加密卷、或者一块分区里还有数据的盘，探测结果
全是 -2。以前一律格掉。真机上插一块别人的数据盘开机，数据就没了。

修法是 fs.c 新增 fs_drive_blank()：只有盘确实是擦除态（扫到的扇区全
0x00 或全 0xFF）才允许自动格式化；读不出来也按"非空"处理（fail closed，
绝不格式化读不动的盘）。

本脚本守住这条线：
  - 全零盘 / 全 0xFF 盘 -> 仍然自动格式化成 exFAT（原行为不能丢）
  - 有内容的陌生盘      -> 一个字节都不许改，且要打印 NOT formatting

用法：python tests/test_autofmt.py   （退出码 0 = 全通过）
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from ezos_env import qemu_exe                                    # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tests"))
import test_corrupt as tc                                        # noqa: E402

# 自己的端口与日志：回归是串行的，但不要和 corrupt 抢同一个文件
tc.PORT = alloc_port()
tc.LOG = os.path.join(ROOT, "temp", "autofmt_serial.log").replace("\\", "/")

DISK = os.path.join(HERE, "autofmt_disk.img").replace("\\", "/")
SIZE = 16 * 1024 * 1024


def make_blank(fill):
    with open(DISK, "wb") as f:
        f.write(bytes([fill]) * SIZE)


def make_unknown():
    """一块"有内容、但 EZOS 认不出来"的盘。

    MBR 指向 LBA1，分区里填 0xA5——既不是全 0x00 也不是全 0xFF，所以
    fs_drive_blank() 必须判它非空；同时不含任何已知 FS 的魔数，所以
    fs_init() 返回 -2。这正是以前会被误格式化的那种盘。
    """
    with open(DISK, "wb") as f:
        f.truncate(SIZE)
    mbr = bytearray(512)
    mbr[446] = 0x00
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00
    mbr[450] = 0x07                      # 分区类型：随便一个已知类型
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF
    mbr[454] = 1; mbr[455] = 0; mbr[456] = 0; mbr[457] = 0
    n = SIZE // 512 - 1
    mbr[458] = n & 0xFF; mbr[459] = (n >> 8) & 0xFF
    mbr[460] = (n >> 16) & 0xFF; mbr[461] = (n >> 24) & 0xFF
    mbr[510] = 0x55; mbr[511] = 0xAA
    with open(DISK, "r+b") as f:
        f.seek(0); f.write(bytes(mbr))
        f.seek(512); f.write(b"\xA5" * (64 * 1024))   # 分区里塞满内容


def head_hash():
    d = open(DISK, "rb").read(64 * 1024)
    return hashlib.sha256(d).hexdigest()


def boot_and_collect():
    """起一台机器，返回串口日志全文（小写）。"""
    p, qmp = tc.launch(DISK)
    try:
        tc.run_cmd(qmp, "df", 60.0)
    finally:
        tc.shutdown(p)
    try:
        return open(tc.LOG, "rb").read().decode("latin1", "replace").lower()
    except OSError:
        return ""


def has_exfat_sig():
    d = open(DISK, "rb").read(4096)
    return b"EXFAT" in d


def main():
    if not os.path.isfile(tc.IMG):
        print("MISSING %s - run ninja first" % tc.IMG)
        return 2
    results = []

    # ---- 1/2：擦除态的盘仍然要自动格式化（原行为不能弄丢） ----
    for fill, tag in ((0x00, "全零盘"), (0xFF, "全 0xFF 盘")):
        make_blank(fill)
        log = boot_and_collect()
        ok = ("auto-formatting" in log and has_exfat_sig())
        results.append(("%s 自动格式化成 exFAT" % tag, ok))
        if not ok:
            print("  !! %s 没被自动格式化（log 里有 auto-formatting=%s，"
                  "盘上有 EXFAT 签名=%s）"
                  % (tag, "auto-formatting" in log, has_exfat_sig()))

    # ---- 3：有内容的陌生盘，一个字节都不许动 ----
    make_unknown()
    before = head_hash()
    log = boot_and_collect()
    after = head_hash()
    untouched = (before == after)
    warned = ("not formatting" in log)
    results.append(("陌生盘未被格式化（盘面前 64KB 一字未改）", untouched))
    results.append(("陌生盘打印 NOT formatting 提示", warned))
    if not untouched:
        print("  !! 陌生盘被改写了：内核把它格掉了，用户数据没了")
    if not warned:
        print("  !! 没有打印 NOT formatting，用户不知道发生了什么")

    npass = sum(1 for _, ok in results if ok)
    for label, ok in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", label))
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
