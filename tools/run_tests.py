# -*- coding: utf-8 -*-
"""run_tests.py - 串行跑 E2E 回归（长程任务的验证回路）。

为什么需要：仓库里有 43 个测试脚本，每个要 1-15 分钟，手动一个个敲既慢
又容易漏。串行是硬要求——测试之间共享 os-image.bin 与 QEMU 端口，并行会
互相抢锁（症状是莫名其妙的 ConnectionRefused）。

分层（越靠前越该常跑）：
    core    每次改动都跑：镜像契约 + 总回归 + 对拍 + 输入
    fs      文件系统相关：exFAT/FAT/路径/系统卷
    usb     USB/输入栈
    net     网络栈
    static  静态检查：内核栈帧预算（tools/check_stack.py，不跑 QEMU）
    deep    慢但能抓到"只在坏盘上暴露"的缺陷：坏盘健壮性 + socket/TCP
    full    全部
    changed 按 git 改动自动选（长程任务里最常用）
    <名字>  直接跑指定测试（可多个）

用法：
    python tools/run_tests.py core
    python tools/run_tests.py changed
    python tools/run_tests.py full
    python tools/run_tests.py test_mouse test_hostfs

输出：每项 PASS/FAIL/TIMEOUT + 耗时，末尾汇总；报告同时写 temp/test_report.txt。
退出码非 0 = 有失败（可直接串进 CI/ninja）。
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
REPORT = os.path.join(ROOT, "temp", "test_report.txt")
# 互斥锁：run_tests 每项开始前会 taskkill 掉所有 QEMU（清上一项残留）。
# 两个实例同时跑就会互相杀——症状是某项莫名其妙 ConnectionRefused/FAIL，
# 看起来像测试坏了。踩过一次，所以加文件锁挡住。
LOCK = os.path.join(ROOT, "temp", "run_tests.lock")
LOCK_MAX_AGE = 4 * 3600.0
PY = sys.executable

# 层级定义。key 是层级名，value 是测试模块名（不含 test_ 前缀与 .py）。
LAYERS = {
    "core": ["image", "vhd", "regress", "fs_matrix", "multilang", "mouse"],
    "fs":   ["fsref", "hostfs", "fatpath", "path", "sysvol", "rmdir",
             "ext4vol", "f2fsvol", "refsvol", "exfatvol"],
    "usb":  ["usbenum", "uhci", "uhci_control", "usb", "usbkbd", "usbmouse",
             "usbmsc", "usbboot", "ehci"],
    "net":  ["netapp", "dhcp", "dns"],
    "gui":  ["vi", "vi_color", "uefi3"],
    # corrupt：故意把盘改坏后断言内核不卡死（6 FS x 3 种子，约 15 分钟）
    "deep": ["step7_sock", "step7_tcp", "corrupt"],
    "hw":   ["ahci", "nvme", "power", "edge", "heap", "jobs", "lock"],
    "util": ["digest"],
    # 静态检查（不是 tests/test_*.py，而是 tools/*.py）
    "static": ["check_stack", "stack"],
}

# 改动文件 -> 必跑层级。键是路径子串。
WATCH = [
    ("exfat", "core fs"), ("fat.c", "core fs"), ("fs.c", "core fs"),
    ("sysvol", "core fs"), ("ref_exfat", "core fs"),
    ("mouse", "core usb"), ("usbmouse", "core usb"), ("usbkbd", "usb"),
    ("keyboard", "usb"), ("uhci", "usb"), ("ehci", "usb"), ("usbc", "usb"),
    ("usb.", "usb"), ("usbenum", "usb"), ("usbhc", "usb"),
    ("net.c", "net"), ("rtl8139", "net"), ("dhcp", "net"), ("dns", "net"),
    # 静态检查不跑 QEMU，10~30s 就完事，所以 kernel/ 下任何 .c 改动都带上
    ("kernel/", "static"), ("shell", "core"),
    ("fs.c", "core fs"), ("ext4", "fs"), ("f2fs", "fs"), ("refs", "fs"),
    # 遍历循环的上界改动只有"坏盘"能验出来，好盘测不出来（见 test_corrupt.py）
    ("ntfs", "fs deep"),
    ("kmalloc", "core hw"), ("pmm", "core hw"), ("lockselftest", "core hw"),
    ("fd.c", "core"), ("elf", "core"), ("exec", "core"),
    ("syscall", "core"), ("kernel.c", "core"), ("build.ninja", "core"),
    ("make_image", "core"), ("gen_diskimg", "core fs"),
    ("vhd", "core"), ("boot", "core"), ("uefi", "core"),
    ("ahci", "hw"), ("nvme", "hw"), ("acpi", "hw"), ("ata", "hw"),
    ("gfx", "gui"), ("gui", "gui"), ("desktop", "gui"), ("games", "gui"),
]

# 单项超时（秒）。慢的（网络真对拍、GUI 像素）单独放宽。
TIMEOUT = {
    "step7_sock": 900, "step7_tcp": 900, "uefi3": 600,
    "vi_color": 420, "vi": 420, "dhcp": 420, "dns": 420,
    "fs_matrix": 600, "hostfs": 900, "netapp": 600, "edge": 700,
    "power": 420, "ahci": 420, "nvme": 600, "regress": 600,
    # edge 每用例独立起一个 QEMU，实测 416s；420 会刚好卡线超时
    # rmdir 用 screendump+OCR 逐个断言，6 个 FS x ~20 条命令，实测 ~7 分钟
    "rmdir": 900,
    # 卷容量测试：QEMU -icount 下格式化慢，单项实测 ~3 分钟。
    # exfatvol 要起两轮 QEMU（中间宿主机改写盘），放宽到 8 分钟。
    "ext4vol": 420, "f2fsvol": 420, "refsvol": 420, "exfatvol": 480,
    # 坏盘健壮性：6 个 FS 各起 1 台机器做黄金盘 + 3 台跑坏盘，实测 ~15 分钟
    "corrupt": 1800,
}

# 这些测试要 GUI 像素/OCR，默认不进 core/full 的常规集合（太慢且脆）
OPTIONAL = {"vi_color", "uefi3", "step7_sock", "step7_tcp"}


def preflight():
    """构建产物与盘可用性。disk.vhd 被 Windows 挂载会让所有 QEMU 静默退出，
    症状伪装成端口连不上——所以先查，并把原因说清楚。"""
    problems = []
    for f in ("os-image.bin", "disk.vhd"):
        if not os.path.isfile(os.path.join(ROOT, f)):
            problems.append("缺 %s，先跑 ninja" % f)
    if problems:
        return problems
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.CreateFileW(os.path.join(ROOT, "disk.vhd"), 0x80000000, 0,
                          None, 3, 0x80, None)
        if h == -1:
            problems.append("disk.vhd 被其他进程独占（多半是 Windows 挂载了它）"
                            "——QEMU 会直接退出，症状伪装成端口连不上。"
                            "请在磁盘管理里分离 VHD 后重试。")
        else:
            k.CloseHandle(h)
    except Exception:
        pass
    return problems


def all_tests(include_optional):
    names = set()
    for v in LAYERS.values():
        names.update(v)
    found = {f[5:-3] for f in os.listdir(TESTS)
             if f.startswith("test_") and f.endswith(".py")}
    unknown = sorted(found - names)
    if unknown:
        # 新测试没登记就默认归到 full 跑，别让它悄悄不跑
        names.update(unknown)
    if not include_optional:
        names -= OPTIONAL
    return sorted(names)


def pick_changed():
    try:
        out = subprocess.run(["git", "diff", "--name-only", "HEAD"],
                             cwd=ROOT, capture_output=True, text=True,
                             timeout=30).stdout.split()
    except Exception:
        out = []
    layers = set()
    for path in out:
        low = path.lower()
        for key, lay in WATCH:
            if key in low:
                layers.update(lay.split())
    if not layers:
        layers = {"core"}
    return sorted({n for lay in layers for n in LAYERS[lay]}), out


def kill_stale_qemu():
    """每项测试前清掉上一次遗留的 QEMU。

    残留进程会占住串口/QMP 端口，症状是测试直接 abort 或 ConnectionRefused
    ——看起来像测试坏了，其实是上一轮没收拾干净。串行跑时上一项正常退出会
    回收，但被 kill/超时打断的那一项不会。
    """
    for exe in ("qemu-system-x86_64.exe", "qemu-system-i386.exe"):
        subprocess.run(["taskkill", "/F", "/IM", exe],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        print(__doc__)
        return 2
    opt = "--all-optional" in sys.argv

    if args == ["changed"]:
        names, files = pick_changed()
        print("changed files: %s" % (", ".join(files) or "(none)"))
        print("=> 选中层级对应测试: %s\n" % " ".join(names))
    elif args in (["full"], ["all"]):
        names = all_tests(True)
    elif args in (["quick"],):
        names = LAYERS["core"] + LAYERS["fs"]
    else:
        names = []
        for a in args:
            if a in LAYERS:
                names.extend(LAYERS[a])
            else:
                names.append(a)
        seen, uniq = set(), []
        for n in names:
            if n not in seen:
                seen.add(n)
                uniq.append(n)
        names = uniq

    problems = preflight()
    if problems:
        for p in problems:
            print("PREFLIGHT FAIL: %s" % p)
        return 2

    # 互斥：见 LOCK 的注释。锁文件比进程探测可靠（Ctrl-C 死掉的进程留不下痕迹，
    # 但锁文件会——所以超过 4 小时的陈旧锁自动忽略）。
    if os.path.exists(LOCK):
        try:
            age = time.time() - os.path.getmtime(LOCK)
        except OSError:
            age = 0.0
        if age < LOCK_MAX_AGE:
            print("PREFLIGHT FAIL: %s 存在（%.1f 分钟前创建）\n"
                  "  run_tests 每项开始前会 taskkill 掉所有 QEMU，两个实例同时跑"
                  "会互相杀。\n  确认没有别的实例在跑后，删掉 %s 再重试。"
                  % (LOCK, age / 60.0, LOCK))
            return 2
    try:
        with open(LOCK, "w", encoding="utf-8") as f:
            f.write("pid=%d started=%s\n" % (os.getpid(), time.ctime()))
    except OSError as e:
        print("PREFLIGHT FAIL: 写不了锁文件 %s (%s)" % (LOCK, e))
        return 2

    try:
        return run_all(names)
    finally:
        try:
            os.remove(LOCK)
        except OSError:
            pass


def run_all(names):
    """串行跑完所有项，返回退出码。"""
    print("=== 串行回归：%d 项 ===" % len(names))
    results = []
    t_all = time.time()
    for i, name in enumerate(names, 1):
        script = os.path.join(TESTS, "test_%s.py" % name)
        if not os.path.isfile(script):
            # 静态检查类：tools/<name>.py 或 tools/check_<name>.py（如 stack）
            for cand in (os.path.join(ROOT, "tools", "%s.py" % name),
                         os.path.join(ROOT, "tools", "check_%s.py" % name)):
                if os.path.isfile(cand):
                    script = cand
                    break
            else:
                results.append((name, "MISSING", 0.0, ""))
                print("[%2d/%2d] %-14s MISSING" % (i, len(names), name))
                continue
        limit = TIMEOUT.get(name, 360)
        kill_stale_qemu()
        print("[%2d/%2d] %-14s ... " % (i, len(names), name), end="",
              flush=True)
        t0 = time.time()
        try:
            p = subprocess.run([PY, script], cwd=ROOT, timeout=limit,
                               capture_output=True, text=True,
                               encoding="latin1", errors="replace")
            code, tail = p.returncode, (p.stdout or "").strip().splitlines()
            state = "PASS" if code == 0 else "FAIL"
        except subprocess.TimeoutExpired:
            code, tail, state = -1, ["timeout after %ds" % limit], "TIMEOUT"
        except Exception as e:
            code, tail, state = -1, [str(e)], "ERROR"
        dt = time.time() - t0
        note = ""
        if state != "PASS" and tail:
            note = tail[-1][:110]
        print("%-7s %6.1fs %s" % (state, dt, note))
        results.append((name, state, dt, note))
        # 每项结束就落盘，中途被打断也留痕
        with open(REPORT, "w", encoding="utf-8") as f:
            f.write("run_tests %s\n" % " ".join(sys.argv[1:]))
            for n, s, d, note2 in results:
                f.write("  %-8s %-14s %6.1fs %s\n" % (s, n, d, note2))

    bad = [r for r in results if r[1] != "PASS"]
    total = time.time() - t_all
    print("\n---- %d/%d passed, %.1f min ----"
          % (len(results) - len(bad), len(results), total / 60.0))
    if bad:
        print("FAILED: %s" % ", ".join("%s(%s)" % (n, s) for n, s, _, _ in bad))
    print("报告: %s" % REPORT)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
