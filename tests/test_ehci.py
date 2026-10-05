# -*- coding: utf-8 -*-
"""test_ehci.py - EHCI（USB 2.0 高速）控制传输 E2E（真机点亮 A1 第一阶段）

判定全部走**串口（COM1）**按子串断言（沿用 uhci/usbenum/... 六套的做法）。
端口 4507(QMP) / 4508(serial)，避开已占用的 4463/4464/4471-4506。

本步验证 EHCI 的四件事：
  1. 认领 EHCI 控制器（PCI class 0x0C03 / prog_if 0x20，MMIO BAR 需分页映射）
     -> "EHCI: 1 EHCI controller(s) found"
  2. 异步调度真的跑起来（USBSTS.ASS=1）
     -> "EHCI: schedule started, USBSTS=0x.... ASS=1"
  3. 端口复位后设备被判为**高速**并由 EHCI 自己拥有（PORTSC.PE=1 且未交还
     companion）-> "EHCI: port0 post-reset conn=1 en=1 ls=0"
  4. 异步环 + qTD 真的完成一笔控制传输（不只是写了寄存器）
     -> "EHCI-CTRL: GET_DESCRIPTOR dev @0 ok len=18 ... VID=0x..."
        "EHCI-CTRL: SET_ADDRESS(1) ok"
        "EHCI-CTRL: GET_DESCRIPTOR @1 ok VID=0x..."

铁律「两组结果必须相反」+「警惕弱断言」：
  A（挂 usb-kbd 到 usb-ehci）：
      - 必须出现 post-reset conn=1 **且 en=1**（高速自留，没被交还 companion）
      - 必须出现 @0 的 GET_DESCRIPTOR ok，且**实收 18 字节**（不是 0）
      - SET_ADDRESS 后 @1 读回的 VID/PID 必须与 @0 **相同**——这条能同时
        证明"真的收到了描述符字节"和"地址真的切过去了"，只断言"有 ok 字样"
        属于弱断言（写错 token 也可能印 ok）。
  B（只挂空 usb-ehci）：
      - 必须打印 "0 high-speed device(s) attached"
      - **绝不**出现 post-reset conn=1 / GET_DESCRIPTOR ok（fail closed 不误报）

QEMU 侧已实测的关键事实（别凭记忆改）：
  - `-device usb-ehci` 把**所有**挂上去的设备都报成 480 Mb/s（`info usb`
    可见），所以 QEMU 里没有 companion 也能在 EHCI 上跑控制传输；真机上
    FS/LS 设备会被交还 companion UHCI，那是另一条路径。
  - 设备必须显式挂到 `bus=ehci.0`，否则会落到别的控制器上。

用法：python tests/test_ehci.py   （退出码 0 = 全通过）
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
KERNEL_DIR = os.path.join(ROOT, "kernel")
sys.path.insert(0, HERE)
import ezocr  # noqa: E402

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4507
SERIAL_PORT = 4508
# icount shift=auto 下这台 QEMU 开机很慢，按"看得见证据"为准，不按乐观估计。
BOOT_WAIT = 150

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
SHOT = os.path.join(HERE, "ehci_shot.ppm").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add', ':': 'shift-semicolon'}


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

    def hmc(self, c):
        return self.cmd("human-monitor-command", **{"command-line": c})

    def type_line(self, s):
        """往 shell 打一行字（shell 输入只走键盘，QMP 只能用 sendkey）。"""
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


def run_case(name, extra_args, mode):
    """mode: "kbd"（只挂键盘）/ "msc"（只挂 U 盘）/ "empty"（什么都不挂）"""
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

        # U 盘场景要等 BOT（bulk）跑完，比纯控制传输慢
        if not wait_for(serial, "EHCI:", BOOT_WAIT):
            print("  FAIL %s: no 'EHCI:' line in serial" % name)
            print("---- serial tail ----\n" + serial.snapshot()[-1500:])
            return False, []

        wait_for(serial, "EHCI: controller ready", 60)
        # 统一传输层里枚举/类驱动跑在 EHCI 之后：死等固定秒数会采样到半截日志
        # （曾出现"只有 USB-HC 行、USB-ENUM 还没落地"的假 FAIL）。
        # 改成"等该场景的最后一条证据行出现"再采样。
        if mode == "msc":
            wait_for(serial, "USB-MSC:", 60)
            wait_for(serial, "registered as drive 12", 90)
        elif mode == "kbd":
            wait_for(serial, "USB-MSC:", 60)
        time.sleep(3.0)

        snap = serial.snapshot()
        # 铁律：收集条件不能只认 "EHCI"。统一传输层落地后，证据散落在
        # USB-HC / USB-ENUM / USB-MSC / USB-KBD 各行里，按 "EHCI" 过滤会
        # 把它们全丢掉 -> 假 FAIL（2026-09 实测踩过一次）。
        lines = [ln.strip() for ln in snap.splitlines()
                 if ("EHCI" in ln or "USB-" in ln or "MSC" in ln)]
        ok = True

        # ---- 两组都要成立：控制器被认领、异步调度真的在跑 ----
        if not any("EHCI controller(s) found" in l for l in lines):
            print("  FAIL %s: EHCI controller was not claimed" % name)
            ok = False
        if not any("schedule started" in l and "ASS=1" in l for l in lines):
            print("  FAIL %s: async schedule never reached ASS=1" % name)
            ok = False

        if mode == "kbd":
            # 高速自留：PE=1 且没被交还 companion
            pr = [l for l in lines if "post-reset" in l]
            if not any("conn=1 en=1" in l for l in pr):
                print("  FAIL %s: port reset did not yield a high-speed port"
                      % name)
                ok = False
            if any("released to companion" in l for l in lines):
                print("  FAIL %s: a 480Mb/s device was handed to companion"
                      % name)
                ok = False
            # 键盘是高速 HID：EHCI 还没周期调度，必须如实说"认领不了"，
            # 而不是假装能用（真机上键鼠是 FS/LS，走 companion UHCI）
            if not any("HID on EHCI skipped" in l for l in lines):
                print("  FAIL %s: expected explicit 'HID on EHCI skipped'" % name)
                ok = False
            if not any("USB-MSC: no mass storage device" in l for l in lines):
                print("  FAIL %s: keyboard-only case must not claim a drive"
                      % name)
                ok = False

            # 控制传输：实收 18 字节 + 地址切换前后 VID/PID 必须一致
            d0 = [l for l in lines if "GET_DESCRIPTOR dev @0 ok" in l]
            if not d0:
                print("  FAIL %s: no successful GET_DESCRIPTOR @0" % name)
                ok = False
                vid0 = None
            else:
                if not any("len=18" in l for l in d0):
                    print("  FAIL %s: device descriptor is not 18 bytes" % name)
                    ok = False
                m = re.search(r"VID=0x([0-9A-Fa-f]+)", d0[0])
                vid0 = m.group(1).upper() if m else None
                if not vid0 or int(vid0, 16) == 0:
                    print("  FAIL %s: VID from descriptor is 0 (no real data)"
                          % name)
                    ok = False

            if not any("SET_ADDRESS(1) ok" in l for l in lines):
                print("  FAIL %s: SET_ADDRESS did not succeed" % name)
                ok = False

            d1 = [l for l in lines if "GET_DESCRIPTOR @1 ok" in l]
            if not d1:
                print("  FAIL %s: re-read at addr 1 failed (address not taken)"
                      % name)
                ok = False
            elif vid0:
                m = re.search(r"VID=0x([0-9A-Fa-f]+)", d1[0])
                vid1 = m.group(1).upper() if m else None
                if vid1 != vid0:
                    print("  FAIL %s: VID changed %s -> %s after SET_ADDRESS"
                          % (name, vid0, vid1))
                    ok = False
        elif mode == "msc":
            # 统一传输层必须把这个口算到 EHCI 头上
            if not any("USB-HC: 1 connected port(s) (UHCI 0 / EHCI 1)" in l
                       for l in lines):
                print("  FAIL %s: unified port table missed the EHCI port" % name)
                ok = False
            if not any("USB-ENUM: 1 device(s) enumerated" in l for l in lines):
                print("  FAIL %s: enumeration over EHCI failed" % name)
                ok = False
            if not any("mps=512" in l for l in lines):
                print("  FAIL %s: bulk endpoints are not high-speed (512)" % name)
                ok = False
            if not any("blksize=512 ready" in l for l in lines):
                print("  FAIL %s: BOT (TEST UNIT READY + READ CAPACITY) failed"
                      % name)
                ok = False
            if not any("registered as drive 12" in l for l in lines):
                print("  FAIL %s: stick was not registered as a block drive"
                      % name)
                ok = False

            # ---- FS 全链路（屏幕 OCR）：挂载 -> 列目录 -> 读文件 ----
            # 这一段必须走真正的 512 字节扇区读，光有控制传输过不了。
            time.sleep(3.0)
            qmp.type_line("setdrive 12")
            time.sleep(6.0)
            qmp.type_line("ls")
            time.sleep(4.0)
            qmp.type_line("cat README.TXT")
            time.sleep(4.0)
            qmp.screendump()
            txt = flat(ocr_text())
            if "readme.txt" not in txt:
                print("  FAIL %s: ls on the EHCI stick lacks readme.txt" % name)
                print("  ocr> " + txt[:400])
                ok = False
            if "welcome to ezos" not in txt:
                print("  FAIL %s: cat README.TXT lacks content (BOT read)" % name)
                print("  ocr> " + txt[:400])
                ok = False
            if "panic" in txt:
                print("  FAIL %s: panic on screen" % name)
                ok = False
        else:
            # 空控制器：必须有"没有高速设备"的结论行，且绝不报任何成功
            if not any("0 high-speed device(s) attached" in l for l in lines):
                print("  FAIL %s: expected '0 high-speed device(s) attached'"
                      % name)
                ok = False
            for bad in ("post-reset conn=1", "GET_DESCRIPTOR dev @0 ok",
                        "SET_ADDRESS(1) ok", "GET_DESCRIPTOR @1 ok"):
                if any(bad in l for l in lines):
                    print("  FAIL %s: device-less case reports '%s'"
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

    ehci = ["-device", "usb-ehci,id=ehci"]
    msc = ["-drive", "format=raw,file=" + DISK.replace("\\", "/") +
           ",if=none,id=usbdata",
           "-device", "usb-storage,drive=usbdata,bus=ehci.0"]
    cases = [
        ("A-kbd", ehci + ["-device", "usb-kbd,bus=ehci.0"], "kbd"),
        ("C-msc", ehci + msc, "msc"),
        ("B-empty", ehci, "empty"),
    ]
    results = []
    for name, args, mode in cases:
        print("=== case: %s ===" % name)
        ok, _ = run_case(name, args, mode)
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
