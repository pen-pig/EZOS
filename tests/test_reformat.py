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


def fmt(fstype):
    """起一台机器把 DISK 格式化成 fstype，然后关机。返回 (ok, 说明)。"""
    fresh_disk()
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

    npass = sum(1 for _, ok in results if ok)
    for label, ok in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", label))
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
