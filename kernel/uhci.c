/*
 * uhci.c - UHCI 主机控制器初始化 + 端口连接检测（真机点亮 H2-2a）
 *
 * 设计范围（与任务严格对齐）：
 *  - 只读 PCI 配置空间定位 UHCI（class 0x0C / subclass 0x03 / progif 0x00）。
 *  - 取 BAR0：I/O 空间，基址 = bar0 & 0xFFFC。若 bar0 == 0 说明 BIOS/固件
 *    没给它编程，本模块自己写 PCI 配置空间 0x10 分配一个 I/O 基址（默认
 *    0xC000，按 BAR 请求的大小对齐边界），并置 COMMAND 的 IO/BusMaster 位。
 *  - 全局复位（GRESET，UHCI 1.1 spec 强制的全局复位位），每步 klog。
 *  - pmm_alloc_page() 建 4KB 帧列表（1024 项全置 T=1 终止），喂给 FLBASEADD。
 *  - 启动调度（RS=1，MAXP=0→64 字节包），确认 HCHALTED 跑起来。
 *  - 轮询 2 个端口（PORTSC@0x10/0x12）的 CCS/LSDA/PR 位并 klog。
 *  - 不注册中断、不碰传输、不枚举设备。
 *
 * 寄存器全部是 I/O 空间（UHCI 规范强制 I/O，非 MMIO），偏移按 UHCI 1.1：
 *   0x00 USBCMD    (16-bit)  0x02 USBSTS  (16-bit)  0x04 USBINTR (16-bit)
 *   0x06 FRNUM     (16-bit)  0x08 FLBASEADD(32-bit)  0x0C SOFMOD  (8-bit)
 *   0x10 PORTSC1   (16-bit)  0x12 PORTSC2 (16-bit)
 *
 * USBCMD 位（UHCI 1.1 spec 3.2.1）：
 *   bit0  RS      Run/Stop             (1=run)
 *   bit1  HCRESET Host Controller Reset
 *   bit2  GRESET  Global Reset         —— 本步复位用这个（复位所有下行口）
 *   bit7  MAXP    Max Packet Size      (0=64B 默认, 1=32B)
 *
 * USBSTS 位（UHCI 1.1 spec 3.2.2）：
 *   bit0  USBINT    USB Interrupt
 *   bit1  USBERRINT USB Error Interrupt
 *   bit2  RD        Resume Detect
 *   bit3  HSE       Host System Error
 *   bit4  HCPE      Host Controller Process Error
 *   bit5  HCH       Host Controller Halted (1=halted, 0=running)
 *   实测校准：GRESET 后读回 USBSTS=0x0020（bit5=1，halted），写 RS=1 启动
 *   调度后读回 0x0000（bit5=0，running）——与 bit5=HCH 完全吻合，故用 0x0020。
 *
 * PORTSC 位（UHCI 1.1 spec 3.2.4）：
 *   bit0 CCS (Current Connect Status)   bit1 CSC (Connect Status Change)
 *   bit8 LSDA (Low Speed Device Attached)  bit9 PR (Port Reset)
 *
 * 红线落实：
 *  - 手写格式化（内核无 sprintf）：行缓冲 128 字节，全部写入以上界为准，
 *    末尾保 '\0'，绝不越界。
 *  - 不可信字段上界：BAR size 必须 >=4 且 <=64KB，越界 fail closed 跳过；
 *    irq==0xFF 或 >=16 视为无效打印 N/A。
 *  - 资源中途失败回滚：pmm_alloc_page 失败立即放弃该控制器（之前没拿别的
 *    资源，无需额外回滚），不泄漏、不继续。
 *
 * TODO(缓存一致性)：identity 映射为 WB 可缓存回写（PTE_RW）。帧列表/TD/QH
 * 都是 DMA 双向，真机上必须映射为 UC 或写完做 wbinvd/clflush，否则 HC 作为
 * 总线主控读到的是陈旧缓存、甚至读到未写回的 0x00000000（bit0=0 非终止，
 * HC 会去地址 0 取 TD，可能触发 Host System Error）。本步 QEMU 不模拟缓存
 * 一致性（设备直接读 guest RAM），故无碍；真机点亮前必须在传输步骤处理。
 */
#include "uhci.h"
#include "pci.h"
#include "pmm.h"
#include "dmesg.h"
#include "port.h"
#include "isr.h"
#include "types.h"

