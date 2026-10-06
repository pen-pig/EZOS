# -*- coding: utf-8 -*-
"""test_mouse.py - 指点设备（PS/2 + USB HID）包解析与注入路径 E2E。

为什么需要它：QEMU 里**没有触摸板**，QMP 也不发滚轮事件，所以滚轮分支与
"Y 轴方向"这类字节级约定在提交前根本没人跑过——真机上差一位就只能靠肉眼看
指针飘。本测试把这两条线钉住：

  1. 纯函数层：mouse_selftest 喂合成包（不同步 / X-Y 溢出 / 应答字节 / 三字节
     与四字节 IntelliMouse / USB boot 报告的 Y 轴反向），随 `selftest` 一起跑。
  2. 真实 IRQ12 路径：用 QMP input-send-event 发**相对位移与按键**，验证
     IRQ12 -> mouse_decode_ps2 -> mouse_accumulate 整条链路的坐标与按键真的
     生效（数值精确，不写"大约"）。
  3. 协议切换：mouseproto 4 / 3 可来回切（真机点亮触摸板滚轮时用得上），
     切完必须恢复 3 字节，否则后续用例的包流会永久错位。

端口 4585(QMP) / 4586(serial)。
"""
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe, qemu32_exe, ovmf_fd, qemu_img_exe  # noqa: E402

import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))

QEMU = qemu_exe()
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
WORK = os.path.join(HERE, "mouse_work.vhd").replace("\\", "/")
QMP_PORT, SER_PORT = 4585, 4586

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa

STATUS_RE = re.compile(
    r"proto=(\d+)B present=(\d+) usb=(\d+) pkts=(\d+) x=(\d+) y=(\d+) btn=(\d+)")


def boot(disk):
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + disk,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SER_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def guest_cmd(qmp, serial, line, settle=3.0):
    before = serial.size()
    qmp.type_line(line)
    time.sleep(settle)
    return serial.tail_from(before)


def status(qmp, serial):
    """跑一次 mouseproto 并解析出状态（取最后一次匹配，跳过键盘回显）"""
    txt = guest_cmd(qmp, serial, "mouseproto", 3.0)
    m = STATUS_RE.findall(txt)
    if not m:
        raise RuntimeError("no mouseproto status in %r" % txt[-200:])
    proto, present, usb, pkts, x, y, btn = [int(v) for v in m[-1]]
    return dict(proto=proto, present=present, usb=usb,
                pkts=pkts, x=x, y=y, btn=btn)


def rel_move(qmp, dx, dy):
    qmp.cmd("input-send-event",
            events=[{"type": "rel", "data": {"axis": "x", "value": dx}},
                    {"type": "rel", "data": {"axis": "y", "value": dy}}])


def button(qmp, name, down):
    qmp.cmd("input-send-event",
            events=[{"type": "btn", "data": {"button": name, "down": down}}])


def flat(s):
    return " ".join(s.split())


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def main():
    results = []
    if not os.path.isfile(IMG):
        print("MISSING os-image.bin - run ninja first")
        return 2
    shutil.copyfile(DISK, WORK)

    proc = boot(WORK)
    time.sleep(1.0)
    try:
        serial = SerialReader(SER_PORT)
        qmp = Qmp(QMP_PORT)
    except Exception as e:
        print("boot failed: %s" % e)
        kill_all_qemu()
        return 2
    wait_for(serial, "TASK: preemptive", 150)
    time.sleep(3.0)

    try:
        # ---- 1. 纯函数层：随 selftest 一起跑的合成包向量 ----
        out = flat(guest_cmd(qmp, serial, "selftest", 40))
        rec(results, "selftest: mouse ps2/usb packet vector",
            "mouse ps2/usb packet: PASS" in out,
            "| %s" % out[out.find("mouse"):out.find("mouse") + 70])
        rec(results, "selftest: mouse detail reports live proto",
            "ps2+usb decode, proto=3B" in out)

        # ---- 2. 真实 IRQ12 链路：相对位移 ----
        s0 = status(qmp, serial)
        rel_move(qmp, 40, -25)
        time.sleep(3.0)
        s1 = status(qmp, serial)
        rec(results, "irq12: ps2 mouse present", s0["present"] == 1,
            "| present=%d usb=%d" % (s0["present"], s0["usb"]))
        rec(results, "irq12: packets advanced after QMP move",
            s1["pkts"] > s0["pkts"],
            "| %d -> %d" % (s0["pkts"], s1["pkts"]))
        # PS/2 原生 Y 正=向上：QEMU 的 rel y=-25（向上）必须让**屏幕 y 减小**
        rec(results, "irq12: +40 x -> screen x +40 (exact)",
            s1["x"] - s0["x"] == 40,
            "| %d -> %d" % (s0["x"], s1["x"]))
        rec(results, "irq12: rel y=-25 -> screen y -25 (axis convention)",
            s1["y"] - s0["y"] == -25,
            "| %d -> %d" % (s0["y"], s1["y"]))

        # ---- 3. 按键：按下 bit0，抬起归零 ----
        button(qmp, "left", True)
        time.sleep(2.0)
        s2 = status(qmp, serial)
        rec(results, "irq12: left button down -> btn bit0",
            s2["btn"] & 1 == 1, "| btn=%d" % s2["btn"])
        button(qmp, "left", False)
        time.sleep(2.0)
        s3 = status(qmp, serial)
        rec(results, "irq12: left button up -> btn cleared",
            (s3["btn"] & 1) == 0, "| btn=%d" % s3["btn"])

        # ---- 4. 协议切换（真机触摸板滚轮用），必须能切回 3 字节 ----
        txt = flat(guest_cmd(qmp, serial, "mouseproto 4", 3.0))
        rec(results, "mouseproto 4 -> IntelliMouse 4-byte packets",
            "proto=4B" in txt, "| %s" % txt[-60:])
        txt = flat(guest_cmd(qmp, serial, "mouseproto 3", 3.0))
        rec(results, "mouseproto 3 -> back to standard 3-byte packets",
            "proto=3B" in txt, "| %s" % txt[-60:])

        # 切回后链路仍然活着（半包状态没被切坏）
        s4 = status(qmp, serial)
        rel_move(qmp, 10, 10)
        time.sleep(3.0)
        s5 = status(qmp, serial)
        rec(results, "irq12: link survives protocol switch",
            s5["pkts"] > s4["pkts"] and s5["x"] - s4["x"] == 10
            and s5["y"] - s4["y"] == 10,
            "| pkts %d->%d, dx=%d dy=%d"
            % (s4["pkts"], s5["pkts"], s5["x"] - s4["x"], s5["y"] - s4["y"]))
    finally:
        try:
            qmp.quit()
        except Exception:
            pass
        try:
            serial.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 15)
        if os.path.isfile(WORK):
            os.remove(WORK)

    bad = [n for n, ok in results if not ok]
    print("---- %d/%d passed ----" % (len(results) - len(bad), len(results)))
    if bad:
        print("FAILED: %s" % "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
