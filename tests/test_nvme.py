# -*- coding: utf-8 -*-
"""test_nvme.py - NVMe（PCIe SSD）块设备 E2E（真机点亮 A2）

判定走**串口（COM1）**按子串断言（沿用 ahci/ehci 那套）。
端口 4509(QMP) / 4510(serial)，避开已占用的 4463-4508。

本步验证 NVMe 的五件事，缺一件就算没点亮：
  1. PCI 认领（class 0x0108）且 BAR 可用
     -> "NVME: 1B36:0010 bar=0xFEBF0000 (BAR0) progif=0x02"
  2. 控制器真的 enable 了（CSTS.RDY=1，Admin 队列起来）
     -> "NVME: controller ready, admin queue up"
  3. Identify 真的取回了数据（不只是"命令返回成功"）
     -> "NVME: ns0 nsid=1 sectors=<后端文件真实扇区数> lbads=9 ready"
        容量断言的期望值由**宿主机按后端文件大小现算**（os.path.getsize/512），
        不写死：写死数字会在任何一次镜像格式迁移后永久假红（disk.vhd 自带
        512 字节 VHD footer，16MB 的盘就是 32769 而不是 32768 扇区）；
        现算既不过时，也照样抓得住 identify 返回 0 或编造值的假成功。
  4. 注册进块层
     -> "NVME: 1 namespace(s) registered as drive 16..16"
  5. **真读真写**（PRP1 单页 + NVM READ/WRITE opcode）
     -> setdrive 16 -> ls/cat 读到 README.TXT
     -> write nvmetest.txt -> ls 看见 -> cat 读回 -> rm 删掉
        写路径走 OPC_NVM_WRITE，读不通只会卡在 write/ls，不会假成功

铁律「两组结果必须相反」+「警惕弱断言」：
  A（挂 -device nvme，后端是 disk.vhd 的副本）：
     - 上面 1-5 全部成立，且**绝不出现** "NVME-IO: read/write fail"
  B（不挂 nvme）：
     - 必须打印 "NVME: no NVMe controller found"
     - **绝不**出现 controller ready / ns0 nsid / registered as drive
     - setdrive 16 必须报 "no filesystem found"（块层里根本没有这块盘）

QEMU 侧已实测的关键事实（别凭记忆改，见 temp/probe_bar.py）：
  - `-device nvme` 的 BAR0 标称 **64 位**，但 SeaBIOS 把它分配在 0xFEBF0000
    （4GB 以下）→ 32 位无 PAE 照样能映射。判据必须是"基址 >= 4GB 才拒绝"，
    不能写成"类型是 64 位就拒绝"（那样会误拒，且测不出来）。
  - QEMU nvme 的 CAP: ver=1.4、mqes=2048、dstrd=0、to=15。
  - Identify Controller 报的 NN 是个大值（>=16），NSID 要顺序试探；
    只有 NSID=1 真的存在，其余 Identify 返回 nsze=0 被跳过。

用法：python tests/test_nvme.py   （退出码 0 = 全通过）
"""
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"
QMP_PORT = 4509
SERIAL_PORT = 4510
BOOT_WAIT = 150

IMG = os.path.join(ROOT, "os-image.bin").replace("\\", "/")
DISK = os.path.join(ROOT, "disk.vhd")
# 写测试会改盘内容，绝不拿 disk.vhd 本体开刀——每次跑复制一份。
NVME_IMG = os.path.join(HERE, "nvme_disk.vhd").replace("\\", "/")

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus',
          '/': 'slash', '*': 'kp_multiply', '+': 'kp_add', ':': 'shift-semicolon'}


