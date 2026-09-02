#include "tty.h"
#include "keyboard.h"
#include "idt.h"
#include "isr.h"
#include "types.h"
#include "ata.h"
#include "shell.h"
#include "fs.h"
#include "mouse.h"
#include "port.h"
#include "desktop.h"
#include "gfxwin.h"
#include "gfx.h"

// �򵥳��Ⱥ��������Զ���ʽ��ʾ��ʹ��
static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void klog(const char *msg);

/* ���������ֵ���־�и�ʽ����h1/h2 Ϊ 1 ʱʮ���������������ʮ���ƣ�?*/
static void klogf(const char *s1, uint32_t v1, int h1,
                  const char *s2, uint32_t v2, int h2, const char *s3) {
    /* line 缓冲 96 字节；所有写入（含数字）统一�?sizeof(line)-1 为上界，
     * 避免数字部分无界写入越过末尾，也保证最后一位留�?'\0'�?*/
    char line[96];
    int n = 0;
    const int lim = (int)sizeof(line) - 1;
    while (*s1 && n < lim) line[n++] = *s1++;
    char t[16];
    int m = 0;
    if (v1 == 0) t[m++] = '0';
    while (v1) {
        if (h1) t[m++] = "0123456789ABCDEF"[v1 & 0xF];
        else    t[m++] = (char)('0' + v1 % 10);
        v1 = h1 ? (v1 >> 4) : (v1 / 10);
    }
    while (m && n < lim) line[n++] = t[--m];
    while (*s2 && n < lim) line[n++] = *s2++;
    m = 0;
    if (v2 == 0) t[m++] = '0';
    while (v2) {
        if (h2) t[m++] = "0123456789ABCDEF"[v2 & 0xF];
        else    t[m++] = (char)('0' + v2 % 10);
        v2 = h2 ? (v2 >> 4) : (v2 / 10);
    }
    while (m && n < lim) line[n++] = t[--m];
    while (*s3 && n < lim) line[n++] = *s3++;
    line[n] = '\0';
    klog(line);
}

/* ����ʮ������������־�� */
static void klog_hex32(const char *prefix, uint32_t val, const char *suffix) {
    char line[80];
    int n = 0;
    while (*prefix && n < 60) line[n++] = *prefix++;
    char t[16];
    int m = 0;
    if (val == 0) t[m++] = '0';
    while (val) {
        t[m++] = "0123456789ABCDEF"[val & 0xF];
        val >>= 4;
    }
    while (m) line[n++] = t[--m];
    while (*suffix && n < 60) line[n++] = *suffix++;
    line[n] = '\0';
    klog(line);
}

/* ͨ�� RTC CMOS �Ĵ�����ʵ̽���ڴ��С��?
 * reg 0x15/0x16 �����ڴ� KB����/���ֽڣ���reg 0x17/0x18 ��չ�ڴ� KB��
 * ��չ�ڴ�Ϊ 16 λ�ֶΣ����� 65535K��Լ 64MB������ʱ���� NMI�� */
static uint16_t cmos_read16(uint8_t reg) {
    outb(0x70, reg | 0x80);
    uint16_t lo = inb(0x71);
    outb(0x70, (reg + 1) | 0x80);
    uint16_t hi = inb(0x71);
    return (uint16_t)(lo | (hi << 8));
}

// ============ Linux-style verbose boot log (real time via PIT 1000Hz) ============
static void kput_uint(uint32_t v, int width) {
    char buf[12];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i < width) buf[i++] = ' ';
    while (i) terminal_putchar(buf[--i]);
}
static void kput_uint3(uint32_t v) {
    terminal_putchar((char)('0' + (v / 100) % 10));
    terminal_putchar((char)('0' + (v / 10) % 10));
    terminal_putchar((char)('0' + v % 10));
}

/* ������?PIT channel 0 ��ǰ��������Ƶ 1193�������� 1193..0�� */
static uint16_t pit_read_counter(void) {
    outb(0x43, 0x00);            /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | (hi << 8));
}

/* ��ʵ΢��ʱ�ӣ�Linux dmesg ��񣩣�?
 * �벿�� = PIT 1000Hz tick��΢�벿�� = PIT �������м�����ÿ���� 1/1193182s �� 0.838us�� */
