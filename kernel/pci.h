/*
 * pci.h - PCI 配置空间访问与总线枚举（步骤 7 网络的前置设施）
 *
 * 网卡（RTL8139 / e1000 / virtio-net）都是 PCI 设备，要知道它在不在、
 * BAR 映射到哪、中断线接在哪个 IRQ，都得先读配置空间。本模块只做
 * **枚举与查询**，不碰具体设备驱动——驱动按 vendor/device id 认领设备。
 *
 * 采用 PCI Mechanism #1（CONFIG_ADDRESS 0xCF8 + CONFIG_DATA 0xCFC），
 * 这是 PC 兼容机上唯一现实可用的方式（#2 在多数学不到、且已被废弃）。
 *
 * 安全边界：本模块只读配置空间。写配置空间（pci_write_*）目前仅用于
 * 打开设备的 MMIO/IO 解码位，调用方必须自行确认设备归属，避免误改
 * 桥片/存储控制器的配置导致系统挂死。
 */
#ifndef PCI_H
#define PCI_H

#include "types.h"

#define PCI_MAX_DEVICES 32          /* 枚举上限：教学 OS 足够，且避免缓冲无限增长 */

/* 配置空间里我们关心的标准寄存器偏移（PCI Local Bus Spec 3.0 第 6 章） */
#define PCI_REG_VENDOR     0x00     /* 低 16 位 vendor id，高 16 位 device id */
#define PCI_REG_COMMAND    0x04     /* bit0 IO Space, bit1 Memory Space, bit2 Bus Master */
#define PCI_REG_STATUS     0x06
#define PCI_REG_REVISION   0x08     /* 低 8 位 revision id */
#define PCI_REG_PROGIF     0x09
#define PCI_REG_SUBCLASS   0x0A
#define PCI_REG_CLASS      0x0B
#define PCI_REG_CACHELINE  0x0C
#define PCI_REG_LATENCY    0x0D
#define PCI_REG_HEADER     0x0E     /* 低 7 位 header type（bit7 = 多功能设备） */
#define PCI_REG_BAR0       0x10     /* BAR0..BAR5 各 4 字节 */
#define PCI_REG_SUBSYS     0x2C
#define PCI_REG_INTR_LINE  0x3C     /* 中断线（BIOS/固件填的 PIC IRQ 号） */
#define PCI_REG_INTR_PIN   0x3D

/* COMMAND 寄存器位 */
#define PCI_CMD_IO_SPACE   0x0001u  /* 允许设备响应 IO 空间访问 */
#define PCI_CMD_MEM_SPACE  0x0002u  /* 允许设备响应 MMIO 访问 */
#define PCI_CMD_BUS_MASTER 0x0004u  /* 允许设备发起 DMA（网卡收发必开） */

/* 设备类别（class << 8 | subclass） */
#define PCI_CLASS_NETWORK  0x02u

typedef struct {
    uint8_t  bus;
    uint8_t  dev;             /* 设备号 0-31 */
    uint8_t  func;            /* 功能号 0-7 */
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint32_t bar[6];
    uint8_t  intr_line;       /* 0xFF = 不使用中断 */
    uint8_t  intr_pin;
} pci_device_t;

/* 总线枚举：扫描 bus 0-255 上的所有 slot/function，填充内部设备表。
 * 返回发现的设备数量（超过 PCI_MAX_DEVICES 的部分丢弃）。
 * 未插任何 PCI 设备的机器（真机可能没有）返回 0，不是错误。 */
int pci_scan(void);

/* 查询结果 */
int          pci_device_count(void);
const pci_device_t *pci_get_device(int index);

/* 按类别查找第一个匹配设备，未找到返回 0 */
const pci_device_t *pci_find_by_class(uint8_t class_code);

/* 配置空间读写（32 位对齐；偏移必须 4 字节对齐） */
uint32_t pci_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write_dword(uint8_t bus, uint8_t dev, uint8_t func,
                         uint8_t offset, uint32_t value);

/* 常用字段的便捷读取（内部做移位/掩码，调用方不必自己算） */
uint16_t pci_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read_byte(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/* 已知设备的可读名称（识别不了时返回 "unknown device"） */
const char *pci_vendor_name(uint16_t vendor_id);
const char *pci_device_name(uint16_t vendor_id, uint16_t device_id);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif
