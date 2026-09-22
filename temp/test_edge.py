# -*- coding: utf-8 -*-
"""test_edge.py - 边界/极端输入压力测试（重建版）

原脚本随 temp/ 一起被清空（temp/* 被 .gitignore 忽略）。按同样方法重建。
现有 E2E 测的都是正常输入，本套件专挑极端值打：
  - 极大数值（32 位溢出）与超长命令行
  - 畸形路径/超长文件名/空参数/重复参数
  - 不存在的命令
判定标准统一为：**不 panic、不卡死**（之后还能正常执行命令）。

实现要点（都是踩过的坑）：
  - **每用例独立启一个 QEMU**：共用一个 VM 时，一个用例 panic 之后所有后续
    用例连锁失败，看起来像"到处都是 bug"，实际只有第一个是真问题
  - 用例前先确认上一轮的 QEMU 已释放端口，否则会连到残留 VM（脏状态）
  - 单用例异常记 FAIL 并继续，不整组中止；QMP 读写都有超时，不会挂死
  - 数据盘用 disk.img 副本（超长文件名会真的写进卷里）
端口 4496。用法：python temp/test_edge.py
"""
import json
import os
import shutil
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
PORT = 4496
BOOT_WAIT = 30

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add'}


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
        # greeting 要等 monitor 就绪才发（监听套接字先就绪），别用 2s 读超时
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

    def screen(self, shot):
        self.cmd("screendump", filename=shot)
        time.sleep(0.3)
        with open(shot, "rb") as fh:
            head = fh.read(64).split(b"\n", 2)[1].decode()
        if head != "720 400":
            return ""
        return ezocr.ocr_text(shot, KERNEL_DIR)


def flat(s):
    return " ".join(s.lower().split())


def run_cmd(qmp, cmd, shot, timeout=6.0):
    """打字并等屏幕稳定后返回文本"""
    qmp.type_line(cmd)
    time.sleep(1.2)
    prev = qmp.screen(shot)
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.0)
        cur = qmp.screen(shot)
        if cur == prev:
            return prev
        prev = cur
    return prev


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


CASES = [
    ("huge-number",   "calc 99999999999999999999*99999999999999999999", 3.0),
    ("huge-mem-addr", "mem 0xFFFFFFFF 4096", 3.0),
    ("huge-hd-len",   "hexdump 0xFFFFFFFF 99999999", 3.0),
    ("empty-calc",    "calc", 3.0),
    ("deep-paren",    "calc " + "(" * 40 + "1" + ")" * 40, 3.0),
    ("long-name",     "write " + "A" * 100 + ".TXT x", 4.0),
    ("bad-path",      "cat /../../../../etc/passwd", 3.0),
    ("no-such-cmd",   "definitelynotacommand", 3.0),
    ("repeat-args",   "ls ls ls ls", 3.0),
    # sleep 自带 60000ms 上限，这里用能在等待窗口内完成的时长
    ("sleep-ok",      "sleep 2000", 8.0),
]


def main():
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    src_disk = os.path.join(ROOT, "disk.img")
    disk = os.path.join(HERE, "disk_edge.img").replace("\\", "/")
    for p in (img, src_disk):
        if not os.path.isfile(p):
            print("MISSING %s - run ninja first" % p)
            return 2
    shutil.copyfile(src_disk, disk)     # 超长文件名会真的写进卷里

    if port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port_free(PORT, 15)

    results = []
    try:
        for name, cmd, wait in CASES:
            if not wait_port_free(PORT, 15):
                subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                wait_port_free(PORT, 15)
            shot = os.path.join(HERE, "ed_%s.ppm" % name).replace("\\", "/")
            proc = subprocess.Popen([
                QEMU, "-icount", "shift=auto", "-vga", "std",
                "-drive", "format=raw,file=" + img,
                "-drive", "format=raw,file=" + disk,
                "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
            ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                qmp = Qmp(PORT)
                time.sleep(BOOT_WAIT)
                t = flat(run_cmd(qmp, cmd, shot, wait))
                results.append(("%s: no panic" % name, "kernel panic" not in t))
                # 之后系统仍须可用：再跑一条 ver
                t2 = flat(run_cmd(qmp, "ver", shot, 4.0))
                results.append(("%s: still responsive" % name,
                                "version 0.9.0" in t2))
            except Exception as e:
                results.append(("%s: error %r" % (name, e), False))
            finally:
                proc.kill()
                try:
                    proc.wait(timeout=10)
                except Exception:
                    pass
                try:
                    os.remove(shot)
                except OSError:
                    pass
    finally:
        try:
            os.remove(disk)
        except OSError:
            pass

    for k, v in results:
        print(("  [OK]   " if v else "  [FAIL] ") + k)
    ok = all(v for _, v in results)
    print("EDGE:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
