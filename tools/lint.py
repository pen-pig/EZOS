# -*- coding: utf-8 -*-
"""lint.py - 极简 C 静态扫描（编译器的补充，专挑 GCC 不管的坑）。

为什么自己写而不是上 cppcheck/clang-tidy：
  1) 交叉工具链是 i686-elf-gcc，clang-tidy 没有对应的 freestanding 配置，
     cppcheck 需要额外下载；
  2) -Wall -Wextra 只管类型和作用域，管不了"分配了不检查""复制长度写错"；
  3) 这套规则是这个项目踩出来的（strcpy 家族 0 处是因为早就禁了，
     要防止有人手滑加回来）。

规则分两级：ERROR 会让退出码非 0（接 CI），WARN 只打印。

用法：
    python tools/lint.py                 # 扫 kernel/ + user/
    python tools/lint.py kernel/net.c    # 扫指定文件
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ERROR：这个仓库明令禁止。裸机没有 libc，越界就是灾难，且历史上有真实事故。
BANNED = [
    (r"\bstrcpy\s*\(", "strcpy: 无长度上界，用 x_strncpy/带长度的拷贝"),
    (r"\bstrcat\s*\(", "strcat: 无长度上界，用带剩余空间的拼接"),
    (r"(?<![a-z_])sprintf\s*\(", "sprintf: 用 snprintf（缓冲区大小必须显式给）"),
    (r"\bvsprintf\s*\(", "vsprintf: 用 vsnprintf"),
    (r"(?<![a-z_])gets\s*\(", "gets: 无法限制输入长度"),
    (r"\balloca\s*\(", "alloca: 内核栈只有 16KB，可变长栈分配会炸"),
]

# WARN：可疑但不必然是 bug，需要人看一眼。
WARN_PATTERNS = [
    (r"\bkmalloc\s*\(", None),   # 单独处理（配合返回值检查）
]

# 分配点：变量名 = kmalloc(...)，要求后面若干行内出现空检查
ALLOC_RE = re.compile(
    r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*=\s*(?:\([^)]*\)\s*)?"
    r"(kmalloc|pmm_alloc_pages|pmm_alloc_page)\s*\(")
MEMCPY_SIZEOF_PTR_RE = re.compile(
    r"\b(?:memcpy|x_memcpy)\s*\([^,]+,\s*[^,]+,\s*sizeof\s*\(\s*[A-Za-z_]\w*\s*\*\s*\)\s*\)")
TODO_RE = re.compile(r"\b(TODO|FIXME|XXX|HACK)\b")

SKIP_DIRS = {"temp", ".workbuddy", ".git", "build", "__pycache__"}


def read_lines(path):
    with open(path, "rb") as f:
        blob = f.read()
    # 仓库里有 10 个文件的中文注释被 GBK->UTF-8 转换永久损坏（U+FFFD），
    # 这里只匹配 ASCII 标识符，用 replace 容错即可，不因编码中断扫描。
    return blob.decode("utf-8", "replace").splitlines()


def strip_comments(line, in_block):
    """返回 (去掉注释后的代码, 是否仍在块注释内)。字符串字面量内的 /* 不算。"""
    out = []
    i = 0
    while i < len(line):
        c = line[i]
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                i += 2
            else:
                i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if c in "\"'":
            q = c
            out.append(c)
            i += 1
            while i < len(line):
                if line[i] == "\\":
                    out.append(line[i:i + 2])
                    i += 2
                    continue
                if line[i] == q:
                    out.append(q)
                    i += 1
                    break
                out.append(line[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block


def scan_file(path):
    errors = []
    warns = []
    lines = read_lines(path)
    in_block = False
    code_lines = []
    for ln in lines:
        stripped, in_block = strip_comments(ln, in_block)
        code_lines.append(stripped)

    for idx, code in enumerate(code_lines, 1):
        for pat, msg in BANNED:
            if re.search(pat, code):
                errors.append((idx, msg, lines[idx - 1].strip()[:90]))
        if MEMCPY_SIZEOF_PTR_RE.search(code):
            warns.append((idx, "memcpy 长度写成 sizeof(指针)=4，多半漏了 * 或元素大小",
                          lines[idx - 1].strip()[:90]))
        m = ALLOC_RE.search(code)
        if m:
            var = m.group(1)
            # 同一行就检查了也算（if (!(p = kmalloc(...)))）
            same_line_ok = re.search(r"!\s*\(?\s*" + re.escape(var), code)
            if not same_line_ok:
                window = "\n".join(code_lines[idx:idx + 8])
                v = re.escape(var)
                checked = (
                    # p == NULL / p != NULL / !p（含 return !p、assert(!p)）
                    re.search(r"\b" + v + r"\b\s*(?:==|!=)\s*(?:NULL|0)\b", window)
                    or re.search(r"if\s*\(\s*!?\s*\(?\s*" + v + r"\b", window)
                    or re.search(r"!\s*\(?\s*" + v + r"\b", window)
                    # 直接把指针当条件用：if (p) / while (p) / p && q
                    or re.search(r"\b(?:if|while)\s*\(\s*\(?\s*" + v + r"\b\s*\)", window)
                    or re.search(r"\b" + v + r"\b\s*(?:&&|\|\|)", window)
                )
                if not checked:
                    warns.append((idx, "分配结果 %r 在之后 8 行内没看到空检查" % var,
                                  lines[idx - 1].strip()[:90]))
        t = TODO_RE.search(code)
        if t:
            warns.append((idx, "%s 标记" % t.group(1), lines[idx - 1].strip()[:90]))
    return errors, warns


def iter_sources(targets):
    if targets:
        for t in targets:
            p = t if os.path.isabs(t) else os.path.join(ROOT, t)
            if os.path.isfile(p):
                yield p
        return
    for base in ("kernel", "user"):
        d = os.path.join(ROOT, base)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith((".c", ".h")):
                yield os.path.join(d, name)


def main():
    targets = sys.argv[1:]
    n_err = n_warn = n_files = 0
    for path in iter_sources(targets):
        n_files += 1
        errors, warns = scan_file(path)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        for ln, msg, src in errors:
            n_err += 1
            print("ERROR %s:%d  %s" % (rel, ln, msg))
            print("      | %s" % src)
        for ln, msg, src in warns:
            n_warn += 1
            print("WARN  %s:%d  %s" % (rel, ln, msg))
            print("      | %s" % src)
    print("--")
    print("scanned %d files: %d error(s), %d warning(s)" % (n_files, n_err, n_warn))
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
