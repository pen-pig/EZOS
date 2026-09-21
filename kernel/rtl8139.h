/*
 * rtl8139.h - Realtek RTL8139 网卡驱动（步骤 7 网络：帧收发层）
 *
 * RTL8139 是 QEMU `-device rtl8139` 提供的教学级百兆网卡，
 * 编程模型简单（4 个 TX 描述符轮转 + 环形 RX 缓冲），常作为 OS 开发
 * 的第一块网卡。
 *
 * 阶段 7.1：PCI 认领 + 上电 + 复位 + 读 MAC
 * 阶段 7.2：8KB RX 环 + 4 TX 描述符 + IRQ 中断收包
 * 阶段 7.3：协议层上收（ARP/ICMP/UDP/TCP 见 net.h），本层只管帧收发，
 *           并新增 rtl8139_poll() 供系统调用内轮询推进收包
 *
 * 依赖 kernel/pci.c 的枚举结果：rtl8139_init() 必须在 pci_scan() 之后、
 * 且在 kmalloc_init()/idt_init()/irq_install() 之后调用（收发缓冲来自
 * 内核堆，中断门要写 IDT）。QEMU 未挂网卡时 init 返回 -1，
 * 调用方静默跳过（无网卡不是错误）。
 */
#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"

/* 初始化：PCI 表认领 10EC:8139 → 开 IO 解码 + BusMaster →
 * 退出低功耗 → 软复位（带超时）→ 读 MAC → 分配 RX/TX 缓冲 →
 * 使能收发（CR.TE|CR.RE）→ 注册 IRQ 中断门 + 放开 PIC 掩码 → 开 IMR。
 * 返回 0 = 找到并初始化成功；-1 = 无此设备或形态异常/复位超时/缓冲不足。 */
int rtl8139_init(void);

/* 发送一个以太网帧（含 14 字节以太网头，不含 CRC——硬件自动补）。
 * 同步接口：拷入轮转描述符缓冲后立即返回，不等发送完成；
 * 4 个描述符全忙时最多自旋 TX_WAIT 次后放弃。
 * 可在 IRQ 上下文调用（内部有 IF 保护防与中断收包路径争抢描述符）。
 * 返回 0 = 已提交；-1 = 参数非法/未初始化/描述符忙超时。 */
int rtl8139_send(const uint8_t *frame, uint32_t len);

/* 轮询收包：查 ISR 的 ROK 位并排水 RX 环。系统调用上下文（IF=0，
 * IRQ 被屏蔽）里 net.c 的阻塞等待循环用它推进网络收包。 */
void rtl8139_poll(void);

/* 状态查询（未初始化时 present=0、mac 全 0） */
int      rtl8139_present(void);
uint16_t rtl8139_io_base(void);
uint8_t  rtl8139_irq(void);
const uint8_t *rtl8139_mac(void);

/* 把 MAC 格式化成 "XX:XX:XX:XX:XX:XX"（17 字符 + '\0'，out 至少 18 字节） */
void rtl8139_mac_str(char *out);

/* ---- IP 配置（ARP/ICMP 过滤用；无 DHCP，静态配置） ---- */

/* 缺省 10.0.2.15：QEMU user-net（slirp）的 guest 缺省地址，
 * 宿主机侧是 10.0.2.2。换网络环境用 rtl8139_set_ip / `nic ip` 命令。 */
const uint8_t *rtl8139_ip(void);
void rtl8139_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/* "a.b.c.d"（最长 15 字符 + '\0'，out 至少 16 字节） */
void rtl8139_ip_str(char *out);

/* ---- 统计（nic 命令 / E2E 断言用；协议层计数见 net.h） ---- */
uint32_t rtl8139_rx_packets(void);    /* 成功收到的帧数 */
uint32_t rtl8139_rx_errors(void);     /* 环数据异常（长度出界）次数 */
uint32_t rtl8139_tx_packets(void);    /* 已提交发送的帧数 */
uint32_t rtl8139_tx_busy(void);       /* 描述符忙超时放弃次数 */
uint32_t rtl8139_irq_count(void);     /* IRQ 中断次数 */

/* 首包诊断：收到的第一个包的 header status / length / 当时 CAPR 读数。
 * 未收到过包时三者均为 0。排查 RX 环偏移约定用。 */
void rtl8139_rx_debug(uint32_t *status, uint32_t *len, uint32_t *capr);

#endif