static uint32_t pit_usec(void) {
    uint32_t c = pit_read_counter();
    if (c > 1193) c = 1193;
    uint32_t elapsed = 1193 - c;           /* ��ǰ tick �����߼��� */
    return g_pit_ticks * 1000u + (elapsed * 838u) / 1000u;
}

/* dmesg ���ʱ�����[    0.000000] �����?�Ҷ��� + 6 λ��ʵ΢�룩 */
static void klog_prefix(void) {
    uint32_t us = pit_usec();              /* ��ʵ����΢�� */
    uint32_t sec = us / 1000000u;
    uint32_t usec = us % 1000000u;
    terminal_putchar('[');
    terminal_putchar(' ');
    kput_uint(sec, 4);
    terminal_putchar('.');
    uint32_t d = 100000;
    while (d) { terminal_putchar((char)('0' + (usec / d) % 10)); d /= 10; }
    terminal_writestring("] ");
}

static void klog(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring("\n");
}

static void klog_ok(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring(" [ OK ]\n");
}

static void klog_fail(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring(" [FAIL]\n");
}

/* ʮ���Ƶ�ֵ��־�� */
static void klog_dec32(const char *prefix, uint32_t val, const char *suffix) {
    char line[80];
    int n = 0;
    while (*prefix && n < 60) line[n++] = *prefix++;
    char t[12];
    int m = 0;
    if (val == 0) t[m++] = '0';
    while (val) { t[m++] = (char)('0' + val % 10); val /= 10; }
    while (m) line[n++] = t[--m];
    while (*suffix && n < 60) line[n++] = *suffix++;
    line[n] = '\0';
    klog(line);
}

/* RTC CMOS ��ȡ��NMI ���ã���BCD ���롣�Ĵ�����0x00 �� 0x02 �� 0x04 ʱ
 * 0x07 �� 0x08 �� 0x09 �꣨����λ������ gfxwin ������ʱ��һ�¡� */
static uint8_t rtc_read(uint8_t reg) {
    outb(0x70, reg | 0x80);
    return inb(0x71);
}
static uint8_t rtc_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* ���?RTC ��ʵ����ʱ�䣨UTC+8���� gfxwin ������ʱ��һ�£���prefix �Դ����� */
static void klog_rtc_time(const char *prefix) {
    uint8_t sec  = rtc_bcd(rtc_read(0x00));
    uint8_t min  = rtc_bcd(rtc_read(0x02));
    uint8_t hour = (uint8_t)((rtc_bcd(rtc_read(0x04)) + 8) % 24);
    uint8_t day  = rtc_bcd(rtc_read(0x07));
    uint8_t mon  = rtc_bcd(rtc_read(0x08));
    uint8_t year = rtc_bcd(rtc_read(0x09));
    char buf[64];
    int n = 0;
    const char *p = prefix;
    while (*p && n < (int)sizeof(buf) - 20) buf[n++] = *p++;
    buf[n++] = (char)('0' + year / 10); buf[n++] = (char)('0' + year % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + mon / 10); buf[n++] = (char)('0' + mon % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + day / 10); buf[n++] = (char)('0' + day % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + hour / 10); buf[n++] = (char)('0' + hour % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + min / 10); buf[n++] = (char)('0' + min % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + sec / 10); buf[n++] = (char)('0' + sec % 10);
    buf[n++] = '\0';
    klog(buf);
}

