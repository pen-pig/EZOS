# -*- coding: utf-8 -*-
r"""ezos_env.py - 项目根级的路径解析（可搬迁的唯一入口）。

**为什么需要**：仓库里曾经有 40+ 处写死 `D:/MyOS/tools/...`，换个盘符/换个
机器就编不过——而且失败方式是"ninja: command not found"这种毫无线索的报错。
现在所有外部工具都从这里解析，优先级：

  1. 环境变量（EZOS_TOOLS / EZOS_QEMU / EZOS_CARGO ...）——CI 与多版本共存用
  2. **按项目自身位置推导**：<项目根>/../tools ——仓库在 X:\MyOS\src 时自动用
     X:\MyOS\tools，天然支持 D/E 多副本，不需要任何配置
  3. 扫 C-H 盘找 MyOS/tools（老布局兜底）
  4. 最后回退 D:/MyOS/tools

任何一处都找不到时，require_* 会抛出**带修复建议**的错误，而不是让构建
脚本在几百行之后报一句 command not found。
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = os.name == "nt"

# 工具的相对位置（相对 tools 根）
REL = {
    "cc": os.path.join("i686-elf-tools-windows", "bin", "i686-elf-gcc.exe"),
    "ld": os.path.join("i686-elf-tools-windows", "bin", "i686-elf-ld.exe"),
    "objcopy": os.path.join("i686-elf-tools-windows", "bin", "i686-elf-objcopy.exe"),
    "nasm": os.path.join("NASM", "nasm.exe"),
    "zig": os.path.join("zig", "zig-x86_64-windows-0.15.1", "zig.exe"),
    "qemu": os.path.join("qemu-portable-20241220", "qemu-system-x86_64.exe"),
    "qemu32": os.path.join("qemu-portable-20241220", "qemu-system-i386.exe"),
    "qemu_img": os.path.join("qemu-portable-20241220", "qemu-img.exe"),
    "ovmf": os.path.join("qemu-portable-20241220", "share",
                         "edk2-i386-code.fd"),
}

ENV_KEYS = {
    "cc": "EZOS_CC", "ld": "EZOS_LD", "objcopy": "EZOS_OBJCOPY",
    "nasm": "EZOS_NASM", "zig": "EZOS_ZIG", "qemu": "EZOS_QEMU",
    "qemu32": "EZOS_QEMU32", "qemu_img": "EZOS_QEMU_IMG", "ovmf": "EZOS_OVMF",
}


def tools_root():
    """工具根目录。环境变量 > <项目根>/../tools > 扫盘 > D:/MyOS/tools"""
    env = os.environ.get("EZOS_TOOLS")
    if env:
        return env
    sibling = os.path.normpath(os.path.join(ROOT, "..", "tools"))
    if os.path.isdir(sibling):
        return sibling
    for drive in "CDEFGH":
        cand = "%s:\\MyOS\\tools" % drive
        if os.path.isdir(cand):
            return cand
    return "D:/MyOS/tools"


def tool(name, must_exist=True):
    """按名字取工具的绝对路径。"""
    env = os.environ.get(ENV_KEYS.get(name, ""))
    if env:
        return env
    path = os.path.normpath(os.path.join(tools_root(), REL[name]))
    if must_exist and not os.path.isfile(path):
        raise SystemExit(
            "EZOS 缺少工具：%s\n"
            "  期望位置：%s\n"
            "  解决办法（三选一）：\n"
            "   1) 把该工具放到上述路径\n"
            "   2) 设环境变量 %s 指向它\n"
            "   3) 设 EZOS_TOOLS 指向完整的 tools 目录"
            % (name, path, ENV_KEYS.get(name, "EZOS_<TOOL>")))
    return path


def qemu_exe():
    return tool("qemu")


def qemu32_exe():
    return tool("qemu32")


def qemu_img_exe():
    return tool("qemu_img")


def ovmf_fd():
    return tool("ovmf")


def cargo_exe():
    """cargo：环境变量 > PATH > ~/.cargo/bin。

    rustup 装的 cargo 默认不进 PATH，但一定在用户目录下——写死
    C:/Users/<用户名>/.cargo 在别人机器上必然失效。
    """
    env = os.environ.get("EZOS_CARGO")
    if env:
        return env
    found = shutil.which("cargo")
    if found:
        return found
    guess = os.path.join(os.path.expanduser("~"), ".cargo", "bin",
                         "cargo.exe" if IS_WINDOWS else "cargo")
    if os.path.isfile(guess):
        return guess
    raise SystemExit(
        "EZOS 找不到 cargo（Rust 增量模块 libezos_rs.a 是链接必需项）。\n"
        "  解决办法：安装 Rust（https://rustup.rs/）后设 EZOS_CARGO 指向 cargo，\n"
        "  或把它加进 PATH。\n"
        "  若只想跳过 Rust/Zig 增量：设 EZOS_SKIP_RUSTZIG=1（会退回纯 C 实现）。")


def kernel_version():
    """内核版本号，从 kernel/version.h 解析（唯一事实来源）。

    别在测试里抄版本字符串：git 上早就打了 v1.1.0，而内核里还印着 0.9.0，
    tests/ 里抄的四份也全是 0.9.0——改一处漏三处，版本号永远对不上，测试
    里那份过期还会变成一次莫名其妙的假红。
    """
    import re as _re
    path = os.path.join(ROOT, "kernel", "version.h")
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    m = _re.search(r'#define\s+EZOS_VERSION\s+"([^"]+)"', txt)
    return m.group(1) if m else None


def preflight(need=("cc", "ld", "objcopy", "nasm")):
    """构建前检查，返回缺失清单（空 = 齐备）。"""
    missing = []
    for name in need:
        if not os.path.isfile(tool(name, must_exist=False)):
            missing.append(name)
    return missing


if __name__ == "__main__":
    print("项目根  :", ROOT)
    print("工具根  :", tools_root())
    for k in sorted(REL):
        p = os.path.join(tools_root(), REL[k])
        print("  %-8s %s %s" % (k, "OK " if os.path.isfile(p) else "MISS", p))
    try:
        print("cargo    :", cargo_exe())
    except SystemExit as e:
        print("cargo    : 缺失 (%s)" % str(e).splitlines()[0])
