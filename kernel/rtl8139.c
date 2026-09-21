/*
 * rtl8139.c - Realtek RTL8139 网卡驱动（步骤 7：帧收发层）
 *
 * 寄存器资料来源：Realtek RTL8139D datasheet + Linux rtl8139too 驱动位定义。
 *
 * 职责边界（7.3 起）：本文件只做"帧进帧出"——
 *   PCI 认领 → 复位 → 8KB RX 环（软件读指针）→ 4 TX 描述符轮转 →
 *   IRQ11 中断收包 + rtl8139_poll() 轮询收包。
 *   收到完整以太帧后上调 net_input()（kernel/net.c）做协议处理；
 *   发送 rtl8139_send() 只管把帧塞进描述符。
 *
 * 两种收包驱动方式：
 *   - IRQ11（系统调用之外、ring3 运行时）：异步排水
 *   - rtl8139_poll()（系统调用内，IF=0 中断被屏蔽）：net.c 的阻塞
 *     等待循环主动调用，保证 syscall 期间网络仍在收
 *   两条路径互斥（单核 + 中断门清 IF），共用 rx_drain 无需加锁。
 *
 * 勘误（对 7.1 注释）：CR 的 TE=bit2(0x04)、RE=bit3(0x08)，
 * 不是旧注释写的 bit1/bit2——按旧注释写 0x06 只会置保留位+缺 RE，
 * 收包永远不使能。使能收发的正确写法是 CR=0x0C。
 */
#include "rtl8139.h"
#include "pci.h"
#include "port.h"
#include "kmalloc.h"
#include "idt.h"
#include "dmesg.h"
#include "net.h"

#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

/* ---------- 寄存器偏移（相对 IO base） ---------- */
#define R_IDR0     0x00    /* MAC 地址字节 0-5 */
#define R_TSD0     0x10    /* TX 状态/长度，描述符 0-3 各占 4 字节 */
#define R_TSAD0    0x20    /* TX 缓冲地址，描述符 0-3 各占 4 字节 */
#define R_RBSTART  0x30    /* RX 环基址（32 位物理） */
#define R_CR       0x37    /* 命令寄存器 */
#define R_CAPR     0x38    /* RX 读指针（16 位，硬件约定 = 已读位置 - 0x10） */
#define R_IMR      0x3C    /* 中断屏蔽（16 位） */
#define R_ISR      0x3E    /* 中断状态（16 位，写 1 清零） */
#define R_RCR      0x44    /* RX 配置（32 位） */
#define R_CONFIG1  0x52    /* 配置 1：写 0 退出低功耗 */

/* CR 位（datasheet：bit2 TE，bit3 RE，bit4 RST） */
#define CR_TE      0x04
#define CR_RE      0x08
#define CR_RST     0x10

/* ISR/IMR 位 */
#define ISR_ROK    0x0001  /* 收包 OK */

/* RCR 位：APM=物理匹配（单播到本卡），AB=广播（ARP request 必需）。
 * RBLEN 位保持 0 = 8KB 环，与 RX_RING_SIZE 一致。 */
#define RCR_APM    0x02
#define RCR_AB     0x08

/* TSD 位（Linux rtl8139too 同名位）：bit14 欠载、bit15 发送成功。
 * 写低 13 位 = 帧长，即触发发送。 */
#define TSD_TOK    0x8000u
#define TSD_TUN    0x4000u

/* 软复位的轮询上限（真机约 1ms；QEMU 即刻自清） */
#define RST_POLL_MAX 100000

/* ---------- RX 环参数 ---------- */
#define RX_RING_SIZE   8192u          /* RCR.RBLEN=0 → 8KB 环 */
#define RX_RING_MASK   (RX_RING_SIZE - 1u)
#define RX_BUF_ALLOC   (RX_RING_SIZE + 16u)  /* datasheet 惯例：+16 字节尾巴 */
#define RX_LEN_MIN     32u            /* 合法帧长（含 CRC）下界 */
#define RX_LEN_MAX     1518u          /* 合法帧长（含 CRC）上界 */
#define RX_HDR_LEN     4u             /* 每包头部：状态 16 + 帧长 16（含 CRC） */

/* 每次 IRQ 最多处理的包数（防中断活锁；剩余包靠下一次中断/轮询） */
#define RX_DRAIN_MAX   64

/* ---------- TX 参数 ---------- */
#define TX_DESC_COUNT  4
#define TX_BUF_SIZE    1792u          /* 1514 最大以太帧 + 余量 */
#define TX_WAIT_MAX    1000000u       /* 描述符忙自旋上限 */

