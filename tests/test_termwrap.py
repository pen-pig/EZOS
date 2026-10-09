# -*- coding: utf-8 -*-
"""test_termwrap.py - shell 输入行折行后必须仍然显示正确（VGA 文本缓冲直读）

为什么要有这个测试
------------------
用户报的现象是"输入太长自动跳到下一行之后，显示就乱了"。根因在
``shell_redraw_line()``：它只 ``terminal_clear_line(current_row)`` 一行，
而输入一折行就占了**两行**——第二行的内容从此再也擦不掉。于是退格、历史
回填、Tab 补全都会在屏幕上留下旧字符，光标还停在第一行，看着像"换行坏了"。

串口日志**看不出这个故障**：串口只有字符流，没有行/列的概念，折行重绘在
串口上永远是"对的"。之前整套 E2E 都只读串口，所以这个缺陷一直没人发现。

所以本测试直接读 **VGA 文本缓冲**（0xB8000，80x25 字符+属性）。用 QMP 的
``memsave`` 把这段内存落盘再解析，比 screendump + OCR 精确得多（不用猜字
形，也不受内核切到 VBE 图形模式的影响——文本缓冲里的内容照样在）。

判据（两组结果必须相反）
------------------------
A. ``clear`` 后输入一条**超过一行宽**的命令（不回车）：
     * 第 0 行写满 80 列、以提示符开头（说明没被滚屏顶掉）；
     * 第 1 行是续行、**不含前导空格**（折行处不能被填进空格）；
     * 整体没有滚屏（提示符还在第 0 行）。
B. 随后退格把命令削回一行以内：
     * 第 1 行必须**全空**——残字留在这儿就是 bug 本身。

注意：QMP ``sendkey`` 有丢键（内核键盘缓冲在重绘期间会丢），所以不断言
"输入了多少个字符"，只断言行结构。
"""
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.append(ROOT)

from ezos_qemu import alloc_port, image_path, disk_path  # noqa: E402
from ezos_env import qemu_exe  # noqa: E402

QEMU = qemu_exe()
IMG = image_path()
DISK = disk_path()
LOG = os.path.join(ROOT, "temp", "termwrap_serial.log")
VGABIN = os.path.join(ROOT, "temp", "termwrap_vga.bin").replace("\\", "/")
QMP_PORT = alloc_port()

COLS, ROWS = 80, 25
BOOT_WAIT = 60.0


def rec(results, name, ok, detail=""):
    results.append((name, bool(ok)))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name,
                       (" | " + detail) if detail else ""))


