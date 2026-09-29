# -*- coding: utf-8 -*-
"""make_vhd.py - 把原始磁盘镜像包装成 Windows 可直接双击挂载的固定 VHD

为什么要这一步：Windows 资源管理器的"装载"只对 .iso 和 .vhd/.vhdx 生效，
.img 双击只会弹"你想如何打开这个文件"。而仅仅把后缀改成 .vhd 也不行——
Windows 认的是文件尾部 512 字节的 Hard Disk Footer（"conectix" cookie）。
固定盘（fixed VHD）的布局恰好最简单：

    [ 原始镜像全部字节 ][ 512 字节 footer ]

数据区从文件偏移 0 开始，所以这份 .vhd 同时还能给 QEMU 当 raw 盘用
（末尾多出的 512 字节只是内核永远读不到的尾巴，且总长仍是 512 的倍数）。

用法:
    python tools/make_vhd.py disk.img [disk.vhd]
    python tools/make_vhd.py --check disk.vhd

footer 字段（全大端）参考 Microsoft "Virtual Hard Disk Image Format
Specification" 的 Hard Disk Footer：
    cookie 'conectix' / Features / FileFormatVersion / DataOffset(全 F=固定盘)
    / TimeStamp(2000-01-01 起秒数) / CreatorApplication / CreatorVersion
    / CreatorHostOS / OriginalSize / CurrentSize / DiskGeometry(C/H/S)
    / DiskType(2=固定) / Checksum(除自身外所有字节之和的按位取反) / UUID
"""
import os
import struct
import sys
import uuid

FOOTER_SIZE = 512
SECTOR_SIZE = 512
COOKIE = b'conectix'
DISK_TYPE_FIXED = 2
# 固定的创建者标识：让 Windows 的"磁盘管理"里能看出是谁生成的
CREATOR_APP = b'EZOS'
CREATOR_HOST_OS = b'Wi2k'
# UUID 用 uuid5 由文件名派生：同一份镜像每次生成的 VHD 字节完全一致
# （ninja 依赖的是文件内容，避免每次构建都重写）
UUID_NS = uuid.UUID('6ba7b810-9dad-11d1-80b4-00c04fd430c8')


def vhd_geometry(total_sectors):
    """返回 (cylinders, heads, sectors_per_track)。

    CHS 要尽量**精确整除**总扇区数：部分工具（qemu-img 的 vpc 驱动、一些老
    的磁盘工具）是按 cyl*heads*spt 反推容量的，向下取整会让它们看到的容量
    小于 Current Size（实测 16MB 盘按 32/16/63 只报 16515072 字节）。
    我们的镜像都是 2 的幂扇区数，先找整除组合，找不到再回退到微软参考
    算法的向下取整值。"""
    for spt, heads in ((63, 16), (32, 16), (32, 8), (16, 16), (63, 4),
                       (32, 4), (16, 8), (8, 8), (4, 4), (17, 4)):
        cyl, rem = divmod(total_sectors, spt * heads)
        if rem == 0 and 1 <= cyl <= 65535:
            return cyl, heads, spt
    cyl, heads, spt = 0, 16, 63
    while True:
        cyl = total_sectors // (spt * heads)
        if cyl <= 65535:
            break
        if heads > 4:
            heads = 4
        else:
            spt = 17
            heads = 4
    return max(1, cyl), heads, spt


def vhd_timestamp(mtime):
    """VHD TimeStamp = 自 2000-01-01 00:00:00 UTC 起的秒数"""
    epoch_delta = 946684800          # 1970-01-01 -> 2000-01-01
    return int(mtime) - epoch_delta


def vhd_checksum(footer):
    """除 Checksum 字段（64..67）外所有字节之和的按位取反（32 位）"""
    s = 0
    for i in range(FOOTER_SIZE):
        if 64 <= i < 68:
            continue
        s = (s + footer[i]) & 0xFFFFFFFF
    return (~s) & 0xFFFFFFFF