/* 以太网帧长界限（不含 CRC） */
#define ETH_HDR_LEN    14u
#define ETH_FRAME_MAX  1514u

/* ---------- 驱动状态 ---------- */
static uint8_t  g_present = 0;
static uint16_t g_io_base = 0;
static uint8_t  g_irq     = 0;
static uint8_t  g_mac[6];

/* IP 静态配置：QEMU user-net guest 缺省 10.0.2.15，宿主 10.0.2.2 */
static uint8_t  g_ip[4] = {10, 0, 2, 15};

static uint8_t *g_rx_ring;                    /* 8208B，kmalloc */
static uint8_t *g_rx_frame;                   /* 收包整帧拷出缓冲（环回安全） */
static uint8_t *g_tx_buf[TX_DESC_COUNT];      /* 4 × 1792B */
static uint32_t g_rx_pos;                     /* 下一个待读包的环内偏移（软件自持） */
static uint8_t  g_tx_used[TX_DESC_COUNT];     /* 描述符已发射过（首次免等待） */
static uint8_t  g_tx_next;

/* 统计（协议层计数在 net.c） */
static uint32_t g_rx_packets, g_rx_errors;
static uint32_t g_tx_packets, g_tx_busy;
static uint32_t g_irq_count;

/* 首包诊断（nic 命令显示，排查环偏移约定用） */
static uint8_t  g_dbg_seen;
static uint32_t g_dbg_status, g_dbg_len, g_dbg_capr;

/* asm 桩（boot/kernel_entry.asm）：中断门入口，压寄存器后调 C handler */
extern void irq11(void);

/* 寄存器端口地址快捷方式（IO base + 偏移截断到 16 位） */
#define REG(off) ((uint16_t)(g_io_base + (off)))

/* ---------- 小工具 ---------- */

/* 环形读：任意偏移按 8KB 取模（帧跨环尾自动回绕） */
static uint8_t rx_r8(uint32_t off) {
    return g_rx_ring[off & RX_RING_MASK];
}
static uint16_t rx_r16(uint32_t off) {
    return (uint16_t)((uint16_t)rx_r8(off) | ((uint16_t)rx_r8(off + 1) << 8));
}
static void rx_copy(uint8_t *dst, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = rx_r8(off + i);
}

/* ---------- RX 环处理 ---------- */

static void rx_drain(void) {
    for (int guard = 0; guard < RX_DRAIN_MAX; guard++) {
        uint16_t status = rx_r16(g_rx_pos);
        uint16_t flen   = rx_r16(g_rx_pos + 2);
        if (flen == 0) return;                 /* 读空：环内无待处理包 */

        if (!g_dbg_seen) {
            g_dbg_seen = 1;
            g_dbg_status = status;
            g_dbg_len = flen;
            g_dbg_capr = inw(REG(R_CAPR));
        }

        if (flen < RX_LEN_MIN || flen > RX_LEN_MAX) {
            /* 环数据损坏（长度出界）：复位读指针，fail closed */
            g_rx_errors++;
            g_rx_pos = 0;
            outw(REG(R_CAPR), (uint16_t)(0u - 0x10u));   /* = 0xFFF0 */
            return;
        }

        uint32_t dlen = flen - 4;              /* 去 4 字节 CRC */
        rx_copy(g_rx_frame, g_rx_pos + RX_HDR_LEN, dlen);
        g_rx_packets++;
        net_input(g_rx_frame, dlen);           /* 协议处理（kernel/net.c） */

        /* 前进：帧占环 = 4 字节头 + flen，按 4 对齐后回绕 */
        g_rx_pos = (g_rx_pos + RX_HDR_LEN + flen + 3) & ~3u;
        g_rx_pos &= RX_RING_MASK;
        /* CAPR 硬件约定 = 已读结束位置 - 0x10（16 位自然回绕） */
        outw(REG(R_CAPR), (uint16_t)(g_rx_pos - 0x10));
    }
    /* 步骤 8a：本批包已入协议栈，踢醒睡在 net 等待队列上的
     * recvfrom/accept/ARP 解析（IRQ 与轮询两种上下文都安全）。 */
    net_rx_wake();
    /* guard 耗尽：剩余包等下一次中断/轮询 */
}

/* ---------- IRQ11 处理 ---------- */

