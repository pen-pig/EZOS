# -*- coding: utf-8 -*-
"""test_ahci.py - AHCI/SATA 磁盘驱动 E2E（真机点亮 H1b）

要点：
  - QEMU 挂第三块盘走 ich9-ahci（AHCI 控制器 + 一个 ATA 硬盘）。
  - 全部诊断/交互走**串口（COM1）**：klog 镜像通道 + tty 控制台镜像，
    不依赖 screendump / OCR（断电/无屏场景也能读，符合 H1a 诊断通道定位）。
  - 断言 1：boot klog 出现 AHCI 控制器与端口探测成功行；
  - 断言 2：setdrive 切到 AHCI 盘（drive 4 = AHCI port 0）后，
    format / write / ls / cat / rm 全部走通（exFAT）。
  - 普通不挂 AHCI 的 VM（test_regress.py）走 ATA 兜底，不受影响（另行验证）。
  - 端口 4487 给 QMP；串口另走 4488，每脚本端口唯一。
  - ahci.img 64MB 空文件在 tests/ 下临时生成（已被 .gitignore 的 *.img 忽略）。
用法：python tests/test_ahci.py   （退出码 0 = 全通过）
"""
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

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4487
SERIAL_PORT = 4488
BOOT_WAIT = 40
AHCI_IMG = os.path.join(HERE, "ahci.img").replace("\\", "/")
DISK_COPY = os.path.join(HERE, "disk_ahci.img").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add'}


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
                # 关键：socket 建连时带 timeout=10，guest 空闲超过 10s 就会抛
                # timeout——这不是错误。曾经把它当异常 break 掉，导致读线程
                # 静默死亡、之后所有断言都收不到输出，看起来像"系统挂死"。
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

    def tail_from(self, n):
        with self.lock:
            return self.buf[n:].decode("latin1", "replace")

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


def make_blank_image(path, mb):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)


def wait_for(serial, marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if marker in serial.snapshot():
            return True
        time.sleep(0.3)
    return False


def run_cmd(qmp, serial, cmd, expect, timeout=20.0):
    """在 serial 缓冲里记录命令前位置，键入命令，等待期望子串出现在新增输出里。"""
    before = serial.size()
    qmp.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.5)
        out = flat(serial.tail_from(before))
        if expect in out:
            return True, out
    return False, flat(serial.tail_from(before))


