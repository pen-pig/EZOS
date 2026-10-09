# -*- coding: utf-8 -*-
"""test_exitmode.py - 进入图形桌面只能靠 `desktop` 命令，`exit` 不许切过去

为什么要有这个测试
------------------
历史上 `exit` 的语义是"退出 shell 并进入图形桌面"（cmd_exit 置
shell_exit_flag，kernel_main 的循环在 shell_run 返回后直接 gw_start）。
这个设计有两个问题：

1. 用户敲 `exit` 只是想"退出当前这层"，却被强行丢进 GUI——语义反直觉；
2. 更糟的是它形成了**隐式通道**：任何一次 shell_run 返回（包括未来的登出
   路径）都会被解读成"该进桌面了"。

现在 `exit` 只打印提示，shell 是系统控制台、不会返回；桌面唯一入口是
`desktop` 命令（cmd_desktop -> gw_start）。

判据
----
A. 敲 `exit`：屏幕**仍然是文本模式**（分辨率不变），且打印了提示语，提示符
   还在——说明没被切进 GUI。
B. 敲 `desktop`：分辨率变成 VBE 图形模式的尺寸——说明桌面确实能进。
C. 串口日志里不能出现 "Shell session ended"（kernel_main 的兜底分支）：
   出现了就说明 shell_run 返回过，隐式通道又活了。
"""
import os
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
LOG = os.path.join(ROOT, "temp", "exitmode_serial.log").replace("\\", "/")
SHOT = os.path.join(ROOT, "temp", "exitmode.ppm").replace("\\", "/")
VGABIN = os.path.join(ROOT, "temp", "exitmode_vga.bin").replace("\\", "/")
QMP_PORT = alloc_port()

COLS, ROWS = 80, 25
BOOT_WAIT = 90.0


def rec(results, name, ok, detail=""):
    results.append((name, bool(ok)))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name,
                       (" | " + detail) if detail else ""))


class Qmp(object):
    def __init__(self, port):
        import json
        import socket
        self.json = json
        for _ in range(80):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2)
                break
            except OSError:
                time.sleep(0.25)
        else:
            raise RuntimeError("QMP connect failed")
        self.sock.settimeout(30)
        self.f = self.sock.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write((self.json.dumps({"execute": name, "arguments": args})
                      + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            try:
                o = self.json.loads(line)
            except ValueError:
                continue
            if "return" in o or "error" in o:
                return o

    def hmc(self, c):
        return self.cmd("human-monitor-command", **{"command-line": c})

    def line(self, text):
        """逐键发送（连发会丢键，见 test_termwrap.py 的说明）。"""
        for i, ch in enumerate(text):
            self.hmc("sendkey " + ("spc" if ch == " " else ch))
            time.sleep(0.03)
            if i % 10 == 9:
                time.sleep(0.15)
        self.hmc("sendkey ret")
        time.sleep(3.0)

    def shot(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.hmc("screendump " + path)
        return path

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


def vga_text(qmp):
    if os.path.exists(VGABIN):
        os.remove(VGABIN)
    qmp.hmc("memsave 0x%x %d %s" % (0xB8000, COLS * ROWS * 2, VGABIN))
    if not os.path.exists(VGABIN):
        return ""
    raw = open(VGABIN, "rb").read()
    if len(raw) < COLS * ROWS * 2:
        return ""
    rows = []
    for y in range(ROWS):
        rows.append("".join(chr(raw[(y * COLS + x) * 2])
                            if 32 <= raw[(y * COLS + x) * 2] < 127 else " "
                            for x in range(COLS)))
    return "\n".join(rows)


def ppm_dims(path):
    with open(path, "rb") as f:
        head = f.read(64)
    parts = head.split(b"\n")
    if len(parts) < 3:
        return ""
    return "%s %s" % (parts[1].decode("latin1", "replace"),
                      parts[2].decode("latin1", "replace"))


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

        text_dims = ppm_dims(qmp.shot(SHOT))

        # --- A: exit 不得切进桌面 ---
        qmp.line("exit")
        scr = vga_text(qmp)
        rec(results, "exit prints the console hint",
            "cannot be left" in scr and "'desktop'" in scr,
            "| %r" % [r.strip() for r in scr.split("\n") if "cannot be left" in r][:1])
        rec(results, "exit leaves the shell running (prompt redrawn)",
            "] > " in scr)
        after_exit = ppm_dims(qmp.shot(SHOT))
        rec(results, "exit does NOT switch to graphics mode",
            after_exit == text_dims != "",
            "| %s -> %s" % (text_dims, after_exit))

        # --- B: desktop 才是唯一入口 ---
        qmp.line("desktop")
        time.sleep(18)      # 开机动画 + 首帧绘制
        gui_dims = ppm_dims(qmp.shot(SHOT))
        rec(results, "desktop command enters the graphical desktop",
            gui_dims != text_dims and gui_dims != "",
            "| %s -> %s" % (text_dims, gui_dims))

        # --- C: shell_run 从未返回（没有隐式通道）---
        txt = open(LOG, "rb").read().decode("latin1", "replace")
        rec(results, "shell never returned to kernel_main fallback path",
            "Shell session ended" not in txt)

        failed = [n for n, ok in results if not ok]
        print("\n%d/%d passed" % (len(results) - len(failed), len(results)))
        if failed:
            for n in failed:
                print("  FAILED: " + n)
        return 1 if failed else 0
    finally:
        if qmp is not None:
            qmp.quit()
        time.sleep(1)
        try:
            proc.kill()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
