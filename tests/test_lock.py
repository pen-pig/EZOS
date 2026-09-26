# -*- coding: utf-8 -*-
"""test_lock.py - 分配器临界区保护自检的 E2E 验证（P1 无锁隐患回归网）

开机自检（boot_selftest）会自动跑 lock_selftest()，串口输出 6 行：
    LOCKTEST: irqflags save(IF=1) PASS
    LOCKTEST: irqflags nested keep PASS
    LOCKTEST: kmalloc in IRQ ctx PASS
    LOCKTEST: pmm in IRQ ctx PASS
    LOCKTEST: kmalloc stress PASS
    LOCKTEST: pmm stress PASS

断言三件事：
  1) 恰好 6 行 LOCKTEST（防自检项被静默删掉/没注册）；
  2) 无任何 FAIL；
  3) 含关键的 "irqflags nested keep PASS"——该项专抓"把 irq_restore
     改成无条件 sti"这类回归（等价于 task_unlock 语义），变异测试已
     证明它抓得住。

端口 4494(serial) / 4495(QMP)，避开 4463/4471-4480/4482/4485-4493/4496。
用法：python tests/test_lock.py   （退出码 0 = 通过）
"""
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4495
SERIAL_PORT = 4494
BOOT_WAIT = 45
N_CASES = 6

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.img")


class SerialReader(object):
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.running:
            try:
                data = self.sock.recv(8192)
            except socket.timeout:
                continue          # 空闲不是异常（见 tests/README.md 铁律）
            except Exception:
                break
            if not data:
                break
            with self.lock:
                self.buf += data

    def snapshot(self):
        with self.lock:
            return self.buf.decode("latin1", "replace")

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass


def kill_all_qemu():
    subprocess.call(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    if not os.path.isfile(IMG) or not os.path.isfile(DISK):
        print("MISSING os-image.bin / disk.img - run ninja first")
        return 1

    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + DISK,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        sr = SerialReader(SERIAL_PORT)
        end = time.time() + BOOT_WAIT
        while time.time() < end:
            if sr.snapshot().count("LOCKTEST:") >= N_CASES:
                break
            time.sleep(0.3)
        time.sleep(1.0)
        txt = sr.snapshot()

        lines = [ln.strip() for ln in txt.splitlines()
                 if ln.strip().startswith("LOCKTEST:")]
        for ln in lines:
            print("  " + ln)

        ok = True
        if len(lines) != N_CASES:
            print("  FAIL: expected %d LOCKTEST lines, got %d"
                  % (N_CASES, len(lines)))
            ok = False
        fails = [ln for ln in lines if " FAIL" in ln]
        if fails:
            print("  FAIL: %d case(s) failed" % len(fails))
            ok = False
        if not any("irqflags nested keep PASS" in ln for ln in lines):
            print("  FAIL: 'irqflags nested keep' missing/renamed - "
                  "this case is the regression guard for unconditional sti")
            ok = False
        print("  => %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        sr.close()
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()


if __name__ == "__main__":
    sys.exit(main())
