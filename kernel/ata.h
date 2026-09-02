#ifndef ATA_H
#define ATA_H

#include "types.h"

int ata_read_sector(uint8_t drive, uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint8_t drive, uint32_t lba, const uint8_t *buffer);
int ata_drive_present(uint8_t drive);
void ata_init(void);

#endif
