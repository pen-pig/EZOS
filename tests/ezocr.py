# -*- coding: utf-8 -*-
"""ezocr.py - VGA 文本模式截图 OCR（真字库像素匹配）

原理：内核启动时把 kernel/vga_font.h 的 8x16 字体写入 VGA plane 2
（gfx_text_font_init），因此文本模式屏幕上的字形与该字体逐位一致。
对 720x400（80x25，9x16 字符单元）的 screendump PPM 逐单元取样，
以"单元内最多见的颜色为背景"生成位图，与全部 256 个字形做汉明距离
匹配，取最近者。

公开接口：ocr_text(ppm_path, kernel_dir) -> str
"""
import re

_FONT = None


def _load_font(kernel_dir):
    global _FONT
    if _FONT is not None:
        return _FONT
    path = kernel_dir.rstrip("/\\") + "/vga_font.h"
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", txt)]
    if len(vals) < 4096:
        raise RuntimeError("vga_font.h: only %d bytes" % len(vals))
    glyphs = []
    for c in range(256):
        rows = vals[c * 16:(c + 1) * 16]
        bits = tuple(tuple((row >> (7 - x)) & 1 for x in range(8))
                     for row in rows)
        glyphs.append(bits)
    _FONT = glyphs
    return glyphs


def _read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 头：magic、width、height、maxval，空白分隔，然后二进制像素
    m = re.match(rb"(P6)\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise RuntimeError("not a P6 ppm: %s" % path)
    w, h = int(m.group(2)), int(m.group(3))
    off = m.end()
    return w, h, data[off:off + w * h * 3]


def _match_cell(bits, glyphs):
    """bits: 16 行 x 8 列的 0/1 元组，返回最佳字符"""
    best = None
    best_d = 999
    for c in range(32, 127):          # 可打印 ASCII 足够覆盖终端输出
        g = glyphs[c]
        d = 0
        for y in range(16):
            gr = g[y]
            br = bits[y]
            for x in range(8):
                if gr[x] != br[x]:
                    d += 1
                    if d >= best_d:
                        break
            if d >= best_d:
                break
        if d < best_d:
            best_d = d
            best = c
    return chr(best) if best else " "


def ocr_text(ppm_path, kernel_dir):
    glyphs = _load_font(kernel_dir)
    w, h, px = _read_ppm(ppm_path)
    if w < 720 or h < 400:
        return ""
    cols, rows = 80, 25
    cw, ch = 9, 16
    out_lines = []
    for r in range(rows):
        line = []
        for c in range(cols):
            x0, y0 = c * cw, r * ch
            # 收集 8x16 像素（跳过第 9 列），统计颜色
            colors = {}
            coords = []
            for y in range(16):
                for x in range(8):
                    p = ((y0 + y) * w + (x0 + x)) * 3
                    col = (px[p], px[p + 1], px[p + 2])
                    coords.append(col)
                    colors[col] = colors.get(col, 0) + 1
            # 背景 = 单元内最多见的颜色
            bg = max(colors, key=lambda k: colors[k])
            bits = tuple(tuple(1 if coords[y * 8 + x] != bg else 0
                               for x in range(8)) for y in range(16))
            # 空单元快速路径
            if all(b == 0 for row in bits for b in row):
                line.append(" ")
            else:
                line.append(_match_cell(bits, glyphs))
        out_lines.append("".join(line).rstrip())
    return "\n".join(out_lines)