void irq11_handler(void) {
    g_irq_count++;
    uint16_t isr = inw(REG(R_ISR));
    if (isr & ISR_ROK) {
        /* 先清后处理：处理后又有新包会重新置位 ROK → 再触发一次中断，
         * 不会丢包。若先处理再清，处理期间到达的包会被写 1 清零吞掉。 */
        outw(REG(R_ISR), ISR_ROK);
        rx_drain();
    } else {
        outw(REG(R_ISR), isr);                 /* 其它事件（错误/溢出）只清零 */
    }
    if (g_irq >= 8) outb(0xA0, 0x20);          /* 从 PIC EOI */
    outb(0x20, 0x20);                          /* 主 PIC EOI（级联始终要） */
}

/* 轮询收包：系统调用上下文（IF=0）里由 net.c 的阻塞循环调用 */
void rtl8139_poll(void) {
    if (!g_present) return;
    uint16_t isr = inw(REG(R_ISR));
    if (isr & ISR_ROK) {
        outw(REG(R_ISR), ISR_ROK);
        rx_drain();
    }
}

/* ---------- 初始化 ---------- */

static const pci_device_t *find_device(void) {
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (d->vendor_id == RTL8139_VENDOR && d->device_id == RTL8139_DEVICE)
            return d;
    }
    return 0;
}

static void bufs_free(void) {
    if (g_rx_ring)  { kfree(g_rx_ring);  g_rx_ring = 0; }
    if (g_rx_frame) { kfree(g_rx_frame); g_rx_frame = 0; }
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        if (g_tx_buf[i]) { kfree(g_tx_buf[i]); g_tx_buf[i] = 0; }
    }
}

/* 按 PCI 中断线注册中断门 + 放开 PIC 掩码（QEMU rtl8139 恒为 IRQ11） */
static int irq_route(void) {
    if (g_irq >= 16 || g_irq == 2) return -1;  /* 2 被级联占用 */
    uint8_t vector = (g_irq < 8) ? (uint8_t)(32 + g_irq)
                                 : (uint8_t)(40 + g_irq - 8);
    idt_set_gate(vector, (uint32_t)irq11, 0x08, 0x8E);
    if (g_irq < 8) {
        uint8_t m = inb(0x21);
        m &= (uint8_t)~(1u << g_irq);
        outb(0x21, m);
    } else {
        uint8_t m = inb(0xA1);
        m &= (uint8_t)~(1u << (g_irq - 8));
        outb(0xA1, m);
    }
    return 0;
}

int rtl8139_init(void) {
    const pci_device_t *d = find_device();
    if (!d) return -1;

    /* BAR0 必须是 IO 空间（bit0=1）；低 2 位是标志位，清掉得到基址。
     * QEMU 实测 BAR0=0xC001 → IO base 0xC000。 */
    uint32_t bar0 = d->bar[0];
    if ((bar0 & 1u) == 0) {
        dmesg_write("RTL8139: BAR0 is not IO space, giving up");
        return -1;
    }
    uint32_t io = bar0 & ~0x3u;
    if (io == 0 || io > 0xFFFFu) {
        dmesg_write("RTL8139: invalid IO base, giving up");
        return -1;
    }
    g_io_base = (uint16_t)io;
    g_irq = d->intr_line;

    /* 开 IO 空间解码 + Bus Master（收发 DMA 必需） */
    uint32_t cmd = pci_read_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND, cmd);

    /* 退出低功耗后软复位；RST 自清零，等它清完才能碰其它寄存器 */
    outb(REG(R_CONFIG1), 0x00);
    outb(REG(R_CR), CR_RST);
    {
        int timeout = RST_POLL_MAX;
        while ((inb(REG(R_CR)) & CR_RST) != 0) {
            if (--timeout <= 0) {
                dmesg_write("RTL8139: reset timeout (CR.RST never cleared)");
                return -1;
            }
        }
    }

    for (int i = 0; i < 6; i++)
        g_mac[i] = inb(REG(R_IDR0 + i));

    /* ---- 收发缓冲（kmalloc，.bss.hi identity，DMA 直投） ---- */
    g_rx_ring  = kmalloc(RX_BUF_ALLOC);
    g_rx_frame = kmalloc(TX_BUF_SIZE);
    int ok = (g_rx_ring && g_rx_frame);
    for (int i = 0; ok && i < TX_DESC_COUNT; i++) {
        g_tx_buf[i] = kmalloc(TX_BUF_SIZE);
        if (!g_tx_buf[i]) ok = 0;
    }
    if (!ok) {
        dmesg_write("RTL8139: kmalloc failed for ring buffers");
        bufs_free();
        return -1;
    }

    g_rx_pos = 0;
    for (int i = 0; i < TX_DESC_COUNT; i++) g_tx_used[i] = 0;
    g_tx_next = 0;

    /* RX 环 + 过滤规则：物理匹配 + 广播（ARP request 是广播） */
    outl(REG(R_RBSTART), (uint32_t)g_rx_ring);
    outl(REG(R_RCR), RCR_APM | RCR_AB);

    /* 清残留中断状态 → 使能收发（TE|RE = 0x0C，见文件头勘误） */
    outw(REG(R_ISR), 0x7FFF);
    outb(REG(R_CR), CR_TE | CR_RE);

    /* 中断路由：IDT 门 + PIC 掩码，最后才开 IMR（防半初始化状态进中断） */
    if (irq_route() != 0)
        dmesg_write("RTL8139: unusual IRQ line, run without interrupts");
    outw(REG(R_IMR), ISR_ROK);

    g_present = 1;
    return 0;
}

