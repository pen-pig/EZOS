/*
 * uhci.h - UHCI 主机控制器初始化 + 端口连接检测（真机点亮 H2-2a）
 *
 * 本步**只**做：定位 UHCI 控制器、必要时自编程 I/O BAR、全局复位、
 * 建立帧列表、启动调度、轮询端口连接状态并 klog。
 * 本步**不**做：设备枚举、任何传输（IN/OUT/SETUP）、中断注册。
 * UHCI 默认 IRQ11 与 RTL8139 撞，共享 IRQ 框架是后面单独一步的事。
 */
#ifndef UHCI_H
#define UHCI_H

#include "types.h"

/* 入口：在 kernel_main 的 usb_scan_log() 之后调用一次。
 * 找不到 UHCI 控制器时打印 "UHCI: no UHCI controller found" 并静默返回。
 * 找到则按上面描述初始化并把端口连接态经 dmesg 镜像进串口。 */
void uhci_init(void);

#endif