class Qmp(object):
    def __init__(self, port):
        for _ in range(60):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2)
                break
            except OSError:
                time.sleep(0.25)
        else:
            raise RuntimeError("QMP connect failed")
        self.sock.settimeout(20)
        self.f = self.sock.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        import json
        self.f.write((json.dumps({"execute": name, "arguments": args})
                      + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if "return" in o or "error" in o:
                return o

    def hmc(self, c):
        return self.cmd("human-monitor-command", **{"command-line": c})

    def keys(self, text):
        """逐键发送。**必须**给键盘控制器留出处理时间：QMP 往返只要 1ms 级，
        而 8042 的输入缓冲只有十几字节，连发会静默丢键——症状是"输入了 150
        个字符只落进去 74 个"，看起来像内核把输入截断了。"""
        for i, ch in enumerate(text):
            self.hmc("sendkey " + ("spc" if ch == " " else ch))
            time.sleep(0.03)
            if i % 10 == 9:
                time.sleep(0.15)
        time.sleep(3.0)      # 让最后一次重绘落地再采样

    def backspace(self, n):
        for i in range(n):
            self.hmc("sendkey backspace")
            time.sleep(0.03)
            if i % 10 == 9:
                time.sleep(0.15)
        time.sleep(3.0)

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


def grab_vga(qmp):
    """把 0xB8000 的文本缓冲抓成 25 行字符串（只取字符字节，丢属性）。"""
    if os.path.exists(VGABIN):
        os.remove(VGABIN)
    qmp.hmc("memsave 0x%x %d %s" % (0xB8000, COLS * ROWS * 2, VGABIN))
    if not os.path.exists(VGABIN):
        return None
    raw = open(VGABIN, "rb").read()
    if len(raw) < COLS * ROWS * 2:
        return None
    out = []
    for y in range(ROWS):
        line = ""
        for x in range(COLS):
            c = raw[(y * COLS + x) * 2]
            line += chr(c) if 32 <= c < 127 else " "
        out.append(line)          # 不 rstrip：尾随空格是"行末"的证据
    return out


def wait_shell(timeout=BOOT_WAIT):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if "[/] > " in open(LOG, "rb").read().decode("latin1", "replace"):
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def main():
    results = []
    if os.path.exists(LOG):
        os.remove(LOG)
    proc = subprocess.Popen(
        [QEMU, "-machine", "pc", "-vga", "std", "-m", "128",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + DISK,
         "-display", "none", "-no-reboot",
         "-serial", "file:" + LOG,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = None
    try:
        if not wait_shell():
            rec(results, "boot reaches shell prompt", False)
            return 1
        qmp = Qmp(QMP_PORT)

        # --- 开机屏幕：不能被逐条自检刷屏（boot_selftest 的契约是"静默"）---
        scr_boot = grab_vga(qmp)
        if scr_boot is not None:
            st_rows = [r for r in scr_boot if "SELFTEST:" in r]
            rec(results, "boot screen not flooded by per-subsystem selftest",
                len(st_rows) <= 1,
                "| %d row(s): %r" % (len(st_rows), st_rows[:2]))
            ok_line = [r for r in scr_boot if "all subsystem checks passed" in r]
            rec(results, "boot selftest summary intact on one row",
                len(ok_line) == 1 and ok_line[0].rstrip().endswith("[ OK ]"),
                "| %r" % (ok_line[0][:60] if ok_line else None))

        # --- 串口：同一行不能打两遍，时间戳不能倒退 ---
        txt = open(LOG, "rb").read().decode("latin1", "replace")
        n_sum = txt.count("all subsystem checks passed")
        rec(results, "no duplicated selftest summary in serial",
            n_sum == 1, "| n=%d" % n_sum)
        ts = [float(m) for m in re.findall(r"\[\s*(\d+\.\d{6})\]", txt)]
        back = [(a, b) for a, b in zip(ts, ts[1:]) if b < a]
        rec(results, "klog timestamps never go backwards",
            not back, "| %d regression(s) e.g. %r" % (len(back), back[:2]))

        # --- baseline：clear 之后提示符回到第 0 行，屏上只有它 ---
        qmp.keys("clear")
        qmp.hmc("sendkey ret")
        time.sleep(3.0)
        scr = grab_vga(qmp)
        if scr is None:
            rec(results, "VGA text buffer readable (memsave)", False)
            return 1
        rec(results, "clear puts the prompt on row 0",
            scr[0].startswith("[/] > "), "| %r" % scr[0][:40])
        rec(results, "row 1 empty right after clear", scr[1].strip() == "",
            "| %r" % scr[1][:40])

        # --- A：输入一条超宽命令（不回车），必须折到下一行 ---
        qmp.keys("echo " + "a" * 150)
        scr = grab_vga(qmp)
        row0, row1 = scr[0], scr[1]
        rec(results, "long input still starts on row 0 (no scroll)",
            row0.startswith("[/] > "), "| %r" % row0[:20])
        rec(results, "row 0 is filled to the full 80 columns",
            len(row0) == COLS, "| len=%d" % len(row0))
        rec(results, "row 1 continues the input (wrapped)",
            row1 != "" and set(row1) == set("a"),
            "| %r" % row1[:40])
        rec(results, "wrap point has no leading blank",
            not row1.startswith(" "), "| %r" % row1[:20])

        # --- B：退格削回一行以内，折出去的那行必须被擦干净 ---
        qmp.backspace(120)
        scr = grab_vga(qmp)
        rec(results, "row 1 fully erased after backspace (no residue)",
            scr[1].strip() == "", "| %r" % scr[1][:40])
        rec(results, "shortened input stays on row 0",
            scr[0].startswith("[/] > echo"), "| %r" % scr[0][:40])
    finally:
        if qmp:
            qmp.quit()
        try:
            proc.wait(8)
        except Exception:
            proc.kill()

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    if bad:
        print("FAILED: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
