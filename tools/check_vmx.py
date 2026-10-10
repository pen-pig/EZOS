#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_vmx.py - VMware vmx 配置完整性闸门。

起因是2026-10-10 的一次真实故障：Workstation 弹
`Could not open virtual machine "EZOS": Internal error. Remove from library?`

排查结论是**生成器少写了一个字段**。tools/make_vmware.py 早期只写
displayName/guestOS/memsize/numvcpus 等少数几项，缺
`virtualHW.version` 与 `config.version`——VMware 26 解析 vmx 时要先靠
`virtualHW.version` 定硬件兼容性、再靠 `config.version` 定配置 schema，
两者任一缺失就直接判定"配置文件读不了"，连报错都只给一句 Internal error。

这类问题特别难查，因为：
  * vmx 语法完全合法，任何通用 INI 解析器都能读；
  * 报错指向"内部错误/从库中移除"，诱导你去动虚拟机库（inventory.vmls），
    而库本身往往是好的；
  * 只有拿一台**已知能打开**的 vmx 做键名对照才看得出来。

所以有了这道闸门：只校验**字段存在性**，不做语法解析——真正的回归防线是
"生成器模板和产物两边都得有这些键"，少写一个立刻红。

注意这只覆盖 VMware 这一条验证通道。QEMU 的命令行参数不受影响。

用法：
    python tools/check_vmx.py            # 检查产物（有产物才查）
    python tools/check_vmx.py --quiet    # 只在失败时输出（回归用）
退出码 0 = 通过（产物不存在也通过，因为 vmx 不是每次回归都会生成）。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VMX = os.path.join(ROOT, "vmware", "ezos.vmx")

# key -> 为什么必须有
REQUIRED = {
    ".encoding": "声明文件编码；缺失时非ASCII 注释按本地代码页解释",
    "virtualHW.version": "硬件/固件兼容性版本。VMware 26 缺它=读不了配置",
    "config.version": "配置 schema 版本，与 virtualHW 配套，缺它同样读不了",
    "displayName": "虚拟机显示名",
    "guestOS": "影响 VMware 注入的设备组合（网卡/显卡型号等）",
    "memsize": "内存大小，MB",
    "numvcpus": "vCPU 数",
    "firmware": "bios=legacy，efi=UEFI。必须显式写，别依赖默认值",
    # EZOS 特有：内核只有 ATA(IDE) 驱动，磁盘必须挂 IDE
    "ide0:0.present": "IDE 磁盘开关。内核只有 ATA(IDE) 与 AHCI 驱动",
    "ide0:0.fileName": "磁盘镜像路径",
    # 串口 -> 文件：内核 COM1 的全部诊断输出在这里，E2E 也按行抓它
    "serial0.present": "串口开关。不开=内核日志全丢，调试变瞎",
    "serial0.fileType": "串口重定向类型，应为 file",
    "serial0.fileName": "串口日志落盘路径",
}


def parse_keys(path):
    """取出 vmx 里所有 key（跳过注释与空行）。

    只做最朴素的切分——不校验取值是否合法，那不是这道闸门的职责。
    """
    keys = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, _, v = line.partition("=")
            keys[k.strip()] = v.strip().strip('"')
    return keys


def main():
    quiet = "--quiet" in sys.argv

    if not os.path.isfile(VMX):
        # vmx 不在默认回归链路上（只有 ninja vmware 才生成），缺产物不算失败
        if not quiet:
            print("SKIP vmware/ezos.vmx 不存在（跑 ninja vmware 生成）")
        return 0

    keys = parse_keys(VMX)
    missing = [k for k in REQUIRED if k not in keys]
    empty = [k for k in REQUIRED if k in keys and not keys[k]]

    if missing or empty:
        print("FAIL vmware/ezos.vmx 字段缺失：")
        for k in missing:
            print("  缺 %-20s  %s" % (k, REQUIRED[k]))
        for k in empty:
            print("  空 %-20s  %s" % (k, REQUIRED[k]))
        print("")
        print("VMware 的表现是 'Internal error. Remove from library?'，")
        print("很容易误以为是虚拟机库坏了，实际是 vmx 少字段。")
        print("修 tools/make_vmware.py 的 VMX 模板，不要动 inventory.vmls。")
        return 1

    # firmware 必须是 bios：VMware 自带 EFI 是 64 位，BOOTIA32.EFI 加载不了
    if keys["firmware"] != "bios":
        print("FAIL firmware = %s，应为 bios" % keys["firmware"])
        print("  VMware 的 EFI 是 64 位，32 位 BOOTIA32.EFI 加载不了，会 No bootable device")
        return 1

    if not quiet:
        print("OK vmware/ezos.vmx（%d 字段齐全）" % len(keys))
    return 0


if __name__ == "__main__":
    sys.exit(main())
