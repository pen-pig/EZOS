# -*- coding: utf-8 -*-
"""test_usbmsc.py - USB Mass Storage（BOT+SCSI）E2E（真机点亮 H1c）

验证三件事：
  1. 内核能认领 usb-storage（MSC 接口 + 一对 bulk 端点）并读容量
     -> "USB-MSC: unit<n> addr=<a> ep_in=0x81 ep_out=0x02 sectors=<s> blksize=512 ready"
  2. U 盘被注册成块设备 drive 12..，FS 层（exFAT）能挂载并读文件
     -> setdrive 13 / ls / cat README.TXT 走完整 BOT 读路径
  3. BOT 写路径可用
     -> write USBTEST.TXT ... / cat 回读

铁律「警惕弱断言」+「两组结果必须相反」：
  A（两个 usb-storage：引导盘+数据盘）：必须出现 2 个 unit ready、
     2 drive(s) registered，且 setdrive 13 后 ls/cat/readme 全通。
  B（不挂 U 盘）            ：必须打印 "no mass storage device"，且**绝不**
     出现 "registered as drive" / "unit*.ready"（fail closed）。

背景（本步最大发现）：SeaBIOS 从 U 盘引导要求镜像扇区数为偶数？实测 993
扇区（奇数）引导失败（"Booting from Hard Disk" 后读盘失败），pad 到 1024
扇区立即成功。本测试用动态 pad 好的镜像（temp/os-image-1024.bin）。

端口 4503(QMP) / 4504(serial)。
"""
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL_DIR = os.path.join(ROOT, "kernel")
sys.path.insert(0, HERE)
import ezocr  # noqa: E402

QEMU = qemu_exe()
QMP_PORT = 4503
SERIAL_PORT = 4504
BOOT_WAIT = 150        # USB 引导 + USB 枚举 + FS，比 IDE 慢
SHELL_WAIT = 12

IMG = os.path.join(ROOT, "os-image.bin")
IMG_PAD = os.path.join(ROOT, "temp", "os-image-1024.bin")
DISK = os.path.join(ROOT, "disk.vhd")
SHOT = os.path.join(HERE, "msc_shot.ppm").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add', ':': 'shift-semicolon'}


