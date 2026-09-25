# -*- coding: utf-8 -*-
"""真机点亮 H1a 回归：串口诊断通道 + ACPI 电源管理。

真机上没有 QMP/screendump，COM1 是开机第一毫秒起唯一的诊断通道；
shutdown 在真机上必须走 ACPI PM1a_CNT（QEMU 的 0x604 硬编码无效）。
本测试在 QEMU 里等价验证这两条路径：
  1. -serial file 抓 COM1：断言启动日志/串口自检/ACPI 解析结果出现在串口侧
  2. shutdown：ACPI S5 使 QEMU 进程退出（等价于真机断电）
  3. reboot：8042 复位后 BIOS 重新走完启动，shell 再次可用

用法：python tests/test_power.py   （退出码 0 = 全通过）
端口 4486（端口必须每脚本唯一，见 tests/README.md）。
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rmdir as T
from test_rmdir import Qmp, ROOT, QEMU

PORT = 4486
SERIAL_LOG = os.path.join(ROOT, "temp", "serial_power.log").replace("\\", "/")


def boot_and_wait(qmp, proc):
    T.run(qmp, "ver", 15.0)          # 稳定屏幕 + 确认 shell 可用
    out = T.last_out(T.run(qmp, "ver", 10.0))
    return "version" in out


def main():
    os.makedirs(os.path.dirname(SERIAL_LOG), exist_ok=True)
    if T.port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        T.wait_port_free(PORT, 15)
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    disk = os.path.join(ROOT, "disk.img").replace("\\", "/")

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-serial", "file:" + SERIAL_LOG,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []

    def check(ok, label):
        results.append((bool(ok), label))
        print(("PASS " if ok else "FAIL ") + label)

    try:
        qmp = Qmp(PORT)
        time.sleep(T.BOOT_WAIT)

        # 1) shell 可用（基础 sanity）
        check(boot_and_wait(qmp, proc), "boot: reached shell")

        # 2) 串口侧应拿到启动日志（自检 + ACPI 解析 + 内核 banner）
        time.sleep(1.0)
        ser = ""
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                ser = f.read()
        except OSError:
            pass
        check("EZOS Kernel" in ser, "serial: kernel banner mirrored to COM1")
        check("SERIAL: COM1 115200 8N1 loopback OK" in ser,
              "serial: loopback self-test logged")
        check("ACPI:" in ser, "serial: ACPI status line mirrored")

        # 3) shutdown -> ACPI S5 -> QEMU 进程退出（真机断电的等价物）。
        # 注意：不能走 T.run 的 screendump 稳定等待——机器一断电 QMP
        # 连接就被掐断（WinError 10054），这里直接打字不等屏幕。
        qmp.type_line("shutdown")
        try:
            rc = proc.wait(timeout=30)
            exited = True
        except subprocess.TimeoutExpired:
            exited = False
            rc = None
        check(exited, "shutdown: QEMU powered off via ACPI (rc=%s)" % rc)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=15)
            except Exception:
                pass

    # 4) reboot：独立 VM（关机测试已退出，不能复用）
    proc2 = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        qmp2 = Qmp(PORT)
        time.sleep(T.BOOT_WAIT)
        check(boot_and_wait(qmp2, proc2), "reboot-pre: shell up")
        T.run(qmp2, "reboot", 10.0)
        time.sleep(3.0)              # 让 BIOS 重新走一遍
        # 等提示符真正出现再敲命令：重启后自检+日志要重跑几十秒，
        # 提前打字会被丢弃（探针实证），且无 ">" 时 last_out 解析错位
        ok_prompt = False
        end = time.time() + 90
        while time.time() < end:
            if ">" in qmp2.screen():
                ok_prompt = True
                break
            time.sleep(2.0)
        check(ok_prompt, "reboot: shell prompt reappeared")
        out = T.last_out(T.run(qmp2, "ver", 15.0))
        check("version" in out, "reboot: shell usable after reboot")
    finally:
        if proc2.poll() is None:
            proc2.kill()
            try:
                proc2.wait(timeout=15)
            except Exception:
                pass

    bad = [l for ok, l in results if not ok]
    print("== power regression: %d/%d passed ==" %
          (len(results) - len(bad), len(results)))
    if bad:
        print("failed: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
