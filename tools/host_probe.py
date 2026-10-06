# -*- coding: utf-8 -*-
"""host_probe.py - 一次性诊断：Windows 挂载 exFAT VHD 报"需要格式化"时，
逐项核对规范里 Windows 真正会看的东西。

只读，不改盘。核对项：
  1. VBR 关键几何字段（FatOffset/ClusterHeapOffset/ClusterCount/...）
  2. Boot Checksum Sector（卷相对 11）+ 备份（12=0 副本，23=11 副本）
  3. 根目录 0x81 Allocation Bitmap 与 FAT 链是否一致
  4. 0x82 Up-case 表及其校验和
  5. MBR 分区表 / 磁盘签名 / ending CHS
"""
import struct
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from ref_exfat import Exfat, set_checksum, name_hash  # noqa

PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "disk.vhd")


def vol_chk(data):
    chk = 0
    for b in data:
        chk = ((chk >> 1) | (chk << 31)) & 0xFFFFFFFF
        chk = (chk + b) & 0xFFFFFFFF
    return chk


def main():
    fs = Exfat(PATH)
    raw = fs.raw
    P = fs.part_off
    print("== geometry ==")
    print("  part_off=%d bps=%d spc=%d cluster=%dB" % (P, fs.bps, fs.spc, fs.cluster_size))
    print("  fat_off=%d fat_len=%d heap_off=%d clusters=%d root=%d"
          % (fs.fat_off, fs.fat_len, fs.heap_off, fs.cluster_count, fs.root))
    vbr = raw[P:P + 512]
    print("  volume_length=%d partition_offset=%d revision=%04X flags=%04X pct=%d"
          % (_u64(vbr, 0x48), _u64(vbr, 0x40), _u16(vbr, 0x68), _u16(vbr, 0x6A), vbr[0x70]))
    ok = True

    print("== boot checksum sector (卷相对 11) ==")
    for slot, sec in (("primary", 11), ("backup", 23)):
        s = raw[P + sec * 512:P + (sec + 1) * 512]
        stored = [_u32(s, i * 4) for i in range(11)]
        calc = []
        for n in range(11):
            d = bytearray(raw[P + n * 512:P + (n + 1) * 512])
            if n == 0:
                d[106] = d[107] = d[112] = 0
            calc.append(vol_chk(bytes(d)))
        bad = [n for n in range(11) if stored[n] != calc[n]]
        print("  %-8s %s" % (slot, "OK" if not bad else "MISMATCH at sectors %s" % bad))
        if bad:
            ok = False
            for n in bad[:3]:
                print("     sector %d: stored 0x%08X  recomputed 0x%08X"
                      % (n, stored[n], calc[n]))
    # 备份引导区（卷相对 12 应等于 0，24 应等于 11）
    b12 = raw[P + 12 * 512:P + 13 * 512]
    b0 = raw[P:P + 512]
    print("  backup VBR (sector 12 == sector 0): %s"
          % ("OK" if b12 == b0 else "MISMATCH"))

    print("== root directory entries ==")
    for e in fs.parse_dir(fs.root):
        print("  %-26s attr=0x%04X size=%-6d first=%-3d nfc=%d chk=%04X/%04X hash=%04X/%04X"
              % (e['name'], e['attr'], e['size'], e['first_cluster'],
                 e['no_fat_chain'], e['chk_stored'], e['chk_calc'],
                 e['hash_stored'], e['hash_calc']))
        if e['chk_stored'] != e['chk_calc'] or e['hash_stored'] != e['hash_calc']:
            ok = False

    print("== allocation bitmap (0x81) vs FAT ==")
    raw_slots = fs.dir_slots(fs.root)
    bmp_cluster = None
    upcase_cluster = None
    for i, s in enumerate(raw_slots):
        if s[0] == 0x81 and bmp_cluster is None:
            bmp_cluster = _u32(s, 20)
        if s[0] == 0x82 and upcase_cluster is None:
            upcase_cluster = _u32(s, 20)
    if bmp_cluster is None:
        print("  NO 0x81 bitmap entry  <-- Windows requires it")
        ok = False
    else:
        bmp = fs.cluster(bmp_cluster)
        fat_used, fat_free = set(), set()
        for c in range(2, fs.cluster_count + 2):
            v = fs.fat(c)
            (fat_free if v == 0 else fat_used).add(c)
        bmp_used = set()
        for c in range(2, fs.cluster_count + 2):
            bit = c - 2
            if bmp[bit // 8] & (1 << (bit % 8)):
                bmp_used.add(c)
        print("  bitmap cluster=%d  used=%d  FAT-chain=%d  FAT-free=%d"
              % (bmp_cluster, len(bmp_used), len(fat_used), len(fat_free)))
        only_bmp = sorted(bmp_used - fat_used)[:8]
        only_fat = sorted(fat_used - bmp_used)[:8]
        if only_bmp:
            print("   bitmap claims used but FAT says free: %s" % only_bmp)
            ok = False
        if only_fat:
            print("   FAT chains it but bitmap says free: %s" % only_fat)
            ok = False
        if not only_bmp and not only_fat:
            print("   bitmap/FAT consistent")

    print("== up-case table (0x82) ==")
    if upcase_cluster is None:
        print("  NO 0x82 up-case entry  <-- Windows requires it")
        ok = False
    else:
        up = fs.cluster(upcase_cluster)
        stored = _u32(up, 0)
        calc = vol_chk(up[4:])
        print("  cluster=%d stored=0x%08X recomputed(after 4B header)=0x%08X  %s"
              % (upcase_cluster, stored, calc, "OK" if stored == calc else "MISMATCH"))
        if stored != calc:
            ok = False
            whole = vol_chk(up)
            print("     (whole-cluster checksum = 0x%08X)" % whole)

    print("== MBR ==")
    print("  sig=%s  part_type=0x%02X  lba_start=%d  sectors=%d  boot=%02X%02X"
          % (raw[440:444].hex(), raw[450], _u32(raw, 454), _u32(raw, 458),
             raw[510], raw[511]))
    print("  ending CHS = head %d sector %d cyl %d"
          % (raw[451], raw[452] & 0x3F, (raw[452] >> 6) | raw[453]))

    print("\n%s" % ("ALL OK" if ok else "PROBLEMS FOUND (see above)"))
    return 0 if ok else 1


def _u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


if __name__ == "__main__":
    sys.exit(main())
