# -*- coding: utf-8 -*-
"""test_jobs.py - shell 后台任务（用户态生态 第③步）的端到端回归

验证点（端口 4485，44xx 区间，与项目其它 E2E 不撞车）：
  1) `SPIN.ELF &` 立刻回提示符、打印 `[1] <pid>`（后台启动、不阻塞前台）
  2) 连续两个 `&` 任务，job id 递增（[1] / [2]）
  3) `jobs` 能看到后台任务，状态 RUNNING；退出后变 DONE
  4) 每次提示符前非阻塞收割 ZOMBIE，打印 `[n] done`
  5) 收割后 `ps` 不再显示 zombie（MAX_TASKS=8 槽位不被长期占用）
  6) 前台执行路径不受影响：`exec HELLO.ELF` 仍正常打印
  7) 全程无 panic

判定方法同 test_rmdir.py：QMP sendkey 打字 -> screendump -> 真字库 OCR，
取倒数第二个 ">" 之间的文本作为"最近一条命令的输出"；屏幕稳定靠连拍两张一致。

数据盘直接用构建产物 disk.img（本测试只读，不污染）。
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
PORT = 4485
BOOT_WAIT = 30
SHOT = os.path.join(HERE, "jobs_shot.ppm").replace("\\", "/")

# '&' 在美式键盘上是 Shift+7；其余特殊字符沿用 test_rmdir 的映射
KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add', '&': 'shift-7'}


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

    def screen(self):
        self.cmd("screendump", filename=SHOT)
        time.sleep(0.3)
        with open(SHOT, "rb") as fh:
            head = fh.read(64).split(b"\n", 2)[1].decode()
        if head != "720 400":
            return ""
        return ezocr.ocr_text(SHOT, KERNEL_DIR)


def flat(s):
    return " ".join(s.lower().split())


def last_out(s):
    """最近一条命令的输出：取倒数第二个 ">" 之后到最后一个 ">" 之间。"""
    parts = s.split(">")
    return flat(parts[-2]) if len(parts) >= 2 else flat(s)


def run(qmp, cmd, timeout=10.0):
    """等到屏幕文本稳定（连拍两张一致）再返回。"""
    qmp.type_line(cmd)
    time.sleep(1.2)
    prev = qmp.screen()
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.2)
        cur = qmp.screen()
        if cur == prev:
            return last_out(prev)
        prev = cur
    return last_out(prev)


def snap(qmp):
    return qmp.screen()


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

    if port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        print("PORT %d occupied - abort" % PORT)
        return 2

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []
    def add(label, ok):
        results.append((label, ok))
        print("%s  %s" % ("PASS" if ok else "FAIL", label))

    try:
        qmp = Qmp(PORT)
        time.sleep(BOOT_WAIT)
        boot_txt = flat(snap(qmp))
        add("boot: reached shell (no panic)", "panic" not in boot_txt)

        # 1) 后台启动第一个 SPIN.ELF，应立刻回提示符并打印 [1] <pid>
        qmp.type_line("SPIN.ELF &")
        time.sleep(2.0)
        t1 = snap(qmp)
        ft1 = flat(t1)
        add("bg launch prints [1] pid", "[1]" in ft1)
        add("bg returns to prompt quickly (not blocked)", "spin" in ft1 and ">" in ft1)

        # 2) 再后台启动一个，job id 应递增为 [2]
        qmp.type_line("SPIN.ELF &")
        time.sleep(2.0)
        t2 = snap(qmp)
        ft2 = flat(t2)
        add("second bg job id increments to [2]", "[2]" in ft2)

        # 3) jobs 能看到后台任务（RUNNING）
        qmp.type_line("jobs")
        time.sleep(2.0)
        tj = snap(qmp)
        ftj = flat(tj)
        add("jobs lists bg tasks", "spin" in ftj)
        add("jobs shows running state", "running" in ftj)

        # 4) 等两个 spin 都结束（屏幕上出现 spin: done），再按一次回车触发收割
        end = time.time() + 40
        done = False
        while time.time() < end:
            time.sleep(1.5)
            if "spin: done" in flat(snap(qmp)):
                done = True
                break
        add("both bg spins finished", done)
        time.sleep(3.0)   # 第二个比第一个晚约 1s 启动，留余量确保都退出

        td = run(qmp, "")   # 空命令 -> 回车 -> 提示符前非阻塞收割
        add("reap prints [1] done", "[1] done" in td)
        add("reap prints [2] done", "[2] done" in td)

        # 5) jobs 现在显示 DONE，且 ps 不再有 zombie
        tjobs = run(qmp, "jobs")
        add("jobs shows done after exit", "done" in tjobs)

        tps = run(qmp, "ps")
        add("ps no zombie after reap", "zombie" not in flat(tps))

        # 6) 前台执行路径不受影响
        thello = run(qmp, "exec HELLO.ELF", 8.0)
        add("foreground exec HELLO.ELF still works",
            "hello from elf user program" in thello)

        # 7) 全程无 panic
        add("no panic during whole session", "panic" not in flat(snap(qmp)))
    except Exception as e:
        add("harness: %r" % e, False)
    finally:
        proc.kill()
        try:
            proc.wait(timeout=15)
        except Exception:
            pass

    npass = sum(1 for _, ok in results if ok)
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
