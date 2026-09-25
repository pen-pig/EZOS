/*
 * usb.h - USB 主机控制器普查（真机点亮 H2：USB HID 第一个小目标）
 *
 * 这一步**只读枚举**，不做任何硬件初始化、不改任何寄存器、不注册中断：
 * 真机点亮前先搞清楚机器上到底有哪些 USB 控制器（UHCI/OHCI/EHCI/xHCI），
 * 再决定下一步写哪个驱动。所有分类结论经 dmesg_write 镜像进串口（真机
 * 无屏也能看见，见 H1a 诊断通道定位）。
 *
 * 与 AHCI 驱动的区别：AHCI 会开 MMIO/置位/探测端口；本模块只读 PCI
 * 配置空间里的 class/subclass/prog_if/bar/irq，然后打印，立即返回。
 */
#ifndef USB_H
#define USB_H

#include "types.h"

/* 普查入口：在 kernel_main 的 PCI 扫描之后、fs_init 之前调用一次。
 * 找不到任何 USB 控制器时打印 "USB: no USB controller found" 并静默返回。 */
void usb_scan_log(void);

#endif
