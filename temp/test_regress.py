# -*- coding: utf-8 -*-
"""test_regress.py - EZOS 广义回归（QEMU QMP + sendkey + screendump + OCR）

重建说明：9-21 发现 temp/ 下的旧 E2E 脚本（test_regress/test_step5e/
test_step6c/test_step7_pci/test_edge/test_step7_rtl8139）已被清空，且
temp/* 被 .gitignore 忽略，无法从 git 恢复。本文件按同样的判定方法重建：
QMP sendkey 打字 → screendump → 真字库 OCR → 断言。

覆盖面（针对 7.3 + 8a 这两组改动的高风险面专门挑）：
  - 开机进 shell（8a：shell 现在睡在键盘等待队列上，键必须能唤醒它）
  - shell 命令：ver / ps / ls / cat / calc / nic / selftest
  - 6c 用户进程：exec HELLO.ELF（系统调用返回路径 + iret + 段寄存器，
    8a 在 syscall_entry 与 task_irq_trampoline 都改过段加载）
  - 边界输入：mem 0xFFFFFFFF 4096、空 calc —— 只要求不 panic
  - 全程断言屏幕无 "panic"，末尾再跑一条命令证明系统仍可用

端口 4463（与项目其它 E2E 端口约定一致，脚本间不撞车）。
用法：python temp/test_regress.py      （退出码 0 = 全通过）
"""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL_DIR = os.path.join(ROOT, "kernel")
sys.path.insert(0, HERE)
import ezocr  # noqa: E402

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
PORT = 4463
BOOT_WAIT = 30          # icount 下到 shell 提示符的保守等待
SHOT = os.path.join(HERE, "rg_shot.ppm").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add', ':': 'shift-semicolon'}


class Qmp(object):
    def __init__(self, port):
        for _ in range(60):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("QMP connect failed")
        # 握手后放宽读超时：QEMU 的监听套接字在 monitor 初始化完成前就已
        # listen（TCP 握手在 backlog 里成功），而 greeting 要等 monitor 就绪
        # 才发——连得快时 2s 读超时会假失败（本次实测：连上后第 3s 才 greeting）。
        self.sock.settimeout(20)
        self.f = self.sock.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        try:
            line = self.f.readline()
        except socket.timeout:
            raise RuntimeError("QMP read timed out")
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line) if line.strip() else None

    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = self._read()
            if r is None:
                continue
            if "return" in r or "error" in r:
                return r

    def hmc(self, c):
        return self.cmd("human-monitor-command", **{"command-line": c})

    def type_line(self, s):
        for ch in s:
            key = KEYMAP.get(ch)
            if key is None:
                key = ('shift-' + ch.lower()) if ('A' <= ch <= 'Z') else ch.lower()
            r = self.hmc("sendkey " + key)
            if "error" in r:
                raise RuntimeError("sendkey %r failed: %r" % (key, r))
            time.sleep(0.05)
        self.hmc("sendkey ret")


def ppm_dims(path):
    with open(path, "rb") as f:
        return f.read(64).split(b"\n", 2)[1].decode()


def screen(qmp):
    """截图 + OCR；dims 不是 720x400 时返回空串（无法 OCR）"""
    qmp.cmd("screendump", filename=SHOT)
    time.sleep(0.3)
    if ppm_dims(SHOT) != "720 400":
        return ""
    return ezocr.ocr_text(SHOT, KERNEL_DIR)


def flat(s):
    return " ".join(s.lower().split())


def port_in_use(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
        s.close()
        return True
    except OSError:
        return False


def wait_port_free(port, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not port_in_use(port):
            return True
        time.sleep(0.3)
    return False


def main():
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    disk = os.path.join(ROOT, "disk.img").replace("\\", "/")
    for p in (img, disk):
        if not os.path.isfile(p):
            print("MISSING %s - run ninja first" % p)
            return 2

    if port_in_use(PORT):       # 上轮残留：不清掉会连到脏旧 VM
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        print("PORT %d still occupied - abort" % PORT)
        return 2

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []
    try:
        qmp = Qmp(PORT)
        time.sleep(BOOT_WAIT)
        boot_txt = flat(screen(qmp))
        results.append(("boot: reached shell (no panic)", "panic" not in boot_txt))

        # (命令, 期望子串（小写，可为 None = 只查不 panic）, 等待秒)
        cases = [
            ("ver",              "version 0.9.0", 2.5),
            ("ps",               "pid",           2.5),
            ("ls",               "readme.txt",    3.0),
            ("cat README.TXT",   "welcome to ezos", 3.0),
            ("calc 6*7",         "42",            2.5),
            ("exec HELLO.ELF",   "hello from elf user program", 6.0),
            ("nic",              "rtl8139",       3.0),
            ("selftest",         "pass",          15.0),
            ("mem 0xFFFFFFFF 4096", None,         3.0),   # 边界：曾整机 panic
            ("calc",             None,            2.5),   # 空参数
            ("ver",              "version 0.9.0", 3.0),   # 末尾仍可用
        ]
        for cmd, expect, wait in cases:
            try:
                qmp.type_line(cmd)
                time.sleep(wait)
                t = flat(screen(qmp))
                if "panic" in t:
                    results.append(("%s: no panic" % cmd, False))
                    continue
                if expect is None:
                    results.append(("%s: no panic" % cmd, True))
                else:
                    results.append(("%s: saw '%s'" % (cmd, expect), expect in t))
            except Exception as e:
                results.append(("%s: error %r" % (cmd, e), False))
    except Exception as e:
        results.append(("harness: %r" % e, False))
    finally:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
        if os.path.isfile(SHOT):
            try:
                os.remove(SHOT)
            except OSError:
                pass

    for k, v in results:
        print(("  [OK]   " if v else "  [FAIL] ") + k)
    ok = all(v for _, v in results)
    print("REGRESSION:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
