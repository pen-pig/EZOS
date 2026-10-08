# -*- coding: utf-8 -*-
"""test_vhd.py - disk.vhd 与 exFAT Boot Checksum 守护（纯宿主侧，不启 QEMU）

背景：disk.img 以前在 Windows 上双击挂不上，两层原因都是静默的——
  1) 资源管理器只给 .iso / .vhd 提供"装载"，.img 只会问你用什么打开；
  2) 就算换后缀，没有尾部 512 字节 Hard Disk Footer（"conectix"）Windows
     也不认；而我们的 exFAT Boot Checksum 扇区一直是"VolumeChecksum@0 +
     整体校验@508"的非规范写法，Windows 会判卷损坏、提示格式化。

现在镜像**本身就是固定 VHD**：temp/gen_diskimg.py 把镜像字节写在文件头
（偏移 0）再追加 512 字节 footer，所以 QEMU 用 format=raw 直接加载 disk.vhd，
不再有第二份 .img。本脚本守住：
  A. footer 字段合法、校验和对、CHS 容量与 Current Size 一致；
  B. 产物可重现（gen_diskimg 重跑一次字节必须完全一致，UUID 由文件名派生）；
  C. tools/make_vhd.py 的 raw -> VHD 往返仍然正确（剥 footer 再包回来）；
  D. Boot Checksum 扇区符合 exFAT 规范：11 个 uint32 分别对应卷相对扇区
     0..10，扇区 0 计算时排除 VolumeFlags(106,107) 与 PercentInUse(112)，
     44 字节之后必须全零，备份扇区与正本一致。

D 的校验在测试里**独立重写一遍算法**（不 import 生成器），两边对不上就说明
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
VHD = os.path.join(ROOT, "disk.vhd")
GEN = os.path.join(ROOT, "temp", "gen_diskimg.py")
MAKE_VHD = os.path.join(ROOT, "tools", "make_vhd.py")
TMP_BAD = os.path.join(HERE, "_vhd_bad.vhd")
TMP_REGEN_DIR = os.path.join(HERE, "_vhd_regen")
TMP_REGEN_DIR2 = os.path.join(HERE, "_vhd_regen2")
TMP_RAW = os.path.join(HERE, "_vhd_roundtrip.img")

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
        return False, "disk image has no MBR signature"
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


def fresh_vhd():
    """自己生成一份干净的 VHD 再校验。

    为什么不用仓库根的 disk.vhd：那是**共享盘**，别的 E2E 用例（fs_matrix、
    rmdir、corrupt…）会拿它去 format，跑完就脏了。本脚本再校验它就会假红，
    看着像"生成器坏了"，其实是"盘被别人写过"。踩过好几次，所以这里每次
    自己生成一份，既不受污染也不污染别人。"""
    os.makedirs(TMP_REGEN_DIR, exist_ok=True)
    p = os.path.join(TMP_REGEN_DIR, os.path.basename(VHD))
    r = subprocess.run([sys.executable, GEN, p, "exfat"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not os.path.isfile(p):
        print("gen_diskimg.py failed to produce a VHD:\n" +
              r.stdout.decode("utf-8", "replace"))
        shutil.rmtree(TMP_REGEN_DIR, ignore_errors=True)
        return None
    return p


def bad_case(name, mutate, blob):
    """把 VHD 改坏，检查器必须判 FAIL"""
    data = bytearray(blob)
    mutate(data)
    with open(TMP_BAD, "wb") as f:
        f.write(data)
    ok, _ = vhd_check(TMP_BAD)
    if os.path.isfile(TMP_BAD):
        os.remove(TMP_BAD)
    return ("%s -> correctly rejected" % name if not ok
            else "%s: checker said OK on a broken image" % name), not ok


def main():
    if not os.path.isfile(MAKE_VHD) or not os.path.isfile(GEN):
        print("MISSING tools/make_vhd.py / temp/gen_diskimg.py")
        return 2

    src = fresh_vhd()
    if not src:
        return 2
    try:
        return run_checks(src)
    finally:
        shutil.rmtree(TMP_REGEN_DIR, ignore_errors=True)


def run_checks(src):
    results = []

    vhd_size = os.path.getsize(src)
    data_size = vhd_size - FOOTER
    with open(src, "rb") as f:
        blob = f.read()
    data, footer = blob[:data_size], blob[data_size:]

    # --- A. VHD 容器 ---
    ok, why = vhd_check(src)
    results.append(("fresh disk.vhd footer: %s" % why.splitlines()[-1] if ok
                    else "fresh disk.vhd footer: %s" % why, ok))
    results.append(("disk.vhd size is 512-aligned and > footer (%d)" % vhd_size,
                    vhd_size % SECTOR == 0 and data_size > 0))
    results.append(("data area starts with a valid MBR (0x55AA)",
                    data[510] == 0x55 and data[511] == 0xAA))
    results.append(("footer cookie 'conectix'", footer[0:8] == b'conectix'))
    results.append(("footer data offset = all-ones (fixed VHD)",
                    footer[16:24] == b'\xff' * 8))
    cur = struct.unpack_from('>Q', footer, 48)[0]
    cyl = struct.unpack_from('>H', footer, 56)[0]
    heads, spt = footer[58], footer[59]
    results.append(("CHS %d/%d/%d capacity == Current Size %d"
                    % (cyl, heads, spt, cur), cyl * heads * spt * SECTOR == cur))
    results.append(("Current Size == data area size", cur == data_size))

    # --- B. 产物可重现（UUID 由文件名派生，所以要放到同名临时目录里重生成）---
    os.makedirs(TMP_REGEN_DIR2, exist_ok=True)
    regen = os.path.join(TMP_REGEN_DIR2, os.path.basename(src))
    subprocess.run([sys.executable, GEN, regen],
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    same = False
    if os.path.isfile(regen):
        with open(regen, "rb") as f:
            same = (f.read() == blob)
    shutil.rmtree(TMP_REGEN_DIR2, ignore_errors=True)
    results.append(("regenerating disk.vhd is byte-reproducible", same))

    # --- C. make_vhd.py 的 raw -> VHD 往返仍然正确 ---
    roundtrip = False
    out = os.path.join(HERE, "_vhd_roundtrip.vhd")
    try:
        with open(TMP_RAW, "wb") as f:
            f.write(data)
        subprocess.run([sys.executable, MAKE_VHD, TMP_RAW, out],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if os.path.isfile(out):
            with open(out, "rb") as f:
                rb = f.read()
            # TimeStamp(24..28) 取源 mtime、UniqueId(68..84) 由源文件名派生、
            # Checksum(64..68) 覆盖全 footer —— 这三处随"源"而变，不参与比较；
            # 其余字段 + 整个数据区必须逐字节一致。
            def strip_volatile(fb):
                return fb[:24] + fb[28:64] + fb[84:]
            roundtrip = (rb[:data_size] == data and
                         strip_volatile(rb[data_size:]) == strip_volatile(footer))
    finally:
        for p in (out, TMP_RAW):
            if os.path.isfile(p):
                os.remove(p)
    results.append(("make_vhd.py round-trip (strip footer, rewrap) is lossless",
                    roundtrip))

    # --- D. exFAT Boot Checksum ---
    ok, why = check_boot_checksum(data)
    results.append(("exFAT boot checksum sector: %s" % why, ok))

    # --- 反例：断言必须咬得住 ---
    def wipe_cookie(d):
        d[len(d) - FOOTER:len(d) - FOOTER + 8] = b'\x00' * 8

    def break_sum(d):
        d[len(d) - FOOTER + 64] ^= 0xFF

    def shrink_size(d):
        off = len(d) - FOOTER + 48
        struct.pack_into('>Q', d, off,
                         struct.unpack_from('>Q', d, off)[0] - SECTOR)

    def not_fixed(d):
        struct.pack_into('>I', d, len(d) - FOOTER + 60, 3)   # 3 = dynamic

    results.append(bad_case("cookie wiped", wipe_cookie, blob))
    results.append(bad_case("footer checksum corrupted", break_sum, blob))
    results.append(bad_case("Current Size shrunk", shrink_size, blob))
    results.append(bad_case("disk type switched to dynamic", not_fixed, blob))

    # boot checksum 反例：改一个被校验扇区的字节，checksum 就不该再匹配
    broken = bytearray(data)
    broken[(1 + 5) * SECTOR] ^= 0x5A       # 卷相对扇区 5（正本区之前）
    ok2, _ = check_boot_checksum(bytes(broken))
    results.append(("boot checksum rejects a mutated boot region", not ok2))

    for name, good in results:
        print(("  [OK]   " if good else "  [FAIL] ") + name)
    allok = all(g for _, g in results)
    print("VHD:", "PASS" if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
