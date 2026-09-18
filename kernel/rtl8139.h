/*
 * rtl8139.h - Realtek RTL8139 网卡驱动（步骤 7 网络第一阶段：设备初始化）
 *
 * RTL8139 是 QEMU `-device rtl8139` 提供的教学级百兆网卡，
 * 编程模型简单（4 个 TX 描述符轮转 + 环形 RX 缓冲），常作为 OS 开发
 * 的第一块网卡。本阶段只做"认领 + 上电 + 复位 + 读 MAC"，
 * 不建收发缓冲、不挂中断——那属于后续阶段（RX/TX、IRQ、ARP/IP）。
 *
 * 依赖 kernel/pci.c 的枚举结果：rtl8139_init() 必须在 pci_scan() 之后调用。
 * QEMU 未挂网卡时 init 返回 -1，调用方静默跳过（无网卡不是错误）。
 */
#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"

/* 初始化：PCI 表认领 10EC:8139 → 开 IO 解码 + BusMaster →
 * 退出低功耗 → 软复位（带超时）→ 读 MAC。
 * 返回 0 = 找到并初始化成功；-1 = 无此设备或形态异常/复位超时。 */
int rtl8139_init(void);

/* 状态查询（未初始化时 present=0、mac 全 0） */
int      rtl8139_present(void);
uint16_t rtl8139_io_base(void);
uint8_t  rtl8139_irq(void);
const uint8_t *rtl8139_mac(void);

/* 把 MAC 格式化成 "XX:XX:XX:XX:XX:XX"（17 字符 + '\0'，out 至少 18 字节） */
void rtl8139_mac_str(char *out);

#endif
