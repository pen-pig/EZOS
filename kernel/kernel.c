#include "tty.h"
#include "keyboard.h"
#include "idt.h"
#include "isr.h"
#include "types.h"
#include "ata.h"
#include "shell.h"
#include "exfat.h"
#include "mouse.h"
#include "port.h"
#include "desktop.h"
#include "gfxwin.h"

// 简单长度函数，供自动格式化示例使用
static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void klog(const char *msg);

/* 带两个数字的日志行格式化（h1/h2 为 1 时十六进制输出，否则十进制） */
static void klogf(const char *s1, uint32_t v1, int h1,
                  const char *s2, uint32_t v2, int h2, const char *s3) {
    /* worst case 60+10+10+1=81: enlarge to 96 to avoid overflow */
    char line[96];
    int n = 0;
    while (*s1 && n < 60) line[n++] = *s1++;
    char t[16];
    int m = 0;
    if (v1 == 0) t[m++] = '0';
    while (v1) {
        if (h1) t[m++] = "0123456789ABCDEF"[v1 & 0xF];
        else    t[m++] = (char)('0' + v1 % 10);
        v1 = h1 ? (v1 >> 4) : (v1 / 10);
    }
    while (m) line[n++] = t[--m];
    while (*s2 && n < 60) line[n++] = *s2++;
    m = 0;
    if (v2 == 0) t[m++] = '0';
    while (v2) {
        if (h2) t[m++] = "0123456789ABCDEF"[v2 & 0xF];
        else    t[m++] = (char)('0' + v2 % 10);
        v2 = h2 ? (v2 >> 4) : (v2 / 10);
    }
    while (m) line[n++] = t[--m];
    while (*s3 && n < 60) line[n++] = *s3++;
    line[n] = '\0';
    klog(line);
}

/* 单个十六进制数的日志行 */
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

/* 通过 RTC CMOS 寄存器真实探测内存大小：
 * reg 0x15/0x16 常规内存 KB（低/高字节），reg 0x17/0x18 扩展内存 KB，
 * 扩展内存为 16 位字段，上限 65535K（约 64MB）。读时禁用 NMI。 */
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

/* 锁存读取 PIT channel 0 当前计数（分频 1193，倒计数 1193..0） */
static uint16_t pit_read_counter(void) {
    outb(0x43, 0x00);            /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | (hi << 8));
}

/* 真实微秒时钟（Linux dmesg 风格）：
 * 秒部分 = PIT 1000Hz tick；微秒部分 = PIT 自由运行计数（每计数 1/1193182s ≈ 0.838us） */
static uint32_t pit_usec(void) {
    uint32_t c = pit_read_counter();
    if (c > 1193) c = 1193;
    uint32_t elapsed = 1193 - c;           /* 当前 tick 内已走计数 */
    return g_pit_ticks * 1000u + (elapsed * 838u) / 1000u;
}

