# -*- coding: utf-8 -*-
"""check_help.py - 命令表里的每条命令都必须有 help 明细条目

为什么要有这个闸门
------------------
`help <cmd>` 查不到用法是静默失败：命令确实存在、敲它也能跑，但用户敲
`help fsck` 只会看到空输出——文档和实现对不上，而且没有任何测试会发现。

2026-10-09 全库审计时用脚本做了一次差集，26 条命令（fsck / desktop /
crc32c / dhcp / ping ... ）没有 help 条目。补齐之后必须有东西拦住"以后
新增命令忘了登记"，否则过两个提交就又漂回去。

判定办法（纯静态，不跑 QEMU）
--------------------------
1. 从 kernel/shell.c 与 kernel/shell_extra.c 里抽出命令表项
   `{"name", cmd_xxx}` —— 这是 shell 真正认的命令集合；
2. 从 kernel/shell_extra.c 的 help 明细分支里抽出 `x_strcasecmp(cmd,"name")`；
3. 两者差集必须为空。反向差集（help 里有、命令表里没有）也报——那是
   已经删掉的命令留下的僵尸条目。

退出码 0 = 通过。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CMD_RE = re.compile(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*cmd_[A-Za-z0-9_]+\s*\}')
HELP_RE = re.compile(r'x_strcasecmp\(\s*cmd\s*,\s*"([A-Za-z0-9_]+)"\s*\)')

SHELL_FILES = [os.path.join(ROOT, "kernel", "shell.c"),
               os.path.join(ROOT, "kernel", "shell_extra.c")]
HELP_FILE = os.path.join(ROOT, "kernel", "shell_extra.c")


def read(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8", "replace")


def main():
    cmds = set()
    for p in SHELL_FILES:
        cmds |= set(CMD_RE.findall(read(p)))
    helps = set(HELP_RE.findall(read(HELP_FILE)))

    if not cmds:
        print("check_help: 没能从命令表里抽到任何命令（正则失配？）")
        return 1

    missing = sorted(cmds - helps)
    zombie = sorted(helps - cmds)
    problems = []

    if missing:
        problems.append("没有 help 明细的命令（%d）：%s"
                        % (len(missing), " ".join(missing)))
    if zombie:
        problems.append("help 里存在但命令表里没有（%d）：%s"
                        % (len(zombie), " ".join(zombie)))

    if problems:
        print("check_help: FAIL")
        for p in problems:
            print("  " + p)
        return 1

    print("check_help: OK（%d 条命令全部有 help 明细）" % len(cmds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
