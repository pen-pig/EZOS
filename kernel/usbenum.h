/*
 * usbenum.h - USB 设备枚举与描述符解析（真机点亮 H2-2c）
 *
 * 职责边界（严格对齐本步）：
 *   对每个"已连接端口"走一遍 USB 规范的标准枚举流程：
 *     端口复位 -> GET_DESCRIPTOR(Device, 8) 取 bMaxPacketSize0
 *              -> GET_DESCRIPTOR(Device, 18) 取完整设备描述符
 *              -> SET_ADDRESS(addr)          （之后 >=2ms 才可用新地址）
 *              -> GET_DESCRIPTOR(Device,18) @addr  验证地址生效
 *              -> GET_DESCRIPTOR(Configuration, 9) 取 wTotalLength
 *              -> GET_DESCRIPTOR(Configuration, wTotalLength) 取完整配置
 *              -> 解析接口/端点（识别 HID class 0x03 与中断 IN 端点）
 *              -> SET_CONFIGURATION(cfg_value)
 *              -> 若为 HID：GET_DESCRIPTOR(HID, 0x21) 取报告描述符长度
 *   本步**不做**：中断传输轮询、HID 报告解析、键盘输入接线（下一步 H2-2d）。
 *
 * 设计要点
 *   - 传输全部走 uhci_control_xfer()（uhci.c 提供的控制传输原语），本模块
 *     不碰任何 UHCI 寄存器，只认"端口句柄 + 标准请求"这一层抽象。
 *   - 所有证据走 dmesg_write（H1a 起的权威诊断通道，真机无屏也可见）。
 *   - fail closed：任一步失败即放弃该端口并打印失败行，**绝不**在没有真实
 *     描述符的情况下报成功。无设备时不打印任何成功行。
 *   - 不可信字段上界：描述符长度/条数先夹紧再当下标，防止越界读；解析循环
 *     遇到 len<2 或越界立即跳出（防死循环）。
 */
#ifndef USBENUM_H
#define USBENUM_H

#include "types.h"

#define USBENUM_MAX_DEV 8

/* 枚举结果（一个 USB 设备一份） */
typedef struct {
    uint16_t io;            /* 所属 UHCI 控制器 I/O 基址 */
    uint8_t  port;          /* 根口编号 */
    uint8_t  addr;          /* 分配到的 USB 地址（1..127） */
    uint8_t  lowspeed;      /* 低速设备 */

    /* 设备描述符 */
    uint16_t vid, pid;
    uint8_t  cls, sub, proto;   /* 设备级 class/subclass/protocol */
    uint8_t  mps0;              /* 端点 0 最大包长 */
    uint8_t  ncfg;              /* 配置数 */

    /* 配置描述符 */
    uint8_t  cfg_value;         /* bConfigurationValue */
    uint16_t cfg_total;         /* wTotalLength */
    uint8_t  nif;               /* 接口数 */

    /* HID（bInterfaceClass == 0x03） */
    int      hid_if;            /* HID 接口号；-1 = 没有 HID 接口 */
    uint8_t  hid_sub, hid_proto;/* subclass(1=boot) / protocol(1=键盘,2=鼠标) */
    uint8_t  hid_ep;            /* 中断 IN 端点地址（bit7=1）；0 = 没找到 */
    uint16_t hid_ep_mps;        /* 该端点最大包长 */
    uint8_t  hid_ep_interval;   /* 轮询间隔（ms） */
    uint16_t hid_rep_len;       /* HID 报告描述符长度（字节） */
    uint8_t  configured;        /* SET_CONFIGURATION 已成功 */

    /* Mass Storage（bInterfaceClass == 0x08，BOT 需 proto==0x50）
     * H1c：U 盘认领用的接口/端点记录。只记第一个 MSC 接口的第一对 bulk
     * 端点（BOT 协议本身只允许一对）。 */
    int      msc_if;            /* MSC 接口号；-1 = 没有 MSC 接口 */
    uint8_t  msc_sub, msc_proto;/* subclass(0x06=SCSI 透明) / protocol(0x50=BOT) */
    uint8_t  msc_ep_in;         /* bulk IN 端点地址（bit7=1）；0 = 没找到 */
    uint16_t msc_ep_in_mps;
    uint8_t  msc_ep_out;        /* bulk OUT 端点地址（bit7=0）；0 = 没找到 */
    uint16_t msc_ep_out_mps;
} usb_dev_t;

/* 入口：在 kernel_main 的 uhci_init() 之后调用一次。
 * 没有任何已连接端口时打印 "USB-ENUM: 0 device(s) enumerated" 并返回。 */
void usbenum_init(void);

/* 枚举到的设备数 */
int usbenum_device_count(void);

/* 取第 i 个设备（i 越界返回 NULL） */
const usb_dev_t *usbenum_get(int i);

#endif
