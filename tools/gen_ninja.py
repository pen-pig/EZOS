# -*- coding: utf-8 -*-
"""gen_ninja.py - 从 build.ninja 生成 build_auto.ninja（工具路径可搬迁）。

为什么要这一步：ninja 文件里没法读环境变量，而直接把 `D:/MyOS/tools/...`
写死就等于把项目钉死在 D 盘。所以真正的路径解析放在 Python 里（ezos_env.py，
唯一入口），再把 build.ninja 里的占位替换成实际路径。

替换规则：
  1. `[A-Za-z]:/MyOS`           -> 探测到的 MyOS 根（默认按项目位置推导）
  2. `CARGO   = <anything>`     -> 实际探测到的 cargo
  3. 形如 `@@cc@@` 的占位       -> ezos_env 里对应工具（给未来留的显式占位）

**必须字节级读写**（曾经踩过：PowerShell 的 Get-Content 把 UTF-8 当 GBK 解码，
中文标点尾部的孤立前导字节会吃掉后面的 0x0A，把 ninja 两行合并成
"unexpected indent"）。这里全程 rb/wb，不经过任何文本解码。
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ezos_env  # noqa: E402

# build.ninja 里工具行的前缀（保持与原文件一致的写法）
LINES = {
    "BIN_DIR": lambda: os.path.dirname(ezos_env.tool("cc")),
    "ASM":     lambda: ezos_env.tool("nasm"),
    "QEMU":    lambda: ezos_env.tool("qemu"),
    "ZIG":     lambda: ezos_env.tool("zig"),
    "CARGO":   lambda: ezos_env.cargo_exe(),
}


def build(root, src, out, skip_rustzig):
    with open(src, "rb") as f:
        data = f.read()

    # 1) MyOS 根：把任何盘符的 MyOS 统一成实际位置
    data = re.sub(rb"[A-Za-z]:/MyOS", root.replace("\\", "/").encode(), data)

    # 2) 逐行替换工具定义
    for key, fn in LINES.items():
        if key == "ZIG" and skip_rustzig:
            continue
        try:
            val = fn().replace("\\", "/")
        except SystemExit as e:
            sys.stderr.write("[ERROR] %s\n" % e)
            return 2
        pat = re.compile(rb"^%s(\s*)=.*$" % key.encode(), re.M)
        if not pat.search(data):
            sys.stderr.write("[WARN] build.ninja 里没有 %s 行，跳过\n" % key)
            continue
        data = pat.sub(lambda m: key.encode() + m.group(1) + b" = " +
                       val.encode(), data)

    with open(out, "wb") as f:
        f.write(data)
    return 0


def main():
    ap = argparse.ArgumentParser(description="generate build_auto.ninja")
    # 默认根 = 工具根的父目录（仓库在 X:\MyOS\src、工具在 X:\MyOS\tools 时就是
    # X:\MyOS）。从工具位置反推最不容易错，也不需要调用方知道布局。
    default_root = os.path.dirname(os.path.normpath(ezos_env.tools_root()))
    ap.add_argument("--root", default=default_root)
    ap.add_argument("--src", default=os.path.join(ezos_env.ROOT, "build.ninja"))
    ap.add_argument("--out", default=os.path.join(ezos_env.ROOT,
                                                   "build_auto.ninja"))
    args = ap.parse_args()
    args.root = os.path.normpath(args.root)

    skip = bool(os.environ.get("EZOS_SKIP_RUSTZIG"))
    rc = build(args.root, args.src, args.out, skip)
    if rc == 0:
        print("gen_ninja: %s (root=%s%s)" % (
            os.path.basename(args.out), args.root,
            ", rustzig skipped" if skip else ""))
    return rc


if __name__ == "__main__":
    sys.exit(main())
