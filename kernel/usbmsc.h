/*
 * usbmsc.h - USB Mass Storage（BOT + SCSI）驱动（真机点亮 H1c）
 *
 * 目标：U 盘（usb-storage）在内核进保护模式后依然可用。
 * 引导阶段靠 BIOS 的 INT 13h USB 仿真；一旦进内核，那条通路就没了——
 * 内核必须自己通过 UHCI 的 bulk 端点走 BOT（Bulk-Only Transport）发 SCSI
 * 命令，才能读写 U 盘。这是 H1c"从 U 盘启动还能日用"的最后一环。
 *
 * 分层（与 ahci.c 同模式）：
 *   usbmsc.c 只跟 BOT/SCSI 打交道，传输走 uhci_bulk_xfer()/uhci_control_xfer()
 *   两个原语，不碰任何 UHCI 寄存器。
 *   块层接线：U 盘映射成 drive = USBMSC_DRIVE_BASE + unit，ata.c 的
 *   read/write/present 在 drive >= USBMSC_DRIVE_BASE 时转发到这里，
 *   FS 层（exfat/fat/ext4/...）完全无感。
 *
 * fail closed 红线：
 *   - 认领条件必须全部满足（MSC 接口 + 一对 bulk 端点 + 已配置）才注册；
 *   - BOT 失败按规范走 Reset + ClearFeature(HALT) 恢复一次，再失败即放弃；
 *   - LBA 越界（> READ CAPACITY 报告的容量）直接拒绝，不碰硬件。
 */
#ifndef USBMSC_H
#define USBMSC_H

#include "types.h"

/* USB 盘在块设备层里的 drive 编号起点。
 * 必须等于 AHCI_DRIVE_BASE(4) + AHCI_MAX_PORTS(8)——ahci.h 里有编译期
 * 一致性断言，改这一处会导致那边编译失败，正好防漏改。 */
#define USBMSC_DRIVE_BASE 12

/* 最多认领这么多个 U 盘（枚举表 USBENUM_MAX_DEV=8，留一半给其他类设备） */
#define USBMSC_MAX_DEV 4

/* 探测/认领/初始化所有 U 盘。在 usbenum_init() 之后调用一次。
 * 没有可认领设备时打印 "USB-MSC: no mass storage device" 并返回 0。
 * 返回认领成功的盘数（>=0），不会失败退出。 */
int usbmsc_init(void);

/* 已认领的 U 盘数 */
int usbmsc_count(void);

/* unit 是否在线（越界返回 0） */
int usbmsc_present(uint8_t unit);

/* U 盘总扇区数（512B，来自 SCSI READ CAPACITY）。未在线返回 0（fail closed）。 */
uint32_t usbmsc_sectors(uint8_t unit);

/* 单扇区（512B）读/写，对齐 ahci_read_sector 的调用形态。
 * 返回 0 成功，-1 失败。unit 越界 / 不在线 / LBA 越界直接失败。 */
int usbmsc_read_sector(uint8_t unit, uint32_t lba, uint8_t *buffer);
int usbmsc_write_sector(uint8_t unit, uint32_t lba, const uint8_t *buffer);

#endif