def pad_image():
    """os-image.bin pad 到 1024 扇区（SeaBIOS U 盘引导的扇区数偶数要求，
    实测 993 奇数失败）。kernel.bin/UEFI 契约不动。"""
    data = open(IMG, "rb").read()
    if len(data) > 1024 * 512:
        raise RuntimeError("os-image.bin exceeds 512KiB budget")
    os.makedirs(os.path.dirname(IMG_PAD), exist_ok=True)
    with open(IMG_PAD, "wb") as f:
        f.write(data)
        f.write(b"\x00" * (1024 * 512 - len(data)))


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

    def wait_for(self, marker, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if marker in self.snapshot():
                return True
            time.sleep(0.5)
        return False

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

    def screendump(self):
        self.cmd("screendump", filename=SHOT)
        time.sleep(0.3)

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


def ocr_text():
    try:
        with open(SHOT, "rb") as f:
            dims = f.read(64).split(b"\n", 2)[1].decode()
    except Exception:
        return ""
    if dims.strip() != "720 400":
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


def kill_all_qemu():
    subprocess.call(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def launch(extra_args):
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT] + extra_args,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial_lines(snap):
    """按「包含前缀」取行（铁律：shell 提示符会与 dmesg 挤同一行，
    startswith 会漏行——test_usbkbd 的教训）"""
    return [ln.strip() for ln in snap.splitlines() if "USB-MSC:" in ln]


def run_case_a():
    """A：引导 U 盘 + 数据 U 盘，全链路（BOT 读容量 / FS 挂载 / 读 / 写）"""
    if not (os.path.isfile(IMG_PAD) and os.path.isfile(DISK)):
        print("MISSING %s / %s" % (IMG_PAD, DISK))
        return False

    if port_in_use(QMP_PORT) or port_in_use(SERIAL_PORT):
        kill_all_qemu()
        if not (wait_port_free(QMP_PORT, 15) and wait_port_free(SERIAL_PORT, 15)):
            print("PORT occupied - abort")
            return False

    proc = launch([
        "-drive", "format=raw,file=" + IMG_PAD.replace("\\", "/") + ",if=none,id=usbos",
        "-device", "piix3-usb-uhci,id=uhci",
        "-device", "usb-storage,drive=usbos,bus=uhci.0,port=1,bootindex=0",
        "-drive", "format=raw,file=" + DISK.replace("\\", "/") + ",if=none,id=usbdata",
        "-device", "usb-storage,drive=usbdata,bus=uhci.0,port=2",
    ])

    serial = None
    qmp = None
    ok = True
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        if not serial.wait_for("USB-MSC:", BOOT_WAIT):
            print("  FAIL A: no 'USB-MSC:' line in serial")
            print("---- serial tail ----\n" + serial.snapshot()[-1200:])
            return False
        time.sleep(SHELL_WAIT)

        lines = serial_lines(serial.snapshot())
        for ln in lines:
            print("  klog> " + ln)

        def have(sub):
            return any(sub in l for l in lines)

        if not have("unit0 addr=") or not have("blksize=512 ready"):
            print("  FAIL A: unit0 not claimed/ready")
            ok = False
        if not have("unit1 addr="):
            print("  FAIL A: unit1 (data stick) not claimed")
            ok = False
        if not have("2 drive(s) registered as drive 12..13"):
            print("  FAIL A: expected '2 drive(s) registered as drive 12..13'")
            ok = False

        # ---- FS 全链路（OCR 断言）----
        time.sleep(3.0)                       # 等 shell 提示符稳定
        qmp.type_line("setdrive 13")
        time.sleep(6.0)                       # exFAT 挂载 = 一串 BOT 读
        qmp.type_line("ls")
        time.sleep(4.0)
        qmp.type_line("cat README.TXT")
        time.sleep(4.0)
        qmp.screendump()
        txt = flat(ocr_text())
        if "readme.txt" not in txt:
            print("  FAIL A: ls after setdrive 13 lacks readme.txt")
            print("  ocr> " + txt[:400])
            ok = False
        if "welcome to ezos" not in txt:
            print("  FAIL A: cat README.TXT lacks content (BOT read path)")
            ok = False

        # ---- BOT 写路径 ----
        qmp.type_line("write USBTEST.TXT hello-usb-msc-e2e")
        time.sleep(6.0)
        qmp.type_line("cat USBTEST.TXT")
        time.sleep(4.0)
        qmp.screendump()
        txt = flat(ocr_text())
        if "hello-usb-msc-e2e" not in txt:
            print("  FAIL A: BOT write/read-back roundtrip failed")
            print("  ocr> " + txt[:400])
            ok = False
        if "panic" in txt:
            print("  FAIL A: panic on screen")
            ok = False

        return ok
    finally:
        if qmp:
            qmp.quit()
        if serial:
            serial.close()
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def run_case_b():
    """B：UHCI 无任何设备。必须 no mass storage device 且无成功行"""
    if port_in_use(QMP_PORT) or port_in_use(SERIAL_PORT):
        kill_all_qemu()
        if not (wait_port_free(QMP_PORT, 15) and wait_port_free(SERIAL_PORT, 15)):
            print("PORT occupied - abort")
            return False

    # 无 U 盘场景：内核只能从 IDE 引导（本用例不带 usb-storage，
    # 没有可启动设备 SeaBIOS 会直接报 no bootable device）
    proc = launch([
        "-drive", "format=raw,file=" + IMG.replace("\\", "/"),
        "-device", "piix3-usb-uhci,id=uhci",
    ])

    serial = None
    qmp = None
    ok = True
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)
        if not serial.wait_for("USB-MSC:", BOOT_WAIT):
            print("  FAIL B: no 'USB-MSC:' line in serial")
            print("---- serial tail ----\n" + serial.snapshot()[-1200:])
            return False
        time.sleep(SHELL_WAIT)

        lines = serial_lines(serial.snapshot())
        for ln in lines:
            print("  klog> " + ln)

        if not any("no mass storage device" in l for l in lines):
            print("  FAIL B: expected 'no mass storage device'")
            ok = False
        for bad in ("registered as drive", "ready"):
            if any(bad in l for l in lines):
                print("  FAIL B: device-less case reports '%s'" % bad)
                ok = False
        return ok
    finally:
        if qmp:
            qmp.quit()
        if serial:
            serial.close()
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

    if not (os.path.isfile(IMG) and os.path.isfile(DISK)):
        print("MISSING %s / %s - run ninja first" % (IMG, DISK))
        return 2
    pad_image()

    print("=== case: A-two-sticks ===")
    ok_a = run_case_a()
    print("  => %s" % ("PASS" if ok_a else "FAIL"))
    time.sleep(0.5)

    print("=== case: B-empty ===")
    ok_b = run_case_b()
    print("  => %s" % ("PASS" if ok_b else "FAIL"))

    print("\n==== SUMMARY ====")
    print("  [%s] A-two-sticks" % ("PASS" if ok_a else "FAIL"))
    print("  [%s] B-empty" % ("PASS" if ok_b else "FAIL"))
    return 0 if (ok_a and ok_b) else 1


if __name__ == "__main__":
    sys.exit(main())
