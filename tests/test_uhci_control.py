# -*- coding: utf-8 -*-
"""test_uhci_control.py - UHCI 控制传输骨架 E2E（真机点亮 H2-2b）

判定沿用 test_usb.py / test_uhci.py：全部诊断走**串口（COM1）**，按子串断言，
不依赖 screendump / OCR。端口用 4475(QMP) / 4476(serial)，避开已被占用的
4463/4471/4472/4473/4474/4477-4480/4485-4488/4493/4494/4495/4496。

本步验证的是 H2-2b 的**控制传输骨架**：在 H2-2a 初始化+端口检测之上，对已连接
端口做最小枚举探针——端口 reset -> GET_DESCRIPTOR(Device)@addr0 ->
SET_ADDRESS(1) -> GET_DESCRIPTOR(Device)@addr1，仅取设备描述符前几个字段做
证据（不解析 HID 报告、不做完整枚举）。

铁律「警惕弱断言」：绝不只断言 "UHCI-CTRL:" 出现，必须断言到具体值，且 A/B 两组
结果必须**相反**，才能证明探针真的在读写硬件而不是硬编码：
  A（插键盘）：断言出现 "UHCI-CTRL: GET_DESCRIPTOR dev @0 ok"（真实控制传输
              拿到设备描述符），且出现 "UHCI: port0 conn=1"。
  B（不插设备）：断言出现 "UHCI: port0 conn=0"，且**绝不**出现
               "UHCI-CTRL: GET_DESCRIPTOR dev @0 ok"（无设备则干净跳过，
               fail closed，不误报成功）。

若 QEMU 仿真限制导致真实控制传输无法验证，A 组应出现明确的 fail-closed 行
（如 "GET_DESCRIPTOR @0 failed" / "xfer timeout" / "no device after reset"），
此时报告真实证据而非伪装成功。

用法：python tests/test_uhci_control.py   （退出码 0 = 全通过）
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
QMP_PORT = 4475
SERIAL_PORT = 4476
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


def run_case(name, extra_args, expect_device):
    """启动一台 VM（额外设备由 extra_args 指定），收串口，断言 UHCI 控制传输。
    返回 (ok, lines) —— lines 为本次启动里所有 UHCI:/UHCI-CTRL: 开头的 klog 行。"""
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

        if not wait_for(serial, "UHCI:", BOOT_WAIT):
            print("  FAIL %s: no 'UHCI:' line seen in serial" % name)
            snap = serial.snapshot()
            print("---- serial tail ----\n" + snap[-2000:])
            return False, []

        # 等控制传输探针跑完（端口 reset + 最多 3 笔传输，每笔最长 1s 超时）
        time.sleep(4.0)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines()
                 if ln.strip().startswith("UHCI:") or ln.strip().startswith("UHCI-CTRL:")]

        ok = True

        # 端口 0 连接态：A 组必须 conn=1，B 组必须 conn=0
        port0 = [l for l in lines if l.startswith("UHCI: port0 ")]
        if not port0:
            print("  FAIL %s: no 'UHCI: port0 ' line" % name)
            ok = False
        else:
            want = "conn=1" if expect_device else "conn=0"
            if want not in port0[0]:
                print("  FAIL %s: port0 line '%s' but expected %s"
                      % (name, port0[0], want))
                ok = False

        if expect_device:
            # A 组：必须真实拿到设备描述符（强断言，证明控制传输在读写硬件）
            if not any("GET_DESCRIPTOR dev @0 ok" in l for l in lines):
                print("  FAIL %s: no 'GET_DESCRIPTOR dev @0 ok' line "
                      "(control xfer did not read a real descriptor)" % name)
                ok = False
        else:
            # B 组：无设备，绝不能出现成功行（fail closed，不误报）
            if any("GET_DESCRIPTOR dev @0 ok" in l for l in lines):
                print("  FAIL %s: device-less case wrongly reports "
                      "GET_DESCRIPTOR success" % name)
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
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def main():
    # 跑前清残留 QEMU（占住 os-image.bin 会让 ninja 报文件锁）
    kill_all_qemu()
    wait_port_free(QMP_PORT, 15)
    wait_port_free(SERIAL_PORT, 15)

    cases = [
        ("A-kbd", ["-device", "piix3-usb-uhci,id=uhci", "-device", "usb-kbd"], True),
        ("B-empty", ["-device", "piix3-usb-uhci,id=uhci"], False),
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
