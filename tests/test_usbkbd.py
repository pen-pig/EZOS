# -*- coding: utf-8 -*-
"""test_usbkbd.py - USB HID 键盘（中断 IN 轮询 + boot 报告解析）E2E（H2-2d）

沿用 usb/uhci/usbenum 三套的判定方式：诊断全部走**串口（COM1）**按子串断言。
端口 4499(QMP) / 4500(serial)，避开已占用的 4463/4464/4471-4480/4482-4488/
4493-4498。

本步验证 H2-2d 的三件事：
  1. 认领 HID boot 键盘并切 boot 协议 -> "USB-KBD: boot kbd ... proto=ok idle=ok"
  2. 中断 IN 轮询真的读到报告      -> "USB-KBD: report ok len=8"
  3. 报告被解析并注入键盘缓冲      -> "USB-KBD: key=<ascii> mod=<hex>"

铁律「警惕弱断言」+「两组结果必须相反」：
  A（挂 usb-kbd）  ：QMP sendkey 'a' 后必须出现 report 行与 key=97（'a'）。
  B（不挂设备）    ：必须打印 "no HID boot keyboard"，且 sendkey 后**绝不**
                     出现 "report ok" / "key="（fail closed，不误报成功）。

注意：QEMU 的 sendkey 会同时喂给 PS/2 与 usb-kbd，所以屏幕回显无法区分来源；
本测试因此只把**串口里的 USB-KBD 行**当作 USB 路径的证据。

用法：python tests/test_usbkbd.py   （退出码 0 = 全通过）
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
QMP_PORT = 4499
SERIAL_PORT = 4500
# icount shift=auto 下这台 QEMU 开机很慢（实测 45s 才走到 shell 提示符），
# 挂了 usb-kbd 之后 keyboard_getchar 会带上 UHCI 轮询，启动还要再慢一些。
# 等待一律按"看得见证据"为准，不按乐观估计。
BOOT_WAIT = 120
SHELL_WAIT = 30

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.img")


class SerialReader(object):
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
                continue          # 铁律：空闲超时不是异常
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

    def hmc(self, line):
        """human-monitor-command：sendkey 走 HMP（会广播给 PS/2 与 usb-kbd）。"""
        return self.cmd("human-monitor-command", **{"command-line": line})

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


def run_case(name, extra_args, expect_kbd):
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

        if not wait_for(serial, "USB-KBD:", BOOT_WAIT):
            print("  FAIL %s: no 'USB-KBD:' line in serial" % name)
            print("---- serial tail ----\n" + serial.snapshot()[-1500:])
            return False, []

        # 等开机自检跑完、shell 进入 read_line 忙轮询（USB 轮询挂在那里）
        time.sleep(SHELL_WAIT)

        # 最多敲 3 次：guest 侧节流 5ms，只要轮询在跑就一定收得到
        for _ in range(3):
            qmp.hmc("sendkey a")
            time.sleep(3.0)
            if expect_kbd and "USB-KBD: report ok" in serial.snapshot():
                break

        time.sleep(1.0)
        snap = serial.snapshot()
        # 铁律「断言文本先跑探针核实」：探针抓到的原始行是 '> USB-KBD: report ok
        # len=8'——shell 提示符 "> " 不带换行，会和 dmesg 输出挤在同一行。
        # 所以这里只能按「包含 USB-KBD:」取行，不能 startswith。
        lines = [ln.strip() for ln in snap.splitlines() if "USB-KBD:" in ln]
        ok = True

        if expect_kbd:
            if not any("boot kbd addr=" in l for l in lines):
                print("  FAIL %s: no 'USB-KBD: boot kbd' claim line" % name)
                ok = False
            if not any("report ok len=8" in l for l in lines):
                print("  FAIL %s: interrupt IN never returned a report" % name)
                ok = False
            if not any("key=97 " in l for l in lines):
                print("  FAIL %s: no parsed key for 'a' (key=97)" % name)
                ok = False
            if not any("proto=ok" in l for l in lines):
                print("  FAIL %s: SET_PROTOCOL(boot) did not succeed" % name)
                ok = False
        else:
            if not any("no HID boot keyboard" in l for l in lines):
                print("  FAIL %s: expected 'no HID boot keyboard'" % name)
                ok = False
            for bad in ("report ok", "key="):
                if any(bad in l for l in lines):
                    print("  FAIL %s: device-less case reports '%s'" % (name, bad))
                    ok = False

        for ln in lines:
            print("  klog> " + ln)
        return ok, lines
    finally:
        try:
            if qmp:
                qmp.quit()
        except Exception:
            pass
        try:
            if serial:
                serial.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def main():
    kill_all_qemu()
    wait_port_free(QMP_PORT, 15)
    wait_port_free(SERIAL_PORT, 15)

    uhci = ["-device", "piix3-usb-uhci,id=uhci"]
    cases = [
        ("A-kbd", uhci + ["-device", "usb-kbd"], True),
        ("B-empty", uhci, False),
    ]
    results = []
    for name, args, exp in cases:
        print("=== case: %s ===" % name)
        ok, _ = run_case(name, args, exp)
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
