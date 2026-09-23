# -*- coding: utf-8 -*-
"""一次性校验：shell.c 加颜色前后，**字符串常量是否一个字都没改**。

本项目 6 套 E2E 用 QMP 截图 + 真字库 OCR 断言终端文本，
加颜色时只要不小心动了文案，整套回归会集体变红——所以必须机器比对。

用法：python tests/check_strings.py <旧文件> <新文件>
     例如 git show HEAD:kernel/shell.c > temp/old.c && python tests/check_strings.py temp/old.c kernel/shell.c
"""
import re
import sys
from collections import Counter

PAT = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def lits(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        s = f.read()
    return PAT.findall(s)


def main():
    if len(sys.argv) < 3:
        print("usage: check_strings.py OLD NEW")
        return 2
    a, b = lits(sys.argv[1]), lits(sys.argv[2])
    ca, cb = Counter(a), Counter(b)
    print("HEAD=%d 常亮, NEW=%d 常亮" % (len(a), len(b)))
    removed, added = ca - cb, cb - ca
    print("--- 被删改的字符串 ---")
    for k in removed:
        print("   ", k)
    print("--- 新增的字符串 ---")
    for k in added:
        print("   ", k)
    if not removed and not added:
        print("OK: 全部字符串常量零改动")
    return 0


if __name__ == '__main__':
    sys.exit(main())
