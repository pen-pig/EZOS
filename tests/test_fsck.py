# -*- coding: utf-8 -*-
"""test_fsck.py - `fsck` 必须真的看出卷上的损伤，而且不能在好盘上瞎报

为什么要有这个测试
------------------
EZOS 所有可写 FS 的元数据更新都没有日志、没有两阶段提交。断电落在"写了
FAT 还没写位图"这种中间态时，卷会留下内核自己看不出来的损伤——读文件时
只跟目录项里的首簇，从不校验分配表与目录树是否自洽。宿主一挂上去就是
chkdsk 报错。

`fsck` 就是补这个洞的（read-only 检测）。但一个"检查器"本身极容易写出
**假阳性**：把元数据簇、NoFatChain 文件、FAT32 根链当成孤儿簇，于是好盘
上也报一堆问题——那比没有 fsck 更糟，因为它会让人学会忽略输出。

所以本测试同时钉住两头：

  * 好盘（刚格式化 + 放了文件的卷）必须报 `0 problem(s) -- consistent`
    ——这一条专门防假阳性，是本测试最有价值的部分。
  * 定向造出来的损伤必须被报出来，且问题类型要对得上
    ——这一条防"永远输出 consistent"的空壳实现。

损伤怎么造
----------
不用随机位翻转（test_corrupt.py 干那个）。这里要的是**可精确预测结果**的
损伤，所以走宿主机参考实现（tests/ref_fat.py / ref_exfat.py，与内核零共享）
定向改一个字节：

  * FAT32 孤儿簇：找一个 FAT 项为 0 的空闲簇，把它改成 EOC。
    于是"分配表说占用、目录树没人指" -> 期望 orphan=1。
  * FAT32 坏链：把根目录首簇的 FAT 项改成指向自己 -> 期望 NOT consistent
    （判 loop / bad_chain 之一，具体落哪个取决于遍历顺序，不钉死数字）。
  * exFAT 孤儿簇：把 Allocation Bitmap 里一个空闲簇的 bit 置 1
    （FAT 不动——exFAT 的权威分配表是位图，NoFatChain 文件在 FAT 里根本
     没有条目）-> 期望 orphan=1。

注意：位图 bit 只挑簇号 < 4096 的（512B/簇时正好落在位图第一簇内），
绕开参考实现 bmp_set 不跨簇的限制。

端口由 ezos_qemu.alloc_port() 分配（每脚本唯一，支持 -j 并行）。
用法：python tests/test_fsck.py   （退出码 0 = 全通过）
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
from ezos_env import qemu_exe  # noqa: E402

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU = qemu_exe()
PORT = alloc_port()
PROMPT = b"[/] > "
BOOT_TIMEOUT = 120.0
CMD_TIMEOUT = 60.0          # fsck 要扫整张 FAT/位图，坏链那例还要跑满上界

IMG = image_path()
SRC = disk_path()
LOG = log_path(os.path.join(HERE, "fsck_serial.log").replace("\\", "/"))
DISK = os.path.join(HERE, "fsck_disk.img").replace("\\", "/")
DISK_BYTES = 64 * 1024 * 1024   # FAT32 需要 >= 65525 簇，16MB 盘会被正确拒绝

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add'}

results = []


def rec(name, ok, note=""):
    results.append((name, ok, note))
    print("  [%s] %s%s" % ("OK" if ok else "FAIL", name,
                           ("  -- " + note) if note else ""))
    sys.stdout.flush()


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


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read()
    except OSError:
        return b""


def flat(b):
    return " ".join(b.decode("latin1", "replace").lower().split())


def wait_for(needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_log():
            return True
        time.sleep(0.5)
    return False


def run_cmd(qmp, cmd, timeout=CMD_TIMEOUT):
    """打一行命令，等到它真的跑完，返回 (跑完了?, 新增输出的扁平化文本)。"""
    n0 = len(read_log())
    qmp.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        new = read_log()[n0:]
        if b"\r\n" in new and new.rstrip().endswith(b">"):
            return True, flat(new)
        time.sleep(0.5)
    return False, flat(read_log()[n0:])


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


def launch(disk):
    if port_in_use(PORT):
        kill_stale_qemu("qemu-system-x86_64.exe")
        wait_port_free(PORT, 15)
    if not wait_port_free(PORT, 10):
        raise RuntimeError("PORT %d occupied" % PORT)
    try:
        os.remove(LOG)
    except OSError:
        pass
    p = subprocess.Popen([
        QEMU, "-vga", "std", "-display", "none", "-m", "128",
        "-drive", "format=raw,file=" + IMG,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-serial", "file:" + LOG,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = Qmp(PORT)
    if not wait_for(PROMPT, BOOT_TIMEOUT):
        p.kill()
        p.wait(timeout=15)
        raise RuntimeError("boot did not reach prompt")
    return p, qmp


def shutdown(p):
    p.kill()
    try:
        p.wait(timeout=15)
    except Exception:
        pass


def fresh_disk():
    shutil.copyfile(SRC, DISK)
    if os.path.getsize(DISK) < DISK_BYTES:
        with open(DISK, "r+b") as f:
            f.truncate(DISK_BYTES)


def gold(name):
    return os.path.join(HERE, "fsck_gold_%s.img" % name).replace("\\", "/")


def variant(name):
    return os.path.join(HERE, "fsck_bad_%s.img" % name).replace("\\", "/")


# ---------------- 定向损伤（走宿主机参考实现，与内核零共享） ----------------

def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def vol_base(path):
    """卷起点（字节）。disk 带 MBR 时分区 LBA 在偏移 454。"""
    with open(path, "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        return 0
    return _u32(mbr, 454) * 512


def make_fat32_orphan(dst):
    """把一个空闲簇的 FAT 项改成 EOC：分配表说占用、目录树没人指。

    期望 fsck 报 orphan=1（而不是"没看到"，更不是"好盘也报"）。
    直接读写文件：tests/ref_fat.py 的 Fat 是只读打开的（write_at 会抛
    io.UnsupportedOperation），它只用来做解析侧的对拍。
    """
    base = vol_base(dst)
    with open(dst, "r+b") as f:
        f.seek(base)
        bpb = f.read(512)
        bps = _u16(bpb, 11)
        reserved = _u16(bpb, 14)
        if bps < 512 or (bps & (bps - 1)) or reserved == 0:
            return None
        fat0 = base + reserved * bps
        # 从 64 开始：跳过根目录等已被元数据占掉的头部簇
        target = None
        for c in range(64, 4096):
            f.seek(fat0 + c * 4)
            b = f.read(4)
            if len(b) != 4:
                break
            if struct.unpack("<I", b)[0] == 0:
                target = c
                break
        if target is None:
            return None
        f.seek(fat0 + target * 4)
        f.write(struct.pack("<I", 0x0FFFFFF8))     # EOC：占用但无目录项指向
    return target


def make_fat32_cycle(dst):
    """把根目录首簇的 FAT 项改成指向自己 -> 目录链成环。"""
    base = vol_base(dst)
    with open(dst, "r+b") as f:
        f.seek(base)
        bpb = f.read(512)
        bps = _u16(bpb, 11)
        reserved = _u16(bpb, 14)
        root = _u32(bpb, 44)
        if bps < 512 or (bps & (bps - 1)) or reserved == 0 or root < 2:
            return None
        off = base + reserved * bps + root * 4
        f.seek(off)
        old = f.read(4)
        if len(old) != 4:
            return None
        f.seek(off)
        f.write(struct.pack("<I", root & 0x0FFFFFFF))
        return root


def make_exfat_orphan(dst):
    """把 Allocation Bitmap 里一个空闲簇的 bit 置 1（FAT 不动）。

    exFAT 的权威分配表是位图：NoFatChain 文件在 FAT 里根本没有条目。只查
    FAT 的实现会把整卷单簇文件全报成孤儿，只查位图 + 漏掉元数据簇的实现
    又会在好盘上瞎报。这里正好把两种错法都钉住。
    """
    from ref_exfat import ExfatRW
    fs = ExfatRW(dst)
    target = None
    for c in range(2, 4096):
        if fs.fat(c) == 0 and not fs.bmp_get(c):
            target = c
            break
    if target is None:
        return None
    fs.bmp_set(target, 1)
    fs.flush()
    return target


# ---------------- 单个 FS 的完整流程 ----------------

def build_gold(fsname, fmt_cmd):
    """格式化 + 放文件 + 立刻 fsck（好盘必须 consistent），存下黄金镜像。"""
    fresh_disk()
    p, qmp = launch(DISK)
    try:
        ok, out = run_cmd(qmp, fmt_cmd, timeout=60.0)
        rec("%s: %s" % (fsname, fmt_cmd), ok and "formatted" in out, out[:80])
        if not ok:
            return None
        time.sleep(1.0)
        run_cmd(qmp, "mkdir SD", timeout=30.0)
        run_cmd(qmp, "write SD/a hello", timeout=30.0)
        run_cmd(qmp, "write B.TXT world", timeout=30.0)
        time.sleep(0.5)
        ok, out = run_cmd(qmp, "fsck", timeout=CMD_TIMEOUT)
        rec("%s: fsck on a healthy volume reports no problem" % fsname,
            ok and "fsck: 0 problem(s) -- consistent" in out,
            out[:160])
    finally:
        shutdown(p)
        shutil.copyfile(DISK, gold(fsname))
    return gold(fsname)


def check_damaged(fsname, tag, corruptor, expect):
    """用坏盘开一台新机，跑 fsck，断言期望文本出现。"""
    src = gold(fsname)
    dst = variant(tag)
    shutil.copyfile(src, dst)
    hit = corruptor(dst)
    if hit is None:
        rec("%s: %s" % (fsname, tag), False, "corruptor could not find a target")
        return
    p, qmp = launch(dst)
    try:
        ok, out = run_cmd(qmp, "fsck", timeout=CMD_TIMEOUT)
        rec("%s: %s -> %s" % (fsname, tag, expect), ok and expect in out,
            out[:160])
    finally:
        shutdown(p)


def main():
    print("FSCK: fsck must see real damage and stay silent on healthy volumes")
    g = build_gold("fat32", "format fat32")
    if g:
        check_damaged("fat32", "fat32_orphan", make_fat32_orphan, "orphan=1")
        check_damaged("fat32", "fat32_cycle", make_fat32_cycle,
                      "not consistent")
    g = build_gold("exfat", "format exfat")
    if g:
        check_damaged("exfat", "exfat_orphan", make_exfat_orphan, "orphan=1")

    bad = [n for (n, ok, _) in results if not ok]
    print("---- %d/%d passed ----" % (len(results) - len(bad), len(results)))
    if bad:
        for n in bad:
            print("  FAILED: %s" % n)
        print("FSCK: FAIL")
        return 1
    print("FSCK: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