def make_footer(size, mtime, name):
    """name 用**源镜像**的文件名派生 UUID：同一份镜像换个输出名再生成，
    字节也应该完全一致（否则 ninja 每次都要重写，产物不可复现）。"""
    if size <= 0 or size % SECTOR_SIZE != 0:
        raise SystemExit("image size %d is not a multiple of %d" % (size, SECTOR_SIZE))
    cyl, heads, spt = vhd_geometry(size // SECTOR_SIZE)
    f = bytearray(FOOTER_SIZE)
    f[0:8] = COOKIE
    struct.pack_into('>I', f, 8, 0x00000002)                  # Features
    struct.pack_into('>I', f, 12, 0x00010000)                 # FileFormatVersion 1.0
    struct.pack_into('>Q', f, 16, 0xFFFFFFFFFFFFFFFF)         # DataOffset（固定盘）
    struct.pack_into('>I', f, 24, vhd_timestamp(mtime))       # TimeStamp
    f[28:32] = CREATOR_APP                                    # CreatorApplication
    struct.pack_into('>I', f, 32, 0x00010000)                 # CreatorVersion 1.0
    f[36:40] = CREATOR_HOST_OS                                # CreatorHostOS
    struct.pack_into('>Q', f, 40, size)                       # OriginalSize
    struct.pack_into('>Q', f, 48, size)                       # CurrentSize
    struct.pack_into('>H', f, 56, cyl)                        # Cylinder
    f[58] = heads                                             # Heads
    f[59] = spt                                               # SectorsPerTrack
    struct.pack_into('>I', f, 60, DISK_TYPE_FIXED)            # DiskType
    struct.pack_into('>I', f, 64, 0)                          # Checksum（占位）
    f[68:84] = uuid.uuid5(UUID_NS, 'ezos-vhd/' + name).bytes  # UniqueId
    f[84] = 0                                                 # SavedState
    struct.pack_into('>I', f, 64, vhd_checksum(f))
    return bytes(f)


def parse_footer(blob):
    """解析并校验 footer，返回字段 dict；任何一项不对就抛 AssertionError"""
    assert len(blob) == FOOTER_SIZE, "footer size"
    assert blob[0:8] == COOKIE, "bad cookie %r" % blob[0:8]
    fields = {
        'features': struct.unpack_from('>I', blob, 8)[0],
        'version': struct.unpack_from('>I', blob, 12)[0],
        'data_offset': struct.unpack_from('>Q', blob, 16)[0],
        'timestamp': struct.unpack_from('>I', blob, 24)[0],
        'creator_app': bytes(blob[28:32]),
        'creator_host': bytes(blob[36:40]),
        'original_size': struct.unpack_from('>Q', blob, 40)[0],
        'current_size': struct.unpack_from('>Q', blob, 48)[0],
        'cylinder': struct.unpack_from('>H', blob, 56)[0],
        'heads': blob[58],
        'spt': blob[59],
        'disk_type': struct.unpack_from('>I', blob, 60)[0],
        'checksum': struct.unpack_from('>I', blob, 64)[0],
        'uuid': bytes(blob[68:84]),
        'saved_state': blob[84],
    }
    assert fields['version'] == 0x00010000, "file format version"
    assert fields['data_offset'] == 0xFFFFFFFFFFFFFFFF, "not a fixed VHD"
    assert fields['disk_type'] == DISK_TYPE_FIXED, "disk type %d" % fields['disk_type']
    assert fields['original_size'] == fields['current_size'], "size mismatch"
    assert fields['current_size'] % SECTOR_SIZE == 0, "size not sector aligned"
    assert 1 <= fields['spt'] <= 63 and 1 <= fields['heads'] <= 255, "geometry"
    assert 1 <= fields['cylinder'] <= 65535, "geometry"
    expect = vhd_checksum(blob)
    assert fields['checksum'] == expect, "checksum %08x != %08x" % (fields['checksum'], expect)
    return fields


def check_vhd(path):
    """校验一个 .vhd：footer 合法 + 文件大小匹配 + 头部是合法 MBR"""
    size = os.path.getsize(path)
    assert size > FOOTER_SIZE, "file too small"
    with open(path, 'rb') as f:
        f.seek(size - FOOTER_SIZE)
        footer = f.read(FOOTER_SIZE)
        f.seek(0)
        head = f.read(SECTOR_SIZE)
    fields = parse_footer(footer)
    assert fields['current_size'] == size - FOOTER_SIZE, \
        "current size %d != file size - footer (%d)" % (fields['current_size'], size - FOOTER_SIZE)
    assert head[510] == 0x55 and head[511] == 0xAA, "first sector is not an MBR (no 0x55AA)"
    nparts = sum(1 for i in range(4) if head[446 + i * 16 + 4])
    assert nparts == 1, "expected exactly 1 partition, got %d" % nparts
    ptype = head[446 + 4]
    lba = struct.unpack_from('<I', head, 446 + 8)[0]
    nsect = struct.unpack_from('<I', head, 446 + 12)[0]
    assert lba >= 1 and nsect >= 1, "bad partition LBA/length"
    assert lba + nsect <= fields['current_size'] // SECTOR_SIZE, "partition exceeds disk"
    return fields, ptype, lba, nsect


def make_vhd(src, dst):
    size = os.path.getsize(src)
    mtime = os.path.getmtime(src)
    footer = make_footer(size, mtime, os.path.basename(src))
    with open(src, 'rb') as fin, open(dst, 'wb') as fout:
        while True:
            chunk = fin.read(1024 * 1024)
            if not chunk:
                break
            fout.write(chunk)
        fout.write(footer)
    fields, ptype, lba, nsect = check_vhd(dst)
    print("OK %s: %d bytes raw -> %d bytes VHD (fixed, CHS %d/%d/%d, "
          "partition type 0x%02X @LBA %d, %d sectors)"
          % (dst, size, os.path.getsize(dst), fields['cylinder'], fields['heads'],
             fields['spt'], ptype, lba, nsect))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--check':
        if len(args) < 2:
            print("usage: make_vhd.py --check <file.vhd>")
            sys.exit(1)
        fields, ptype, lba, nsect = check_vhd(args[1])
        print("OK %s: fixed VHD, %d bytes data, CHS %d/%d/%d, "
              "partition type 0x%02X @LBA %d (%d sectors)"
              % (args[1], fields['current_size'], fields['cylinder'],
                 fields['heads'], fields['spt'], ptype, lba, nsect))
        return
    if not args:
        print("usage: make_vhd.py <input.img> [output.vhd]")
        sys.exit(1)
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + '.vhd'
    make_vhd(src, dst)


if __name__ == '__main__':
    main()
