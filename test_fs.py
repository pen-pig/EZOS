# test_fs.py - 文件系统回归测试：exFAT / FAT32 / FAT16 / FAT12 / 无盘 / format / ext4-RW
# 每组：QEMU 启动 -> df 报 FS 类型 -> ls 见 README + LFN 长名 -> cat 读内容
#       -> write 创建/覆盖 -> ls -> rm 删除 -> mkdir/cd/pwd 目录操作 -> exit 进桌面
# format 变体：format {fat32,fat16,fat12,exfat,ext4,ntfs,f2fs} 后写读删验证
# ext4-rw 变体：生成镜像上的 ext4 挂载 + 读 + 写入/覆盖/删除/目录
#     （EROFS 保持只读；ntfs/f2fs 无镜像生成器，经 format 变体覆盖）
# 验证：fs.c 统一分发层 + fat.c/exfat.c/ext4.c/ntfs.c/f2fs.c 驱动 + 无盘降级
# 注意：QMP type_text 仅支持小写字母/数字/空格，文件名匹配大小写不敏感
import subprocess, sys, time, os, re
from test_features import ocr_text, Qmp

QEMU = "D:/MyOS/tools/qemu-portable-20241220/qemu-system-x86_64.exe"

PASS, FAIL = [], []


def check(name, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {name}" + (f"  ({detail})" if detail and not ok else ""))
    (PASS if ok else FAIL).append(name)
    return ok


def norm(s):
    return re.sub(r'\s+', ' ', s.lower())


def load_size(path):
    with open(path, 'rb') as f:
        parts = f.read().split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h


class Vm:
    def __init__(self, disks, tag):
        args = [QEMU, "-vga", "std"]
        for d in disks:
            args += ["-drive", f"format=raw,file={d}"]
        args += ["-qmp", "tcp:127.0.0.1:4444,server,nowait",
                 "-display", "none", "-serial", "none"]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.q = Qmp()
        time.sleep(4.0)

    def sh(self, cmd, pause=1.2):
        """执行 shell 命令并返回 OCR 文本（按命令长度动态等待输入+执行完成）"""
        self.q.type_text(cmd + "\n")
        time.sleep(max(pause, len(cmd) * 0.12 + 0.8))
        shot = f"fsreg_last.ppm"
        self.q.screendump(shot)
        return ocr_text(shot)

    def shot(self, name):
        self.q.screendump(name)
        return name

    def close(self):
        try:
            self.proc.kill()
        except OSError:
            pass
        time.sleep(1.0)


def run_fs_variant(name, img, fstype):
    print(f"\n===== 变体 {name}: {img} =====")
    vm = Vm(["os-image.bin", img], name)
    try:
        # 1) df 报告 FS 类型
        t = norm(vm.sh("df"))
        check("df 挂载类型", fstype in t, t.replace('\n', ' ')[:160])

        # 2) ls 列出两个预置文件（含 LFN 长名）
        t = norm(vm.sh("ls"))
        check("ls 含 README.TXT", "readme.txt" in t, t.replace('\n', ' ')[:160])
        check("ls 含 LFN 长名", "long file name test.txt" in t,
              t.replace('\n', ' ')[:160])

        # 3) cat 读取内容
        t = norm(vm.sh("cat readme.txt"))
        check("cat 读文件内容", "welcome to ezos" in t, t.replace('\n', ' ')[:160])

        # 4) write 创建 + 覆盖（create-or-replace）
        t = norm(vm.sh("write newfile.txt hello regression"))
        check("write 创建文件", "successfully" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write newfile.txt second version"))
        check("write 覆盖已存在文件", "successfully" in t, t[:160])
        raw = vm.sh("cat newfile.txt")
        # 仅检查屏幕末尾输出（跳过滚回区中的历史命令回显，如 "write ... hello regression"）
        tail = norm(' '.join([ln for ln in raw.split('\n') if ln.strip()][-3:]))
        check("cat 验证覆盖内容", "second version" in tail and "hello" not in tail,
              tail[:160])

        # 5) rm 删除
        t = norm(vm.sh("rm newfile.txt"))
        check("rm 删除文件", "deleted" in t, t[:160])
        t = norm(vm.sh("clear", 0.8))
        t = norm(vm.sh("ls"))
        check("删除后 ls 不含该文件", "newfile" not in t and "readme.txt" in t,
              t.replace('\n', ' ')[:160])

        # 6) 目录操作
        t = norm(vm.sh("mkdir docs"))
        t = norm(vm.sh("cd docs"))
        t = norm(vm.sh("pwd"))
        check("mkdir/cd/pwd 目录操作", "docs" in t, t.replace('\n', ' ')[:160])

        # 7) exit 进桌面（图形模式正常）
        vm.q.type_text("cd /\n"); time.sleep(0.8)
        vm.q.type_text("exit\n"); time.sleep(3.0)
        vm.shot(f"fsreg_{name}_desk.ppm")
        w, h = load_size(f"fsreg_{name}_desk.ppm")
        check("exit 进入图形桌面", w >= 640 and h >= 480, f"{w}x{h}")
    finally:
        vm.close()


def run_nodisk_variant():
    print("\n===== 变体 nodisk: 仅引导盘 =====")
    vm = Vm(["os-image.bin"], "nodisk")
    try:
        t = norm(vm.sh("df"))
        check("df 报告无文件系统", "no filesystem" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("ls"))
        check("ls 优雅报错", "no filesystem" in t or "failed" in t or "error" in t,
              t.replace('\n', ' ')[:160])
        vm.q.type_text("exit\n"); time.sleep(3.0)
        vm.shot("fsreg_nodisk_desk.ppm")
        w, h = load_size("fsreg_nodisk_desk.ppm")
        check("无盘仍可进图形桌面", w >= 640 and h >= 480, f"{w}x{h}")
    finally:
        vm.close()


def gen_blank_img(path, size=76 * 1024 * 1024):
    """空白测试盘（sparse）：覆盖 FAT32 布局的 64MB 分区 + MBR"""
    with open(path, 'wb') as f:
        f.truncate(size)


def run_format_variant():
    print("\n===== 变体 format: format 命令选文件系统 =====")
    img = "disk_fmt.img"
    gen_blank_img(img)          # 每次重新生成，避免上次残留状态
    vm = Vm(["os-image.bin", img], "format")
    try:
        # 1) format fat32 -> 挂载 FAT32 -> 写读
        t = norm(vm.sh("format fat32", 3.0))
        check("format fat32 成功", "formatted as fat32" in t,
              t.replace('\n', ' ')[:160])
        t = norm(vm.sh("df"))
        check("df 报告 fat32", "fat32" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write fmt.txt hello format"))
        check("fat32 write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat fmt.txt"))
        check("fat32 cat", "hello format" in t, t[:160])

        # 2) format fat16 -> 清盘验证 -> 写读
        t = norm(vm.sh("format fat16", 3.0))
        check("format fat16 成功", "formatted as fat16" in t, t[:160])
        vm.sh("clear", 0.8)
        t = norm(vm.sh("ls"))
        check("format 后旧文件清除", "fmt.txt" not in t,
              t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write a.txt second try"))
        check("fat16 write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat a.txt"))
        check("fat16 cat", "second try" in t, t[:160])

        # 3) format fat12 -> 写读
        t = norm(vm.sh("format fat12", 3.0))
        check("format fat12 成功", "formatted as fat12" in t, t[:160])
        t = norm(vm.sh("write b.txt third one"))
        check("fat12 write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat b.txt"))
        check("fat12 cat", "third one" in t, t[:160])

        # 4) format 无参数默认 exFAT
        t = norm(vm.sh("format", 3.0))
        check("format 默认 exfat", "formatted as exfat" in t, t[:160])
        t = norm(vm.sh("df"))
        check("df 报告 exfat", "exfat" in t, t.replace('\n', ' ')[:160])

        # 5) format ext4 -> 写读删
        t = norm(vm.sh("format ext4", 8.0))
        check("format ext4 成功", "formatted as ext4" in t, t[:160])
        t = norm(vm.sh("df"))
        check("df 报告 ext4", "ext4" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write ext4.txt ext4 rw test"))
        check("ext4 write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat ext4.txt"))
        check("ext4 cat", "ext4 rw test" in t, t[:160])
        t = norm(vm.sh("rm ext4.txt"))
        check("ext4 rm", "deleted" in t, t[:160])

        # 6) format ntfs -> 写读删
        t = norm(vm.sh("format ntfs", 8.0))
        check("format ntfs 成功", "formatted as ntfs" in t, t[:160])
        t = norm(vm.sh("df"))
        check("df 报告 ntfs", "ntfs" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write ntfs.txt ntfs rw test"))
        check("ntfs write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat ntfs.txt"))
        check("ntfs cat", "ntfs rw test" in t, t[:160])
        t = norm(vm.sh("rm ntfs.txt"))
        check("ntfs rm", "deleted" in t, t[:160])

        # 7) format f2fs -> 写读删
        t = norm(vm.sh("format f2fs", 8.0))
        check("format f2fs 成功", "formatted as f2fs" in t, t[:160])
        t = norm(vm.sh("df"))
        check("df 报告 f2fs", "f2fs" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("write f2fs.txt f2fs rw test"))
        check("f2fs write", "successfully" in t, t[:160])
        t = norm(vm.sh("cat f2fs.txt"))
        check("f2fs cat", "f2fs rw test" in t, t[:160])
        t = norm(vm.sh("rm f2fs.txt"))
        check("f2fs rm", "deleted" in t, t[:160])

        # 8) exit 进桌面
        vm.q.type_text("exit\n"); time.sleep(3.0)
        vm.shot("fsreg_format_desk.ppm")
        w, h = load_size("fsreg_format_desk.ppm")
        check("format 变体进桌面", w >= 640 and h >= 480, f"{w}x{h}")
    finally:
        vm.close()


def run_ext4_variant():
    """ext4 读写（生成镜像）：挂载/读/列目录 + 写入/覆盖/删除/目录操作"""
    print("\n===== 变体 ext4-rw: ext4 读写文件系统 =====")
    img = "disk_ext4.img"
    subprocess.run([sys.executable, "temp/gen_diskimg.py", img, "ext4"],
                   check=True)
    vm = Vm(["os-image.bin", img], "ext4rw")
    try:
        t = norm(vm.sh("df"))
        check("df 挂载 ext4", "ext4" in t, t.replace('\n', ' ')[:160])
        t = norm(vm.sh("ls"))
        check("ls 含 README.TXT", "readme.txt" in t, t.replace('\n', ' ')[:160])
        check("ls 含长名文件", "long file name test.txt" in t,
              t.replace('\n', ' ')[:160])
        t = norm(vm.sh("cat readme.txt"))
        check("cat 读文件内容", "welcome to ezos" in t, t[:160])

        # 写入（create-or-replace）+ 覆盖验证
        t = norm(vm.sh("write newfile.txt hello ext4"))
        check("ext4 write 创建", "successfully" in t, t[:160])
        t = norm(vm.sh("write newfile.txt second life"))
        check("ext4 write 覆盖", "successfully" in t, t[:160])
        raw = vm.sh("cat newfile.txt")
        tail = norm(' '.join([ln for ln in raw.split('\n') if ln.strip()][-3:]))
        check("ext4 cat 覆盖内容", "second life" in tail and "hello ext4" not in tail,
              tail[:160])

        # 删除
        t = norm(vm.sh("rm newfile.txt"))
        check("ext4 rm 删除", "deleted" in t, t[:160])

        # 目录操作
        t = norm(vm.sh("mkdir docs"))
        t = norm(vm.sh("cd docs"))
        t = norm(vm.sh("pwd"))
        check("ext4 mkdir/cd/pwd", "docs" in t, t.replace('\n', ' ')[:160])

        vm.q.type_text("cd /\n"); time.sleep(0.8)
        vm.q.type_text("exit\n"); time.sleep(3.0)
        vm.shot("fsreg_ext4rw_desk.ppm")
        w, h = load_size("fsreg_ext4rw_desk.ppm")
        check("ext4-rw 变体进桌面", w >= 640 and h >= 480, f"{w}x{h}")
    finally:
        vm.close()


def main():
    for name, fstype in [("exfat", "exfat"), ("fat32", "fat32"),
                         ("fat16", "fat16"), ("fat12", "fat12")]:
        img = f"disk_{name}.img"
        if not os.path.exists(img):
            print(f"[gen] {img}")
            subprocess.run([sys.executable, "temp/gen_diskimg.py", img, name],
                           check=True)
        run_fs_variant(name, img, fstype)
    run_format_variant()
    run_ext4_variant()
    run_nodisk_variant()

    print(f"\n========== 结果: {len(PASS)} passed, {len(FAIL)} failed ==========")
    if FAIL:
        for f in FAIL:
            print("  FAILED:", f)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
