/*
 * ehci.h - EHCI（USB 2.0 高速）主机控制器驱动（真机点亮 A1 第一阶段）
 *
 * 定位：UHCI（USB 1.x，低速/全速）已在 H2 打通；真机上键盘/鼠标这类
 * FS/LS 设备会被 EHCI 的 companion（UHCI）接管，而 **U 盘、高速设备走
 * EHCI 自己**——没有 EHCI 就只能跑 12Mbps，U 盘读写会慢一个数量级，
 * 且不少机器（含目标真机）的 USB 口后面根本没挂 companion。
 *
 * 本步范围（严格对齐）：
 *   - PCI 认领 class 0x0C / subclass 0x03 / prog_if 0x20（EHCI）。
 *   - BAR 是 **MMIO**（EHCI 规范强制内存映射，与 UHCI 的 I/O 空间相反），
 *     且通常在 32MB identity 之外 -> 必须 paging_map（带 PCD/PWT 关缓存）。
 *   - BIOS/OS handoff（EECP 里的 USBLEGSUP）：真机上 EHCI 常被固件/SMM
 *     占用，不做交接则端口永远不工作。
 *   - 控制器复位 + 异步调度（async QH 环）+ 端口路由（高速自留、
 *     全速/低速交还 companion）+ 控制传输（SETUP/DATA/STATUS）。
 *   - 诊断全部走 dmesg_write（COM1），前缀 "EHCI:" / "EHCI-CTRL:"。
 *
 * 本步**不做**：周期调度（中断 IN，HID 键盘/鼠标走 EHCI 是下一步 A1 第二阶段）、
 * bulk 传输（U 盘走 EHCI 也是第二阶段）、设备枚举接线（usbenum 目前只认 UHCI）。
 *
 * 关键差异备忘（别照抄 UHCI）：
 *   - UHCI 传输靠"帧列表 -> QH/TD"，EHCI 靠"异步环 + qTD"。
 *   - EHCI 的 qTD 一次最多 5 个 4KB 页（20KB），缓冲跨页必须填多个页指针。
 *   - EHCI 的 PORT_OWNER(bit13)=1 表示"这个口交给 companion"，写了之后
 *     本控制器就不再看这个口（真实硬件上 companion 是同一块 PCI 上的 UHCI）。
 */
#ifndef EHCI_H
#define EHCI_H

#include "types.h"

/* 控制器表容量（真机常见 1-2 个 EHCI；本步只初始化第一个，其余记一笔跳过） */
#define EHCI_MAX_CTL   2
/* 单控制器根口上界（HCSPARAMS N_PORTS 是 4 位，最多 15） */
#define EHCI_MAX_PORTS 16

/*
 * 入口：在 kernel_main 的 uhci_init() 之后调用一次。
 * 找不到 EHCI 控制器时打印 "EHCI: no EHCI controller found" 并静默返回。
 * 找到则复位 + 建异步环 + 路由端口，并对首个高速口做一次最小控制传输探针
 * （GET_DESCRIPTOR / SET_ADDRESS），证据同样进串口。
 */
void ehci_init(void);

/*
 * 已登记的高速端口数（ehci_init 之后有效；0 = 没有高速设备接入）。
 * 全速/低速设备会被交还 companion，不计入这里——它们由 UHCI 侧枚举。
 */
int ehci_port_count(void);

/*
 * 取第 i 个高速端口的句柄。ctl_out/port_out 均可为 NULL。
 *   *ctl_out   控制器索引（传给 ehci_control_xfer 等）
 *   *port_out  根口编号（0 起）
 * 返回 0 成功，-1 越界。
 */
int ehci_port_get(int i, int *ctl_out, int *port_out);

/*
 * 一笔标准控制传输（SETUP + 可选 DATA + STATUS），同步轮询完成。
 * 返回 0 成功，<0 失败（超时/硬件错误）；绝不静默挂死（超时上界 fail closed）。
 *   ctl       控制器索引（ehci_port_get 给出）
 *   addr      设备地址（0-127）
 *   ep        端点号（0-15）
 *   setup     8 字节 SETUP 包
 *   dir_in    数据阶段方向（1=IN 设备到主机，0=OUT）
 *   buf/blen  数据缓冲与长度；blen 上界 256
 *   actlen    实际传输字节数（可为 NULL）
 * 端点 0 的 mps 按 USB2 规范对高速设备恒为 64，故不设参数。
 */
int ehci_control_xfer(int ctl, uint8_t addr, uint8_t ep,
                      const uint8_t *setup, int dir_in,
                      uint8_t *buf, int blen, int *actlen);

/*
 * 端口级复位：PR 置位保持 >=50ms 再清 0。
 * 返回 1 = 复位后仍是**高速**设备（端口被使能且由本控制器拥有）；
 *      0 = 不是高速（已交还 companion）或根本没有设备。
 */
int ehci_port_reset(int ctl, int port);

#endif
