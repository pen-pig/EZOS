/*
 * pci.c - PCI 配置空间访问与总线枚举（步骤 7 网络的前置设施）
 *
 * 机制说明（PCI Local Bus Spec 3.0）：
 *   CONFIG_ADDRESS (0xCF8) 写入一个 32 位"地址包"：
 *     bit31    = 使能（必须为 1）
 *     bit30-24 = 保留
 *     bit23-16 = bus
 *     bit15-11 = device (slot)
 *     bit10-8  = function
 *     bit7-2   = 配置空间内的 dword 偏移（故要求 4 字节对齐）
 *     bit1-0   = 00
 *   随后对 CONFIG_DATA (0xCFC) 的读写就落到目标设备的那个 dword 上。
 *
 * 为什么枚举要判 vendor 0xFFFF：
 *   PCI 规范规定"没有设备"时该 slot 的配置空间读回全 1。不判这个会枚举出
 *   256*32*8 = 65536 个虚构的 0xFFFF:0xFFFF 设备，后面的驱动认领会全乱。
 *
 * 为什么多功能只扫 func 1-7：
 *   func 0 一定存在（否则 slot 就是空的）；header type 的 bit7 才是
 *   "本 slot 是多功能设备"标志。不看这个标志而盲目扫 8 个功能，会把
 *   单功能设备的 func1-7 也当成候选，触发大量无效配置周期。
 */
#include "pci.h"
#include "port.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA    0x0CFCu

/* 配置空间地址包：只有 offset 的 bit7-2 有效，故调用方必须 4 字节对齐 */
static uint32_t pci_make_addr(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset) {
    return ((uint32_t)1u << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1Fu) << 11)
         | ((uint32_t)(func & 0x7u) << 8)
         | (uint32_t)(offset & 0xFCu);
}

uint32_t pci_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_dword(uint8_t bus, uint8_t dev, uint8_t func,
                     uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_make_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t d = pci_read_dword(bus, dev, func, offset & 0xFCu);
    /* 低位半字（offset%4==0）或高位半字（offset%4==2） */
    return (uint16_t)((offset & 0x2u) ? (d >> 16) : (d & 0xFFFFu));
}

uint8_t pci_read_byte(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t d = pci_read_dword(bus, dev, func, offset & 0xFCu);
    return (uint8_t)(d >> ((offset & 0x3u) * 8u));
}

/* ---------- 设备表 ---------- */

static pci_device_t g_devices[PCI_MAX_DEVICES];
static int          g_count;

int pci_device_count(void) { return g_count; }

const pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= g_count) return 0;
    return &g_devices[index];
}

const pci_device_t *pci_find_by_class(uint8_t class_code) {
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].class_code == class_code) return &g_devices[i];
    }
    return 0;
}

/* ---------- 枚举 ---------- */

static void pci_fill_device(pci_device_t *d, uint8_t bus, uint8_t dev,
                            uint8_t func) {
    uint32_t id = pci_read_dword(bus, dev, func, PCI_REG_VENDOR);

    d->bus         = bus;
    d->dev         = dev;
    d->func        = func;
    d->vendor_id   = (uint16_t)(id & 0xFFFFu);
    d->device_id   = (uint16_t)((id >> 16) & 0xFFFFu);

    uint32_t misc = pci_read_dword(bus, dev, func, PCI_REG_REVISION);
    d->revision    = (uint8_t)(misc & 0xFFu);
    d->prog_if     = (uint8_t)((misc >> 8) & 0xFFu);
    d->subclass    = (uint8_t)((misc >> 16) & 0xFFu);
    d->class_code  = (uint8_t)((misc >> 24) & 0xFFu);

    d->header_type = pci_read_byte(bus, dev, func, PCI_REG_HEADER) & 0x7Fu;

    /* 只保留 6 个 BAR：header type 2（CardBus 桥）的布局不同，本 OS 不处理 */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read_dword(bus, dev, func,
                                   (uint8_t)(PCI_REG_BAR0 + i * 4));

    uint32_t intr = pci_read_dword(bus, dev, func, PCI_REG_INTR_LINE);
    d->intr_line = (uint8_t)(intr & 0xFFu);
    d->intr_pin  = (uint8_t)((intr >> 8) & 0xFFu);
}

int pci_scan(void) {
    g_count = 0;

    for (uint16_t bus = 0; bus < 256u; bus++) {
        for (uint8_t dev = 0; dev < 32u; dev++) {
            /* func 0 不存在 = 整个 slot 空 */
            uint32_t id = pci_read_dword((uint8_t)bus, dev, 0, PCI_REG_VENDOR);
            if ((id & 0xFFFFu) == 0xFFFFu) continue;

            if (g_count < PCI_MAX_DEVICES)
                pci_fill_device(&g_devices[g_count++], (uint8_t)bus, dev, 0);

            /* 多功能设备才扫 func 1-7：header type bit7 = 多功能标志 */
            uint8_t header = pci_read_byte((uint8_t)bus, dev, 0, PCI_REG_HEADER);
            if (header & 0x80u) {
                for (uint8_t func = 1; func < 8u; func++) {
                    uint32_t fid =
                        pci_read_dword((uint8_t)bus, dev, func, PCI_REG_VENDOR);
                    if ((fid & 0xFFFFu) == 0xFFFFu) continue;
                    if (g_count < PCI_MAX_DEVICES)
                        pci_fill_device(&g_devices[g_count++],
                                        (uint8_t)bus, dev, func);
                }
            }
        }
    }
    return g_count;
}

/* ---------- 可读名称 ---------- */

const char *pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
    case 0x10EC: return "Realtek Semiconductor";
    case 0x8086: return "Intel Corporation";
    case 0x1022: return "Advanced Micro Devices";
    case 0x1AF4: return "Red Hat / Virtio";
    case 0x1234: return "Technical Corp (QEMU std)";
    case 0x1B36: return "Red Hat / QEMU";
    default:     return "unknown vendor";
    }
}

const char *pci_device_name(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id == 0x10EC && device_id == 0x8139) return "RTL8139 Fast Ethernet";
    if (vendor_id == 0x8086 && device_id == 0x100E) return "82540EM Gigabit Ethernet";
    if (vendor_id == 0x8086 && device_id == 0x100F) return "82545EM Gigabit Ethernet";
    if (vendor_id == 0x1AF4 && device_id == 0x1000) return "Virtio Network Device";
    if (vendor_id == 0x1234 && device_id == 0x1111) return "QEMU Virtual VGA";
    return "unknown device";
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x00: return "Unclassified device";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI bus controller";
        case 0x01: return "IDE interface";
        case 0x06: return "SATA controller";
        default:   return "Mass storage controller";
        }
    case 0x02: return "Ethernet controller";
    case 0x03:
        switch (subclass) {
        case 0x00: return "VGA compatible controller";
        default:   return "Display controller";
        }
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06: return "Bridge";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus";
        default:   return "Serial bus controller";
        }
    default:   return "Unknown class";
    }
}