/* CPUID ̽�⣨��ʵ���������ַ��������Ҷ�ӡ������?*/
static void klog_cpuinfo(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t efl;
    asm volatile("pushfl; popl %0" : "=r"(efl));
    uint32_t fl = efl ^ (1u << 21);
    asm volatile("pushl %0; popfl; pushfl; popl %0" : "+r"(fl));
    if ((fl & (1u << 21)) == (efl & (1u << 21))) {
        klog("CPU: CPUID not supported");
        return;
    }
    eax = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    uint32_t max_leaf = eax;
    char vendor[13];
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
    {
        char line[80];
        int n = 0;
        const char *p = "CPU: vendor '";
        while (*p) line[n++] = *p++;
        p = vendor;
        while (*p && n < 60) line[n++] = *p++;
        p = "', max CPUID leaf ";
        while (*p && n < 60) line[n++] = *p++;
        uint32_t ml = max_leaf;
        char t[12]; int m = 0;
        if (ml == 0) t[m++] = '0';
        while (ml) { t[m++] = (char)('0' + ml % 10); ml /= 10; }
        while (m) line[n++] = t[--m];
        line[n] = '\0';
        klog(line);
    }
    if (max_leaf >= 1) {
        eax = 1;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
        char line[96];
        int n = 0;
        const char *p = "CPU: features";
        while (*p) line[n++] = *p++;
        if (edx & (1u << 4))  { p = " TSC";  while (*p) line[n++] = *p++; }
        if (edx & (1u << 11)) { p = " SEP";  while (*p) line[n++] = *p++; }
        if (edx & (1u << 15)) { p = " CMOV"; while (*p) line[n++] = *p++; }
        if (edx & (1u << 23)) { p = " MMX";  while (*p) line[n++] = *p++; }
        if (edx & (1u << 25)) { p = " SSE";  while (*p) line[n++] = *p++; }
        if (edx & (1u << 26)) { p = " SSE2"; while (*p) line[n++] = *p++; }
        if (ecx & (1u << 0))  { p = " SSE3";   while (*p) line[n++] = *p++; }
        if (ecx & (1u << 9))  { p = " SSSE3";  while (*p) line[n++] = *p++; }
        if (ecx & (1u << 19)) { p = " SSE4.1"; while (*p) line[n++] = *p++; }
        if (ecx & (1u << 20)) { p = " SSE4.2"; while (*p) line[n++] = *p++; }
        line[n] = '\0';
        klog(line);
    }
}

