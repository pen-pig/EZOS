# -*- coding: utf-8 -*-
"""ezos_qemu.py - 并行回归的资源隔离（端口段 + 私有镜像副本）

为什么需要：run_tests.py 原来是纯串行的，因为所有测试共享三样东西——
os-image.bin、disk.vhd、以及写死在脚本里的 QEMU 端口。两个测试同时跑就会
互相踩：抢同一个端口、抢同一个镜像文件（Windows 上第二个 QEMU 直接起不
来），症状是莫名其妙的 ConnectionRefused。

隔离办法：每个并行 worker 一个 slot 号（环境变量 EZOS_SLOT，由 run_tests
在启动子进程时注入）。

  - 端口：每个 slot 独占一段（100 个），段内按需递增分配，分配前探测是否
    真被占用（有上一轮残留的 QEMU 就跳过）。
  - 镜像/数据盘：slot>0 时复制一份到 temp/slot<N>/，各写各的。
    **slot 0（也就是不开启并行时）一律用仓库里的原路径**，行为与以前完全
    一致——这样并行功能的风险不会波及默认回归路径。

用法（在测试脚本里替换掉写死的常量）：

    from ezos_qemu import alloc_port, image_path, disk_path
    PORT = alloc_port()
    QMP_PORT, SER_PORT = alloc_port(2)
    PORT_A = alloc_port(3)
    IMG  = image_path()
    DISK = disk_path()
"""
import os
import socket
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# slot 号：run_tests 并行时为每个 worker 注入不同的值；直接跑脚本时为 0
try:
    SLOT = int(os.environ.get("EZOS_SLOT", "0") or "0")
except ValueError:
    SLOT = 0

PORTS_PER_SLOT = 100
PORT_BASE = 4400 + SLOT * PORTS_PER_SLOT

_next = [0]


def _in_use(port):
    """端口是否已被监听。用 connect 探测，比 bind 准（bind 会被 TIME_WAIT
    误导，connect 不会）。"""
    s = socket.socket()
    s.settimeout(0.15)
    try:
        return s.connect_ex(("127.0.0.1", port)) == 0
    except OSError:
        return False
    finally:
        try:
            s.close()
        except OSError:
            pass


def alloc_port(n=1):
    """分配 n 个本 slot 独占的、当前没人监听的端口。n=1 返回 int，否则 tuple。"""
    out = []
    tried = 0
    while len(out) < n:
        if tried >= PORTS_PER_SLOT:
            raise RuntimeError("slot %d 的端口段（%d..%d）用完了"
                               % (SLOT, PORT_BASE,
                                  PORT_BASE + PORTS_PER_SLOT - 1))
        p = PORT_BASE + _next[0]
        _next[0] = (_next[0] + 1) % PORTS_PER_SLOT
        tried += 1
        if not _in_use(p):
            out.append(p)
    return out[0] if n == 1 else tuple(out)


def _slot_copy(name):
    """把仓库根的 name 复制到本 slot 的私有目录（只在必要时复制）。

    slot 0 直接用原文件：不开启并行时行为与以前逐字节一致。
    """
    src = os.path.join(ROOT, name)
    if SLOT == 0:
        return src.replace("\\", "/")
    d = os.path.join(ROOT, "temp", "slot%d" % SLOT)
    try:
        os.makedirs(d, exist_ok=True)
    except OSError:
        pass
    dst = os.path.join(d, name)
    try:
        if (not os.path.isfile(dst)
                or os.path.getmtime(dst) < os.path.getmtime(src)
                or os.path.getsize(dst) != os.path.getsize(src)):
            shutil.copyfile(src, dst)
    except OSError:
        return src.replace("\\", "/")
    return dst.replace("\\", "/")


def image_path():
    """os-image.bin 的路径（slot>0 时是本 slot 的私有副本）。"""
    return _slot_copy("os-image.bin")


def disk_path():
    """disk.vhd 的路径（slot>0 时是本 slot 的私有副本）。

    多个 QEMU 同时打开同一个 raw 文件在 Windows 上会失败，而且这些盘会被
    写脏——各写各的副本才不会互相污染。
    """
    return _slot_copy("disk.vhd")


def kill_stale_qemu(exe="qemu-system-x86_64.exe"):
    """清掉上一轮遗留的 QEMU。

    串行（slot 0）时和以前一样直接 taskkill 全体——那时候机器上确实只该有
    自己的 QEMU。
    并行时**绝不能**这么做：会把别的槽位正在跑的机器一起杀掉，症状是某个
    测试毫无理由地 FAIL（实测 regress/fs_matrix/mouse 并行全红、串行全绿，
    就是这个）。并行时各槽位端口独立、镜像独立，残留进程不碍事（alloc_port
    会跳过被占用的端口），留到整轮结束由 run_tests 统一清理。
    """
    if SLOT != 0:
        return 0
    import subprocess
    return subprocess.call(["taskkill", "/F", "/IM", exe],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)


def slot():
    return SLOT


def log_path(path):
    """串口日志路径：并行时每个 slot 一份，slot 0 原样返回。

    两个测试共用同一个日志文件的后果很隐蔽——A 一直读到 B 写的内容，断言
    时红时绿。串行时不冲突（一个跑完才跑下一个），所以只在 slot>0 时改名。
    """
    p = path.replace("\\", "/")
    if SLOT == 0:
        return p
    d, base = os.path.split(p)
    return os.path.join(d, "slot%d_%s" % (SLOT, base)).replace("\\", "/")
