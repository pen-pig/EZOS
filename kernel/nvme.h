/*
 * nvme.h - NVMe（PCIe SSD）块设备驱动（真机点亮 A2）
 *
 * 与 AHCI 同模式：这是 ATA/AHCI/USB-MSC 之外的第四条块设备通路。
 *   drive 编号：ATA 0-3（PIO）、AHCI 4-11、USB MSC 12-15、**NVMe 16-19**。
 *   ata.c 的 present/read/write 在 drive >= NVME_DRIVE_BASE 时转发到这里，
 *   FS 层（exFAT/FAT/ext4/...）完全无感。
 *
 * 为什么需要它：现代笔记本的内置盘基本都是 NVMe，AHCI/SATA 通路看不到它。
 *
 * 32 位无 PAE 的硬约束（fail closed）：
 *   NVMe 的 BAR0 通常声明成 **64 位 MMIO**，但固件多半把它分配在 4GB 以下
 *   （QEMU 实测 0xfebf0000）。判据因此是"基址 >= 4GB 才拒绝"，而不是
 *   "类型是 64 位就拒绝"——后者会误拒完全可用的控制器。
 *
 * 缓冲：队列/Identify/数据全部静态落在 .bss（19MB 区，identity 映射
 *   == 物理地址），NVMe 直接拿内核地址当物理地址用，无需 bounce。
 *   队列基址必须按内存页（4KB）对齐，故一律 aligned(4096)。
 *
 * 简化（教学 OS，够用即止）：
 *   - 只认 512 字节逻辑块（LBADS==9），其它 LBA 格式 fail closed；
 *   - 单条命令只传一个扇区（512B），PRP1 单页即可，**永不走 PRP List**；
 *   - 轮询完成队列，不接中断、不接 MSI-X（与 AHCI/网络栈一致）；
 *   - 只初始化第一个控制器，其余打日志跳过。
 */
#ifndef NVME_H
#define NVME_H

#include "types.h"
#include "usbmsc.h"

/* NVMe namespace 在块设备层里的 drive 编号起点：drive = NVME_DRIVE_BASE + ns */
#define NVME_DRIVE_BASE 16

/* 最多认领这么多个 namespace */
#define NVME_MAX_NS 4

/* 编译期一致性断言：NVMe 区间必须正好接在 USB MSC 区间之后。
 * 改了 USBMSC_MAX_DEV 而忘改这里（或反之）立刻编译失败，防漏改。 */
_Static_assert(NVME_DRIVE_BASE == USBMSC_DRIVE_BASE + USBMSC_MAX_DEV,
               "NVME_DRIVE_BASE must follow the USB MSC drive range");

/* 控制器探测与初始化。找不到控制器（PCI class 0x0108）时打一行日志并返回 0；
 * 找到且初始化成功返回 1；找到但初始化失败返回 -1（绝不破坏其它块设备）。 */
int nvme_init(void);

/* 已认领的 namespace 数 */
int nvme_ns_count(void);

/* ns 是否在线（越界返回 0） */
int nvme_ns_present(uint8_t ns);

/* 单扇区（512B）读/写，对齐 ahci_read_sector 的调用形态。
 * 返回 0 成功，-1 失败。ns 越界 / 不在线 / LBA 越界直接失败（fail closed）。 */
int nvme_read_sector(uint8_t ns, uint32_t lba, uint8_t *buffer);
int nvme_write_sector(uint8_t ns, uint32_t lba, const uint8_t *buffer);

#endif
