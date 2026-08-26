#include "ata.h"
#include "port.h"
#include "tty.h"

int ata_read_sector(uint8_t drive, uint32_t lba, uint8_t *buffer) {
    uint8_t dev_head = (drive == 0) ? 0xE0 : 0xF0;
    dev_head |= ((lba >> 24) & 0x0F);

    outb(0x1F6, dev_head);
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba >> 8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba >> 16) & 0xFF));
    outb(0x1F7, 0x20);  /* READ SECTORS */

    int timeout = 1000000;
    uint8_t status = inb(0x1F7);
    while ((status & 0x80) && timeout--) {
        status = inb(0x1F7);
    }
    if (timeout <= 0) return -1;

    timeout = 1000000;
    while (((status & 0x08) == 0) && timeout--) {
        status = inb(0x1F7);
        if (status & 0x01) return -1;  /* ERR */
        if (status & 0x20) return -1;  /* DF */
    }
    if (timeout <= 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(0x1F0);
        buffer[i * 2] = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)((data >> 8) & 0xFF);
    }
    return 0;
}

int ata_write_sector(uint8_t drive, uint32_t lba, const uint8_t *buffer) {
    uint8_t dev_head = (drive == 0) ? 0xE0 : 0xF0;
    dev_head |= ((lba >> 24) & 0x0F);

    outb(0x1F6, dev_head);
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba >> 8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba >> 16) & 0xFF));
    outb(0x1F7, 0x30);  /* WRITE SECTORS */

    int timeout = 1000000;
    uint8_t status = inb(0x1F7);
    while ((status & 0x80) && timeout--) {
        status = inb(0x1F7);
    }
    if (timeout <= 0) return -1;

    timeout = 1000000;
    while (((status & 0x08) == 0) && timeout--) {
        status = inb(0x1F7);
        if (status & 0x01) return -1;  /* ERR */
        if (status & 0x20) return -1;  /* DF */
    }
    if (timeout <= 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t data = (uint16_t)buffer[i * 2] | ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(0x1F0, data);
    }

    timeout = 1000000;
    status = inb(0x1F7);
    while ((status & 0x80) && timeout--) {
        status = inb(0x1F7);
    }
    if (timeout <= 0) return -1;

    return 0;
}

static void print_dec(uint32_t num) {
    char buf[16];
    int len = 0;
    if (num == 0) {
        terminal_putchar('0');
        return;
    }
    while (num > 0) {
        buf[len++] = '0' + (num % 10);
        num /= 10;
    }
    while (len > 0) terminal_putchar(buf[--len]);
}

static void print_hex_byte(uint8_t val) {
    char hex[] = "0123456789ABCDEF";
    terminal_putchar(hex[val >> 4]);
    terminal_putchar(hex[val & 0x0F]);
}

void ata_init(void) {
    uint8_t mbr[512];

    terminal_writestring("ATA: Detecting primary master...\n");
    if (ata_read_sector(0, 0, mbr) != 0) {
        terminal_writestring("  Master: not present or read error\n");
    } else {
        if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
            terminal_writestring("  Master: OK, MBR signature valid\n");
            for (int i = 0; i < 4; i++) {
                uint8_t *entry = mbr + 446 + i * 16;
                uint8_t type = entry[4];
                if (type == 0) continue;
                uint32_t start_lba = *((uint32_t *)(entry + 8));
                uint32_t num_sectors = *((uint32_t *)(entry + 12));
                terminal_writestring("    Partition ");
                terminal_putchar('0' + i);
                terminal_writestring(": type=0x");
                print_hex_byte(type);
                terminal_writestring(", start=");
                print_dec(start_lba);
                terminal_writestring(", sectors=");
                print_dec(num_sectors);
                terminal_putchar('\n');
            }
        } else {
            terminal_writestring("  Master: no valid MBR\n");
        }
    }

    terminal_writestring("ATA: Detecting primary slave...\n");
    if (ata_read_sector(1, 0, mbr) != 0) {
        terminal_writestring("  Slave: not present or read error\n");
    } else {
        if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
            terminal_writestring("  Slave: OK, MBR signature valid\n");
        } else {
            terminal_writestring("  Slave: no valid MBR\n");
        }
    }
}
