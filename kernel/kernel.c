#include "tty.h"
#include "keyboard.h"
#include "idt.h"
#include "isr.h"
#include "panic.h"
#include "paging.h"
#include "pmm.h"
#include "gdt.h"
#include "syscall.h"
#include "dmesg.h"
#include "kmalloc.h"
#include "types.h"
#include "ata.h"
#include "pci.h"
#include "shell.h"
#include "shell_extra.h"
#include "task.h"
#include "fpu.h"
#include "fs.h"
#include "mouse.h"
#include "port.h"
#include "serial.h"
#include "acpi.h"
#include "desktop.h"
#include "gfxwin.h"
#include "gfx.h"
#include "rtl8139.h"

// �򵥳��Ⱥ��������Զ���ʽ��ʾ��ʹ��
static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void klog(const char *msg);
static void dm_mirror(const char *body, const char *tail);

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

/* klog 镜像进 dmesg 环形缓冲：[ 秒.微秒] 正文 tail\n */
static void dm_mirror(const char *body, const char *tail) {
    char line[192];
    uint32_t o = 0;
    uint32_t us = pit_usec();
    /* 手写数字格式（无 sprintf）：[ N.NNNNNN] */
    char t[12]; int n = 0;
    uint32_t v = us / 1000000u;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    line[o++] = '['; line[o++] = ' ';
    while (n && o + 1 < sizeof(line)) line[o++] = t[--n];
    line[o++] = '.';
    for (int32_t d = 100000; d; d /= 10) {
        if (o + 1 >= (int32_t)sizeof(line)) break;
        line[o++] = (char)('0' + (us / d) % 10);
    }
    line[o++] = ']'; line[o++] = ' ';
    for (const char *p = body; *p && o + 1 < sizeof(line); p++) line[o++] = *p;
    for (const char *p = tail; *p && o + 1 < sizeof(line); p++) line[o++] = *p;
    if (o + 1 < sizeof(line)) line[o++] = '\n';
    line[o] = 0;
    dmesg_write(line);
}

static void klog(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring("\n");
    dm_mirror(msg, "");
}

static void klog_ok(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring(" [ OK ]\n");
    dm_mirror(msg, " [ OK ]");
}