class SerialReader(object):
    def __init__(self, port):
        # QEMU 的 `-serial tcp:...,server` 监听建立得比脚本连接晚一点（机器
        # 忙的时候能差好几秒），一次性 connect 会拿 ConnectionRefused。这里
        # 轮询到就绪为止——所有 E2E 都从这里取串口，修一处全部受益。
        self.sock = None
        last = None
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port),
                                                     timeout=10)
                break
            except Exception as exc:      # 铁律：连接未就绪不是测试失败
                last = exc
                time.sleep(0.3)
        if self.sock is None:
            raise last
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
                continue          # 铁律：空闲超时不是异常
            except Exception:
                break
            if not data:
                break
            with self.lock:
                self.buf += data

    def snapshot(self):
        with self.lock:
            return self.buf.decode("latin1", "replace")

    def size(self):
        with self.lock:
            return len(self.buf)

    def tail_from(self, n):
        with self.lock:
            return self.buf[n:].decode("latin1", "replace")

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
            self.hmc("sendkey " + key)
            time.sleep(0.05)
        self.hmc("sendkey ret")

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


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


def wait_for(serial, marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if marker in serial.snapshot():
            return True
        time.sleep(0.3)
    return False


def run_cmd(qmp, serial, cmd, expect, timeout=25.0):
    """键入一行命令，等期望子串出现在**新增**输出里。"""
    before = serial.size()
    qmp.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.5)
        out = flat(serial.tail_from(before))
        if expect in out:
            return True, out
    return False, flat(serial.tail_from(before))