/* ---------- 对外 API ---------- */

int rtl8139_send(const uint8_t *frame, uint32_t len) {
    if (!g_present || len < ETH_HDR_LEN || len > ETH_FRAME_MAX) return -1;

    /* IF 保护：防 ring0 任务态与 IRQ 收包路径并发争抢描述符轮转 */
    uint32_t efl;
    asm volatile("pushfl; popl %0" : "=r"(efl));
    asm volatile("cli");
    int rc = -1;

    int i = g_tx_next;
    uint32_t t = TX_WAIT_MAX;
    while (g_tx_used[i] &&
           !(inl(REG(R_TSD0 + 4 * i)) & (TSD_TOK | TSD_TUN))) {
        if (t-- == 0) { g_tx_busy++; goto out; }   /* 4 描述符全忙 */
    }

    uint8_t *buf = g_tx_buf[i];
    for (uint32_t k = 0; k < len; k++) buf[k] = frame[k];
    outl(REG(R_TSAD0 + 4 * i), (uint32_t)buf);
    outl(REG(R_TSD0 + 4 * i), len);       /* 写长度即触发发送 */
    g_tx_used[i] = 1;
    g_tx_next = (uint8_t)((i + 1) & (TX_DESC_COUNT - 1));
    g_tx_packets++;
    rc = 0;

out:
    if (efl & 0x200u) asm volatile("sti");
    return rc;
}

int      rtl8139_present(void) { return g_present; }
uint16_t rtl8139_io_base(void) { return g_io_base; }
uint8_t  rtl8139_irq(void)     { return g_irq; }
const uint8_t *rtl8139_mac(void) { return g_mac; }

void rtl8139_mac_str(char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[g_mac[i] >> 4];
        out[i * 3 + 1] = hex[g_mac[i] & 0xF];
        out[i * 3 + 2] = (i == 5) ? '\0' : ':';
    }
}

const uint8_t *rtl8139_ip(void) { return g_ip; }

void rtl8139_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_ip[0] = a; g_ip[1] = b; g_ip[2] = c; g_ip[3] = d;
}

void rtl8139_ip_str(char *out) {
    for (int i = 0; i < 4; i++) {
        uint8_t v = g_ip[i];
        if (v >= 100) {
            *out++ = (char)('0' + v / 100);
            *out++ = (char)('0' + (v / 10) % 10);
            *out++ = (char)('0' + v % 10);
        } else if (v >= 10) {
            *out++ = (char)('0' + v / 10);
            *out++ = (char)('0' + v % 10);
        } else {
            *out++ = (char)('0' + v);
        }
        if (i < 3) *out++ = '.';
    }
    *out = '\0';
}

uint32_t rtl8139_rx_packets(void)   { return g_rx_packets; }
uint32_t rtl8139_rx_errors(void)    { return g_rx_errors; }
uint32_t rtl8139_tx_packets(void)   { return g_tx_packets; }
uint32_t rtl8139_tx_busy(void)      { return g_tx_busy; }
uint32_t rtl8139_irq_count(void)    { return g_irq_count; }

void rtl8139_rx_debug(uint32_t *status, uint32_t *len, uint32_t *capr) {
    if (status) *status = g_dbg_seen ? g_dbg_status : 0;
    if (len)    *len    = g_dbg_seen ? g_dbg_len : 0;
    if (capr)   *capr   = g_dbg_seen ? g_dbg_capr : 0;
}