static void klog_fail(const char *msg) {
    klog_prefix();
    terminal_writestring(msg);
    terminal_writestring(" [FAIL]\n");
    dm_mirror(msg, " [FAIL]");
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

    /* 串口最先就绪：之后的每条 klog/dmesg 都同步镜像 COM1（真机通道） */
    serial_init();
    klog(serial_ready() ? "SERIAL: COM1 115200 8N1 loopback OK"
                        : "SERIAL: COM1 not present - debug output disabled");

    /* ACPI：解析 RSDP/FADT/DSDT-_S5，shutdown 在真机上才真正断电 */
    acpi_init();
    klog(acpi_status_line());

    kmalloc_init();           /* 内核堆（384KB @ .bss.hi，先于一切使用者） */

    /* 分页：identity map 0-32MB + VBE LFB 后开 CR0.PG。
     * 必须在任何可能写 LFB / 触发 #PF 的驱动之前完成。 */
    if (paging_init() != 0) {
        terminal_writestring("PAGING: init failed - halting\n");
        for (;;) asm volatile("cli; hlt");
    }
    /* 物理页帧池：ELF 加载（步骤 5）与将来的进程都要从这里拿页。
     * 必须在 paging_init 之后——池本身要能正常访问。 */
    pmm_init();

    klog("EZOS Kernel 0.9.0 loaded at 0x10000, i686 protected mode");
    klog("Boot: 992 sectors kernel image read by BIOS INT 13h AH=42h (64-sector batches, 3 retries)");
    klog("Boot: A20 gate enabled (BIOS int 15h / port 0x92 / KBC fallback)");
    klog("Boot: GDT rebuilt in kernel - 6 descriptors (null/kcode/kdata/ucode DPL3/udata DPL3/TSS), TSS esp0=0x900000");
    klog("VGA text mode: 80x25 active");
    klog("APIC: local APIC disabled via MSR 0x1B, IRQ routing via legacy 8259 PIC");

    /* GDT 必须先于 IDT 之后的任何用户态机制建立：ring3 段与 TSS 都在这里 */
    gdt_init();

    idt_init();
    isr_register_stubs();     /* CPU 异常门 0-31（#GP/#PF 等触发蓝屏 panic） */
    isr_install();
    irq_install();
    syscall_init();           /* int 0x80 DPL=3 门：用户态唯一合法陷入入口 */
    pit_init();               /* 1000Hz ϵͳʱ�ӣ��˺���־ʱ���Ϊ��ʵ����ʱ��?*/
    asm volatile("sti");
    klog_ok("PIT: system timer 1000Hz (channel 0 rate generator)");
    klog_rtc_time("RTC: boot time 20");   /* ��ʵ����ʱ�䣨CMOS BCD, UTC+8�� */
    klog_ok("IDT: 256 gates installed");
    klog_ok("PIC: IRQ0-15 remapped to INT 0x20-0x2f, IRQ0/1/12 enabled");
    klog_ok("ISR: 32 CPU exception gates installed (panic screen on fault)");
    klogf("Paging: identity map 0-", PAGING_IDENTITY_END / (1024 * 1024), 0,
          "MB, 4KB pages, PD 0x", PAGING_PD_ADDR, 1, ", CR0.PG=1");
    klog_ok("SYSCALL: int 0x80 gate (DPL=3), SYS_READ/SYS_WRITE/SYS_EXIT");
    klogf("Kmalloc: ", 384, 0, "KB heap at .bss.hi, 16B align, magic guard", 0, 0, "");

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

    /* PCI 总线枚举（步骤 7 网络前置）：只扫描登记，不驱动任何设备。
     * 必须在任何设备驱动初始化之前——网卡/存储控制器都靠这张表认领设备。
     * 没有 PCI 总线的机器（或 QEMU 未挂 PCI 设备）返回 0，不是错误。 */
    {
        int npci = pci_scan();
        klog_dec32("PCI: scanned bus, ", (uint32_t)npci, " device(s) found");
    }

    /* RTL8139 NIC (step 7.2): claim + reset + MAC + 8K RX ring + 4 TX
     * descriptors + IRQ + ARP/ICMP echo reply. Returns -1 when QEMU has
     * no NIC attached; that is not an error, so we stay silent then. */
    if (rtl8139_init() == 0) {
        char l[96];
        int n = 0;
        const char *p = "RTL8139: MAC ";
        while (*p) l[n++] = *p++;
        rtl8139_mac_str(l + n);
        klog_ok(l);
        char l2[96];
        int n2 = 0;
        p = "RTL8139: RX 8K ring + 4 TX desc, IRQ 11, ARP/ICMP echo, IP ";
        while (*p) l2[n2++] = *p++;
        rtl8139_ip_str(l2 + n2);
        klog_ok(l2);
    }

    int ret = fs_init();
    if (ret == -2) {
        klog("FS: no filesystem on data drive, auto-formatting exFAT...");
        if (fs_format(FS_EXFAT) == 0) {
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

    /* 任务与抢占式调度器（步骤 6a）。
     * 必须在 pit_init + sti 之后：调度由 IRQ0 驱动，早于此时开调度会让
     * PIT 在 IDT 就绪前就尝试切换。当前执行流登记为 0 号任务（shell）。
     * 默认不额外创建任务——shell 仍是唯一可运行任务，行为与之前完全一致，
     * 抢占只在用 ktask 起了线程后才真正发生。 */
    task_init();
    klog_ok("TASK: preemptive scheduler ready (round-robin, 10ms slice)");

    /* x87 FPU：calc/浮点运算的前提。fninit 把控制字归一到 0x037F
     * （精度/舍入默认，异常全屏蔽）。无 FPU 只是禁用 calc，不是致命错。 */
    if (fpu_init() == 0) {
        klog_ok("FPU: x87 present, control word initialized (0x037F)");
    } else {
        klog("FPU: not present, floating point disabled (calc unavailable)");
    }

    /* 开机一键自检：所有子系统静默跑一遍断言，只报总结。
     * 放在 shell 之前——用户看到 banner 时就已经知道系统是否健全。
     * 失败不 panic：自检的意义是给出信号，不是拦住启动（单任务系统
     * 没有"拒绝调度到坏节点"这个选项）。 */
    {
        int n_fail = boot_selftest();
        if (n_fail == 0) {
            klog_ok("SELFTEST: all subsystem checks passed (boot-time)");
        } else {
            klog_fail("SELFTEST: boot-time self-test reported failures");
            klog_dec32("SELFTEST: failing subsystems: ", (uint32_t)n_fail, "");
            klog("SELFTEST: run 'selftest' in the shell for per-subsystem details");
        }
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
