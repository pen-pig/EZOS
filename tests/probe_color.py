# -*- coding: utf-8 -*-
"""一次性诊断：用像素证明"命令输出真的上色了"。

E2E 那套 OCR 只认文本，加没加颜色它看不出来——但这个交付物的本体就是颜色，
所以直接从 screendump 的 RGB PPM 里采样：VGA 文本模式调色板里
9=亮蓝(#5555FF)、10=亮绿(#55FF55)、12=亮红(#FF5555)、11=亮青(#55FFFF)、
8=深灰(#555555)、7=浅灰(#AAAAAA)。只要 ls 的行里出现亮蓝像素，
就说明目录确实被着色了。

用法：python tests/probe_color.py
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ezocr  # noqa: E402
import test_rmdir as T
from collections import Counter

PAL = {
    (0x57, 0x57, 0xFF): "亮蓝9(目录)",
    (0x57, 0xFF, 0x57): "亮绿10(成功/exe)",
    (0xFF, 0x57, 0x57): "亮红12(错误/grep命中)",
    (0x57, 0xFF, 0xFF): "亮青11(强调/表头)",
    (0xFF, 0xFF, 0x57): "黄14(警告)",
    (0x57, 0x57, 0x57): "深灰8(次要信息)",
    (0xA8, 0xA8, 0xA8): "浅灰7(正文)",
}

SEQ = [
    "format exfat",
    "mkdir SDCOLOR",
    "touch FILE.TXT",
    "ls",
    "cat NOPE.TXT",
    "rm NOPE.TXT",
    "help",
]


def pixels(ppm):
    """返回 {(r,g,b): 计数}"""
    with open(ppm, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n<max>\n
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    body = parts[3]
    cnt = Counter()
    for i in range(0, len(body) - 2, 3):
        cnt[(body[i], body[i + 1], body[i + 2])] += 1
    return cnt, w, h


def main():
    img = os.path.join(T.ROOT, "os-image.bin").replace("\\", "/")
    disk = os.path.join(HERE, "disk_color.img").replace("\\", "/")
    shutil.copyfile(os.path.join(T.ROOT, "disk.img"), disk)
    shot = os.path.join(HERE, "color_shot.ppm").replace("\\", "/")

    if T.port_in_use(T.PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        T.wait_port_free(T.PORT, 15)

    proc = subprocess.Popen([
        T.QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % T.PORT,
    ], cwd=T.ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        qmp = T.Qmp(T.PORT)
        time.sleep(T.BOOT_WAIT)
        for c in SEQ:
            T.run(qmp, c, 12.0)
            qmp.cmd("screendump", filename=shot)
            time.sleep(0.4)
            cnt, w, h = pixels(shot)
            found = []
            for rgb, cnt_n in cnt.most_common(8):
                if rgb in PAL and cnt_n > 20:
                    found.append("%s x%d" % (PAL[rgb], cnt_n))
            print("%-14s => %s" % (c, " | ".join(found) if found else "(无语义色)"))
            if os.environ.get("COLOR_DUMP"):
                top = ["%02x%02x%02x x%d" % (r, g, b, n)
                       for (r, g, b), n in cnt.most_common(8)]
                print("    top:", " | ".join(top))
    finally:
        proc.kill()
        try:
            proc.wait(timeout=15)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
