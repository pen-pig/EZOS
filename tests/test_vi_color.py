# -*- coding: utf-8 -*-
"""vi 语法高亮按扩展名分流的颜色回归（C++ / Python）。

颜色只能用像素验证（OCR 看不出颜色，见 tests/README.md 铁律）：
    建 TEST.CPP / TEST.PY -> vi 重新打开 -> screendump -> 逐字符单元采样 RGB

断言点（VGA 文本 80x25，字符单元 9x16 像素，取单元中心）：
    C++ : class 蓝 / 标识符灰 / // 注释暗灰 / "字符串" 绿 / 数字红
    PY  : def 蓝 / def 后名字青 / # 注释暗灰 / "字符串" 绿 / 数字红

用法：python tests/test_vi_color.py   （退出码 0 = 全通过）
端口 4483（端口每脚本唯一，见 tests/README.md）。
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rmdir as T
from test_rmdir import Qmp, ROOT, QEMU  # noqa: E402

PORT = 4483
SHOT_CPP = os.path.join(HERE, "vicolor_cpp.ppm").replace("\\", "/")
SHOT_PY = os.path.join(HERE, "vicolor_py.ppm").replace("\\", "/")

# sendkey 键名（test_rmdir.KEYMAP 之外的符号补充）
KEYS = {' ': 'spc', '.': 'dot', ',': 'comma', '-': 'minus', '_': 'shift-minus',
        '/': 'slash', ';': 'semicolon', ':': 'shift-semicolon',
        "'": 'apostrophe', '"': 'shift-apostrophe',
        '#': 'shift-3', '@': 'shift-2',
        '(': 'shift-9', ')': 'shift-0',
        '{': 'shift-bracket_left', '}': 'shift-bracket_right',
        '=': 'equal', '!': 'shift-1', '*': 'shift-8'}

# VGA 0x57 系调色板（screendump PPM 实测值，见 tests/README.md）
BLUE = (0x57, 0x57, 0xFF)
GREEN = (0x57, 0xFF, 0x57)
RED = (0xFF, 0x57, 0x57)
CYAN = (0x57, 0xFF, 0xFF)
GREY = (0xA8, 0xA8, 0xA8)
DARKGREY = (0x57, 0x57, 0x57)   # VGA 8 号色，screendump 实测 0x57 系

RESULTS = []


def check(ok, label):
    RESULTS.append((bool(ok), label))
    print(("PASS " if ok else "FAIL ") + label)


def key_for(ch):
    if ch in KEYS:
        return KEYS[ch]
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    return ch.lower()


def vi_type(qmp, text, enter=True):
    """vi 插入模式逐键输入（不经过 shell 行编辑）。"""
    for ch in text:
        r = qmp.hmc("sendkey " + key_for(ch))
        if "error" in r:
            raise RuntimeError("sendkey %r failed: %r" % (key_for(ch), r))
        time.sleep(0.05)
    if enter:
        qmp.hmc("sendkey ret")
        time.sleep(0.1)


def ppm_pixel(path, x, y):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n720 400\n255\n -> 二进制 RGB
    idx = 0
    for _ in range(3):
        nl = data.index(b"\n", idx)
        idx = nl + 1
    off = (y * 720 + x) * 3
    return (data[off], data[off + 1], data[off + 2])


def cell_color(path, row, col):
    """字符单元 (row, col) 的前景色：9x16 单元内非黑像素的主色。
    （取单元中心单点会踩到字形空心，如 'c' 的开口。）"""
    with open(path, "rb") as f:
        data = f.read()
    counts = {}
    for y in range(row * 16, row * 16 + 16):
        for x in range(col * 9, col * 9 + 9):
            off = (y * 720 + x) * 3
            p = (data[off], data[off + 1], data[off + 2])
            if p == (0, 0, 0):
                continue
            counts[p] = counts.get(p, 0) + 1
    if not counts:
        return (0, 0, 0)
    return max(counts.items(), key=lambda kv: kv[1])[0]


def close(a, b, tol=0x18):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def make_file(qmp, name, lines):
    """用 vi 新建文件并写入多行内容，:wq 存盘退出。"""
    T.run(qmp, "vi " + name, 10.0)
    time.sleep(0.4)
    qmp.hmc("sendkey i")
    time.sleep(0.3)
    for i, ln in enumerate(lines):
        vi_type(qmp, ln, enter=(i < len(lines) - 1))
    qmp.hmc("sendkey esc")
    time.sleep(0.3)
    qmp.hmc("sendkey shift-semicolon")
    time.sleep(0.1)
    qmp.hmc("sendkey w")
    time.sleep(0.1)
    qmp.hmc("sendkey q")
    time.sleep(0.1)
    qmp.hmc("sendkey ret")
    time.sleep(1.0)


def dump_vi(qmp, name, shot):
    T.run(qmp, "vi " + name, 10.0)
    time.sleep(0.8)
    qmp.cmd("screendump", filename=shot)
    time.sleep(0.4)
    # 退出（未修改，:q 即可）
    qmp.hmc("sendkey shift-semicolon")
    time.sleep(0.1)
    qmp.hmc("sendkey q")
    time.sleep(0.1)
    qmp.hmc("sendkey ret")
    time.sleep(0.8)


def main():
    disk = os.path.join(HERE, "disk_vicolor.img").replace("\\", "/")
    shutil.copyfile(os.path.join(ROOT, "disk.img"), disk)
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")

    if T.port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        T.wait_port_free(PORT, 15)

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    CPP_LINES = ["class Foo {", "// note", "\"str\"", "42"]
    PY_LINES = ["def foo():", "# note", "\"str\"", "x = 42"]

    try:
        qmp = Qmp(PORT)
        time.sleep(T.BOOT_WAIT)

        make_file(qmp, "TEST.CPP", CPP_LINES)
        make_file(qmp, "TEST.PY", PY_LINES)

        # 内容保底：cat 回读（OCR 宽松断言关键单词）
        out_cpp = T.run(qmp, "cat TEST.CPP", 10.0)
        check("class" in out_cpp and "note" in out_cpp, "cpp file content saved")
        out_py = T.run(qmp, "cat TEST.PY", 10.0)
        check("def" in out_py and "note" in out_py, "py file content saved")

        # C++ 高亮
        dump_vi(qmp, "TEST.CPP", SHOT_CPP)
        probes = [("cpp class kw", 0, 0, BLUE), ("cpp ident grey", 0, 6, GREY),
                  ("cpp // comment", 1, 0, DARKGREY), ("cpp string", 2, 0, GREEN),
                  ("cpp number", 3, 0, RED)]
        for label, row, col, want in probes:
            got = cell_color(SHOT_CPP, row, col)
            print("  probe %s (%d,%d): got %s want %s" % (label, row, col, got, want))
            check(close(got, want), "%s color" % label)

        # Python 高亮
        dump_vi(qmp, "TEST.PY", SHOT_PY)
        probes = [("py def kw", 0, 0, BLUE), ("py def name cyan", 0, 4, CYAN),
                  ("py # comment", 1, 0, DARKGREY), ("py string", 2, 0, GREEN),
                  ("py number", 3, 4, RED)]
        for label, row, col, want in probes:
            got = cell_color(SHOT_PY, row, col)
            print("  probe %s (%d,%d): got %s want %s" % (label, row, col, got, want))
            check(close(got, want), "%s color" % label)

        # 退出 vi 后 shell 仍可用
        out = T.run(qmp, "ver", 10.0)
        check(len(out) > 0, "shell alive after vi")
    finally:
        proc.kill()
        try:
            proc.wait(timeout=15)
        except Exception:
            pass

    bad = [l for ok, l in RESULTS if not ok]
    print("== vi color (cpp/py): %d/%d passed ==" % (len(RESULTS) - len(bad), len(RESULTS)))
    if bad:
        print("failed: " + "; ".join(bad))
        sys.exit(1)


if __name__ == "__main__":
    main()
