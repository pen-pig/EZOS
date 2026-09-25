/*
 * ahci.h - AHCI/SATA 磁盘驱动接口（真机点亮 H1b）
 *
 * 设计要点：
 *   - 这是 ATA 驱动之外的第二条块设备通路。ATA 用 drive 0-3（PIO），
 *     AHCI 用 port 0..N，内核里映射成 block drive = AHCI_DRIVE_BASE + port。
 *     ata.c 的 read/write/present 在 drive >= AHCI_DRIVE_BASE 时直接转发到
 *     本驱动，FS 层（exfat/fat/ext4/...）完全无感——最小侵入、不破坏 ATA 路径。
 *   - 所有 DMA 缓冲（命令列表 / FIS / 命令表 / IDENTIFY 缓冲）都静态分配在
 *     .bss（19MB 区）。该区在 identity 映射内（0-32MB），所以"虚拟地址 ==
 *     物理地址"是本项目不变量，AHCI 直接拿地址当物理地址用，无需 bounce。
 *   - 轮询模式（CI 位轮询 + 超时 fail closed），暂不接 IRQ（与网络栈一致）。
 *   - 不可信字段（CAP.NP、PI 位图、port 序号）当下标/除数前一律上界检查。
 */
#ifndef AHCI_H
#define AHCI_H

#include "types.h"

/* AHCI port 在块设备层里的 drive 编号起点：drive = AHCI_DRIVE_BASE + port */
#define AHCI_DRIVE_BASE 4

/* 本驱动支持的最大端口数（同时是 drive 编号上界）。AHCI 规范上限 32，
 * 实测控制器（ich9-ahci）通常 6 个；取 8 足够且缓冲开销极小。 */
#define AHCI_MAX_PORTS 8

/* 块设备层最大 drive 编号（ATA 4 个 + AHCI_MAX_PORTS 个） */
#define BLK_MAX_DRIVE (AHCI_DRIVE_BASE + AHCI_MAX_PORTS)

/* 控制器探测与端口初始化。找不到控制器（PCI class 0x0106）时静默跳过，
 * 返回 0；找到并完成初始化返回 1；找到但初始化失败返回 -1（不影响 ATA）。 */
int ahci_init(void);

/* 端口存在性（SSTS.DET==3 且签名是 ATA）。port 越界返回 0。 */
int ahci_port_present(uint8_t port);

/* 已上线（已初始化且可用）的端口数 */
uint8_t ahci_port_count(void);

/* 单扇区（512B）读/写，对齐 ata_read_sector / ata_write_sector 的调用形态。
 * 返回 0 成功，-1 失败。port 越界或不存在直接失败（fail closed）。 */
int ahci_read_sector(uint8_t port, uint32_t lba, uint8_t *buffer);
int ahci_write_sector(uint8_t port, uint32_t lba, const uint8_t *buffer);

#endif
