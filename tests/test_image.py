# -*- coding: utf-8 -*-
"""test_image.py - 镜像布局守护（纯宿主侧，不启 QEMU）

镜像大小改成动态之后，契约变成三条，任何一条断了都是"黑屏且看不出为什么"：
  1) os-image.bin = 引导扇区(512) + kernel.bin，两者都是 512 的倍数
  2) 引导扇区偏移 0x1FC 的 LE16 扇区数 == (os-image 大小 - 512) / 512
  3) 扇区数 <= 992（496KB：0x10000~0x90000 之间还要留 16KB 栈），
     且**整个镜像**的扇区数是偶数（引导扇区 + 内核）——H1c 实测：总数为
     奇数的镜像写 U 盘，SeaBIOS 认得出设备却报 could not read the boot disk

本脚本只读文件、秒级返回，适合在每次 ninja 之后当门禁跑。
为了让断言真的"咬得住"，最后用三个故意做坏的镜像跑同一套检查，
必须全部判 FAIL——否则说明断言是摆设。

用法：python tests/test_image.py   （退出码 0 = 全通过）
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMAGE = os.path.join(ROOT, "os-image.bin")
KERNEL = os.path.join(ROOT, "kernel.bin")
TMP = os.path.join(HERE, "_image_bad.bin")

SECTOR = 512
MAX_SECTORS = 992
FIELD = 0x1FC


def check_image(path):
    """返回 (ok, 说明)。任何一条不满足就整体判失败。"""
    if not os.path.isfile(path):
        return False, "missing file"
    size = os.path.getsize(path)
    if size < 2 * SECTOR:
        return False, "too small (%d B)" % size
    if size % SECTOR != 0:
        return False, "size %d not 512-aligned" % size
    with open(path, "rb") as f:
        boot = f.read(SECTOR)
    if boot[0x1FE] != 0x55 or boot[0x1FF] != 0xAA:
        return False, "no 0x55AA boot signature"
    sectors = boot[FIELD] | (boot[FIELD + 1] << 8)
    if sectors == 0:
        return False, "sector field at 0x1FC is 0 (make_image.py did not patch it)"
    if sectors > MAX_SECTORS:
        return False, "sector count %d > %d (496KB cap)" % (sectors, MAX_SECTORS)
    if sectors != (size - SECTOR) // SECTOR:
        return False, "sector field %d != actual kernel sectors %d" % (
            sectors, (size - SECTOR) // SECTOR)
    total = size // SECTOR
    if total % 2:
        return False, "total sector count %d is odd (H1c: some BIOS refuse " \
                      "to boot an odd-sized USB image)" % total
    return True, "%d kernel sectors, %d total (%d B)" % (sectors, total, size)


def bad_case(name, mutate):
    """把一个真镜像改坏，检查器必须判 FAIL。"""
    with open(IMAGE, "rb") as f:
        data = bytearray(f.read())
    mutate(data)
    with open(TMP, "wb") as f:
        f.write(data)
    ok, why = check_image(TMP)
    if os.path.isfile(TMP):
        os.remove(TMP)
    if ok:
        return ("%s: checker said OK on a broken image" % name, False)
    return ("%s -> correctly rejected (%s)" % (name, why), True)


def main():
    if not os.path.isfile(IMAGE):
        print("MISSING os-image.bin - run ninja first")
        return 2

    results = []

    ok, why = check_image(IMAGE)
    results.append(("os-image.bin layout: %s" % why, ok))

    if os.path.isfile(KERNEL):
        with open(IMAGE, "rb") as f:
            img = f.read()
        with open(KERNEL, "rb") as f:
            ker = f.read()
        results.append(("kernel.bin == os-image.bin[512:]", ker == img[SECTOR:]))
        results.append(("kernel.bin is 512-aligned", len(ker) % SECTOR == 0))

    # 反例：断言必须咬得住
    def odd_total(d):
        """扇区字段 +1 并真的补一个扇区：字段仍然自洽，但总数变奇数"""
        n = (d[FIELD] | (d[FIELD + 1] << 8)) + 1
        d[FIELD] = n & 0xFF
        d[FIELD + 1] = (n >> 8) & 0xFF
        d.extend(b"\x00" * SECTOR)

    results.append(bad_case("odd total sector count", odd_total))
    results.append(bad_case("sector field zeroed",
                            lambda d: d.__setitem__(slice(FIELD, FIELD + 2),
                                                    b"\x00\x00")))
    results.append(bad_case("boot signature wiped",
                            lambda d: d.__setitem__(slice(0x1FE, 0x200),
                                                    b"\x00\x00")))

    for name, good in results:
        print(("  [OK]   " if good else "  [FAIL] ") + name)
    allok = all(g for _, g in results)
    print("IMAGE:", "PASS" if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
