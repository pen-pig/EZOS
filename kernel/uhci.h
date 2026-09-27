/*
 * uhci.h - UHCI 主机控制器初始化 + 端口连接检测（真机点亮 H2-2a）
 *
 * 本步**只**做：定位 UHCI 控制器、必要时自编程 I/O BAR、全局复位、
 * 建立帧列表、启动调度、轮询端口连接状态并 klog。
 * 本步**不**做：设备枚举、任何传输（IN/OUT/SETUP）、中断注册。
 * UHCI 默认 IRQ11 与 RTL8139 撞，共享 IRQ 框架是后面单独一步的事。
 *
 * H2-2b 起本模块同时是**传输层**：对上提供控制传输（uhci_control_xfer）
 * 与端口复位（uhci_port_reset）两个原语，并维护"已连接端口表"供上层
 * 枚举模块（usbenum）消费。中断（周期/批量）传输是后面单独一步。
 */
#ifndef UHCI_H
#define UHCI_H

#include "types.h"

/* 一个控制器最多登记这么多个已连接端口（PIIX3 UHCI 只有 2 个根口）。
 * 端口表是静态的：枚举前由 uhci_init 填好，之后只读，无动态分配、
 * 无失败回滚问题。 */
#define UHCI_MAX_PORTS 8

/* 入口：在 kernel_main 的 usb_scan_log() 之后调用一次。
 * 找不到 UHCI 控制器时打印 "UHCI: no UHCI controller found" 并静默返回。
 * 找到则按上面描述初始化并把端口连接态经 dmesg 镜像进串口。 */
void uhci_init(void);

/* ---- H2-2b/2c：对上提供的传输层原语 ---- */

/*
 * 一笔标准控制传输（SETUP + 可选 DATA + STATUS），同步轮询完成。
 * 返回 0 成功，<0 失败（超时/硬件错误）；绝不静默挂死（超时上界 fail closed）。
 *   io        控制器 I/O 基址（由 uhci_port_get 给出）
 *   addr      设备地址（0-127）
 *   ep        端点号（0-15）
 *   setup     8 字节 SETUP 包
 *   dir_in    数据阶段方向（1=IN 设备到主机，0=OUT）
 *   buf/blen  数据缓冲与长度；blen 上界 256（配置描述符需要 >64）
 *   lowspeed  该端口挂的是低速设备（LSDA）则置 1
 *   actlen    实际传输字节数（可为 NULL）
 */
int uhci_control_xfer(uint16_t io, uint8_t addr, uint8_t ep,
                      const uint8_t *setup, int dir_in,
                      uint8_t *buf, int blen, int lowspeed, int *actlen);

/*
 * 端口级复位：PR 置位保持 >=10ms 再清 0，并显式重新使能端口。
 * 返回复位后是否仍连接（CCS）；未连设备返回 0（调用方须 fail closed 跳过）。
 */
int uhci_port_reset(uint16_t io, int port);

/*
 * 从中断 IN 端点轮询一次（H2-2d：HID 键盘报告通道）。
 * 返回 0 = 轮询完成（**可能没有数据**：设备 NAK 是常态，此时 *actlen=0）；
 *      <0 = 失败（超时 / STALL / CRC 等致命位）。
 * 语义要点：
 *   - NAK 不算错误：没有新报告时设备恒 NAK，调用方必须容忍 actlen==0。
 *   - DATA toggle 由本模块按地址维护，**只在真的收到数据时翻转**（NAK/零长度
 *     不翻），否则设备会因 toggle 不匹配拒绝后续报告。
 *   - blen 上界 16（HID boot 报告 8 字节，留余量）。
 */
int uhci_interrupt_in(uint16_t io, uint8_t addr, uint8_t ep,
                      uint8_t *buf, int blen, int lowspeed, int *actlen);

/*
 * 确保控制器处于运行态：重新锁存帧列表物理地址并置 RS=1/CF=1。
 * GRESET 会清掉 CF/RS，任何在复位之后发起的传输都必须先调这个，
 * 否则 HC 只发 SOF、不执行 TD（表现为"调度在跑但传输永不完成"）。
 * 返回 0 = 已运行（USBSTS.HCH=0），-1 = 仍处于 halted。
 */
int uhci_hc_start(uint16_t io);

/* 已连接端口表条数（uhci_init 之后有效；0 = 没有任何设备接入）。 */
int uhci_port_count(void);

/*
 * 取第 i 个已连接端口的句柄。io_out/port_out/ls_out 均可为 NULL。
 * 返回 0 成功，-1 越界。
 */
int uhci_port_get(int i, uint16_t *io_out, int *port_out, int *ls_out);

#endif
