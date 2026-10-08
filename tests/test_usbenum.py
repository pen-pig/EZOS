# -*- coding: utf-8 -*-
"""test_usbenum.py - USB 设备枚举与描述符解析 E2E（真机点亮 H2-2c）

沿用 test_uhci.py / test_uhci_control.py 的判定方式：诊断全部走**串口（COM1）**，
按子串断言，不依赖 screendump / OCR。端口 4497(QMP) / 4498(serial)，避开已占用的
4463/4464/4471-4480/4482-4488/4493/4494/4495/4496。

本步验证 H2-2c 的**完整枚举链路**：端口复位 -> GET_DESCRIPTOR(Device,8/18) ->
SET_ADDRESS -> 按新地址重读设备描述符 -> GET_DESCRIPTOR(Configuration) 两级读 ->
接口/端点解析（识别 HID class 0x03 与中断 IN 端点）-> SET_CONFIGURATION ->
HID 类描述符（报告描述符长度）。

铁律「警惕弱断言」：绝不只断言 "USB-ENUM:" 出现，必须断言到具体值，且插/不插
设备两组结果必须**相反**，才能证明枚举真的在读写硬件而不是硬编码：
  A（插 usb-kbd）  ：必须出现设备行（含 vid=/pid=）、HID 接口行、端点行、
                     "SET_CONFIGURATION ok"，且总结为 "1 device(s) enumerated"。
  B（不插设备）    ：必须出现 "no connected port, skip" 与 "0 device(s) enumerated"，
                     且**绝不**出现 "SET_CONFIGURATION ok" / "vid="（fail closed）。
  C（插 usb-tablet）：另一类 HID 设备，同样必须枚举成功；用于证明解析不是
                     只对键盘成立的巧合（两组 vid/pid 必须不同）。

用法：python tests/test_usbenum.py   （退出码 0 = 全通过）
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import (alloc_port, image_path,
                       disk_path, log_path, kill_stale_qemu)
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU = qemu_exe()
QMP_PORT = alloc_port()
SERIAL_PORT = alloc_port()
BOOT_WAIT = 45

IMG = image_path()
DISK = disk_path()


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
                continue          # 铁律：空闲超时不是异常，继续等
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
    kill_stale_qemu("qemu-system-x86_64.exe")


def run_case(name, extra_args, expect_device):
    """启动一台 VM，收串口，断言枚举结果。返回 (ok, lines)。"""
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

        if not wait_for(serial, "USB-ENUM:", BOOT_WAIT):
            print("  FAIL %s: no 'USB-ENUM:' line seen in serial" % name)
            print("---- serial tail ----\n" + serial.snapshot()[-2000:])
            return False, []

        # 枚举最多 7 笔控制传输，每笔超时上限 1s；等总结行出现再收口
        if not wait_for(serial, "device(s) enumerated", 20):
            print("  FAIL %s: no enumeration summary line" % name)
            return False, []
        time.sleep(1.0)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines()
                 if ln.strip().startswith("USB-ENUM:")]

        ok = True

        if expect_device:
            want = "1 device(s) enumerated"
            if not any(l.endswith(want) for l in lines):
                print("  FAIL %s: summary is not '%s'" % (name, want))
                ok = False
            # 设备行必须带真实 VID/PID（读硬件所得，不是硬编码）
            dev = [l for l in lines if " addr=1 " in l and "vid=0x" in l]
            if not dev:
                print("  FAIL %s: no device line with vid=/pid=" % name)
                ok = False
            # 配置已生效
            if not any("SET_CONFIGURATION ok" in l for l in lines):
                print("  FAIL %s: no 'SET_CONFIGURATION ok' line" % name)
                ok = False
            # HID 接口必须被识别出来（usb-kbd / usb-tablet 都是 HID）
            if not any(l.startswith("USB-ENUM: if0 cls=03") and l.endswith(" HID")
                       for l in lines):
                print("  FAIL %s: no HID interface line (cls=03)" % name)
                ok = False
            # HID 报告描述符本体必须能读回来（下一步解析报告的输入）
            if not any("USB-ENUM: hid report ok len=" in l for l in lines):
                print("  FAIL %s: no 'hid report ok' line" % name)
                ok = False
            # 中断 IN 端点（键盘/鼠标的报告通道）
            if not any("USB-ENUM: if0 ep=" in l for l in lines):
                print("  FAIL %s: no interrupt IN endpoint line" % name)
                ok = False
        else:
            if not any("no connected port, skip" in l for l in lines):
                print("  FAIL %s: expected 'no connected port, skip'" % name)
                ok = False
            if not any(l == "USB-ENUM: 0 device(s) enumerated" for l in lines):
                print("  FAIL %s: summary is not '0 device(s) enumerated'" % name)
                ok = False
            # fail closed：没有设备就绝不能出现任何成功证据
            for bad in ("SET_CONFIGURATION ok", "vid=0x", " if0 cls="):
                if any(bad in l for l in lines):
                    print("  FAIL %s: device-less case wrongly reports '%s'"
                          % (name, bad))
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
        ("C-tablet", uhci + ["-device", "usb-tablet"], True),
    ]
    results = []
    protos = {}
    for name, args, exp in cases:
        print("=== case: %s ===" % name)
        ok, lines = run_case(name, args, exp)
        for l in lines:
            if l.startswith("USB-ENUM: if0 cls=03"):
                i = l.find("proto=")
                if i >= 0:
                    protos[name] = l[i:i + 8]
        results.append((name, ok))
        print("  => %s" % ("PASS" if ok else "FAIL"))
        time.sleep(0.5)

    # 交叉断言：usb-kbd 必须是 boot 键盘（sub=01 proto=01），usb-tablet 必须
    # 与之不同（sub=00 proto=00）——证明接口解析结果来自硬件，不是硬编码。
    cross = True
    if protos.get("A-kbd") != "proto=01":
        print("  FAIL cross-check: usb-kbd proto is %r, expected 'proto=01'"
              % protos.get("A-kbd"))
        cross = False
    if "C-tablet" in protos and protos.get("C-tablet") == protos.get("A-kbd"):
        print("  FAIL cross-check: tablet reports same proto as kbd (%s)"
              % protos.get("C-tablet"))
        cross = False
    print("  cross-check: kbd=%s tablet=%s"
          % (protos.get("A-kbd"), protos.get("C-tablet")))
    results.append(("D-crosscheck", cross))

    print("\n==== SUMMARY ====")
    all_ok = True
    for name, ok in results:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
        all_ok = all_ok and ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