/* dmesg 风格时间戳：[    0.000000] （秒宽4右对齐 + 6 位真实微秒） */
static void klog_prefix(void) {
    uint32_t us = pit_usec();              /* 真实经过微秒 */
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

/* 十进制单值日志行 */
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

/* RTC CMOS 读取（NMI 禁用），BCD 解码。寄存器：0x00 秒 0x02 分 0x04 时
 * 0x07 日 0x08 月 0x09 年（后两位），与 gfxwin 任务栏时钟一致。 */
static uint8_t rtc_read(uint8_t reg) {
    outb(0x70, reg | 0x80);
    return inb(0x71);
}
static uint8_t rtc_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* 输出 RTC 真实日期时间（UTC+8，与 gfxwin 任务栏时钟一致），prefix 自带世纪 */
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
    while (*p) buf[n++] = *p++;
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

/* CPUID 探测（真实）：厂商字符串、最大叶子、特征位 */
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
        char t[8]; int m = 0;
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
    /* 禁用本地 APIC：EZOS 使用传统 8259 PIC 中断路由。
     * QEMU 默认 LAPIC enabled 且 LVT0(ExtINT) masked，会吞掉 PIC 的
     * 键盘/鼠标中断请求；关闭 LAPIC 后 LINT0 恢复为 INTR 引脚直通 PIC。 */
    {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
        lo &= ~(1u << 11);
        asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1B));
    }

    terminal_initialize();


    klog("EZOS Kernel 0.9.0 loaded at 0x10000, i686 protected mode");
    klog("Boot: 512 sectors kernel image read by BIOS INT 13h AH=42h (64-sector batches)");
    klog("Boot: A20 gate enabled via port 0x92");
    klog("Boot: GDT 3 descriptors (null/code/data, DPL=0); no TSS, no user-mode yet");
    klog("VGA text mode: 80x25 active");
    klog("APIC: local APIC disabled via MSR 0x1B, IRQ routing via legacy 8259 PIC");

    idt_init();
    isr_install();
    irq_install();
    pit_init();               /* 1000Hz 系统时钟：此后日志时间戳为真实经过时间 */
    asm volatile("sti");
    klog_ok("PIT: system timer 1000Hz (channel 0 rate generator)");
    klog_rtc_time("RTC: boot time 20");   /* 真实日期时间（CMOS BCD, UTC+8） */
    klog_ok("IDT: 256 gates installed");
    klog_ok("PIC: IRQ0-15 remapped to INT 0x20-0x2f, IRQ0/1/12 enabled");
    klog("ISR: exception stubs not yet installed (isr_install is a stub)");

    /* CPU：真实 CPUID 探测 */
    klog_cpuinfo();

    /* 内存探测：RTC CMOS 真实读数，非虚构 */
    {
        uint16_t conv = cmos_read16(0x15);
        uint16_t ext  = cmos_read16(0x17);
        klogf("Memory: conventional ", conv, 0, "K, extended ", ext, 0, "K (CMOS 16-bit field)");
        klog_dec32("Memory: total ", (uint32_t)conv + ext, "K");
    }

    /* ATA 磁盘：真实探测主从盘 LBA0 */
    uint8_t mbr[512];
    klog("ATA: PIO mode, ports 0x1F0-0x1F7/0x3F6, probing primary master/slave (LBA0 read)...");
    if (ata_read_sector(0, 0, mbr) == 0) {
        if (mbr[510] == 0x55 && mbr[511] == 0xAA)
            klog_ok("ATA: primary master detected (MBR signature valid)");
        else
            klog_ok("ATA: primary master detected (no MBR signature)");
    } else {
        klog_fail("ATA: primary master not present / read error");
    }
    if (ata_read_sector(1, 0, mbr) == 0) {
        if (mbr[510] == 0x55 && mbr[511] == 0xAA)
            klog_ok("ATA: primary slave detected (MBR signature valid)");
        else
            klog_ok("ATA: primary slave detected (no MBR signature)");
    } else {
        klog_fail("ATA: primary slave not present / read error");
    }

    int ret = exfat_init();
    if (ret == -2) {
        klog("exFAT: no filesystem on slave, auto-formatting...");
        if (exfat_format() == 0) {
            klog_ok("exFAT: formatted, creating README.TXT");
            const char *example = "Hello from EZOS exFAT!\n";
            exfat_create_file("README.TXT", (const uint8_t*)example, my_strlen(example));
        } else {
            klog_fail("exFAT: auto-format failed");
        }
    } else if (ret == 0) {
        klog_ok("exFAT: filesystem ready on slave (drive 1)");
    } else {
        klog_fail("exFAT: init error (ATA read / VBR parse failed)");
    }

    /* exFAT 卷参数（真实 VBR 值，带时间戳日志） */
    {
        const exfat_info_t *ei = exfat_get_info();
        klogf("exFAT: volume ", (uint32_t)ei->volume_length, 0, " sectors, ", ei->cluster_count, 0, " clusters");
        klogf("exFAT: ", ei->bytes_per_sector, 0, "B/sector, ", ei->sectors_per_cluster, 0, " sector(s)/cluster");
        klogf("exFAT: FAT@", ei->fat_offset, 0, ", heap@", ei->cluster_heap_offset, 0, "");
        klog_dec32("exFAT: root dir cluster ", ei->root_dir_cluster, "");
    }

    keyboard_init();
    klog_ok("Keyboard: PS/2 keyboard initialized (8042 IRQ1 enabled)");
    mouse_init();
    if (mouse_present()) {
        klog_ok("PS/2 mouse: detected");
    } else {
        klog("PS/2 mouse: not detected, keyboard only");
    }

    /* VBE 图形模式：读取 boot.asm 实模式探测结果（0x5000 结构）。
     * 用户态 gw_start() 才真正激活 LFB；此处仅报告真实探测状态。 */
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
    /* 保留启动日志不清屏：banner 直接跟在 verbose boot log 之后 */
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

    /* ===== 主循环：内核 shell -> exit -> User Shell -> desktop 图形桌面 -> 返回 User Shell -> exit 回内核 shell ===== */
    for (;;) {
        /* kernel-mode shell first (no gui/desktop/games in command table) */
        shell_run();

        /* exit -> user-mode: User Shell（非图形桌面，需输入 desktop 进入） */
        terminal_writestring("\n");
        klog("user-mode: entering User Shell (type 'desktop' to launch graphical desktop)");
        shell_set_user_mode(1);
        shell_run();

        /* User Shell exit (desktop 命令返回后 shell_run 继续，不走到这里) -> back to kernel shell */
        shell_set_user_mode(0);
        terminal_writestring("\n");
        klog("user shell exited, returning to kernel shell");
    }
}
