# -*- coding: utf-8 -*-
"""test_uefi3.py - U3b 双路径 E2E：UEFI(OVMF) 启动全链冒烟

覆盖（U3a 参数交接 + U3b 双路径）：
  1. OVMF 32 位启动链串口标记：EZEFI:gop write / uefi magic / ebs ok / mmap handoff
  2. 内核 UEFI 参数接收日志：GOP: resolution 1280x800（32bpp 直色 fb）
  3. pmm UEFI mmap 应用（type 1-4 EBS 后可回收）+ selftest 全 PASS（OCR）
  4. GUI 桌面 1280x800 渲染：任务栏深灰 + Win10 壁纸蓝渐变（像素断言）
  5. 开始菜单打开（帧差断言）-> "返回终端" -> 回切 720x400 文本 shell
  6. 回切后 shell 仍可用（ver 命令 OCR）+ 全程无 "kernel panic"

前置：uefi/esp/kernel.bin（507904 字节）+ uefi/esp/EFI/BOOT/BOOTIA32.EFI
     + disk.img（拷为本测试私有副本，避免污染基线盘）。
端口 4464（端口每脚本唯一，见 tests/README.md）。
用法：python tests/test_uefi3.py   （退出码 0 = 全通过）
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

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-i386.exe"
FD = "D:/MyOS/tools/qemu-portable-20241220/share/edk2-i386-code.fd"
ESP = os.path.join(ROOT, "uefi/esp").replace("\\", "/")
KERNEL_BIN = os.path.join(ESP, "kernel.bin")
BOOT_EFI = os.path.join(ESP, "EFI/BOOT/BOOTIA32.EFI").replace("\\", "/")
DISK_SRC = os.path.join(ROOT, "disk.img").replace("\\", "/")
DISK = os.path.join(HERE, "uefi3_disk.img").replace("\\", "/")
LOG = os.path.join(ROOT, "uefi/serial.log").replace("\\", "/")
PORT = 4464
BOOT_WAIT = 120
SHOT = os.path.join(HERE, "uefi3_shot.ppm").replace("\\", "/")
SHOT2 = os.path.join(HERE, "uefi3_shot2.ppm").replace("\\", "/")

# 1280x800 Win10 主题几何（gfxwin.c 推导 + 实测）：
# taskbar 32px | start btn (20,784) | menu row_h=53, list_y=69
# "return to terminal" row i=9 -> (100, 69 + 9*53 = 546)
START_BTN = (20, 784)
EXIT_ROW = (100, 546)

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus'}


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
        self.sock.settimeout(30)
        self.f = self.sock.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")
        # cursor starts at gw_start's warp point (screen center) and is
        # tracked across move_to calls - PS/2 deltas are cumulative.
        self.cx, self.cy = 640, 400

    def _read(self):
        line = self.f.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line) if line.strip() else None

    def cmd(self, name, **args):
        self.f.write((json.dumps(
            {"execute": name, "arguments": args}) + "\n").encode())
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

    def move_to(self, x, y):
        """Move cursor to absolute (x,y) in <=100px mouse_move steps.
        Huge single deltas clamp at +-255 and misalign the PS/2 packet
        stream, freezing the cursor - always move in small steps."""
        while self.cx != x or self.cy != y:
            dx = max(-100, min(100, x - self.cx))
            dy = max(-100, min(100, y - self.cy))
            self.hmc("mouse_move %d %d" % (dx, dy))
            self.cx += dx
            self.cy += dy
            time.sleep(0.08)
        time.sleep(0.15)

    def click(self):
        self.hmc("mouse_button 1")
        time.sleep(0.15)
        self.hmc("mouse_button 0")
        time.sleep(0.4)


def ppm_load(fn):
    with open(fn, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def ppm_dims(fn):
    with open(fn, "rb") as f:
        return f.read(64).split(b"\n", 2)[1].decode()


def zone_avg(buf, w, x0, y0, x1, y1, step=4):
    r = g = b = n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            o = (y * w + x) * 3
            n += 1
            r += buf[o]; g += buf[o + 1]; b += buf[o + 2]
    return (r // n, g // n, b // n)


def frame_diff(a, b, w, h, step=4):
    n = d = 0
    for y in range(0, h, step):
        base = y * w
        for x in range(0, w, step):
            o = (base + x) * 3
            n += 1
            if a[o:o + 3] != b[o:o + 3]:
                d += 1
    return d, n


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


def main():
    missing = [p for p in (QEMU, FD, KERNEL_BIN, BOOT_EFI, DISK_SRC)
               if not os.path.isfile(p)]
    if missing:
        print("MISSING %s - build kernel/EFI first (see uefi/build.sh)"
              % ", ".join(missing))
        return 2
    if os.path.getsize(KERNEL_BIN) != 507904:
        print("BAD esp/kernel.bin size %d (expect 507904)"
              % os.path.getsize(KERNEL_BIN))
        return 2
    shutil.copyfile(DISK_SRC, DISK)      # 私有副本，不污染基线盘
    if os.path.exists(LOG):
        os.remove(LOG)

    if port_in_use(PORT):                # 上轮残留：清掉再跑
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-i386.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        print("PORT %d still occupied - abort" % PORT)
        return 2

    proc = subprocess.Popen([
        QEMU, "-machine", "pc", "-m", "256", "-vga", "std",
        "-icount", "shift=auto",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + FD,
        "-drive", "if=none,format=raw,file=fat:rw:" + ESP + ",id=esp",
        "-usb", "-device", "usb-storage,drive=esp",
        "-drive", "format=raw,file=" + DISK,
        "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
        "-serial", "file:" + LOG,
        "-display", "none",
        "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []

    def check(name, ok):
        results.append((name, bool(ok)))

    def serial():
        if not os.path.exists(LOG):
            return ""
        return open(LOG, "rb").read().decode("latin-1", "replace")

    def ocr_720(fn):
        if ppm_dims(fn) != "720 400":
            return ""
        return ezocr.ocr_text(fn, KERNEL_DIR)

    def shot(fn):
        qmp.cmd("screendump", filename=fn)
        time.sleep(0.5)
        return fn

    try:
        # ---- boot: OVMF -> EZEFI loader -> kernel -> shell ----
        deadline = time.time() + BOOT_WAIT
        while time.time() < deadline:
            if "EZOS Shell" in serial():
                break
            time.sleep(1)
        else:
            check("boot: reached shell", False)
            raise RuntimeError("boot timeout")
        time.sleep(3)
        log = serial()
        check("loader: GOP fb write marker", "EZEFI:gop write@0x5000 ok" in log)
        check("loader: UEFI magic marker", "EZEFI:uefi magic@0x5010 ok" in log)
        check("loader: EBS ok marker", "EZEFI:ebs ok" in log)
        check("loader: mmap handoff marker", "EZEFI:mmap handoff size=" in log)
        check("kernel: GOP 1280x800 log", "GOP: resolution 1280x800" in log)
        check("boot: no kernel panic", "kernel panic" not in log)

        qmp = Qmp(PORT)
        qmp.type_line("ver")               # 吃 OVMF 键盘残留字节
        time.sleep(2)

        # ---- selftest via OCR (details go to console, not serial) ----
        qmp.type_line("selftest")
        time.sleep(20)
        t = flat(ocr_720(shot(SHOT)))
        check("selftest: ALL PASS", "all pass" in t)
        check("selftest: 0 failed", "0 failed" in t)

        # ---- shell -> desktop (32bpp GUI) ----
        qmp.type_line("exit")
        settled = False
        prev = shot(SHOT2)
        deadline = time.time() + BOOT_WAIT
        while time.time() < deadline:      # 等 boot anim 结束：两帧一致
            time.sleep(2.5)
            cur = shot(SHOT2)
            if open(cur, "rb").read() == open(prev, "rb").read():
                settled = True
                break
            prev = cur
        check("desktop: settled (boot anim done)", settled)

        shot(SHOT)
        check("desktop: 1280x800 mode", ppm_dims(SHOT) == "1280 800")
        w, h, dsk = ppm_load(SHOT)
        tb = zone_avg(dsk, w, 0, 772, 1280, 796)
        check("desktop: taskbar dark gray (22,23,23)+-15",
              all(abs(tb[i] - v) <= 15 for i, v in enumerate((22, 23, 23))))
        top = zone_avg(dsk, w, 900, 60, 1200, 260)    # 壁纸右侧三段
        mid = zone_avg(dsk, w, 900, 340, 1200, 540)
        bot = zone_avg(dsk, w, 900, 620, 1200, 756)
        check("desktop: wallpaper blue gradient top>mid>bot",
              top[2] > mid[2] > bot[2] and top[2] - bot[2] > 40)

        # ---- start menu open (frame diff) ----
        qmp.move_to(*START_BTN)
        qmp.click()
        time.sleep(1.5)
        shot(SHOT2)
        _, _, menu = ppm_load(SHOT2)
        d, n = frame_diff(dsk, menu, w, h)
        check("menu: opened (frame diff >25%%)", d > n // 4)

        # ---- "return to terminal" -> back to text shell ----
        qmp.move_to(*EXIT_ROW)
        qmp.click()
        time.sleep(3)
        shot(SHOT)
        check("exit GUI: back to 720x400 text mode", ppm_dims(SHOT) == "720 400")
        check("exit GUI: shell banner reprinted", serial().count("EZOS Shell") >= 2)

        # ---- shell still alive after round trip ----
        qmp.type_line("ver")
        time.sleep(3)
        t = flat(ocr_720(shot(SHOT)))
        check("shell after GUI: ver works", "version 0.9.0" in t)
        check("run: no kernel panic (final)", "kernel panic" not in serial())
    except Exception as e:
        results.append(("harness: %r" % e, False))
    finally:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
        for p in (SHOT, SHOT2, DISK):
            if os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass

    for k, v in results:
        print(("  [OK]   " if v else "  [FAIL] ") + k)
    ok = all(v for _, v in results)
    print("UEFI3:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
