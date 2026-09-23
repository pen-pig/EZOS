# -*- coding: utf-8 -*-
"""test_rmdir.py - 六个可写文件系统上 rmdir 的回归

背景：rmdir 之前全代码库不存在。exFAT/FAT/ReFS 靠 delete_file 内部顺带支持
删空目录（用户得用 `rm` 才能删目录，提示语还写成 "File deleted."），
而 ext4/NTFS/F2FS 的 delete_file 明确 return -1 拒绝目录 —— 那三个 FS 上
**目录一旦创建就永久删不掉，块永久泄漏**。本矩阵把六个都钉死。

每个 FS 跑一组用例：
  mkdir SD        -> 成功
  ls              -> 能看到 SD
  rmdir SD        -> 成功（"Directory removed."）
  ls              -> SD 消失
  mkdir SD2 + 内部建文件 -> rmdir SD2 必须**失败**（非空）
  ls              -> SD2 还活着（失败时不允许有副作用）
  清理后 rmdir    -> 成功
  rmdir <文件>    -> 失败（rmdir 只收目录）
  rmdir <不存在>  -> 失败

判定用 QMP sendkey 打字 -> screendump -> 真字库 OCR，和 test_fs_matrix.py
同一套方法（含"取倒数第二个 >"、"等屏幕文本稳定"两条硬规矩）。

EROFS 是只读卷不提供 format，不在矩阵里。
数据盘用 disk.img 的副本（tests/disk_rmdir.img），绝不污染构建产物。
端口 4478（4477 被 test_netapp.py 的 netdev 占用，端口必须每脚本唯一）。用法：python tests/test_rmdir.py   （退出码 0 = 全通过）
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
PORT = 4478
BOOT_WAIT = 30
SHOT = os.path.join(HERE, "rmdir_shot.ppm").replace("\\", "/")

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


def last_out(s):
    """最近一条命令的输出 —— 必须取倒数第二个 ">" 之后到最后一个 ">" 之间。
    shell 跑完命令会立刻打一个新提示符，取 [-1] 恒为空串，断言会假红/假绿。"""
    parts = s.split(">")
    return flat(parts[-2]) if len(parts) >= 2 else flat(s)


def run(qmp, cmd, timeout=10.0):
    """等到屏幕文本稳定（连拍两张一致）再返回，不要用固定秒数：
    不同 FS 驱动响应差数倍，固定 sleep 会让同一脚本忽红忽绿。"""
    qmp.type_line(cmd)
    time.sleep(1.2)
    prev = qmp.screen()
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.2)
        cur = qmp.screen()
        if cur == prev:
            return last_out(prev)
        prev = cur
    return last_out(prev)


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


FS_LIST = ["exfat", "fat32", "ext4", "ntfs", "f2fs", "refs"]


def fs_cases(qmp, name):
    """返回 [(用例名, 通过?)]"""
    r = []

    t = run(qmp, "format " + name, 10.0)
    r.append(("%s: format ok" % name,
              ("disk formatted as" in t) and ("format failed" not in t)))
    r.append(("%s: df reports type" % name, name in run(qmp, "df")))

    # 1) 空目录：mkdir -> ls 可见 -> rmdir -> ls 不可见
    r.append(("%s: mkdir SD" % name, "mkdir: failed" not in run(qmp, "mkdir SD")))
    r.append(("%s: ls sees SD" % name, "sd" in run(qmp, "ls")))
    r.append(("%s: rmdir SD" % name, "directory removed" in run(qmp, "rmdir SD")))
    r.append(("%s: ls lost SD" % name, "sd" not in run(qmp, "ls")))

    # 2) 非空目录必须被拒绝，且失败后不留副作用
    r.append(("%s: mkdir SD2" % name, "mkdir: failed" not in run(qmp, "mkdir SD2")))
    run(qmp, "cd SD2")
    r.append(("%s: touch in SD2" % name, "file created" in run(qmp, "touch F.TXT")))
    run(qmp, "cd ..")
    r.append(("%s: rmdir SD2 refused (non-empty)" % name,
              "failed to remove directory" in run(qmp, "rmdir SD2")))
    r.append(("%s: SD2 survived" % name, "sd2" in run(qmp, "ls")))

    # 3) 清空后再删应当成功
    run(qmp, "cd SD2")
    run(qmp, "rm F.TXT")
    run(qmp, "cd ..")
    r.append(("%s: rmdir SD2 after cleanup" % name,
              "directory removed" in run(qmp, "rmdir SD2")))

    # 4) rmdir 只收目录：喂文件 / 不存在的名字都必须失败
    run(qmp, "touch FILE.TXT")
    r.append(("%s: rmdir <file> refused" % name,
              "failed to remove directory" in run(qmp, "rmdir FILE.TXT")))
    r.append(("%s: rmdir <missing> refused" % name,
              "failed to remove directory" in run(qmp, "rmdir NOSUCH")))
    run(qmp, "rm FILE.TXT")
    return r


def main():
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
    src = os.path.join(ROOT, "disk.img")
    disk = os.path.join(HERE, "disk_rmdir.img").replace("\\", "/")
    for p in (img, src):
        if not os.path.isfile(p):
            print("MISSING %s - run ninja first" % p)
            return 2
    shutil.copyfile(src, disk)

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
        for name in FS_LIST:
            results.extend(fs_cases(qmp, name))
    finally:
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
