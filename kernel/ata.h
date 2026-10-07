#ifndef ATA_H
#define ATA_H

#include "types.h"

int ata_read_sector(uint8_t drive, uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint8_t drive, uint32_t lba, const uint8_t *buffer);
int ata_drive_present(uint8_t drive);
void ata_init(void);

/* 磁盘总扇区数（512B/扇区），ATA/AHCI/USB-MSC/NVMe 四条通路统一入口。
 * 拿不到容量（未上线 / IDENTIFY 失败 / ATAPI）返回 0 —— fail closed，
 * 调用方必须据此拒绝格式化，而不是按猜的大小写盘。 */
uint32_t ata_capacity(uint8_t drive);

#endif
