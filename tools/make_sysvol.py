#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""make_sysvol.py - 把用户态程序打包成内核内置只读系统卷。

为什么要有这个东西
------------------
之前所有用户程序（user/*.elf）都放在 disk.vhd 的数据分区根目录里，
而 `format` 只要格式化数据盘就会把它们全部抹掉（实测：format 之后
`ls` 变空、`exec HELLO.ELF` 报 file not found）。Linux 的做法是
/bin 与 /home 分属不同挂载点，格式化数据分区不影响系统。

EZOS 这里的最小可靠实现：把系统卷做进内核镜像（.rodata），
挂载成 /system 与 /bin 两个只读路径。好处：
  1. format 绝对擦不到它（在内存里，不在任何盘上）；
  2. 不依赖"分区布局没写错"这个前提，legacy/UEFI/AHCI/NVMe 都在；
  3. 不碰现有 7 个 FS 后端的全局单例状态，回归风险接近零。

代价：镜像多 ~17KB。kernel_raw.bin 目前 ~442KB，496KB 上限还有
约 54KB 余量，够用（不够的时间点会由 linker.ld 的 ASSERT 拦下）。

用法
----
    python tools/make_sysvol.py            # 生成 kernel/sysvol_data.c

产物是纯数据（无指针），不产生任何重定位项，链接期零风险。
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USER_DIR = os.path.join(ROOT, "user")
OUT = os.path.join(ROOT, "kernel", "sysvol_data.c")

# 进 /bin 的程序（顺序即 /bin 列表顺序）
BIN_FILES = [
    "hello.elf",
    "forktest.elf",
    "fdtest.elf",
    "fdleak.elf",
    "spin.elf",
    "segprobe.elf",
    "netecho.elf",
    "nettcp.elf",
    "netcli.elf",
]

MAX_PATH = 32          # 必须与 kernel/sysvol.h 的 SYSVOL_MAX_PATH 一致
PER_LINE = 12          # 每行多少字节


def read(name):
    p = os.path.join(USER_DIR, name)
    if not os.path.isfile(p):
        sys.stderr.write("make_sysvol: missing %s\n" % p)
        sys.exit(1)
    with open(p, "rb") as f:
        return f.read()


def main():
    entries = []          # (path, bytes)
    blob = bytearray()

    for n in BIN_FILES:
        data = read(n)
        entries.append(("/bin/" + n, data))

    # /system 下放两个纯文本元信息文件，方便 shell/E2E 直接 cat 验证
    stamp = time.strftime("%Y-%m-%d")
    names = "".join(n + "\n" for n in BIN_FILES)
    version = ("EZOS built-in system volume\n"
               "mount=/system /bin\n"
               "readonly=yes\n"
               "built=%s\n" % stamp).encode("ascii")
    files = ("# /bin contents\n" + names).encode("ascii")

    entries.append(("/system/version", version))
    entries.append(("/system/files", files))

    for path, data in entries:
        if len(path) >= MAX_PATH:
            sys.stderr.write("make_sysvol: path too long: %s\n" % path)
            sys.exit(1)
        blob += data

    out = []
    out.append("/* 本文件由 tools/make_sysvol.py 自动生成，请勿手改。 */")
    out.append("/* 内核内置只读系统卷：挂载为 /system 与 /bin。 */")
    out.append("")
    out.append('#include "types.h"')
    out.append('#include "sysvol.h"')
    out.append("")
    out.append("/* 全部文件内容首尾相接。off/size 见下表。 */")
    out.append("const uint8_t sysvol_blob[] = {")
    for i in range(0, len(blob), PER_LINE):
        chunk = blob[i:i + PER_LINE]
        out.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
    out.append("};")
    out.append("")
    out.append("/* 路径一律小写、'/' 分隔。查找时大小写不敏感。 */")
    out.append("const sysvol_entry_t sysvol_table[] = {")
    off = 0
    for path, data in entries:
        out.append('    { "%s", %du, %du },' % (path, off, len(data)))
        off += len(data)
    out.append("};")
    out.append("")
    out.append("const uint32_t sysvol_count = %du;" % len(entries))
    out.append("const uint32_t sysvol_bytes = %du;" % len(blob))
    out.append("")

    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(out))

    sys.stderr.write("make_sysvol: %d files, %d bytes -> %s\n"
                     % (len(entries), len(blob), os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