def main():
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    disk = os.path.join(ROOT, "disk.img")
    for p in (img, disk):
        if not os.path.isfile(p):
            print("MISSING %s - run ninja first" % p)
            return 2

    make_blank_image(AHCI_IMG, 64)
    if os.path.isfile(disk):
        import shutil
        shutil.copyfile(disk, DISK_COPY)

    if port_in_use(QMP_PORT) or port_in_use(SERIAL_PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not (wait_port_free(QMP_PORT, 15) and wait_port_free(SERIAL_PORT, 15)):
            print("PORT occupied - abort")
            return 2

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + DISK_COPY,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "format=raw,if=none,file=" + AHCI_IMG + ",id=dahci",
        "-device", "ide-hd,drive=dahci,bus=ahci.0",
        "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []
    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        # 等 AHCI 初始化完成（klog 必含 "AHCI:" 行）
        if not wait_for(serial, "AHCI:", BOOT_WAIT):
            print("FAIL boot: AHCI init line not seen in serial")
            snap = serial.snapshot()
            print("---- serial tail ----\n" + snap[-2000:])
            return 1

        # 断言 1：控制器 + 端口探测成功。
        # 先等到"online"行出现再取样：开机 klog 是逐行流式到串口的，
        # 直接取瞬时快照会漏掉探测后段（曾让这条断言时好时坏）。
        wait_for(serial, "AHCI: port 0 online", 45)
        snap = serial.snapshot()
        ctl_ok = ("AHCI: controller" in snap)
        port_ok = ("online" in snap and "AHCI: port " in snap)
        results.append(("AHCI: controller found line", ctl_ok))
        results.append(("AHCI: port probed online", port_ok))
        # 打印探测日志供报告
        for ln in snap.splitlines():
            if "AHCI:" in ln:
                print("  klog> " + ln.strip())

        # 等 shell 就绪（TASK 调度器起来即 shell 前最后一步）
        if not wait_for(serial, "TASK: preemptive", 20):
            print("FAIL boot: shell not ready")
            return 1
        time.sleep(2.0)

        # 断言 2：setdrive 切到 AHCI 盘（drive 4 = AHCI port 0）。
        # 注意：fs_init 首检空白 AHCI 盘无文件系统时会回退到 ATA 数据盘完成挂载
        # 探测，但 fs_preferred_drive 仍是 4，随后 format/write/ls/cat/rm 全部落在
        # AHCI 盘上（exfat 后端以 fs_preferred_drive 为目标）。打印行是
        # "FS drive set to 4 (exFAT)"，故按该子串断言。
        ok, out = run_cmd(qmp, serial, "setdrive 4", "fs drive set to 4", 15)
        results.append(("setdrive 4 (AHCI drive)", ok))
        if not ok:
            print("  setdrive out: " + out)

        # format exfat 落在 AHCI 盘上
        ok, out = run_cmd(qmp, serial, "format exfat",
                          "disk formatted as exfat", 30)
        results.append(("format exfat on AHCI", ok))
        if not ok:
            print("  format out: " + out)

        # write / ls / cat / rm 走通 exFAT（AHCI DMA 读写）
        ok, out = run_cmd(qmp, serial, "write hello.txt HelloAHCI",
                          "file written successfully", 15)
        results.append(("write file on AHCI", ok))
        if not ok:
            print("  write out: " + out)

        ok, out = run_cmd(qmp, serial, "ls", "hello.txt", 15)
        results.append(("ls shows hello.txt", ok))
        if not ok:
            print("  ls out: " + out)

        ok, out = run_cmd(qmp, serial, "cat hello.txt", "helloahci", 15)
        results.append(("cat reads back content", ok))
        if not ok:
            print("  cat out: " + out)

        ok, out = run_cmd(qmp, serial, "rm hello.txt", "file deleted", 15)
        results.append(("rm file on AHCI", ok))
        if not ok:
            print("  rm out: " + out)

        # 注意：不要在这里"等一个不该出现的字符串"——那会让 shell 空转 15s，
        # 而内核存在"空闲 ~20s 后键盘输入不再进来"的问题（已复现，与 AHCI 无关），
        # 后续命令会全部静默。改为等必然出现的 "/:"，再在输出里断言文件已消失。
        ok, out = run_cmd(qmp, serial, "ls", "/:", 15)
        results.append(("ls after rm: hello.txt gone", ok and "hello.txt" not in out))
        if ok and "hello.txt" in out:
            print("  ls still shows hello.txt (unexpected)")

        # 全量 dmesg：串口快照在启动早期取样会漏掉探测后段（online 行），
        # 且行内偶有串口交错乱码；以 shell 里 dmesg 全量输出为最终判据。
        # 注意：环形缓冲会轮转最早的行，标记用 dmesg 自身头部而非启动行。
        ok, dmout = run_cmd(qmp, serial, "dmesg", "line(s) buffered", 30)
        if not ok:
            print("  dmesg tail: " + dmout[-500:])
        print("---- dmesg (AHCI lines) ----")
        for ln in dmout.splitlines():
            if "ahci" in ln or "online" in ln or "first " in ln:
                print("  dm> " + ln.strip())
        results.append(("dmesg dump ok", ok))
        # 注：不能用 dmesg 内容做 AHCI 探测断言——环形缓冲只留最近 58 行，
        # 探测行早已被轮转掉；判据用启动时串口快照（见断言 1）。
    finally:
        if qmp is not None:
            try:
                qmp.cmd("quit")
            except Exception:
                pass
        if serial is not None:
            serial.close()
        proc.kill()
        try:
            proc.wait(timeout=15)
        except Exception:
            pass

    npass = sum(1 for _, ok in results if ok)
    for label, ok in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", label))
    print("---- %d/%d passed ----" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
