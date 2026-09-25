# -*- coding: utf-8 -*-
"""test_usb.py - USB 主机控制器普查 E2E 验证（只读枚举，真机点亮 H2 前置）

判定方法沿用 test_ahci.py：全部诊断走**串口（COM1）**，按子串断言，
不依赖 screendump / OCR。

分别挂 UHCI / OHCI / EHCI / xHCI 启动，确认 usb_scan_log 按 ProgIF 正确
分类（每类 1 个控制器 + 汇总行计数正确）；再跑一次不挂任何 USB 控制器，
确认静默打印 "USB: no USB controller found" 而非崩。

端口用 4471(QMP) / 4472(serial)，避开头文件约定已占用的
4463/4493/4496/4478/4485/4486/4487/4488/4482。
用法：python tests/test_usb.py   （退出码 0 = 全通过）
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
QMP_PORT = 4471
SERIAL_PORT = 4472
BOOT_WAIT = 45

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.img")


class SerialReader(object):
    """实时收串口（COM1）输出进缓冲，供测试按子串断言。"""
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.lock = threading.Lock()
        self.running = True
        t = threading.Thread(target=self._run, daemon=True)
        t.start()

    def _run(self):
        while self.running:
            try:
                data = self.sock.recv(8192)
            except socket.timeout:
                continue
            except Exception:
                break
            if not data:
                break
            with self.lock:
                self.buf += data

    def snapshot(self):
        with self.lock:
            return self.buf.decode("latin1", "replace")

    def size(self):
        with self.lock:
            return len(self.buf)

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass


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
        return __import__("json").loads(line) if line.strip() else None

    def cmd(self, name, **args):
        self.f.write((__import__("json").dumps({"execute": name,
                                                "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            r = self._read()
            if r is None:
                continue
            if "return" in r or "error" in r:
                return r

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


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


def wait_for(serial, marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if marker in serial.snapshot():
            return True
        time.sleep(0.3)
    return False


def kill_all_qemu():
    subprocess.call(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_case(name, extra_args, expected_type):
    """启动一台 VM（额外设备由 extra_args 指定），收串口，断言 USB 分类。
    返回 (ok, lines) —— lines 为该次启动里所有 USB: 开头的 klog 行。"""
    if not os.path.isfile(IMG) or not os.path.isfile(DISK):
        print("MISSING %s / %s - run ninja first" % (IMG, DISK))
        return False, []

    if port_in_use(QMP_PORT) or port_in_use(SERIAL_PORT):
        kill_all_qemu()
        if not (wait_port_free(QMP_PORT, 15) and wait_port_free(SERIAL_PORT, 15)):
            print("PORT occupied - abort")
            return False, []

    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + DISK,
         "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT] + extra_args,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        if not wait_for(serial, "USB:", BOOT_WAIT):
            print("  FAIL %s: no 'USB:' line seen in serial" % name)
            snap = serial.snapshot()
            print("---- serial tail ----\n" + snap[-2000:])
            return False, []

        # 等汇总行打全
        time.sleep(1.5)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines() if ln.strip().startswith("USB:")]

        ok = True
        if expected_type is None:
            # 不挂任何 USB 控制器：应静默报告未找到，且不应出现任何类型计数>0
            if "USB: no USB controller found" not in snap:
                print("  FAIL %s: expected 'no USB controller found'" % name)
                ok = False
            summary = [l for l in lines if "controller(s)" in l]
            if summary and "UHCI 0 / OHCI 0 / EHCI 0 / xHCI 0" not in summary[0]:
                print("  FAIL %s: summary unexpectedly shows controllers: %s"
                      % (name, summary))
                ok = False
        else:
            # 每个命中设备行应含类型名；汇总行应计该类型=1
            per_dev = [l for l in lines if expected_type in l and "controller(s)" not in l]
            summary = [l for l in lines if "controller(s)" in l]
            if not per_dev:
                print("  FAIL %s: no per-device line with %s" % (name, expected_type))
                ok = False
            if not summary or ("%s 1" % expected_type) not in summary[0]:
                print("  FAIL %s: summary missing '%s 1' (got %s)"
                      % (name, expected_type, summary))
                ok = False

        for ln in lines:
            print("  klog> " + ln)
        return ok, lines
    finally:
        try:
            if qmp: qmp.quit()
        except Exception:
            pass
        try:
            if serial: serial.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        # 清端口，避免影响下一组
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def main():
    cases = [
        ("UHCI", ["-device", "piix3-usb-uhci,id=uhci"], "UHCI"),
        ("OHCI", ["-device", "pci-ohci,id=ohci"],       "OHCI"),
        ("EHCI", ["-device", "usb-ehci,id=ehci"],       "EHCI"),
        ("xHCI", ["-device", "qemu-xhci,id=xhci"],      "xHCI"),
        ("NONE", [],                                    None),
    ]
    results = []
    for name, args, etype in cases:
        print("=== case: %s ===" % name)
        ok, _ = run_case(name, args, etype)
        results.append((name, ok))
        print("  => %s" % ("PASS" if ok else "FAIL"))
        time.sleep(0.5)

    print("\n==== SUMMARY ====")
    all_ok = True
    for name, ok in results:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
        all_ok = all_ok and ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
