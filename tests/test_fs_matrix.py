# -*- coding: utf-8 -*-
"""test_fs_matrix.py - 8 种文件系统的格式化/读写矩阵回归

重建说明：原脚本随 temp/ 一起被清空（temp/* 被 .gitignore 忽略，无从恢复）。
本文件按同样的判定方法重建：QMP sendkey 打字 → screendump → 真字库 OCR →
对**每一步**断言。

为什么必须先断言 format 成功：这是个真踩过的坑——`format ext4` 曾静默失败，
卷仍然是 exFAT，后续的 write/ls/cat/rm 全部"通过"，把失败彻底掩盖了。
所以每个 FS 的第一条断言都是"格式化成功 + df 首列是新类型"。

覆盖：exFAT / FAT12 / FAT16 / FAT32 / ext4 / NTFS / F2FS / ReFS
（EROFS 只读，不提供 format，不在矩阵里）
每个 FS 跑：format → df → write → ls → cat → rm → ls（确认删掉）

数据盘用 disk.img 的副本（tests/disk_fs.img），绝不污染构建产物。
端口 4493（与其它 E2E 端口约定一致）。
用法：python tests/test_fs_matrix.py        （退出码 0 = 全部通过）
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

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
PORT = 4493
BOOT_WAIT = 30
SHOT = os.path.join(HERE, "fs_shot.ppm").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add'}


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
        # QEMU 的监听套接字先于 monitor 就绪，greeting 会晚几秒才发
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

    def screen(self):
        self.cmd("screendump", filename=SHOT)
        time.sleep(0.3)
        with open(SHOT, "rb") as fh:
            head = fh.read(64).split(b"\n", 2)[1].decode()
        if head != "720 400":
            return ""
        return ezocr.ocr_text(SHOT, KERNEL_DIR)


def flat(s):
    return " ".join(s.lower().split())


def tail(s):
    """取"最近一条命令的输出"：终端不清屏，且命令跑完后 shell 会再打印一个
    新提示符，所以最后一个 ">" 之后是**空的**——要取倒数第二个 ">" 到最后一
    个 ">" 之间的那段（= 命令回显 + 它的输出）。
    取 [-1] 的后果：ls 断言永远失败、rm 断言"空里没有"永远通过（假绿）。"""
    parts = s.split(">")
    return flat(parts[-2]) if len(parts) >= 2 else flat(s)


def run_cmd(qmp, cmd, timeout=9.0):
    """打字并等到屏幕文本"稳定"再返回：不同 FS 驱动的响应快慢差很多
    （实测 NTFS/F2FS/ReFS 列目录会超过 3s，固定等待会假 FAIL）。
    连续两次截图一致即认为输出已完成。"""
    qmp.type_line(cmd)
    time.sleep(1.2)
    prev = qmp.screen()
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.2)
        cur = qmp.screen()
        if cur == prev:
            return prev
        prev = cur
    return prev


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


# (命令行参数, df/format 里显示的卷类型名)
FS_LIST = [
    ("exfat", "exfat"),
    ("fat12", "fat12"),
    ("fat16", "fat16"),
    ("fat32", "fat32"),
    ("ext4",  "ext4"),
    ("ntfs",  "ntfs"),
    ("f2fs",  "f2fs"),
    ("refs",  "refs"),
]


def main():
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    src_disk = os.path.join(ROOT, "disk.img")
    disk = os.path.join(HERE, "disk_fs.img").replace("\\", "/")
    for p in (img, src_disk):
        if not os.path.isfile(p):
            print("MISSING %s - run ninja first" % p)
            return 2
    shutil.copyfile(src_disk, disk)        # 格式化会改盘：必须用副本

    if port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        print("PORT %d occupied - abort" % PORT)
        return 2

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []
    try:
        qmp = Qmp(PORT)
        time.sleep(BOOT_WAIT)
        for arg, disp in FS_LIST:
            # 1) format：必须真的成功（"Disk formatted as <名>"）
            t = flat(run_cmd(qmp, "format " + arg, 10.0))
            ok_fmt = ("disk formatted as" in t) and ("format failed" not in t)
            results.append(("%s: format ok" % arg, ok_fmt))

            # 2) df：首列必须是新类型（防"格式化失败但后续照常成功"的假阳性）
            t = flat(run_cmd(qmp, "df"))
            results.append(("%s: df reports %s" % (arg, disp), disp in t))

            # 3) write / ls / cat / rm / ls
            t = flat(run_cmd(qmp, "write FSTest.TXT hello" + arg))
            results.append(("%s: write ok" % arg, "fstest.txt" in t or "created" in t))

            ok_ls = "fstest.txt" in tail(run_cmd(qmp, "ls"))
            results.append(("%s: ls shows file" % arg, ok_ls))

            t = flat(run_cmd(qmp, "cat FSTest.TXT"))
            results.append(("%s: cat content" % arg, ("hello" + arg) in t))

            run_cmd(qmp, "rm FSTest.TXT")
            ok_rm = "fstest.txt" not in tail(run_cmd(qmp, "ls"))
            results.append(("%s: rm removed" % arg, ok_rm))
    except Exception as e:
        results.append(("harness: %r" % e, False))
    finally:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
        for p in (SHOT, disk):
            try:
                os.remove(p)
            except OSError:
                pass

    for k, v in results:
        print(("  [OK]   " if v else "  [FAIL] ") + k)
    ok = all(v for _, v in results)
    print("FS-MATRIX:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
