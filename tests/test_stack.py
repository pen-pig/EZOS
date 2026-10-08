# -*- coding: utf-8 -*-
"""test_stack.py - 主栈水位守护：真实峰值必须离 .data 还有足够余量。

为什么要它
----------
内核栈**没有 MMU 保护**，主栈只有 [__data_end, 0x90000) 约 19KB，任务内核栈
TASK_KSIZE 只有 16KB。栈越界在这里不会崩，只会静默写穿紧邻的 .data：

    shell.c cmd_ls 曾在栈上放 fs_dir_entry_t entries[64]（16.9KB），
    一进 ls 就写穿并覆盖 fs.c 的 ro_cwd[256] —— 提示符变 [A.TXT]、
    cat/write 全部失败。看起来完全是文件系统坏了，其实盘是好的
    （宿主 ref_ext4 读得出），查了很久才定位。

两道防线，缺一不可：
  1. tools/check_stack.py（静态，单帧 > 12KB FAIL）—— 看不见**调用链叠加**
  2. 本测试（运行时，`stack` 命令读水位）—— 看不见"还没被踩到"的风险

水位的实现：启动时 kernel.c stack_paint() 把空闲栈涂成 0xA5，之后"第一个
不是 0xA5 的字节"就是历史最低 esp。所以只要曾经越过 __data_end，这里会直接
读到 100% —— 断言 <75% 就能挡住。

端口 4661(QMP) / 4662(serial)。
"""
import os as _os_ez
import sys as _sys_ez
_sys_ez.path.insert(0, _os_ez.path.dirname(_os_ez.path.abspath(__file__)))
from ezos_qemu import alloc_port, image_path, disk_path, log_path
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
import os as _os_ezos
import sys as _sys_ezos
_sys_ezos.path.append(_os_ezos.path.dirname(
    _os_ezos.path.dirname(_os_ezos.path.abspath(__file__))))
from ezos_env import qemu_exe  # noqa: E402

QEMU = qemu_exe()
IMG = image_path()
# 一块干净的小裸盘就够了：本测试不格式化、不写盘（避免脏了共享的 disk.vhd）
WORK = os.path.join(HERE, "stack_scratch.img").replace("\\", "/")
QMP_PORT, SER_PORT = alloc_port(2)

from test_nvme import SerialReader, Qmp, wait_for, wait_port_free, kill_all_qemu  # noqa

# 余量阈值。实测正常峰值约 50%（9776 / 19452 B）。留到 75% 是给未来
# 合理的调用链加深留空间，同时也远在"写穿 .data"之前。
LIMIT_PCT = 75


def rec(results, name, ok, detail=""):
    results.append((name, ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def flat(s):
    return " ".join(s.split())


RE_STACK = re.compile(r"main stack:\s*(\d+)\s*/\s*(\d+)\s*B used\s*\((\d+)%\)")


def parse_stack(out):
    m = RE_STACK.search(out)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def main():
    if not os.path.isfile(IMG):
        print("MISSING os-image.bin - run ninja first")
        return 2
    with open(WORK, "wb") as f:
        f.truncate(8 * 1024 * 1024)

    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + WORK,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SER_PORT],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        serial = SerialReader(SER_PORT)
        qmp = Qmp(QMP_PORT)
    except Exception as e:
        print("boot failed: %s" % e)
        kill_all_qemu()
        return 2
    results = []
    try:
        wait_for(serial, "TASK: preemptive", 150)
        wait_for(serial, "[/] >", 120)
        time.sleep(1.0)

        def run(cmd, settle=2.5, quiet=1.5, timeout=120.0):
            before = serial.size()
            qmp.type_line(cmd)
            t0 = time.time()
            last = -1
            stable = 0.0
            while time.time() - t0 < timeout:
                time.sleep(0.3)
                if time.time() - t0 < settle:
                    continue
                n = serial.size()
                if n == last:
                    stable += 0.3
                    if stable >= quiet:
                        break
                else:
                    stable = 0.0
                    last = n
            return flat(serial.tail_from(before))

        # 1) 命令存在且格式可解析
        out = run("stack", 2.5)
        st = parse_stack(out)
        rec(results, "stack command reports a high-water mark",
            st is not None, "| %s" % out[-70:])

        # 2) 数值本身要可信：总容量是链接期算出来的，太小说明符号没生效
        if st is None:
            print("\n0/%d passed" % len(results))
            return 1
        used, total, pct = st
        rec(results, "stack total looks sane (>8KB)", total > 8192,
            "| total=%d B" % total)

        # 3) 跑一批日常命令（含 ls —— 历史上就是它炸的），再看水位
        for c in ("ls", "ls /bin", "help", "sysinfo", "meminfo", "dmesg 5",
                  "ls /system"):
            run(c, 2.0)
        out = run("stack", 2.5)
        st2 = parse_stack(out)
        rec(results, "stack readable after running ls/help/sysinfo",
            st2 is not None, "| %s" % out[-70:])
        if st2 is None:
            print("\n%d/%d passed" % (sum(1 for _, ok in results if ok),
                                      len(results)))
            return 1
        used, total, pct = st2

        # 4) 核心断言：峰值离写穿 .data 还得有余量
        rec(results, "main stack peak < %d%% (越界会静默写穿 .data)"
            % LIMIT_PCT, pct < LIMIT_PCT,
            "| %d / %d B used (%d%%)" % (used, total, pct))
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
            proc.wait(timeout=15)
        except Exception:
            kill_all_qemu()
        wait_port_free(QMP_PORT, 20)
        try:
            os.remove(WORK)
        except OSError:
            pass

    bad = [n for n, ok in results if not ok]
    print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
