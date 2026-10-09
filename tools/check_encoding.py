# -*- coding: utf-8 -*-
r"""check_encoding.py - 仓库里不许再有乱码（静态检查，不跑 QEMU）

为什么需要
----------
这批源文件经历过一次 GBK <-> UTF-8 的来回转码：用 UTF-8 去读 GBK 写的中文
时，对不上 UTF-8 结构的字节被替换成 U+FFFD（真丢字，不可逆）；反过来用 GBK
去读 UTF-8，每个字节变成一个汉字（"串口" -> "涓叉"）。表现就是注释里一串
问号方块，或者一堆不成词的汉字。

2026-10-09 把 9 个文件（shell.c / kernel.c / gfx.c / tty.c / keyboard.c /
isr.c / boot.asm / kernel_entry.asm / uefi/main.c）共 4001 处乱码注释全部
重写了一遍，仓库归零。这个文件就是**防止它复发**的闸门：

  * 文件内容必须是合法 UTF-8（不允许残留 GBK 字节）；
  * 不允许出现 U+FFFD（替换字符 = 字节已经被丢过一次）；
  * 不允许出现"拷"字乱码（U+FFFD 的 UTF-8 字节被当 GBK 读出来的形态，
    同样是丢过字的证据；见下面的 MOJIBAKE_MARKERS）。

它跑得很快（几百个文件、纯字节扫描），所以进了 run_tests 的 static 层，
每次回归都会过一遍。手动跑：``python tools/check_encoding.py``。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SUFFIX = (".c", ".h", ".asm", ".inc", ".py", ".sh", ".md", ".ninja",
          ".ld", ".bat", ".txt", ".json", ".yml", ".yaml")

# 不扫生成物、第三方与临时目录
SKIP_DIRS = {".git", "temp", "__pycache__", ".workbuddy", "vmware",
             "archive", "release", "build"}

# U+FFFD 的 UTF-8 字节 EF BF BD 被当 GBK 读出来的样子（用转义写，
# 免得这份检查把自己文档里的示例当成真乱码报出来）
MOJIBAKE_MARKERS = ("\u951f\u65a4\u6320", "\u951f\u77eb", "\u65a4\u6320")


def iter_files():
    for dp, dns, fns in os.walk(ROOT):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        rel = os.path.relpath(dp, ROOT)
        if rel != "." and rel.split(os.sep)[0] in SKIP_DIRS:
            continue
        for fn in fns:
            if fn.endswith(SUFFIX):
                yield os.path.join(dp, fn)


def main():
    problems = []
    scanned = 0
    for path in sorted(iter_files()):
        with open(path, "rb") as f:
            data = f.read()
        scanned += 1
        rel = os.path.relpath(path, ROOT)
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as e:
            problems.append("%s: 不是合法 UTF-8（%s，多半是 GBK 字节残留）"
                            % (rel, e.reason))
            continue
        n = text.count("\ufffd")
        if n:
            problems.append("%s: %d 处 U+FFFD（字节已被丢弃，不可逆，只能重写）"
                            % (rel, n))
        for mark in MOJIBAKE_MARKERS:
            if mark in text:
                problems.append("%s: 含乱码片段 %r（U+FFFD 的 GBK 形态）"
                                % (rel, mark))

    if problems:
        print("编码检查失败：%d 个文件有问题（共扫 %d 个）" % (len(problems),
                                                            scanned))
        for p in problems:
            print("  " + p)
        print("\n修法：乱码字节已经丢失，无法还原，只能按上下文重写那几行注释；"
              "tools/fix_encoding.py 能救回其中可逆的一部分。")
        return 1
    print("编码检查通过：%d 个文件全部是合法 UTF-8，无 U+FFFD、无乱码片段"
          % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
