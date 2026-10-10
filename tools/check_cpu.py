#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_cpu.py - 非 C 语言产物里的 CPU 指令集闸门。

内核只初始化 x87（kernel/fpu.c 的 fninit），**从不设 CR4.OSFXSR，
也不保存任何 SSE 上下文**——任务切换保存的是通用寄存器 + FPU 环境，
不含 XMM0-15。所以内核代码里出现任何 SSE/SSE2 指令就是必然 #UD。

这不是理论风险，是实测踩过的坑：Zig 侧 `levenshtein_zig` 里一个
`[25]u16 = undefined` 的数组清零，被 baseline 档编成 6 条 movdqa，
`rstest` 一跑就 panic（vector 06 Invalid Opcode）。Rust 侧一直用
target features "-mmx,-sse" 关着，只有 Zig 侧漏了。

于是有了这道闸门：扫最终内核里 Rust/Zig 两个产物的全部反汇编，
发现 SSE/SSE2 指令就红。比对的是**产物**而不是构建脚本——脚本被
改回去、或者换了个别的开关把 SSE 放进来，产物这一侧照样抓得到。

用法：
    python tools/check_cpu.py            # 检查
    python tools/check_cpu.py --quiet    # 只在失败时输出（回归用）
退出码 0 = 通过。
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _find_objdump():
    """找一个能反汇编 i386 的 objdump。

    开发机用 i686-elf-tools 里的专用版本；CI 没有那个 800MB 的包，走
    gcc-multilib 自带的 objdump（靠 -m i386 出 32 位视图，效果等价）。
    返回 (路径, 额外参数列表)。
    """
    exe = ".exe" if os.name == "nt" else ""
    bases = ["D:/MyOS/tools/i686-elf-tools-windows/bin",
             "/d/MyOS/tools/i686-elf-tools-windows/bin",
             os.path.join(ROOT, "..", "tools", "i686-elf-tools-windows", "bin")]
    for b in bases:
        cand = os.path.join(b, "i686-elf-objdump" + exe)
        if os.path.isfile(cand):
            return cand, []
    # 系统 objdump（CI 的 binutils）
    for name in ("i686-elf-objdump", "objdump"):
        cand = shutil.which(name)
        if cand:
            return cand, ["-m", "i386"]
    return None, []


OBJDUMP, OBJDUMP_ARGS = _find_objdump()

# 检查对象：只查外来语言产物。内核自己的 C 代码由 CFLAGS 里的
# -mno-sse -mno-sse2 -mno-mmx 约束着（见 build.ninja），不需要这里重复查。
TARGETS = [
    ("rust/ezos_zig/ezos_zig.o", "Zig"),
    ("rust/ezos_rs/target/i686-ezos/release/libezos_rs.a", "Rust"),
]

# SSE/SSE2 指令助记符（含 movdqa/movaps/movups/movdqu 这些"搬 XMM"的，
# 以及 pack/unpack/shuf/pshufd/pxor/pand/paddb/pmovmskb 等）。
# 只列 x86-64 上已废弃或低效、不该出现在 i686 目标里的那类，
# 不做全量表——宁可多一个假阳性，也不要漏一个真阳性。
SSE_PAT = re.compile(
    r"\b(movdq[au]|movap[sd]|movup[sd]|movnt[di]|maskmov[qd]|"
    r"ldmxcsr|stmxcsr|sfence|lfence|mfence|emms|"
    r"p[a-z]{1,4}(?:[bwdq]|ss|us)\b|"
    r"pshuf[bdw]|pslldq|psrldq|palignr|pextr[bdwq]|pinsr[bdw]|"
    r"pextrw|insertps|roundp[sd]|blendp[sd]|dp[sd]|rounds[sd]|"
    r"cvtp[sd]2|cvtt?s[sd]2|comis[sd]|ucomis[sd]|"
    r"haddp[sd]|hsubp[sd]|addsubp[sd]|movddup|movshdup|movsldup)\b",
    re.I,
)


def objdump_all(path):
    """返回反汇编文本；工具缺失或目标不存在时返回 None。"""
    if OBJDUMP is None or not os.path.isfile(path):
        return None
    try:
        r = subprocess.run([OBJDUMP] + OBJDUMP_ARGS + ["-d", path],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=180)
    except Exception:
        return None
    return r.stdout.decode("utf-8", "replace")


def scan(path, label):
    text = objdump_all(path)
    if text is None:
        return None
    hits = []
    cur = "<unknown>"
    for line in text.splitlines():
        # "00001234 <levenshtein_zig+0xab0>:" 这种函数头
        m = re.match(r"^[0-9a-fA-F]+\s+<([^>]+)>:", line)
        if m:
            cur = m.group(1)
            continue
        # " 1368434:\t66 0f 7f 44 24 30 \tmovdqa %xmm0,0x30(%esp)"
        m2 = re.match(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-f]{2} )+\s*\t(\S+)", line)
        if not m2:
            m2 = re.match(r"^\s*[0-9a-fA-F]+:\s*\t(\S+)", line)
        if not m2:
            continue
        mnem = m2.group(1)
        if SSE_PAT.search(mnem):
            hits.append((cur, mnem, line.strip()[:100]))
    return hits


def main():
    quiet = "--quiet" in sys.argv
    if OBJDUMP is None:
        print("check_cpu: 找不到能反汇编 i386 的 objdump"
              "（装 i686-elf-tools，或让 CI 装 gcc-multilib）")
        return 1

    print("check_cpu: 非 C 语言产物的指令集闸门")
    bad = 0
    skipped = []
    for rel, label in TARGETS:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            skipped.append("%s（%s 未构建）" % (label, rel))
            continue
        hits = scan(path, label)
        if hits is None:
            skipped.append("%s（objdump 失败）" % label)
            continue
        if hits:
            bad += len(hits)
            print("  FAIL %-5s %s：发现 %d 条 SSE/SSE2 指令" % (label, rel, len(hits)))
            seen = set()
            for fn, mnem, raw in hits[:6]:
                key = (fn, mnem)
                if key in seen:
                    continue
                seen.add(key)
                print("       %-34s %-10s %s" % (fn, mnem, raw))
            if len(hits) > 6:
                print("       ...（另有 %d 条）" % (len(hits) - 6))
        else:
            print("  OK   %-5s %s：无 SSE/SSE2" % (label, rel))

    for s in skipped:
        print("  SKIP %s" % s)

    if bad:
        print("check_cpu: FAIL（内核不开 CR4.OSFXSR、不保存 XMM，这些指令必然 #UD）")
        return 1
    print("check_cpu: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())