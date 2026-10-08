# -*- coding: utf-8 -*-
"""test_reformat.py - 连续格式化：旧文件系统的签名必须被抹掉

背景（这个 bug 是怎么挖出来的）：给 test_corrupt.py 加定向用例时做了反向
验证——拆掉内核里的保护闸重跑，期望用例变红，结果拆了闸照样绿。一层层查
下去才发现测试**一条都没碰到坏盘**：命令全跑在内置系统卷上。修完切盘，
还是不对，才挖出内核真有 bug——

`format` 以前只写自己的元数据、**不清盘**。把一张 exFAT 盘格成 ext4，盘上
1024 偏移确实变成了 ef53，但偏移 0 的 "EXFAT" 三个字母还留在原地；重启后
fs_init 探测时先撞见 exFAT 就挂回 exFAT。用户看到的是"格式化了但没生效"，
而且旧文件还在。

修法是 fs_format() 里加 fs_wipe_head()：格式化前把卷起点后 16 个扇区清零
（所有 FS 的签名区都在前 8KB 内，不至于把整盘写一遍；MBR 在 LBA 0，不动）。

本脚本守住的就是这条：**格式化后重启，卷必须还是刚格式化的那个 FS**。
每一轮都是"格式化 -> 关机 -> 重新开机 -> 看认成了什么"，因为残留签名只在
重新探测时才暴露。

用法：python tests/test_reformat.py   （退出码 0 = 全通过）
"""
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from ezos_env import qemu_exe                                    # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tests"))
import test_corrupt as tc                                        # noqa: E402

SRC = os.path.join(ROOT, "disk.vhd")
DISK = os.path.join(HERE, "reformat_disk.img").replace("\\", "/")

# 不含 fat32：disk.vhd 只有 16MB，FAT32 要求 >= 65525 簇，格式化会被正确
# 拒绝（见 test_fs_matrix.py 里扩到 64MB 再测的做法）。
FS_LIST = ["exfat", "ext4", "ntfs", "f2fs", "refs"]


def fresh_disk():
    shutil.copyfile(SRC, DISK)


def fresh_disk_at(start):
    """造一张"分区不在 LBA 1"的盘：MBR 分区表指向 start，前面全清零。

    真机上的常态就是这种盘——Windows/Linux 建的第一个分区普遍从 LBA 2048
    开始（1MB 对齐），只有虚拟机里手搓的测试盘才会在 LBA 1。以前各
    xxx_format 把 part_start 写死成 1，而 fs_wipe_head 按 MBR 里记录的
    起点清签名，于是"清 2048 处、写 1 处"——旧签名留在真正被探测的
    位置，重启后挂回上一个文件系统（或干脆认不出来）。
    """
    shutil.copyfile(SRC, DISK)
    size = os.path.getsize(DISK)
    total = size // 512
    mbr = bytearray(512)
    mbr[446] = 0x00                                  # boot flag
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00  # CHS start
    mbr[450] = 0x07                                  # 分区类型
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF  # CHS end
    struct.pack_into("<I", mbr, 454, start)
    struct.pack_into("<I", mbr, 458, total - start)
    mbr[510] = 0x55; mbr[511] = 0xAA
    with open(DISK, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))
        f.seek(512)
        f.write(b"\0" * (start + 16) * 512)


def fmt(fstype, start=0):
    """起一台机器把 DISK 格式化成 fstype，然后关机。返回 (ok, 说明)。"""
    fresh_disk_at(start) if start else fresh_disk()
    p, qmp = tc.launch(DISK)
    try:
        done, out = tc.run_cmd(qmp, "setdrive 1")
        if not done or "drive set to 1" not in tc.flat(out).lower():
            return False, "setdrive 1 失败：%r" % tc.flat(out)[:120]
        done, out = tc.run_cmd(qmp, "format " + fstype, 180.0)
        if not done:
            return False, "format 卡住"
        if "disk formatted as" not in tc.flat(out):
            return False, "format 失败：%r" % tc.flat(out)[:120]
    finally:
        tc.shutdown(p)
    return True, "ok"


# 各 FS 的卷签名相对**分区起点**的偏移。用来在宿主机上直接验证"卷到底
# 写在哪个 LBA"——只看 shell 输出是看不出来的：xxx_format 会把 MBR 的分
# 区起点一并改成自己用的 part_start，于是"挪了位置 + 改了分区表"自洽了，
# 重启照样能挂载，端到端断言是绿的。所以必须绕开内核、直接读盘看字节。
SIG = {
    "exfat": (3,   b"EXFAT"),
    "ntfs":  (3,   b"NTFS"),
    "refs":  (3,   b"ReFS"),
    "ext4":  (1024 + 56, b"\x53\xef"),
    "f2fs":  (1024, b"\x10\x20\xf5\xf2"),
}