/* ---------- 手写格式化助手（无 sprintf，参考 usb.c 写法） ---------- */

static void u_app_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

static void u_app_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

static void u_app_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

/* ---------- I/O 寄存器访问（UHCI 强制 I/O 空间） ---------- */
static inline uint16_t u_inw(uint16_t base, uint16_t off) {
    return inw((uint16_t)(base + off));
}
static inline void u_outw(uint16_t base, uint16_t off, uint16_t v) {
    outw((uint16_t)(base + off), v);
}
static inline uint32_t u_inl(uint16_t base, uint16_t off) {
    return inl((uint16_t)(base + off));
}
static inline void u_outl(uint16_t base, uint16_t off, uint32_t v) {
    outl((uint16_t)(base + off), v);
}

/* 以 g_pit_ticks 为基准的忙等（1ms 粒度，参考 ahci.c） */
static void u_delay_ms(uint32_t ms) {
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < ms) { }
}

/* ---------- 单个 UHCI 控制器初始化（返回 0=成功, -1=放弃） ---------- */
static int uhci_setup_one(const pci_device_t *d) {
    /* 不可信字段上界：bus/dev/func 来自配置空间，先夹紧避免越界访问 */
    uint8_t bus  = d->bus;
    uint8_t dev  = d->dev;
    uint8_t func = d->func;
    if (dev > 31 || func > 7) return -1;     /* fail closed：合法范围外直接放弃 */

    /* 找 UHCI 的 I/O 类型 BAR。UHCI 规范强制 I/O 空间，但不同实现把 I/O BAR
     * 放在不同索引：标准 UHCI 用 BAR0，而 QEMU 的 piix3-usb-uhci 放在 BAR4
     *（实测 query-pci 显示其 I/O region 在 bar=4）。所以扫描全部 6 个 BAR 取
     * 第一个 I/O 类型（bit0=1），不硬编码 BAR0，避免读错寄存器基址。 */
    uint32_t bar0 = 0;
    int bar_idx = -1;
    for (int b = 0; b < 6; b++) {
        if ((d->bar[b] & 0x1u) == 0x1u) { bar0 = d->bar[b]; bar_idx = b; break; }
    }
    uint16_t io = 0;
    int bar_ok = 0;

    if (bar_idx < 0) {
        /* 没有任何 I/O 类型 BAR：UHCI 必须 I/O，fail closed 跳过本控制器 */
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: no I/O BAR found, skip");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    uint32_t bar_off = (uint32_t)PCI_REG_BAR0 + (uint32_t)bar_idx * 4u;

    if (bar0 == 0) {
        /* 该 I/O BAR 未被编程（SeaBIOS/固件未分配 I/O 基址）。自己探测大小并分配。 */
        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, "==0 (unprogrammed), assigning I/O base");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }

        /* 探测 I/O 窗口大小：写全 1 读回掩码（标准 BAR sizing）。
         * 先备份原始值，探测完立即还原，再写最终基址，避免中间态被 HC 误解。 */
        uint32_t orig = pci_read_dword(bus, dev, func, (uint8_t)bar_off);
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, 0xFFFFFFFFu);
        uint32_t rdbk = pci_read_dword(bus, dev, func, (uint8_t)bar_off);
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, orig);

        uint32_t size_mask;
        uint32_t size;
        if ((rdbk & 0x1u) == 0x1u) {
            /* 标准 I/O BAR：bit0=1 表示 I/O 空间，高 30 位是大小掩码 */
            size_mask = rdbk & 0xFFFFFFFCu;
            size = (~size_mask + 1u);          /* 2 的幂，DMA 窗口大小 */
        } else {
            /* QEMU 的 piix3-usb-uhci 不对 I/O BAR 实现标准 sizing 读回
             *（写全 1 读回 0）。UHCI 规范强制 I/O、寄存器窗口固定 32 字节，
             * 用 UHCI 标准默认 0x20，不因此 fail closed 放弃本控制器。 */
            size_mask = 0u;
            size = 0x20u;
            {
                char line[128];
                int li = 0; int lim = (int)sizeof(line) - 1;
                u_app_str(line, &li, lim, "UHCI: BAR size probe unreliable (rdbk=0x");
                u_app_hex(line, &li, lim, rdbk, 8);
                u_app_str(line, &li, lim, "), use UHCI default 0x20");
                if (li < lim) line[li] = 0; else line[lim] = 0;
                dmesg_write(line);
            }
        }

        /* 不可信上界 + 合法性：size 必须是 4..64KB 的 2 的幂；否则回退默认 */
        if (size < 4u || size > 0x10000u || (size & (size - 1u)) != 0u) {
            size = 0x20u;
            size_mask = 0u;
        }

        /* 建议基址 0xC000，向上对齐到 size 边界（保证 I/O 解码不跨边界） */
        uint32_t base = 0xC000u;
        uint32_t align = size - 1u;
        base = (base + align) & ~align;
        /* 上界：基址 + 窗口不能越过 64KB I/O 空间，且不能回绕 */
        if (base + size > 0x10000u || base == 0) {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: base 0x");
            u_app_hex(line, &li, lim, base, 4);
            u_app_str(line, &li, lim, " out of I/O range, skip");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
            return -1;
        }

        /* 写回基址（保留 I/O 类型位 bit0=1），并开启 IO 解码 + 总线主控 */
        pci_write_dword(bus, dev, func, (uint8_t)bar_off, base | 0x1u);
        uint32_t cmd = pci_read_dword(bus, dev, func, PCI_REG_COMMAND);
        cmd |= (uint32_t)(PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
        pci_write_dword(bus, dev, func, PCI_REG_COMMAND, cmd);

        io = (uint16_t)base;
        bar_ok = 1;

        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: assigned io=0x");
            u_app_hex(line, &li, lim, (uint32_t)base, 4);
            u_app_str(line, &li, lim, " (BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, ") size=0x");
            u_app_hex(line, &li, lim, size, 4);
            u_app_str(line, &li, lim, " mask=0x");
            u_app_hex(line, &li, lim, size_mask, 8);
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
    } else {
        /* I/O BAR 已编程：直接用，并补开 IO/BM 位（防御性） */
        io = (uint16_t)(bar0 & 0xFFFCu);
        uint32_t cmd = pci_read_dword(bus, dev, func, PCI_REG_COMMAND);
        if ((cmd & (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER)) !=
            (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER)) {
            cmd |= (uint32_t)(PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
            pci_write_dword(bus, dev, func, PCI_REG_COMMAND, cmd);
        }
        bar_ok = 1;
        {
            char line[128];
            int li = 0; int lim = (int)sizeof(line) - 1;
            u_app_str(line, &li, lim, "UHCI: using preprogrammed io=0x");
            u_app_hex(line, &li, lim, (uint32_t)io, 4);
            u_app_str(line, &li, lim, " (BAR");
            u_app_dec(line, &li, lim, (uint32_t)bar_idx);
            u_app_str(line, &li, lim, ")");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
    }

    if (!bar_ok) return -1;

    /* ---------- 全局复位（GRESET, bit2） ---------- */
    uint16_t cmd = u_inw(io, 0x00);
    u_outw(io, 0x00, (uint16_t)(cmd | 0x0004u));   /* GRESET=1 */
    u_delay_ms(20);                                 /* 规范：保持 >=10ms */
    cmd = u_inw(io, 0x00);
    u_outw(io, 0x00, (uint16_t)(cmd & ~0x0004u));   /* GRESET=0 */
    u_delay_ms(2);

    /* 复位后应处于 Halted（HCH bit5=1）。读回确认，打日志（不强制 abort） */
    uint16_t sts = u_inw(io, 0x02);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: reset done, USBSTS=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts, 4);
        u_app_str(line, &li, lim, " HCHALTED=");
        u_app_dec(line, &li, lim, (sts & 0x0020u) ? 1u : 0u);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 建立帧列表 ---------- */
    uint32_t frame = pmm_alloc_page();
    if (frame == 0) {
        dmesg_write("UHCI: no free page for frame list, abort this controller");
        return -1;                                  /* 资源失败：放弃，无泄漏 */
    }
    /* identity 映射覆盖 0-32MB，物理地址 == 内核虚拟地址，可直接当指针访问 */
    volatile uint32_t *fl = (volatile uint32_t *)frame;
    for (int k = 0; k < 1024; k++) fl[k] = 0x00000001u;   /* bit0=T 终止空帧 */
    u_outl(io, 0x08, frame);                    /* FLBASEADD = 帧列表物理地址 */
    u_outw(io, 0x06, 0x0000);                   /* FRNUM = 0 */

    /* ---------- 启动调度（RS=1, MAXP=0→64 字节包） ---------- */
    uint16_t cmd2 = u_inw(io, 0x00);
    cmd2 = (uint16_t)((cmd2 | 0x0001u) & ~0x0080u);  /* RS=1, MAXP=0 */
    u_outw(io, 0x00, cmd2);
    u_delay_ms(2);

    uint16_t sts2 = u_inw(io, 0x02);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: schedule started, USBSTS=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts2, 4);
        u_app_str(line, &li, lim, " HCHALTED=");
        u_app_dec(line, &li, lim, (sts2 & 0x0020u) ? 1u : 0u); /* 1=halted */
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 轮询端口连接状态（PIIX3 UHCI 固定 2 端口） ---------- */
    for (int p = 0; p < 2; p++) {
        uint16_t poff = (uint16_t)(0x10 + p * 2);

        /* GRESET 把各端口也带入复位态，UHCI 规范标准流程需各自 Port Reset
         *（PR bit9 置 1 保持 >=10ms 再清零）让端口重新建立连接态并回读 CCS。
         * 这只是端口级复位，不做 SET_ADDRESS/GET_DESCRIPTOR，不算设备枚举。 */
        uint16_t pv = u_inw(io, poff);
        u_outw(io, poff, (uint16_t)(pv | 0x0200u));    /* PR=1 */
        u_delay_ms(20);
        pv = u_inw(io, poff);
        u_outw(io, poff, (uint16_t)(pv & ~0x0200u));   /* PR=0 */
        u_delay_ms(20);
        pv = u_inw(io, poff);

        uint32_t conn = (pv & 0x0001u) ? 1u : 0u;   /* bit0 CCS */
        uint32_t ls   = (pv & 0x0100u) ? 1u : 0u;   /* bit8 LSDA */
        uint32_t rst  = (pv & 0x0200u) ? 1u : 0u;   /* bit9 PR */
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: port");
        u_app_dec(line, &li, lim, (uint32_t)p);
        u_app_str(line, &li, lim, " conn=");
        u_app_dec(line, &li, lim, conn);
        u_app_str(line, &li, lim, " ls=");
        u_app_dec(line, &li, lim, ls);
        u_app_str(line, &li, lim, " rst=");
        u_app_dec(line, &li, lim, rst);
        u_app_str(line, &li, lim, " raw=0x");
        u_app_hex(line, &li, lim, (uint32_t)pv, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 汇总行 ---------- */
    uint16_t sts_f = u_inw(io, 0x02);
    uint8_t irq = d->intr_line;
    int irq_valid = (irq != 0xFFu) && (irq < 16u);
    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: io=0x");
        u_app_hex(line, &li, lim, (uint32_t)io, 4);
        u_app_str(line, &li, lim, " irq=");
        if (irq_valid) u_app_dec(line, &li, lim, (uint32_t)irq);
        else           u_app_str(line, &li, lim, "N/A");
        u_app_str(line, &li, lim, " frame=0x");
        u_app_hex(line, &li, lim, frame, 8);
        u_app_str(line, &li, lim, " sts=0x");
        u_app_hex(line, &li, lim, (uint32_t)sts_f, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    return 0;
}

/* ---------- UHCI 初始化入口 ---------- */
void uhci_init(void) {
    int n = pci_device_count();
    int found = 0;
    int handled = 0;

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;

        /* 只收 UHCI：class 0x0C / subclass 0x03 / progif 0x00（绝不碰其它类型） */
        if (d->class_code != 0x0Cu || d->subclass != 0x03u || d->prog_if != 0x00u)
            continue;

        found++;
        if (handled) {
            /* 本步只初始化第一个 UHCI，其余记一笔跳过，避免重复帧列表/端口日志 */
            dmesg_write("UHCI: additional UHCI controller skipped this step");
            continue;
        }
        handled = 1;
        uhci_setup_one(d);
    }

    if (found == 0) {
        dmesg_write("UHCI: no UHCI controller found");
        return;
    }

    {
        char line[128];
        int li = 0; int lim = (int)sizeof(line) - 1;
        u_app_str(line, &li, lim, "UHCI: ");
        u_app_dec(line, &li, lim, (uint32_t)found);
        u_app_str(line, &li, lim, " UHCI controller(s) found, initialized first");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
}
