# -*- coding: utf-8 -*-
"""test_sysvol.py - 内核内置只读系统卷（/system + /bin）E2E

要解决的问题（实测坐实过，不是推理）：
    用户程序原先放在数据盘根目录，一条 `format` 就把 9 个 ELF 全抹掉
    -> format 之后 `ls` 空、`exec HELLO.ELF` 报 file not found。
Linux 的解法是系统与数据分属不同挂载点，格式化数据分区不影响系统。
EZOS 这里把系统卷编进内核镜像（.rodata），挂成 /system 与 /bin，只读。

判定走**串口（COM1）**按子串断言（沿用 nvme/ehci 那套）。
端口 A 组 4511(QMP)/4512(serial)，B 组 4513/4514，避开已占用的 4463-4510。

铁律「两组结果必须相反」+「警惕弱断言」：
  A（系统卷隔离性）：
     - format exfat 之后：`ls`（数据卷）变空，**但** `ls /bin` 仍有
       hello.elf、`exec hello` 仍 exited with code 42 —— 同一时刻两种
       相反结果，这是本测试存在的意义；只测一半就是弱断言。
     - 写 /bin 必须失败（Failed to write file. / Failed to delete file.）
     - cat /bin/nope.elf 必须 not found（证明查找不是"全都命中"）
     - `cd /system` 之后裸名 `cat version` 必须等价于 `cat /system/version`
       （cwd 只在呈现层记着时这条会红），且 `cd ..` 能离开
  B（数据卷对照组，独立 QEMU）：
     - 数据盘 write/ls/cat/rm 全通 —— 证明"拒绝"只针对 /bin，
       不是 fs_create_file 被改坏了；A 组若全拒这里是反向证据。
     - `ls /bin` 绝不出现数据盘上新建的文件（两个卷不串）

用法：python tests/test_sysvol.py   （退出码 0 = 全通过）
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from test_nvme import (SerialReader, Qmp, flat, wait_for, wait_port_free,  # noqa: E402
                       kill_all_qemu, run_cmd)

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.img")

PORT_A = (4511, 4512)      # (QMP, serial)
PORT_B = (4513, 4514)
BOOT_WAIT = 150

# 写测试会改盘内容，绝不拿 disk.img 本体开刀——每次跑复制一份。
WORK_A = os.path.join(HERE, "sysvol_a.img").replace("\\", "/")
WORK_B = os.path.join(HERE, "sysvol_b.img").replace("\\", "/")

BIN_PROGRAMS = ["hello.elf", "forktest.elf", "fdtest.elf", "fdleak.elf",
                "spin.elf", "segprobe.elf", "netecho.elf", "nettcp.elf",
                "netcli.elf"]


def boot(work, ports):
    qmp_port, ser_port = ports
    if os.path.isfile(DISK):
        shutil.copyfile(DISK, work)
    return subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + work,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % ser_port,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def shell_up(serial, qmp):
    """等 shell 就绪。返回 True/False。"""
    if not wait_for(serial, "TASK: preemptive", BOOT_WAIT):
        return False
    time.sleep(3.0)
    return True


def teardown(proc, qmp, serial, qmp_port):
    try:
        if qmp:
            qmp.quit()
    except Exception:
        pass
    try:
        if serial:
            serial.close()
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        kill_all_qemu()
    wait_port_free(qmp_port, 15)


def case_sysvol():
    """A 组：format 打不掉系统卷；/bin 只读。"""
    results = []
    proc = boot(WORK_A, PORT_A)
    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_A[1])
        qmp = Qmp(PORT_A[0])

        snap0 = None
        if not wait_for(serial, "SYSVOL:", BOOT_WAIT):
            print("  FAIL A: no SYSVOL: line in serial")
            print("---- serial tail ----\n" + serial.snapshot()[-1200:])
            return [("sysvol init line seen", False)]
        snap0 = serial.snapshot()

        # 1) 内置卷真的编进来了（文件数与字节数是生成脚本的实数，编的过不了）
        results.append(("sysvol ready line with real counts",
                        "11 files" in snap0 and "16922 bytes" in snap0
                        and "read-only" in snap0))
        for ln in snap0.splitlines():
            if "SYSVOL:" in ln:
                print("  klog> " + ln.strip())

        if not shell_up(serial, qmp):
            print("  FAIL A: shell not ready")
            results.append(("shell ready", False))
            return results

        # 2) ls /bin 列出全部内置程序
        ok, out = run_cmd(qmp, serial, "ls /bin", "hello.elf")
        missing = [p for p in BIN_PROGRAMS if p not in out]
        results.append(("ls /bin lists all 9 built-in programs", ok and not missing))
        if missing:
            print("  ls /bin missing: " + ",".join(missing))

        # 3) ls /system 列出元信息文件
        ok, out = run_cmd(qmp, serial, "ls /system", "version")
        results.append(("ls /system lists version + files",
                        ok and "files" in out))

        # 4) cat /system/version 真读到内容
        ok, out = run_cmd(qmp, serial, "cat /system/version",
                          "mount=/system /bin")
        results.append(("cat /system/version reads content", ok))

        # 5) exec 裸名从 /bin 跑起来（补 .elf 后缀）
        ok, out = run_cmd(qmp, serial, "exec hello", "exited with code 42")
        results.append(("exec hello runs from /bin (bare name)", ok))
        if not ok:
            print("  exec hello out: " + out)

        # 6) 老的 exec HELLO.ELF 写法仍可用（大小写不敏感）
        ok, out = run_cmd(qmp, serial, "exec HELLO.ELF", "exited with code 42")
        results.append(("exec HELLO.ELF still works (case-insensitive)", ok))

        # 7) 写 /bin 必须被拒
        ok, out = run_cmd(qmp, serial, "write /bin/x hi", "failed to write file")
        results.append(("write to /bin rejected", ok))
        if not ok:
            print("  write /bin out: " + out)

        # 8) 删 /bin 里的程序必须被拒
        ok, out = run_cmd(qmp, serial, "rm /bin/hello.elf",
                          "failed to delete file")
        results.append(("rm /bin/hello.elf rejected", ok))

        # 9) 不存在的文件必须 not found（防"查找恒真"型假成功）
        ok, out = run_cmd(qmp, serial, "cat /bin/nope.elf", "file not found")
        results.append(("cat /bin/nope.elf reports not found", ok))

        # ---- 相对路径：cd 进系统卷后裸名必须解析到系统卷 ----
        # 守的 bug：cwd 曾只记在 shell 呈现层（ls/pwd 看着对），但 fs_* 入口
        # 拿裸名直接查数据盘 -> `cd /system` 后 `cat version` 报 no such file，
        # 只有写绝对路径 `/system/version` 才行。
        ok, out = run_cmd(qmp, serial, "cd /system", "")
        ok, out = run_cmd(qmp, serial, "pwd", "/system")
        results.append(("cd /system then pwd shows /system", ok))

        ok, out = run_cmd(qmp, serial, "cat version", "mount=/system /bin")
        results.append(("relative cat resolves inside /system", ok))
        if not ok:
            print("  relative cat out: " + out)

        # 反例：解析不能是"全都命中"——不存在的文件照样 not found
        ok, out = run_cmd(qmp, serial, "cat nope", "file not found")
        results.append(("relative cat of missing file reports not found", ok))

        # 反例：相对路径的写也要被系统卷只读拦住，不能漏写到数据盘根目录
        ok, out = run_cmd(qmp, serial, "write v2 hi", "failed to write file")
        results.append(("relative write inside /system rejected", ok))

        ok, out = run_cmd(qmp, serial, "cd ..", "")
        ok, out = run_cmd(qmp, serial, "pwd", "/")
        left = ok and "system" not in out
        results.append(("cd .. leaves the system volume", left))
        if not left:
            print("  pwd after cd .. out: " + out)

        # ---- 核心：format 之后数据卷空了，系统卷还在 ----
        ok, out = run_cmd(qmp, serial, "format exfat", "disk formatted as exfat",
                          timeout=60.0)
        results.append(("format exfat succeeded", ok))
        if not ok:
            print("  format out: " + out)
            return results

        ok, out = run_cmd(qmp, serial, "ls", "/:")
        data_empty = ok and ("readme.txt" not in out) and (".elf" not in out)
        results.append(("data volume wiped by format", data_empty))
        if not data_empty:
            print("  ls after format out: " + out)

        ok, out = run_cmd(qmp, serial, "ls /bin", "hello.elf")
        still = ok and all(p in out for p in BIN_PROGRAMS)
        results.append(("CORE: ls /bin still lists programs after format",
                        still))
        if not still:
            print("  ls /bin after format out: " + out)

        ok, out = run_cmd(qmp, serial, "exec hello", "exited with code 42")
        results.append(("CORE: exec hello still runs after format", ok))
        if not ok:
            print("  exec after format out: " + out)

        return results
    finally:
        teardown(proc, qmp, serial, PORT_A[0])


def case_datavol():
    """B 组：数据盘读写仍正常（拒绝只针对 /bin），且两卷不串。"""
    results = []
    proc = boot(WORK_B, PORT_B)
    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(PORT_B[1])
        qmp = Qmp(PORT_B[0])
        if not shell_up(serial, qmp):
            print("  FAIL B: shell not ready")
            return [("shell ready", False)]

        ok, out = run_cmd(qmp, serial, "write datavol.txt hello",
                          "file written successfully")
        results.append(("data volume write still works", ok))
        if not ok:
            print("  write out: " + out)

        ok, out = run_cmd(qmp, serial, "ls", "datavol.txt")
        results.append(("ls data volume shows the new file", ok))

        ok, out = run_cmd(qmp, serial, "cat datavol.txt", "hello")
        results.append(("cat data volume reads back", ok))

        # 数据盘新建的文件绝不能出现在 /bin 里（两个卷不串）
        ok, out = run_cmd(qmp, serial, "ls /bin", "hello.elf")
        results.append(("ls /bin does not show data-volume file",
                        ok and "datavol.txt" not in out))

        ok, out = run_cmd(qmp, serial, "rm datavol.txt", "file deleted")
        results.append(("data volume rm works", ok))
        if not ok:
            print("  rm out: " + out)

        ok, out = run_cmd(qmp, serial, "ls", "/:")
        results.append(("file gone after rm", ok and "datavol.txt" not in out))
        return results
    finally:
        teardown(proc, qmp, serial, PORT_B[0])


def main():
    print("=== A: system volume survives format, /bin read-only ===")
    ra = case_sysvol()
    print("=== B: data volume read/write still normal ===")
    rb = case_datavol()

    print("\n===== RESULTS =====")
    bad = 0
    for name, ok in ra:
        print("  [%s] A: %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            bad += 1
    for name, ok in rb:
        print("  [%s] B: %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            bad += 1
    print("\n%d check(s), %d failed" % (len(ra) + len(rb), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
