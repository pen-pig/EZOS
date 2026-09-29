# -*- coding: utf-8 -*-
"""test_vhd.py - disk.vhd 与 exFAT Boot Checksum 守护（纯宿主侧，不启 QEMU）

为什么需要它：disk.img 在 Windows 上双击挂不上，原因有两层，都是静默的——
  1) 资源管理器只给 .iso / .vhd 提供"装载"，.img 只会问你用什么打开；
  2) 就算换后缀，没有尾部 512 字节 Hard Disk Footer（"conectix"）Windows
     也不认；而我们的 exFAT Boot Checksum 扇区一直是"VolumeChecksum@0 +
     整体校验@508"的非规范写法，Windows 会判卷损坏、提示格式化。

所以这里守住两件事：
  A. tools/make_vhd.py 产出的 disk.vhd：数据是 disk.img 原样搬运（偏移 0），
     footer 字段合法、校验和对、几何容量与 Current Size 一致；
  B. disk.img 的 Boot Checksum 扇区符合 exFAT 规范：11 个 uint32 分别对应
     卷相对扇区 0..10，扇区 0 计算时排除 VolumeFlags(106,107) 与
     PercentInUse(112)，44 字节之后必须全零，备份扇区与正本一致。

B 的校验在测试里**独立重写一遍算法**（不 import 生成器），两边对不上就说明
生成器写错了或者被人改回旧写法。最后照例跑反例：故意改坏的 VHD / 改坏的
checksum 都必须被判 FAIL。

用法：python tests/test_vhd.py   （退出码 0 = 全通过）
"""
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMG = os.path.join(ROOT, "disk.img")
VHD = os.path.join(ROOT, "disk.vhd")
MAKE_VHD = os.path.join(ROOT, "tools", "make_vhd.py")
TMP_VHD = os.path.join(HERE, "_vhd_bad.vhd")
TMP_REGEN = os.path.join(HERE, "_vhd_regen.vhd")

SECTOR = 512
FOOTER = 512