def kill_all_qemu():
    subprocess.call(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def boot(extra_args):
    proc = subprocess.Popen(
        [QEMU, "-icount", "shift=auto", "-vga", "std",
         "-drive", "format=raw,file=" + IMG,
         "-drive", "format=raw,file=" + DISK,
         "-serial", "tcp:127.0.0.1:%d,server,nowait" % SERIAL_PORT,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT] + extra_args,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return proc


def case_nvme():
    results = []
    if os.path.isfile(DISK):
        shutil.copyfile(DISK, NVME_IMG)
    args = ["-drive", "format=raw,if=none,file=" + NVME_IMG + ",id=nvdisk",
            "-device", "nvme,drive=nvdisk,serial=ezosnvme"]
    proc = boot(args)
    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        if not wait_for(serial, "NVME:", BOOT_WAIT):
            print("  FAIL A-nvme: no 'NVME:' line in serial")
            print("---- serial tail ----\n" + serial.snapshot()[-1500:])
            return [("NVMe init line seen", False)], []

        # 等注册行落地再采样（klog 流式输出到串口，瞬时快照会漏后段）
        wait_for(serial, "registered as drive 16", 60)
        time.sleep(2.0)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines() if "NVME" in ln]

        def has(sub):
            return any(sub in l for l in lines)

        results.append(("controller claimed (PCI 0108 + BAR)",
                        has("NVME: ") and has("bar=0x")))
        results.append(("controller ready / admin queue up",
                        has("controller ready, admin queue up")))
        # 期望容量**由宿主机现算**（后端文件真实大小 / 512），不写死数字：
        # 后端以前是裸 16MB（32768 扇区），全仓库 img->vhd 之后 disk.vhd 自带
        # 512 字节 VHD footer，同一个盘子就是 32769 扇区。写死 32768 会在
        # 迁移后永久假红，而"从文件算"既不会过时，也照样能抓住 identify
        # 返回 0 / 编造值的那种假成功。
        want_sectors = os.path.getsize(NVME_IMG) // 512
        results.append(("identify namespace with real capacity",
                        has("ns0 nsid=1 sectors=%d" % want_sectors)
                        and has("lbads=9 ready")))
        results.append(("registered as drive 16",
                        has("1 namespace(s) registered as drive 16..16")))
        # 失败行一条都不许有：读写出过错就是没点亮
        results.append(("no NVME-IO failure line",
                        not has("NVME-IO:")))

        for ln in lines:
            print("  klog> " + ln)

        # ---- 真读真写（走 PRP1 + NVM READ/WRITE，不是只认领）----
        if not wait_for(serial, "TASK: preemptive", 30):
            print("  FAIL A-nvme: shell not ready")
            results.append(("shell ready", False))
            return results, lines
        time.sleep(2.0)

        ok, out = run_cmd(qmp, serial, "setdrive 16", "fs drive set to 16")
        results.append(("setdrive 16 (NVMe namespace)", ok))
        if not ok:
            print("  setdrive out: " + out)

        # 读路径：盘里原有的 README.TXT
        ok, out = run_cmd(qmp, serial, "ls", "readme.txt")
        results.append(("ls over NVMe shows readme.txt", ok))
        if not ok:
            print("  ls out: " + out)

        ok, out = run_cmd(qmp, serial, "cat README.TXT", "welcome to ezos")
        results.append(("cat over NVMe reads content", ok))
        if not ok:
            print("  cat out: " + out)

        # 写路径：OPC_NVM_WRITE
        ok, out = run_cmd(qmp, serial, "write nvmetest.txt HelloNVMe",
                          "file written successfully")
        results.append(("write file over NVMe", ok))
        if not ok:
            print("  write out: " + out)

        ok, out = run_cmd(qmp, serial, "ls", "nvmetest.txt")
        results.append(("ls shows the new file", ok))
        if not ok:
            print("  ls out: " + out)

        ok, out = run_cmd(qmp, serial, "cat NVMETEST.TXT", "hellonvme")
        results.append(("cat reads back what was written", ok))
        if not ok:
            print("  cat out: " + out)

        ok, out = run_cmd(qmp, serial, "rm nvmetest.txt", "file deleted")
        results.append(("rm file over NVMe", ok))
        if not ok:
            print("  rm out: " + out)

        # 写完之后仍然不许出现 IO 失败行（写路径最容易错在 PRP/长度）
        if "NVME-IO:" in serial.snapshot():
            print("  FAIL A-nvme: NVME-IO failure appeared after I/O")
            results.append(("no NVME-IO failure after I/O", False))
        return results, lines
    finally:
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
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def case_none():
    results = []
    proc = boot([])
    serial = None
    qmp = None
    try:
        time.sleep(1.0)
        serial = SerialReader(SERIAL_PORT)
        qmp = Qmp(QMP_PORT)

        if not wait_for(serial, "NVME:", BOOT_WAIT):
            print("  FAIL B-none: no 'NVME:' line in serial")
            return [("NVMe init line seen", False)]
        time.sleep(3.0)
        snap = serial.snapshot()
        lines = [ln.strip() for ln in snap.splitlines() if "NVME" in ln]

        def has(sub):
            return any(sub in l for l in lines)

        results.append(("prints 'no NVMe controller found'",
                        has("NVME: no NVMe controller found")))
        for bad in ("controller ready", "ns0 nsid=", "registered as drive"):
            results.append(("must not report '%s'" % bad, not has(bad)))

        for ln in lines:
            print("  klog> " + ln)

        # 块层里就不该有 drive 16：setdrive 必须报"盘不存在"。
        # （只看 fs_init 返回值是不够的——它探测不到文件系统会回退到现有
        #   数据盘并报成功，所以 shell 侧必须先按 ata_drive_present fail closed。）
        if not wait_for(serial, "TASK: preemptive", 30):
            print("  FAIL B-none: shell not ready")
            results.append(("shell ready", False))
            return results
        time.sleep(2.0)
        ok, out = run_cmd(qmp, serial, "setdrive 16", "not present")
        results.append(("setdrive 16 reports drive not present", ok))
        if not ok:
            print("  setdrive out: " + out)
        return results
    finally:
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
        wait_port_free(QMP_PORT, 15)
        wait_port_free(SERIAL_PORT, 15)


def main():
    kill_all_qemu()
    wait_port_free(QMP_PORT, 15)
    wait_port_free(SERIAL_PORT, 15)

    if not os.path.isfile(IMG) or not os.path.isfile(DISK):
        print("MISSING %s / %s - run ninja first" % (IMG, DISK))
        return 2

    all_ok = True
    for title, fn in (("A-nvme", case_nvme), ("B-none", case_none)):
        print("=== case: %s ===" % title)
        res = fn()
        if isinstance(res, tuple):
            res = res[0]
        for name, ok in res:
            print("  [%s] %s" % ("OK" if ok else "FAIL", name))
            all_ok = all_ok and ok
        print("  => %s" % ("PASS" if all((o for _, o in res)) else "FAIL"))

    print("\n==== SUMMARY: %s ====" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