void kernel_main(void) {
    /* ���ñ��� APIC��EZOS ʹ�ô�ͳ 8259 PIC �ж�·�ɡ�
     * QEMU Ĭ�� LAPIC enabled �� LVT0(ExtINT) masked�����̵� PIC ��
     * ����/����ж����󣻹ر�?LAPIC �� LINT0 �ָ�Ϊ INTR ����ֱͨ PIC�� */
    {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
        lo &= ~(1u << 11);
        asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1B));
    }

    terminal_initialize();
    gfx_text_font_init();     /* unify text-mode font with GUI/OCR font table */

    klog("EZOS Kernel 0.9.0 loaded at 0x10000, i686 protected mode");
    klog("Boot: 512 sectors kernel image read by BIOS INT 13h AH=42h (64-sector batches, 3 retries)");
    klog("Boot: A20 gate enabled (BIOS int 15h / port 0x92 / KBC fallback)");
    klog("Boot: GDT 3 descriptors (null/code/data, DPL=0); no TSS, no user-mode yet");
    klog("VGA text mode: 80x25 active");
    klog("APIC: local APIC disabled via MSR 0x1B, IRQ routing via legacy 8259 PIC");

    idt_init();
    isr_install();
    irq_install();
    pit_init();               /* 1000Hz ϵͳʱ�ӣ��˺���־ʱ���Ϊ��ʵ����ʱ��?*/
    asm volatile("sti");
    klog_ok("PIT: system timer 1000Hz (channel 0 rate generator)");
    klog_rtc_time("RTC: boot time 20");   /* ��ʵ����ʱ�䣨CMOS BCD, UTC+8�� */
    klog_ok("IDT: 256 gates installed");
    klog_ok("PIC: IRQ0-15 remapped to INT 0x20-0x2f, IRQ0/1/12 enabled");
    klog("ISR: exception stubs not yet installed (isr_install is a stub)");

    /* CPU����ʵ CPUID ̽�� */
    klog_cpuinfo();

    /* �ڴ�̽�⣺RTC CMOS ��ʵ���������鹹 */
    {
        uint16_t conv = cmos_read16(0x15);
        uint16_t ext  = cmos_read16(0x17);
        klogf("Memory: conventional ", conv, 0, "K, extended ", ext, 0, "K (CMOS 16-bit field)");
        klog_dec32("Memory: total ", (uint32_t)conv + ext, "K");
    }

    /* ATA ���̣���ʵ̽�������� LBA0 */
    uint8_t mbr[512];
    klog("ATA: PIO mode, probing 4 drives (0x1F0 primary / 0x170 secondary bus)");
    for (uint8_t d = 0; d < 4; d++) {
        if (!ata_drive_present(d)) {
            klog_dec32("ATA: drive ", d, " absent");
            continue;
        }
        if (ata_read_sector(d, 0, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA)
            klog_dec32("ATA: drive ", d, " present (MBR signature valid)");
        else
            klog_dec32("ATA: drive ", d, " present (no MBR signature)");
    }

    int ret = fs_init();
    if (ret == -2) {
        klog("FS: no filesystem on data drive, auto-formatting exFAT...");
        if (fs_format() == 0) {
            klog_ok("FS: formatted, creating README.TXT");
            const char *example = "Hello from EZOS exFAT!\n";
            fs_create_file("README.TXT", (const uint8_t*)example, my_strlen(example));
        } else {
            klog_fail("FS: auto-format failed");
        }
    } else if (ret == 0) {
        klog_prefix();
        terminal_writestring("FS: filesystem ready (");
        terminal_writestring(fs_type_name());
        terminal_writestring(") [ OK ]\n");
    } else {
        klog_fail("FS: no disk (all ATA drives absent)");
    }

    /* FS volume info from the live mount (fs.c unified layer) */
    {
        const fs_info_t *fi = fs_get_info();
        if (fi->type != FS_NONE) {
            klogf("FS: on ATA drive ", fi->drive, 0, ", volume LBA ", fi->part_start, 0, "");
            klogf("FS: volume ", fi->volume_sectors, 0, " sectors, ", fi->cluster_count, 0, " clusters");
            klogf("FS: ", fi->bytes_per_sector, 0, "B/sector, ", fi->sectors_per_cluster, 0, " sector(s)/cluster");
        }
    }

    keyboard_init();
    klog_ok("Keyboard: PS/2 keyboard initialized (8042 IRQ1 enabled)");
    mouse_init();
    if (mouse_present()) {
        klog_ok("PS/2 mouse: detected");
    } else {
        klog("PS/2 mouse: not detected, keyboard only");
    }

    /* VBE ͼ��ģʽ����ȡ boot.asm ʵģʽ̽������0x5000 �ṹ����
     * �û�̬ gw_start() ���������� LFB���˴���������ʵ̽��״̬�� */
    {
        uint32_t lfb  = *(volatile uint32_t*)0x5000;
        uint16_t vxr  = *(volatile uint16_t*)0x5004;
        uint16_t vyr  = *(volatile uint16_t*)0x5006;
        uint8_t  vbpp = *(volatile uint8_t*)0x5008;
        if (lfb >= 0x00100000u && lfb < 0xFFF00000u && vxr >= 320 && vyr >= 200 && vbpp == 16) {
            klog_hex32("VBE: boot probe OK, LFB 0x", lfb, " (16bpp RGB565)");
            klogf("VBE: resolution ", vxr, 0, "x", vyr, 0, ", activated at user-mode");
            klog("VBE: probed 0x11A/0x117/0x115/0x110 in order, first LFB match wins");
        } else {
            klog("VBE: no LFB mode probed, will fallback to VGA 0x13 320x200x256");
        }
    }
    /* ����������־��������banner ֱ�Ӹ��� verbose boot log ֮�� */
    //ascii art
	terminal_writestring("\n");
    terminal_writestring("  _____   ______  _____   _____\n");
    terminal_writestring(" |  ___| |___  / |  _  | /  ___|\n");
    terminal_writestring(" | |__      / /  | | | | \\ `--. \n");
    terminal_writestring(" |  __|    / /   | | | |  `--. \\\n");
    terminal_writestring(" | |___  ./ /___ | |_| | /\\__/ /\n");
    terminal_writestring(" \\____/  \\_____/ \\_____/ \\____/ \n");
    terminal_writestring("\n");
//	klog_ok("EZOS Kernel Shell - type 'help' for commands, 'exit' to continue boot.");

    /* ===== ��ѭ����shell -> exit -> ͼ������ -> �˳������ shell ===== */
    for (;;) {
        shell_run();

        /* exit ֱ�ӽ���ͼ�����棨���پ��� User Shell�� */
        terminal_writestring("\n");
        klog("entering graphical desktop - quit from start menu to return to shell");
        gw_start();
    }
}
