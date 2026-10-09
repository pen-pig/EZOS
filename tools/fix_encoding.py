# -*- coding: utf-8 -*-
r"""fix_encoding.py - 还原被 GBK/UTF-8 来回碾过的中文注释

背景（为什么会有这个工具）
--------------------------
这些文件的注释经历过两次错误转码：

  1. 原始注释是 UTF-8。有人用 GBK 去读它 → 每个字节变成一个汉字
     （"串口" -> "涓叉..."），又按 UTF-8 存回磁盘。**这一步可逆**：
     乱码汉字 ``.encode('gbk')`` 正好得到原来的 UTF-8 字节，再
     ``.decode('utf-8')`` 就是原文。
  2. 反过来，用 UTF-8 去读 GBK 注释时，对不上 UTF-8 结构的字节被替换成
     U+FFFD。**这一步不可逆**，那几个字真的丢了（留下一串 U+FFFD，或者
     被后续 GBK 误读显示成"拷"字乱码）。

所以本工具只做**确定可逆**的那部分：

  * 文件里的非法 UTF-8 字节段（GBK 残留）-> 按 GBK 解码还原成中文；
  * 由"UTF-8 被当 GBK 读"产生的**乱码汉字段** -> 逐段做 gbk/utf-8 往返
    还原。正常中文做这个往返必然失败（汉字 GBK 双字节几乎不可能构成合法
    UTF-8 序列），所以不会误伤本来就正确的注释。

U+FFFD 还原不了，本工具原样保留并统计出来，交给人工重写。

用法
----
    python tools/fix_encoding.py --check           # 只报告，不写盘
    python tools/fix_encoding.py                   # 还原并写盘（原地）
    python tools/fix_encoding.py kernel/tty.c      # 只处理指定文件

字节级安全：读/写都走 rb/wb，行尾（CRLF/LF）与非注释内容逐字节保留。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SUFFIX = (".c", ".h", ".asm", ".inc", ".py", ".sh", ".md", ".ninja",
          ".ld", ".bat", ".txt", ".json", ".yml", ".yaml")
SKIP_DIRS = (".git", "temp", "__pycache__", ".workbuddy", "vmware",
             "release", "archive")

# 段的范围：CJK/全角 + U+FFFD。
# **必须把 U+FFFD 包进段里**：真实的损坏是"原始 UTF-8 字节流里某些字节被
# 换成了 U+FFFD"，U+FFFD 一隔开，剩下的乱码汉字就是半个 UTF-8 序列，逐段
# 往返必然失败（实测第一版把 U+FFFD 排除在段外，一个字都没还原出来）。
CJK_RUN = re.compile(
    "[" "\u4e00-\u9fff"          # CJK 统一表意文字
    "\u3000-\u303f"              # 中文标点
    "\uff01-\uff5e"              # 全角 ASCII/标点（含 ，。（）等）
    "\u2e80-\u2eff"              # CJK 部首补充
    "\u3400-\u4dbf"              # 扩展 A
    "\ufffd"                     # 真丢字，占位用
    "]+")

SURROGATE_RUN = re.compile("[\ufffd\udc80-\udcff]+")


def _unwrap(m):
    """把 surrogateescape 解出来的非法字节段按 GBK 还原。

    U+FFFD 会打断 surrogate 段（它是合法 UTF-8），所以这里按 U+FFFD 切分，
    各子段分别还原，U+FFFD 原样保留。
    """
    seg = m.group(0)
    out = []
    for part in seg.split("\ufffd"):
        if not part:
            out.append("\ufffd")
            continue
        raw = part.encode("utf-8", "surrogateescape")
        try:
            out.append(raw.decode("gbk"))
        except (UnicodeDecodeError, LookupError):
            out.append(part)      # 还原不了就保持原样
        out.append("\ufffd")
    out.pop()                     # 末尾多补的那个
    return "".join(out)


def _roundtrip(m):
    """乱码段 -> 字节级重建。

    段里每个字符都是"原始 UTF-8 的一个字节被 GBK 读出来的汉字"，所以把
    它们逐个 ``encode('gbk')`` 拼回去，就还原出了原始的 UTF-8 字节流；
    U+FFFD 表示那几个字节真丢了，用 U+FFFD 自身的 UTF-8 占位，位置不变。

    安全阀：**还原后 U+FFFD 的数量不能变多**。正常中文注释走这个流程必然
    产生一堆 U+FFFD（汉字 GBK 双字节几乎不可能凑成合法 UTF-8），会被引擎
    判为失败而原样保留——所以本来就对的注释不会被"修"坏。
    """
    seg = m.group(0)
    lost = seg.count("\ufffd")
    buf = bytearray()
    for ch in seg:
        if ch == "\ufffd":
            buf += b"\xef\xbf\xbd"
            continue
        try:
            buf += ch.encode("gbk")
        except UnicodeEncodeError:
            return seg
    try:
        back = bytes(buf).decode("utf-8", "replace")
    except UnicodeDecodeError:
        return seg
    if back.count("\ufffd") > lost:
        return seg                      # 越修越坏 = 这本来是正常中文，不动
    if not any("\u4e00" <= ch <= "\u9fff" for ch in back):
        return seg
    return back


def fix_text(s):
    """s 是 surrogateescape 解码后的文本，返回还原后的文本。"""
    s = SURROGATE_RUN.sub(_unwrap, s)
    s = CJK_RUN.sub(_roundtrip, s)
    return s


def fix_file(path, write=True):
    with open(path, "rb") as f:
        data = f.read()
    s = data.decode("utf-8", "surrogateescape")
    before_bad = s.count("\ufffd")
    new = fix_text(s)
    after_bad = new.count("\ufffd")
    changed = (new != s)
    if changed and write:
        out = new.encode("utf-8", "surrogateescape")
        with open(path, "wb") as f:
            f.write(out)
    return {
        "path": path,
        "changed": changed,
        "fffd_before": before_bad,
        "fffd_after": after_bad,
        "bad_bytes_before": sum(1 for ch in s if 0xDC80 <= ord(ch) <= 0xDCFF),
        "bad_bytes_after": sum(1 for ch in new if 0xDC80 <= ord(ch) <= 0xDCFF),
    }


def targets(only=None):
    if only:
        return [os.path.normpath(p) for p in only]
    out = []
    for dp, dns, fns in os.walk(ROOT):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        rel = os.path.relpath(dp, ROOT)
        if rel != "." and rel.split(os.sep)[0] in SKIP_DIRS:
            continue
        for fn in fns:
            if fn.endswith(SUFFIX):
                out.append(os.path.join(dp, fn))
    return sorted(out)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    check = "--check" in argv
    files = targets(args)
    total = {"changed": 0, "fffd": 0, "bytes": 0}
    for p in files:
        try:
            r = fix_file(p, write=not check)
        except OSError as e:
            print("SKIP %s (%s)" % (p, e))
            continue
        if r["changed"] or r["fffd_after"] or r["bad_bytes_after"]:
            print("%-40s fffd %d->%d  badbytes %d->%d%s"
                  % (os.path.relpath(p, ROOT), r["fffd_before"],
                     r["fffd_after"], r["bad_bytes_before"],
                     r["bad_bytes_after"],
                     "  (dry-run)" if check and r["changed"] else ""))
            total["changed"] += 1 if r["changed"] else 0
        total["fffd"] += r["fffd_after"]
        total["bytes"] += r["bad_bytes_after"]
    print("\n%d file(s) scanned, %d changed, %d U+FFFD left, %d invalid byte(s) left"
          % (len(files), total["changed"], total["fffd"], total["bytes"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
