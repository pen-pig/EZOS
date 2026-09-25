# -*- coding: utf-8 -*-
"""vi 编辑器的功能回归（不是验证颜色，是验证"改了渲染路径后编辑/存盘没坏"）。

vi 的渲染被改成了逐字符着色渲染（语法高亮），画字符的方式变了，
最怕的是把插入/删除/存盘这些老功能弄坏——所以这里跑一遍最小闭环：
    vi 新建文件 -> 插入模式打字 -> ESC -> :wq -> cat 回读内容一致

用法：python tests/test_vi.py   （退出码 0 = 全通过）
端口 4482（端口必须每脚本唯一，见 tests/README.md）。
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rmdir as T
from test_rmdir import Qmp, ROOT, QEMU  # noqa: F401

PORT = 4482
NAME = "VITEST.C"
TEXT = "int main"          # 只用字母和空格：QEMU sendkey 对标点符号键名很挑


RESULTS = []


def check(ok, label):
    RESULTS.append((bool(ok), label))
    print(("PASS " if ok else "FAIL ") + label)


def summary():
    bad = [l for ok, l in RESULTS if not ok]
    print("== vi regression: %d/%d passed ==" % (len(RESULTS) - len(bad), len(RESULTS)))
    if bad:
        print("failed: " + "; ".join(bad))


def type_text(qmp, s):
    for ch in s:
        qmp.hmc("sendkey " + ("spc" if ch == ' ' else ch.lower()))
        time.sleep(0.05)


def main():
    disk = os.path.join(HERE, "disk_vi.img").replace("\\", "/")
    shutil.copyfile(os.path.join(ROOT, "disk.img"), disk)
    img = os.path.join(ROOT, "os-image.bin").replace("\\", "/")

    if T.port_in_use(PORT):
        subprocess.call(["taskkill", "//F", "//IM", "qemu-system-x86_64.exe"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        T.wait_port_free(PORT, 15)

    proc = subprocess.Popen([
        QEMU, "-icount", "shift=auto", "-vga", "std",
        "-drive", "format=raw,file=" + img,
        "-drive", "format=raw,file=" + disk,
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fails = 0
    try:
        qmp = Qmp(PORT)
        time.sleep(T.BOOT_WAIT)

        # 1) 新建并编辑
        T.run(qmp, "vi " + NAME, 10.0)
        time.sleep(0.5)
        qmp.hmc("sendkey i")            # 进入插入模式
        time.sleep(0.3)
        type_text(qmp, TEXT)
        qmp.hmc("sendkey esc")
        time.sleep(0.3)
        qmp.hmc("sendkey shift-semicolon")   # ':'
        time.sleep(0.2)
        qmp.hmc("sendkey w")
        qmp.hmc("sendkey q")
        qmp.hmc("sendkey ret")
        time.sleep(2.0)

        # 2) 文件必须存在
        out = T.last_out(T.run(qmp, "ls", 10.0)).lower()
        ok1 = NAME.lower() in out
        check(ok1, "vi 存盘后 ls 能看到 %s" % NAME)
        if not ok1:
            fails += 1

        # 3) 回读内容一致（证明插入与存盘都没坏）
        out2 = T.last_out(T.run(qmp, "cat " + NAME, 10.0)).lower()
        ok2 = TEXT.lower() in out2
        check(ok2, "cat 回读到写入的内容 '%s'" % TEXT)
        if not ok2:
            fails += 1

        # 4) 二次打开能读到已存内容（证明加载路径没坏）
        T.run(qmp, "vi " + NAME, 10.0)
        time.sleep(0.5)
        scr = qmp.screen().lower()
        ok3 = TEXT.lower() in scr
        check(ok3, "再次 vi 打开能看到已存内容")
        if not ok3:
            fails += 1
        qmp.hmc("sendkey esc")
        qmp.hmc("sendkey shift-semicolon")
        qmp.hmc("sendkey q")
        qmp.hmc("sendkey ret")
        time.sleep(1.0)

        # 5) 退出 vi 后 shell 还能正常用（证明 vi 没把终端状态带走）
        out4 = T.last_out(T.run(qmp, "ver", 10.0)).lower()
        ok4 = "version" in out4
        check(ok4, "退出 vi 后 shell 仍可用")
        if not ok4:
            fails += 1

        summary()
        return 1 if fails else 0
    finally:
        proc.kill()
        try:
            proc.wait(timeout=15)
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())
