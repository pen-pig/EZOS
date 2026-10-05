# -*- coding: utf-8 -*-
"""test_usbmouse.py - USB HID 鼠标（中断 IN 轮询 + boot 报告解析）E2E（H2-2e）

沿用 usb/uhci/usbenum/usbkbd/usbmsc 五套的判定方式：诊断全部走**串口（COM1）**
按子串断言。端口 4505(QMP) / 4506(serial)，避开已占用的 4463/4464/4471-4480/
4482-4488/4493-4504。

本步验证 H2-2e 的三件事：
  1. 认领 HID boot 鼠标并切 boot 协议 -> "USB-MOU: boot mouse ... proto=ok"
  2. 中断 IN 轮询真的读到报告      -> "USB-MOU: report ok len=<N>"
  3. 报告被解析并注入指针状态      -> "USB-MOU: move dx=<n> dy=<n> ... x=<n> y=<n>"

铁律「警惕弱断言」+「两组结果必须相反」：
  A（挂 usb-mouse）：QMP 注入鼠标移动后必须出现 report 行与 move 行，且指针
                     坐标随移动**单调增加**（证明真的注入了 mouse 层，不只是
                     收到了字节）。
  B（不挂设备）    ：必须打印 "no HID boot mouse"，且注入移动后**绝不**出现
                     "report ok" / "move dx="（fail closed，不误报成功）。

QEMU 侧两个必知的坑（都实测过，别改回去）：
  - 鼠标事件只发给 `info mice` 里带 * 的当前设备，PS/2 鼠标默认在册且会抢，
    所以注入前要先 HMP `mouse_set <idx>` 切到 QEMU HID Mouse。
  - **内核一旦发送 SET_IDLE，QEMU 的 usb-mouse 就不再在中断端点上报**（中断
    IN 恒 NAK，控制端点 GET_REPORT 也是全 0）。usbmouse 因此故意不发
    SET_IDLE，靠设备默认 idle（=无限，变化即报告）。若哪天有人补发 SET_IDLE，
    本测试会立刻 FAIL。
  - QMP 没有 mouse_move 命令，鼠标注入只能走 HMP（human-monitor-command）。

用法：python tests/test_usbmouse.py   （退出码 0 = 全通过）
"""
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4505
SERIAL_PORT = 4506
# icount shift=auto 下这台 QEMU 开机很慢（实测 45s 才走到 shell 提示符），
# 等 shell 一律按"看得见证据"为准（串口出现 shell banner），不按乐观估计。
BOOT_WAIT = 150
SHELL_WAIT = 120
MOVE_WAIT = 6

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")


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
        """human-monitor-command：鼠标注入只有 HMP 这条路（QMP 无 mouse_move）。"""
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


def pick_hid_mouse(qmp):
    """把 QEMU 的当前鼠标切到 QEMU HID Mouse（默认当前是 PS/2，会吃掉事件）。"""
    out = qmp.hmc("info mice")
    txt = out.get("return", "") or ""
    for line in txt.splitlines():
        line = line.strip()
        if not line:
            continue
        idx = line.split()[1].lstrip("#").rstrip(":")
        if "HID Mouse" in line or "HID" in line:
            qmp.hmc("mouse_set " + idx)
            return line
    return txt.strip()


def run_case(name, extra_args, expect_mouse):
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
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT] + extra_args,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        if not wait_for(serial, "USB-MOU:", BOOT_WAIT):
            print("  FAIL %s: no 'USB-MOU:' line in serial" % name)
            print("---- serial tail ----\n" + serial.snapshot()[-1500:])
            return False, []

        # 等 shell 真正进入 keyboard_getchar 忙轮询（USB 鼠标的轮询挂在那里）
        if not wait_for(serial, "EZOS Shell", SHELL_WAIT):
            print("  FAIL %s: shell banner never appeared" % name)
            return False, []
        time.sleep(5.0)

        # 当前鼠标默认可能是 PS/2，不切过去注入的事件全被 PS/2 吃掉
        if expect_mouse:
            pick_hid_mouse(qmp)

        # 注入移动：HMP mouse_move 与 QMP input-send-event 各来几次，
        # 只要轮询在跑就一定收得到（内核侧 5ms 节流）。
        for _ in range(4):
            qmp.hmc("mouse_move 12 8")
            qmp.cmd("input-send-event", **{"events": [
                {"type": "rel", "data": {"axis": "x", "value": 20}},
                {"type": "rel", "data": {"axis": "y", "value": 10}}]})
            time.sleep(MOVE_WAIT)
            if expect_mouse:
                # 坐标单调断言需要至少两行；内核侧 move 行有 250ms 节流，
                # 一轮 6s 通常只出 1-2 行，所以跑够两行才提前退出。
                got = [l for l in serial.snapshot().splitlines()
                       if "USB-MOU: move" in l]
                if len(got) >= 2:
                    break

        time.sleep(1.0)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines() if "USB-MOU:" in ln]
        ok = True

        if expect_mouse:
            if not any("boot mouse addr=" in l for l in lines):
                print("  FAIL %s: no 'USB-MOU: boot mouse' claim line" % name)
                ok = False
            if not any("proto=ok" in l for l in lines):
                print("  FAIL %s: SET_PROTOCOL(boot) did not succeed" % name)
                ok = False
            if not any("report ok len=" in l for l in lines):
                print("  FAIL %s: interrupt IN never returned a report" % name)
                ok = False
            moves = [l for l in lines if "move dx=" in l]
            if not moves:
                print("  FAIL %s: no movement was parsed and injected" % name)
                ok = False
            else:
                # 强断言：指针坐标必须随"只往正方向的移动"单调增加——
                # 只收到字节但没注入 mouse 层的话，x/y 会一直停在初始值。
                coords = []
                for l in moves:
                    # 注意：不能用 split("x=")——第一个 "x=" 落在 "dx=" 里。
                    mx = re.search(r"(?<![\w])x=(-?\d+)", l)
                    my = re.search(r"(?<![\w])y=(-?\d+)", l)
                    if mx and my:
                        coords.append((int(mx.group(1)), int(my.group(1))))
                if len(coords) < 2:
                    print("  FAIL %s: only %d move line(s) with coordinates"
                          % (name, len(coords)))
                    ok = False
                elif not (coords[-1][0] > coords[0][0] and
                          coords[-1][1] > coords[0][1]):
                    print("  FAIL %s: pointer did not advance %s" % (name, coords))
                    ok = False
        else:
            if not any("no HID boot mouse" in l for l in lines):
                print("  FAIL %s: expected 'no HID boot mouse'" % name)
                ok = False
            for bad in ("report ok", "move dx="):
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
        ("A-mouse", uhci + ["-device", "usb-mouse"], True),
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