def vhd_check(path):
    """返回 (ok, 说明)：调 make_vhd.py --check，退出码 0 才算通过"""
    r = subprocess.run([sys.executable, MAKE_VHD, "--check", path],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0, r.stdout.decode("utf-8", "replace").strip()


def exfat_checksum(data):
    chk = 0
    for b in data:
        chk = ((chk >> 1) | (chk << 31)) & 0xFFFFFFFF
        chk = (chk + b) & 0xFFFFFFFF
    return chk


def check_boot_checksum(img):
    """独立重算 exFAT Boot Checksum 扇区并比对（返回 (ok, 说明)）"""
    mbr = img[0:SECTOR]
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        return False, "disk.img has no MBR signature"
    part_start = struct.unpack_from('<I', mbr, 446 + 8)[0]
    if part_start < 1:
        return False, "partition start LBA = %d" % part_start

    def sect(n):                       # 卷相对扇区 n
        off = (part_start + n) * SECTOR
        return img[off:off + SECTOR]

    if sect(0)[3:8] != b'EXFAT':
        return False, "volume boot record is not exFAT"
    bcs = sect(11)
    for s in range(11):
        d = bytearray(sect(s))
        if s == 0:                     # 规范：排除这两个会被宿主改写的字段
            d[106] = 0; d[107] = 0; d[112] = 0
        want = exfat_checksum(bytes(d))
        got = struct.unpack_from('<I', bcs, s * 4)[0]
        if want != got:
            return False, "checksum[%d] %08x != recomputed %08x" % (s, got, want)
    if any(bcs[44:]):
        return False, "bytes 44..511 of the checksum sector must be zero"
    if sect(23) != bcs:
        return False, "backup checksum sector (23) differs from primary (11)"
    return True, "11 x uint32 verified, backup matches"


def bad_case(name, mutate, target=VHD):
    """把 VHD/checksum 改坏，检查器必须判 FAIL"""
    with open(target, "rb") as f:
        data = bytearray(f.read())
    mutate(data)
    with open(TMP_VHD, "wb") as f:
        f.write(data)
    ok, _ = vhd_check(TMP_VHD)
    if os.path.isfile(TMP_VHD):
        os.remove(TMP_VHD)
    return ("%s -> correctly rejected" % name if not ok
            else "%s: checker said OK on a broken image" % name), not ok


def main():
    results = []
    if not os.path.isfile(IMG) or not os.path.isfile(VHD):
        print("MISSING disk.img / disk.vhd - run ninja first")
        return 2
    if not os.path.isfile(MAKE_VHD):
        print("MISSING tools/make_vhd.py")
        return 2

    img_size = os.path.getsize(IMG)
    vhd_size = os.path.getsize(VHD)

    # --- A. VHD 容器 ---
    ok, why = vhd_check(VHD)
    results.append(("disk.vhd footer: %s" % why.splitlines()[-1] if ok
                    else "disk.vhd footer: %s" % why, ok))
    results.append(("disk.vhd size == disk.img + 512 footer (%d)" % vhd_size,
                    vhd_size == img_size + FOOTER))
    with open(IMG, "rb") as f:
        raw = f.read()
    with open(VHD, "rb") as f:
        head = f.read(img_size)
    results.append(("disk.vhd data area is byte-identical to disk.img", raw == head))
    with open(VHD, "rb") as f:
        f.seek(vhd_size - FOOTER)
        footer = f.read(FOOTER)
    results.append(("footer cookie 'conectix'", footer[0:8] == b'conectix'))
    results.append(("footer data offset = all-ones (fixed VHD)",
                    footer[16:24] == b'\xff' * 8))
    cur = struct.unpack_from('>Q', footer, 48)[0]
    cyl = struct.unpack_from('>H', footer, 56)[0]
    heads, spt = footer[58], footer[59]
    results.append(("CHS %d/%d/%d capacity == Current Size %d"
                    % (cyl, heads, spt, cur), cyl * heads * spt * SECTOR == cur))

    # 重新生成一次：UUID 由文件名派生，字节必须完全一致（ninja 幂等）
    subprocess.run([sys.executable, MAKE_VHD, IMG, TMP_REGEN],
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    same = False
    if os.path.isfile(TMP_REGEN):
        with open(TMP_REGEN, "rb") as f:
            regen = f.read(img_size + FOOTER)
        with open(VHD, "rb") as f:
            cur_bytes = f.read()
        same = (regen == cur_bytes)
        os.remove(TMP_REGEN)
    results.append(("regenerating disk.vhd is byte-reproducible", same))

    # --- B. exFAT Boot Checksum ---
    ok, why = check_boot_checksum(raw)
    results.append(("exFAT boot checksum sector: %s" % why, ok))

    # --- 反例：断言必须咬得住 ---
    def wipe_cookie(d):
        d[len(d) - FOOTER:len(d) - FOOTER + 8] = b'\x00' * 8

    def break_sum(d):
        off = len(d) - FOOTER + 64
        d[off] ^= 0xFF

    def shrink_size(d):
        off = len(d) - FOOTER + 48
        v = struct.unpack_from('>Q', d, off)[0] - SECTOR
        struct.pack_into('>Q', d, off, v)

    def not_fixed(d):
        struct.pack_into('>I', d, len(d) - FOOTER + 60, 3)   # 3 = dynamic

    results.append(bad_case("cookie wiped", wipe_cookie))
    results.append(bad_case("footer checksum corrupted", break_sum))
    results.append(bad_case("Current Size shrunk", shrink_size))
    results.append(bad_case("disk type switched to dynamic", not_fixed))

    # boot checksum 反例：改 VBR 的 PercentInUse 之外的字节，checksum 就不该再匹配
    def break_bcs(d):
        d[(1 + 5) * SECTOR] ^= 0x5A       # 卷相对扇区 5 首字节（正本区之前）

    with open(IMG, "rb") as f:
        img2 = bytearray(f.read())
    break_bcs(img2)
    ok2, _ = check_boot_checksum(bytes(img2))
    results.append(("boot checksum rejects a mutated boot region", not ok2))

    for name, good in results:
        print(("  [OK]   " if good else "  [FAIL] ") + name)
    allok = all(g for _, g in results)
    print("VHD:", "PASS" if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
