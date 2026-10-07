# -*- coding: utf-8 -*-
"""check_stack.py - 内核栈帧预算检查（静态，编译期）。

为什么需要这一关
----------------
内核的栈预算极小且**没有 MMU 保护**：
  * 主栈：boot.asm/kernel_entry.asm 里 `mov esp, 0x90000`，而镜像（text+data）
    末梢大约在 0x8b000 —— 总共只有 ~21KB。
  * 任务内核栈：TASK_KSIZE = 16KB（kernel/task.h）。
  * ring0 IRQ 不切栈，中断还会往当前栈上再压一帧。

栈越界在这套系统里**不会立刻崩**，而是静默写穿紧邻的 .data：
历史上 shell.c 的 cmd_ls 在栈上放了 `fs_dir_entry_t entries[64]`
（64 x 264B = 16.9KB），越界后把 fs.c 的 `ro_cwd[256]` 覆盖成刚写的文件名，
症状是提示符变 `[A.TXT]`、之后的 cat/write/ls 全部失败——看起来像文件系统
坏了，其实是栈溢出。查了很久才定位。

所以大缓冲必须 static（落 .bss，19MB 区），这条规则要靠工具守住，
不能靠记性。

用法
----
    python tools/check_stack.py          # 全量编译 kernel/*.c 并报超预算的函数
    ninja stack                          # 同上（可选目标，不进 default）

退出码非 0 = 有函数超硬上限。
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ezos_env  # noqa: E402

OUT = os.path.join(ROOT, "temp", "su_stack")

# 硬上限：单帧 12KB。理由见文件头——任务栈 16KB，单帧吃掉 12KB 之后剩下的
# 4KB 还要塞调用链 + IRQ 帧，已经是极限。超过就直接 FAIL。
FAIL_LIMIT = 12 * 1024
# 软上限：4KB 以上就列出来提醒（可能是可以搬走的缓冲，也可能确实需要）。
WARN_LIMIT = 4 * 1024

# 与 build.ninja 的 CFLAGS 保持一致（-Werror 在这里无所谓，只求栈用量一致）
CFLAGS = ["-ffreestanding", "-O2", "-Wall", "-Wextra", "-Ikernel", "-MMD"]


def main():
    cc = ezos_env.tool("cc")
    os.makedirs(OUT, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(ROOT, "kernel", "*.c")))
    rows = []
    errors = []
    for s in srcs:
        base = os.path.basename(s)[:-2]
        obj = os.path.join(OUT, base + ".o")
        su = obj[:-2] + ".su"
        if os.path.exists(su):
            try:
                os.remove(su)
            except OSError:
                pass
        cmd = [cc] + CFLAGS + ["-fstack-usage", "-c", s, "-o", obj]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            errors.append("%s: %s" % (s, (p.stderr or "").strip().splitlines()[:1]))
            continue
        if not os.path.exists(su):
            continue
        for line in open(su, encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            parts = line.split("\t") if "\t" in line else line.split()
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            rows.append((int(parts[1]), parts[0]))

    rows.sort(reverse=True)
    print("kernel 栈帧预算：FAIL > %d B，提醒 >= %d B（共 %d 个函数）"
          % (FAIL_LIMIT, WARN_LIMIT, len(rows)))
    print("栈总预算：主栈 ~21KB（0x8b000..0x90000），任务栈 %d KB"
          % (16))
    print("")
    over = [(n, loc) for n, loc in rows if n > FAIL_LIMIT]
    warn = [(n, loc) for n, loc in rows
            if WARN_LIMIT <= n <= FAIL_LIMIT]
    for n, loc in rows[:12]:
        print("  %7d B  %s" % (n, loc))
    if warn:
        print("\n提醒（>= %d B，能搬就搬去 static）：" % WARN_LIMIT)
        for n, loc in warn:
            print("  %7d B  %s" % (n, loc))
    if errors:
        print("\n编译失败的源文件：")
        for e in errors:
            print("  " + e)
    if over:
        print("\nFAIL：以下函数单帧超过 %d B，主栈 (~21KB) 与任务栈 (16KB) "
              "都撑不住 —— 把大数组改成 static（落 .bss）再重跑。" % FAIL_LIMIT)
        for n, loc in over:
            print("  %7d B  %s" % (n, loc))
        return 1
    if errors:
        return 1
    print("\nOK：没有函数超过 %d B 的单帧预算。" % FAIL_LIMIT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
