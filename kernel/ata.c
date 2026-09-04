/*
 * ata.c - ATA/IDE PIO 驱动（主/副总线，四驱动器，硬件兼容加固）
 *
 * 兼容性措施：
 *   - 支持 primary(0x1F0)/secondary(0x170) 两条总线，drive 0-3；
 *   - 选盘后读 4 次备用状态口产生 400ns 延迟（等待驱动器就绪）；
 *   - 浮动总线检测：状态口读回 0xFF 说明驱动器不存在，快速失败，
 *     避免对空插槽做长时间轮询超时；
 *   - 命令完成后逐位检查 BSY/DRQ/ERR/DF，任何异常立即返回错误；
 *   - 写命令在数据阶段结束后额外等待 BSY 清除（flush）。
 */
#include "ata.h"
#include "port.h"
#include "tty.h"

#define ATA_STAT_BSY  0x80
#define ATA_STAT_DRDY 0x40
#define ATA_STAT_DF   0x20
#define ATA_STAT_DRQ  0x08
#define ATA_STAT_ERR  0x01

/* 命令后轮询上限（PIO 下远大于任何真实驱动器需求） */
#define ATA_TIMEOUT 4000000u

/* drive 0/1 = 主总线主/从盘；2/3 = 副总线主/从盘 */
static uint16_t ata_io_base(uint8_t drive) {
    return (drive >= 2) ? 0x170 : 0x1F0;
}

/* 400ns 延迟：读 4 次备用状态口（base+0x206），不清除中断状态 */
static void ata_400ns(uint16_t base) {
    for (int i = 0; i < 4; i++) inb(base + 0x206);
}

/* 选盘；浮动总线（0xFF）或超时视为驱动器不存在，返回 -1 */
static int ata_select(uint8_t drive) {
    uint16_t base = ata_io_base(drive);
    outb(base + 6, (drive & 1) ? 0xF0 : 0xE0);
    ata_400ns(base);

    uint32_t t = ATA_TIMEOUT;
    uint8_t status = inb(base + 7);
    while (status & ATA_STAT_BSY) {
        if (status == 0xFF) return -1;
        if (t-- == 0) return -1;
        status = inb(base + 7);
    }
    if (status == 0xFF) return -1;      /* 空插槽：数据线浮动 */
    return 0;
}

/* 等 BSY 清除；检查错误位 */
static int ata_wait_idle(uint16_t base) {
    uint32_t t = ATA_TIMEOUT;
    uint8_t status = inb(base + 7);
    while (status & ATA_STAT_BSY) {
        if (status == 0xFF) return -1;
        if (t-- == 0) return -1;
        status = inb(base + 7);
    }
    if (status & ATA_STAT_ERR) return -1;
    if (status & ATA_STAT_DF) return -1;
    return 0;
}

/* 等 DRQ 置位（数据就绪）；期间 ERR/DF 立即失败 */
static int ata_wait_drq(uint16_t base) {
    uint32_t t = ATA_TIMEOUT;
    for (;;) {
        uint8_t status = inb(base + 7);
        if (status & ATA_STAT_ERR) return -1;
        if (status & ATA_STAT_DF) return -1;
        if (status & ATA_STAT_DRQ) return 0;
        if (status == 0xFF) return -1;
        if (t-- == 0) return -1;
    }
}

/*
 * 驱动器存在性检测：IDENTIFY DEVICE 验证。
 *
 * 仅靠浮动总线 0xFF 判存不可靠：QEMU 及部分控制器对空插槽
 * 返回 0x00（BSY/DRQ 均不置位），ata_select 会误判为存在，
 * 随后 READ SECTORS 无人应答只能等满超时（启动扫描明显变慢）。
 *
 * 标准做法：发 0xEC 后——
 *   - 空插槽无响应（状态持续 0x00/0xFF）→ 不存在；
 *   - DRQ 置位 → ATA 设备；
 *   - ERR 置位（如 ATAPI 的 ABRT）→ 设备存在但不支持 IDENTIFY。
 */
int ata_drive_present(uint8_t drive) {
    if (drive > 3) return 0;
    uint16_t base = ata_io_base(drive);
    if (ata_select(drive) != 0) return 0;

    outb(base + 2, 0);                          /* sector count = 0 */
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, 0xEC);                       /* IDENTIFY DEVICE */

    uint32_t t = ATA_TIMEOUT;
    for (;;) {
        uint8_t status = inb(base + 7);
        if (status == 0xFF) return 0;           /* 浮动总线 */
        if (status == 0x00) return 0;           /* 空插槽：命令无应答 */
        if (status & ATA_STAT_ERR) return 1;    /* ATAPI 等：存在（无数据阶段） */
        if (status & ATA_STAT_DRQ) {            /* ATA 设备：读空 256 words
                                                 * 必须清掉 IDENTIFY 数据，否则
                                                 * DRQ 滞留 + 数据滞留 FIFO，下一
                                                 * 条读命令会先取到这份残留数据 */
            for (int i = 0; i < 256; i++) (void)inw(base);
            return 1;
        }
        if (status & ATA_STAT_BSY) {            /* 忙：继续等结果 */
            if (t-- == 0) return 0;
            continue;
        }
        /* 无 BSY/DRQ/ERR 且非 0：反复采样少量次数后放弃 */
        if (t-- == 0) return 0;
        if (t < ATA_TIMEOUT - 16) return 0;     /* 17 次无有效应答：判空 */
    }
}

int ata_read_sector(uint8_t drive, uint32_t lba, uint8_t *buffer) {
    uint16_t base = ata_io_base(drive);
    if (drive > 3) return -1;
    if (ata_select(drive) != 0) return -1;

    outb(base + 2, 1);                          /* sector count */
    outb(base + 3, (uint8_t)(lba & 0xFF));
    outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(base + 7, 0x20);                       /* READ SECTORS */

    if (ata_wait_idle(base) != 0) return -1;
    if (ata_wait_drq(base) != 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(base);
        buffer[i * 2] = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)((data >> 8) & 0xFF);
    }
    return 0;
}

int ata_write_sector(uint8_t drive, uint32_t lba, const uint8_t *buffer) {
    uint16_t base = ata_io_base(drive);
    if (drive > 3) return -1;
    if (ata_select(drive) != 0) return -1;

    outb(base + 2, 1);                          /* sector count */
    outb(base + 3, (uint8_t)(lba & 0xFF));
    outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(base + 6, ((drive & 1) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(base + 7, 0x30);                       /* WRITE SECTORS */

    if (ata_wait_idle(base) != 0) return -1;
    if (ata_wait_drq(base) != 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t data = (uint16_t)buffer[i * 2] | ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(base, data);
    }

    /* 数据写完后等待驱动器真正落盘（BSY 清除） */
    if (ata_wait_idle(base) != 0) return -1;
    return 0;
}

/* ============ 探测输出 ============ */

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
    static const char *bus_name[2] = { "primary", "secondary" };

    for (uint8_t d = 0; d < 4; d++) {
        terminal_writestring("ATA: ");
        terminal_writestring(bus_name[d >> 1]);
        terminal_writestring((d & 1) ? " slave" : " master");
        terminal_writestring(" (drive ");
        terminal_putchar('0' + d);
        terminal_writestring(")...\n");
        if (!ata_drive_present(d)) {
            terminal_writestring("  not present\n");
            continue;
        }
        if (ata_read_sector(d, 0, mbr) != 0) {
            terminal_writestring("  read error\n");
            continue;
        }
        if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
            terminal_writestring("  OK, MBR signature valid\n");
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
            terminal_writestring("  no valid MBR (superfloppy?)\n");
        }
    }
}
