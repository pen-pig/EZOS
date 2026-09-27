/*
 * usbhc.h - USB 主机控制器传输层统一入口（真机点亮 A1 第二阶段）
 *
 * 为什么需要这一层：H2 里所有类驱动（枚举/HID 键鼠/U 盘）都是直接调
 * uhci_* 的，那时机器上只有 UHCI 一种控制器。A1 加了 EHCI 之后，
 * 同一个设备可能挂在 UHCI 端口（全速/低速）或 EHCI 端口（高速）上，
 * 走哪条路必须由设备自己携带的句柄决定，而不是让类驱动记住寄存器基址。
 *
 * 这一层只做**分派**，不碰任何寄存器：
 *   usbhc_t = { 控制器类型, 端口, 低速标志, UHCI io / EHCI ctl }
 *   usbhc_* 原语 -> uhci_* 或 ehci_*
 *
 * 能力差异显式化（fail closed，绝不假装成功）：
 *   - EHCI 目前只实现控制传输与 bulk（U 盘的 BOT 通道）——这正是真机上
 *     高速设备的两个刚需。
 *   - EHCI 的中断 IN（周期调度）尚未实现：usbhc_interrupt_in 对 EHCI
 *     直接返回 -1。真机上 HID 键鼠绝大多数是 FS/LS，会被 EHCI 交还
 *     companion UHCI，走的是 UHCI 那条已验证的路径，不受影响。
 */
#ifndef USBHC_H
#define USBHC_H

#include "types.h"

/* 控制器类型 */
#define USBHC_UHCI  0u
#define USBHC_EHCI  1u

/* 统一端口表容量：UHCI 根口 + EHCI 根口，教学 OS 足够 */
#define USBHC_MAX_PORTS 16

/*
 * 一个"已连接端口"的句柄。枚举层（usbenum）拿它去做标准请求，
 * 类驱动（usbkbd/usbmouse/usbmsc）拿它去发中断/bulk 传输。
 * 字段按控制器类型解释：
 *   hc == USBHC_UHCI：io 有效，ctl 无意义
 *   hc == USBHC_EHCI：ctl（控制器索引）有效，io 无意义，ls 恒 0（高速）
 */
typedef struct {
    uint8_t  hc;      /* USBHC_UHCI / USBHC_EHCI */
    uint8_t  ctl;     /* EHCI 控制器索引 */
    uint8_t  port;    /* 根口编号 */
    uint8_t  ls;      /* 低速设备（仅 UHCI 有意义） */
    uint16_t io;      /* UHCI I/O 基址 */
} usbhc_t;

/*
 * 合并 UHCI/EHCI 两边的已连接端口表。必须在 uhci_init() 与 ehci_init()
 * 之后、usbenum_init() 之前调用一次。
 */
void usbhc_scan(void);

int usbhc_port_count(void);
int usbhc_port_get(int i, usbhc_t *out);

/* 控制器名（"UHCI"/"EHCI"），日志用 */
const char *usbhc_name(uint8_t hc);

/*
 * 端口复位：让设备回到默认态（地址 0）。
 * 返回 1 = 复位后仍有设备（可以枚举）；0 = 空口或已交还 companion。
 */
int usbhc_port_reset(const usbhc_t *h);

/* 确保控制器处于运行态（UHCI 的 CF/RS；EHCI 恒真）。返回 0 = 可用 */
int usbhc_hc_start(const usbhc_t *h);

/* 标准控制传输。返回 0 成功，<0 失败。actlen 可为 NULL */
int usbhc_control_xfer(const usbhc_t *h, uint8_t addr, uint8_t ep,
                       const uint8_t *setup, int dir_in,
                       uint8_t *buf, int blen, int *actlen);

/*
 * 中断 IN 轮询一次。返回 0 = 轮询完成（**可能没数据**：NAK 是常态，
 * 此时 *actlen=0）；<0 = 失败，或该控制器未实现中断传输（EHCI）。
 */
int usbhc_interrupt_in(const usbhc_t *h, uint8_t addr, uint8_t ep,
                       uint8_t *buf, int blen, int *actlen);

/* 一笔 bulk 传输。返回 0 成功（*actlen=实际字节数），<0 失败 */
int usbhc_bulk_xfer(const usbhc_t *h, uint8_t addr, uint8_t ep,
                    uint8_t *buf, int blen, int mps, int *actlen);

/* bulk 端点 toggle 清零（BOT reset / ClearFeature(HALT) 之后必须调用） */
void usbhc_bulk_tog_reset(const usbhc_t *h, uint8_t addr);

/* 该控制器是否支持中断 IN 轮询（EHCI 暂不支持，类驱动据此跳过认领） */
int usbhc_supports_interrupt(const usbhc_t *h);

#endif