def layout_ok(fstype, start):
    """宿主机视角：MBR 分区起点没被挪动，且卷签名确实写在那个起点上。

    返回 (ok, 说明)。这条断言是本测试里唯一能区分"尊重已有分区表"与
    "写死 LBA 1"的判据——反向验证过：把 fs.c 的起点改回写死 1，
    重启探测那 5 条照样全绿，只有这里会红。
    """
    d = open(DISK, "rb").read()
    got = int.from_bytes(d[454:458], "little")
    if got != start:
        return False, "MBR 分区起点被改成了 %d（应为 %d）" % (got, start)
    off, sig = SIG[fstype]
    base = start * 512 + off
    if d[base:base + len(sig)] != sig:
        return False, "LBA%d+%d 处没有 %s 签名" % (start, off, fstype)
    return True, "ok"


def probe_type():
    """起一台机器，返回数据盘被识别成的 FS 名（如 'ext4'）；认不出返回 None。"""
    p, qmp = tc.launch(DISK)
    try:
        done, out = tc.run_cmd(qmp, "setdrive 1")
        if not done:
            return None
        low = tc.flat(out).lower()
        key = "drive set to 1 ("
        i = low.rfind(key)
        if i < 0:
            return None
        rest = low[i + len(key):]
        j = rest.find(")")
        if j < 0:
            return None
        return rest[:j].strip()
    finally:
        tc.shutdown(p)


def main():
    for path in (tc.IMG, SRC):
        if not os.path.isfile(path):
            print("MISSING %s - run ninja first" % path)
            return 2

    results = []
    for fstype in FS_LIST:
        ok, why = fmt(fstype)
        if not ok:
            results.append(("格式化成 %s" % fstype, False))
            print("  !! %s：%s" % (fstype, why))
            continue
        got = probe_type()
        # 关键断言：重启后必须还是刚才格式化的那个 FS。残留的旧签名会让
        # fs_init 认回上一个 FS（历史上就是这么挂回 exFAT 的）。
        good = (got == fstype)
        results.append(("格式化成 %s 后重启仍识别为 %s" % (fstype, fstype),
                        good))
        if not good:
            print("  !! 格式化成 %s，重启后认成了 %r —— 旧签名没清掉"
                  % (fstype, got))

    # 连着换：每一轮的"上一个 FS"都不同，交叉覆盖残留签名的各种组合
    prev = FS_LIST[0]
    for fstype in FS_LIST[1:] + [FS_LIST[0]]:
        if fstype == prev:
            continue
        ok, _ = fmt(fstype)
        if not ok:
            results.append(("从 %s 改格成 %s" % (prev, fstype), False))
            continue
        got = probe_type()
        good = (got == fstype)
        results.append(("从 %s 改格成 %s 后重启为 %s" % (prev, fstype, fstype),
                        good))
        if not good:
            print("  !! 从 %s 改格成 %s，重启后认成了 %r" % (prev, fstype, got))
        prev = fstype

    # 分区不在 LBA 1 的盘（真机常态：1MB 对齐 → LBA 2048）。
    # 这里守的是"清签名"与"写元数据"用的是同一个卷起点。
    OFF = 2048
    for fstype in FS_LIST:
        ok, why = fmt(fstype, OFF)
        if not ok:
            results.append(("分区@%d 格式化成 %s" % (OFF, fstype), False))
            print("  !! 分区@%d %s：%s" % (OFF, fstype, why))
            continue
        # 先看字节落在哪：分区表有没有被挪、卷是不是写在了 LBA 2048。
        # 这条必须在重启之前看，重启后盘上的 MBR 已经被 format 改过了。
        ok, why = layout_ok(fstype, OFF)
        results.append(("分区@%d 格式化成 %s 后卷确实写在 %d"
                        % (OFF, fstype, OFF), ok))
        if not ok:
            print("  !! 分区@%d %s 布局不对：%s" % (OFF, fstype, why))
        got = probe_type()
        good = (got == fstype)
        results.append(("分区@%d 格式化成 %s 后重启仍识别为 %s"
                        % (OFF, fstype, fstype), good))
        if not good:
            print("  !! 分区@%d 格式化成 %s，重启后认成了 %r"
                  % (OFF, fstype, got))

    npass = sum(1 for _, ok in results if ok)
    for label, ok in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", label))
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
