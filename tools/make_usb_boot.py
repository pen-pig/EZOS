#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_usb_boot.py —— 把 os-image.bin 安全写进 U 盘的小工具

设计原则（看 docs/USB_BOOT.md 了解为什么这样写）：
  * 默认 dry-run：只列出本机磁盘 + 校验镜像，绝不写任何东西。
  * 只有显式 --write --disk <id> 才写盘。
  * 写之前会"二次输入盘号确认"：必须手打一遍目标盘 id，输错立即中止。
  * 绝不自动选盘：目标盘必须由你用 --disk 指定。
  * 写前校验：镜像大小必须是 512 的倍数，且首扇区以 0x55 0xAA 结尾。
  * 写前打印：将要写入的字节数、目标盘容量；若镜像比盘大直接拒绝。

用法：
  python3 tools/make_usb_boot.py                 # 只读：列出磁盘 + 校验镜像
  python3 tools/make_usb_boot.py --image X.bin   # 指定镜像（默认 ../os-image.bin）
  python3 tools/make_usb_boot.py --write --disk 2 # 真的写盘（Windows 需管理员 / Linux macOS 需 sudo）
  python3 tools/make_usb_boot.py --write --disk 2 --verify  # 写后逐块回读比对（很慢，但最稳）

注意：写错盘 = 那块盘数据全清。本工具只负责"逼你确认两遍"，后果仍需你承担。
"""

import os
import sys
import json
import subprocess
import argparse

CHUNK = 1 << 20  # 1 MiB 写入块

IS_WIN = os.name == "nt"


# --------------------------------------------------------------------------
# 磁盘枚举：返回 [ {id, path, model, size, extra}, ... ]
#   id    : 传给 --disk 的令牌（Windows 是整数盘号，Linux 是 sdX，macOS 是 diskN）
#   path  : 用于 os.open 写入的设备路径
#   size  : 字节为单位的盘容量
# --------------------------------------------------------------------------
def list_disks():
    if IS_WIN:
        return _list_windows()
    if sys.platform == "darwin":
        return _list_darwin()
    return _list_linux()


def _run(cmd, shell=False):
    try:
        out = subprocess.run(cmd, shell=shell, capture_output=True, text=True,
                             timeout=30, creationflags=(subprocess.CREATE_NO_WINDOW if IS_WIN else 0))
        return out.stdout, out.returncode
    except Exception as e:  # noqa
        return "", 1


def _list_windows():
    disks = []
    # 优先 PowerShell Get-Disk（能看到盘号/型号/容量/总线/序列号）
    ps = ('Get-Disk | Select-Object Number,FriendlyName,Size,BusType,SerialNumber '
          '| ConvertTo-Json -Compress')
    stdout, rc = _run(["powershell", "-NoProfile", "-Command", ps])
    if rc == 0 and stdout.strip():
        try:
            obj = json.loads(stdout)
            items = obj if isinstance(obj, list) else [obj]
            for d in items:
                num = d.get("Number")
                if num is None:
                    continue
                size = d.get("Size") or 0
                disks.append({
                    "id": str(num),
                    "path": r"\\.\PhysicalDrive" + str(num),
                    "model": (d.get("FriendlyName") or "").strip(),
                    "size": int(size),
                    "extra": "Bus=%s SN=%s" % (d.get("BusType"), d.get("SerialNumber")),
                })
            return disks
        except Exception:  # noqa
            pass
    # 回退：wmic（cmd 内置可用）
    stdout, rc = _run("wmic diskdrive list brief")
    if rc == 0:
        for line in stdout.splitlines()[1:]:
            parts = line.split()
            if len(parts) < 2:
                continue
            # 形如：Model  Size  DeviceID  ...
            dev = parts[-1]  # \\.\PHYSICALDRIVE2
            if not dev.upper().startswith("\\\\.\\PHYSICALDRIVE"):
                continue
            num = dev.split("PHYSICALDRIVE")[-1]
            try:
                size = int(parts[1])
            except Exception:  # noqa
                size = 0
            disks.append({
                "id": num,
                "path": dev,
                "model": parts[0],
                "size": size,
                "extra": "",
            })
    return disks


def _list_linux():
    disks = []
    stdout, rc = _run(["lsblk", "-b", "-d", "-n", "-o",
                       "NAME,SIZE,MODEL,SERIAL,TRAN,TYPE"])
    if rc == 0:
        for line in stdout.splitlines():
            parts = line.split()
            if len(parts) < 6:
                continue
            name, size, model, serial, tran, dtype = parts
            if dtype != "disk":
                continue
            disks.append({
                "id": name,
                "path": "/dev/" + name,
                "model": (model + " " + serial).strip(),
                "size": int(size),
                "extra": "TRAN=%s" % tran,
            })
    return disks


def _list_darwin():
    disks = []
    # 枚举 /dev/diskN，逐个取信息（盘数量通常很少）
    for name in sorted(os.listdir("/dev")):
        if not (name.startswith("disk") and name[4:].isdigit()):
            continue
        path = "/dev/" + name
        stdout, rc = _run(["diskutil", "info", "-plist", path])
        if rc != 0 or not stdout.strip():
            continue
        try:
            import plistlib
            info = plistlib.loads(stdout.encode("utf-8", "ignore"))
        except Exception:  # noqa
            continue
        size = info.get("TotalSize", 0) or 0
        if not size:
            continue
        disks.append({
            "id": name,
            "path": path,
            "model": info.get("MediaName", "") or info.get("DeviceName", ""),
            "size": int(size),
            "extra": "Bus=%s" % info.get("BusProtocol", ""),
        })
    return disks


# --------------------------------------------------------------------------
# 镜像校验
# --------------------------------------------------------------------------
def validate_image(image_path):
    if not os.path.isfile(image_path):
        return None, "镜像文件不存在: %s" % image_path
    size = os.path.getsize(image_path)
    if size % 512 != 0:
        return None, "镜像大小 %d 不是 512 的倍数（引导扇区必须 512 对齐）" % size
    with open(image_path, "rb") as f:
        first = f.read(512)
    if len(first) < 512 or first[510] != 0x55 or first[511] != 0xAA:
        return None, "首扇区结尾不是 0x55 0xAA（不是合法引导扇区）"
    return size, None


# --------------------------------------------------------------------------
# 写入（1 MiB 块，原样）
# --------------------------------------------------------------------------
def write_image(image_path, device_path, verify):
    flags = os.O_WRONLY
    if IS_WIN:
        flags |= getattr(os, "O_BINARY", 0)
    try:
        fd = os.open(device_path, flags)
    except PermissionError:
        raise SystemExit(
            "无法以写方式打开 %s：权限不足或设备被占用。\n"
            "  Windows：请用管理员身份运行本脚本，并先在资源管理器里'弹出'该 U 盘。\n"
            "  Linux/macOS：请用 sudo 运行。\n"
            "  仍失败可改用图形化工具：Win32 Disk Imager / Rufus(DD 镜像模式) / dd。" % device_path)
    except OSError as e:
        raise SystemExit("无法打开设备 %s：%s\n提示：确认盘号/设备名正确、且不是系统盘。" % (device_path, e))

    written = 0
    try:
        with open(image_path, "rb") as fsrc:
            while True:
                buf = fsrc.read(CHUNK)
                if not buf:
                    break
                os.write(fd, buf)
                written += len(buf)
        # 落盘
        try:
            os.fsync(fd)
        except OSError:
            pass
    finally:
        os.close(fd)

    if verify:
        written_back = _verify_readback(image_path, device_path)
        if written_back != written:
            raise SystemExit("回读校验失败：写入 %d 字节，回读 %d 字节。" % (written, written_back))
        print("校验通过：回读 %d 字节与镜像一致。" % written_back)
    return written


def _verify_readback(image_path, device_path):
    flags = os.O_RDONLY
    if IS_WIN:
        flags |= getattr(os, "O_BINARY", 0)
    fd = os.open(device_path, flags)
    total = 0
    try:
        with open(image_path, "rb") as fsrc:
            while True:
                src = fsrc.read(CHUNK)
                if not src:
                    break
                dev = os.read(fd, len(src))
                if len(dev) != len(src) or dev != src:
                    raise SystemExit("回读比对在第 %d 字节处不一致，U 盘可能损坏。" % total)
                total += len(src)
    finally:
        os.close(fd)
    return total


# --------------------------------------------------------------------------
# 输出
# --------------------------------------------------------------------------
def human(n):
    if n <= 0:
        return "0 B"
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    f = float(n)
    for u in units:
        if f < 1024 or u == units[-1]:
            return ("%.0f %s" % (f, u)) if u == "B" else ("%.2f %s" % (f, u))
        f /= 1024


def main():
    ap = argparse.ArgumentParser(
        description="安全地把 os-image.bin 整盘写入 U 盘（默认只读检查，不写）。")
    ap.add_argument("--image", default=None, help="镜像路径（默认 ../os-image.bin）")
    ap.add_argument("--write", action="store_true", help="真正写盘（必须同时给 --disk）")
    ap.add_argument("--disk", default=None, help="目标盘 id（Windows 如 2，Linux 如 sdb，macOS 如 disk4）")
    ap.add_argument("--verify", action="store_true", help="写后逐块回读比对（慢但稳）")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    image_path = args.image or os.path.normpath(os.path.join(here, "..", "os-image.bin"))

    print("=" * 64)
    print("EZOS U 盘制作工具（只读优先，绝不自动选盘）")
    print("=" * 64)

    # 1) 列出本机磁盘
    disks = list_disks()
    print("\n[本机磁盘]")
    if not disks:
        print("  （未能枚举到磁盘；Windows 试了 Get-Disk/wmic，Linux 需 lsblk，macOS 需 diskutil）")
    else:
        print("  %-6s %-14s %-22s %s" % ("id", "容量", "型号", "备注"))
        for d in disks:
            print("  %-6s %-14s %-22s %s" % (d["id"], human(d["size"]),
                                             (d["model"] or "?")[:22], d.get("extra", "")))
        print("  （Windows 上 Number 0 通常是系统盘，绝对不要写它）")

    # 2) 校验镜像
    print("\n[镜像校验] %s" % image_path)
    size, err = validate_image(image_path)
    if err:
        print("  ✗ " + err)
        raise SystemExit(1)
    print("  ✓ 大小 %d 字节（%d 扇区，512 对齐），首扇区 0x55 0xAA OK" % (size, size // 512))

    if not args.write:
        print("\n[dry-run] 未指定 --write，什么都没写。")
        print("  要真正写入，请先按上面 id 确认目标盘，然后：")
        print("    python3 tools/make_usb_boot.py --write --disk <id>")
        print("  脚本会再次要求你输入盘号确认。")
        return

    if not args.disk:
        raise SystemExit("\n[--write] 必须同时用 --disk <id> 指定目标盘。上面列出的 id 就是候选。")

    # 3) 写盘：定位目标盘并二次确认
    target = None
    for d in disks:
        if str(d["id"]) == str(args.disk):
            target = d
            break
    if not target:
        raise SystemExit("\n[--disk %s] 不在本机磁盘列表中，先核对上面的 id 再写。" % args.disk)

    if size > target["size"]:
        raise SystemExit("\n[中止] 镜像 %s 比目标盘 %s（%s）还大，无法写入。" %
                         (human(size), target["id"], human(target["size"])))

    print("\n[将要写入]")
    print("  目标盘 : id=%s  path=%s  容量=%s" % (target["id"], target["path"], human(target["size"])))
    print("  写入字节: %s（仅镜像大小，不会动盘上其余空间，但会覆盖 LBA0 起的等量区域）" % human(size))
    print("  ⚠ 警告：写错盘会清空该盘全部数据，且难以恢复。")

    # 二次确认：必须原样输入盘号
    try:
        confirm = input("  请输入目标盘 id「%s」以确认写入（输错或留空=中止）：" % target["id"])
    except EOFError:
        confirm = ""
    if confirm.strip() != str(target["id"]):
        raise SystemExit("\n[已中止] 确认输入 '%s' 与 '%s' 不符，未写入任何数据。" %
                         (confirm.strip(), target["id"]))

    print("\n写入中（块大小 %s）..." % human(CHUNK))
    written = write_image(image_path, target["path"], args.verify)
    print("✓ 完成：已写入 %s 到 %s" % (human(written), target["path"]))
    print("  别在 Windows 里点'格式化'！直接安全弹出 U 盘去真机试。")


if __name__ == "__main__":
    main()
