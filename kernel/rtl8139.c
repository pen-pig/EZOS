/*
 * rtl8139.c - Realtek RTL8139 网卡驱动（步骤 7：收发 + ARP/ICMP echo）
 *
 * 寄存器资料来源：Realtek RTL8139D datasheet + Linux rtl8139too 驱动位定义。
 *
 * 阶段覆盖：
 *   7.1  PCI 认领 → COMMAND 开 IO 解码 + BusMaster → CONFIG1 退出低功耗 →
 *       CR.RST 软复位（轮询自清，带超时）→ IDR0-5 读 MAC
 *   7.2  8KB RX 环（RBSTART + 软件读指针，CAPR 偏移 -0x10 约定）、
 *       4 个 TX 描述符轮转（TSD/TSAD，提交即返回）、IRQ 中断收包
 *       （IMR 只开 ROK，先清后处理防丢包竞态）、
 *       ARP request 应答、ICMP echo reply（IP/ICMP 校验和自算）。
 *
 * 内存模型：收发缓冲来自 kmalloc（.bss.hi 1-2MB，identity 映射，
 * 物理=线性，设备 DMA 直接用缓冲线性地址）；所有进程页目录共享内核
 * PDE（exec.c 只换 PDE1），故 IRQ 在任意 CR3 下访问缓冲都不会缺页。
 *
 * 并发模型：收包在 IRQ11 上下文（IF=0），send() 用 pushf/cli 保护
 * 描述符轮转——将来 ring0 任务态调用 send 与中断路径不会互踩。
 *
 * 安全边界：COMMAND 寄存器 32 位读-改-写保留 STATUS 原值；只对认领到的
 * 10EC:8139 写配置空间；RX 环长度字段做上界校验（防坏帧把读指针推飞），
 * 越界即复位环，fail closed。
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

/* TSD 位（Linux rtl8139too 同名位）：bit13 主机所有权、
 * bit14 欠载、bit15 发送成功。写低 13 位 = 帧长，即触发发送。 */
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

/* 每包头部 4 字节：低 16 位状态（bit0 ROK），高 16 位帧长（含 CRC，
 * 不含头部本身）——与 Linux rx_status>>16 的取法一致。 */
#define RX_HDR_LEN     4u

/* 每次 IRQ 最多处理的包数（防中断活锁；剩余包靠下一次中断） */
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
static uint8_t *g_tx_build;                   /* ARP/ICMP 回复构造缓冲 */
static uint32_t g_rx_pos;                     /* 下一个待读包的环内偏移（软件自持） */
static uint8_t  g_tx_used[TX_DESC_COUNT];     /* 描述符已发射过（首次免等待） */
static uint8_t  g_tx_next;

/* 统计 */
static uint32_t g_rx_packets, g_rx_errors;
static uint32_t g_tx_packets, g_tx_busy;
static uint32_t g_arp_rx, g_arp_replied;
static uint32_t g_icmp_rx, g_icmp_replied;
static uint32_t g_irq_count;

/* 首包诊断（nic 命令显示，排查环偏移约定用） */
static uint8_t  g_dbg_seen;
static uint32_t g_dbg_status, g_dbg_len, g_dbg_capr;

/* asm 桩（boot/kernel_entry.asm）：中断门入口，压寄存器后调 C handler */
extern void irq11(void);

/* 寄存器端口地址快捷方式（IO base + 偏移截断到 16 位） */
#define REG(off) ((uint16_t)(g_io_base + (off)))

/* ---------- 小工具 ---------- */

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int ip4_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Internet 校验和（RFC 1071）：16 位反码和，入参/返回均为网络字节序 */
static uint16_t cksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0;
    while (n >= 2) { s += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)(~s);
}

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

/* ---------- 协议处理（g_rx_frame 里是不含 CRC 的完整以太帧） ---------- */

static void arp_input(const uint8_t *f, uint32_t len) {
    g_arp_rx++;
    if (len < ETH_HDR_LEN + 28) return;
    const uint8_t *a = f + ETH_HDR_LEN;
    /* 只回 IPv4-over-Ethernet 的 request */
    if (be16(a + 0) != 0x0001 || be16(a + 2) != 0x0800) return;
    if (a[4] != 6 || a[5] != 4) return;
    if (be16(a + 6) != 1) return;
    if (!ip4_eq(a + 24, g_ip)) return;          /* 问的不是我们的 IP */

    uint8_t rmac[6], rip[4];                    /* 先取出再覆写缓冲 */
    for (int i = 0; i < 6; i++) rmac[i] = a[8 + i];
    for (int i = 0; i < 4; i++) rip[i]  = a[14 + i];

    uint8_t *r = g_tx_build;
    for (int i = 0; i < 6; i++) r[i] = rmac[i];     /* dst = 请求方 */
    for (int i = 0; i < 6; i++) r[6 + i] = g_mac[i];/* src = 本卡 */
    r[12] = 0x08; r[13] = 0x06;
    uint8_t *p = r + ETH_HDR_LEN;
    p[0] = 0; p[1] = 1;                         /* htype = Ethernet */
    p[2] = 0x08; p[3] = 0x00;                   /* ptype = IPv4 */
    p[4] = 6; p[5] = 4;                         /* hlen / plen */
    p[6] = 0; p[7] = 2;                         /* oper = reply */
    for (int i = 0; i < 6; i++) p[8 + i] = g_mac[i];
    for (int i = 0; i < 4; i++) p[14 + i] = g_ip[i];
    for (int i = 0; i < 6; i++) p[18 + i] = rmac[i];
    for (int i = 0; i < 4; i++) p[24 + i] = rip[i];
    if (rtl8139_send(r, ETH_HDR_LEN + 28) == 0) g_arp_replied++;
}

static void ip_input(const uint8_t *f, uint32_t len) {
    if (len < ETH_HDR_LEN + 20) return;
    const uint8_t *ip = f + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) return;                       /* 非 IPv4 */
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4u;
    if (ihl < 20 || ETH_HDR_LEN + ihl > len) return;
    uint32_t totlen = be16(ip + 2);
    if (totlen < ihl || totlen > 1500) return;
    if (ETH_HDR_LEN + totlen > len) return;              /* 截断帧丢弃 */
    if (ip[9] != 1) return;                              /* 只处理 ICMP */
    if (!ip4_eq(ip + 16, g_ip)) return;                  /* 非本机 IP */

    const uint8_t *ic = ip + ihl;
    uint32_t iclen = totlen - ihl;
    if (iclen < 8 || iclen > 1472) return;
    if (ic[0] != 8 || ic[1] != 0) return;                /* 只回 echo request */
    g_icmp_rx++;

    uint32_t flen = ETH_HDR_LEN + totlen;
    if (flen > TX_BUF_SIZE) return;

    uint8_t *r = g_tx_build;                 /* 与 g_rx_frame 不同块，无别名 */
    for (uint32_t i = 0; i < flen; i++) r[i] = f[i];
    for (int i = 0; i < 6; i++) { r[i] = f[6 + i]; r[6 + i] = g_mac[i]; }

    uint8_t *rip = r + ETH_HDR_LEN;
    rip[8] = 64;                             /* TTL */
    rip[10] = 0; rip[11] = 0;                /* 校验和先清零再算 */
    for (int i = 0; i < 4; i++) {
        rip[12 + i] = g_ip[i];               /* src = 本机 */
        rip[16 + i] = ip[12 + i];            /* dst = 请求方 */
    }
    uint16_t c = cksum(rip, ihl);
    rip[10] = (uint8_t)(c >> 8); rip[11] = (uint8_t)(c & 0xFF);

    uint8_t *ric = rip + ihl;
    ric[0] = 0;                              /* echo reply */
    ric[2] = 0; ric[3] = 0;
    c = cksum(ric, iclen);
    ric[2] = (uint8_t)(c >> 8); ric[3] = (uint8_t)(c & 0xFF);

    if (rtl8139_send(r, flen) == 0) g_icmp_replied++;
}

static void net_input(const uint8_t *f, uint32_t len) {
    if (len < ETH_HDR_LEN) return;
    uint16_t et = be16(f + 12);
    if (et == 0x0806)      arp_input(f, len);
    else if (et == 0x0800) ip_input(f, len);
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
            /* 环数据损坏（长度出界）：复位读指针，fail closed。
             * 真实原因通常是主机侧注入了畸形帧或硬件状态异常。 */
            g_rx_errors++;
            g_rx_pos = 0;
            outw(REG(R_CAPR), (uint16_t)(0u - 0x10u));   /* = 0xFFF0 */
            return;
        }

        uint32_t dlen = flen - 4;              /* 去 4 字节 CRC */
        rx_copy(g_rx_frame, g_rx_pos + RX_HDR_LEN, dlen);
        g_rx_packets++;
        net_input(g_rx_frame, dlen);

        /* 前进：帧占环 = 4 字节头 + flen，按 4 对齐后回绕 */
        g_rx_pos = (g_rx_pos + RX_HDR_LEN + flen + 3) & ~3u;
        g_rx_pos &= RX_RING_MASK;
        /* CAPR 硬件约定 = 已读结束位置 - 0x10（16 位自然回绕） */
        outw(REG(R_CAPR), (uint16_t)(g_rx_pos - 0x10));
    }
    /* guard 耗尽：剩余包等下一次 ROK 中断 */
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
    if (g_tx_build) { kfree(g_tx_build); g_tx_build = 0; }
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
    if ((bar0 & 1u) == 0) {            /* 不是 IO BAR，异常形态，fail closed */
        dmesg_write("RTL8139: BAR0 is not IO space, giving up");
        return -1;
    }
    uint32_t io = bar0 & ~0x3u;
    if (io == 0 || io > 0xFFFFu) {     /* IO 端口空间只有 64KB */
        dmesg_write("RTL8139: invalid IO base, giving up");
        return -1;
    }
    g_io_base = (uint16_t)io;
    g_irq = d->intr_line;

    /* 开 IO 空间解码 + Bus Master（收发 DMA 必需）。 */
    uint32_t cmd = pci_read_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND, cmd);

    /* 退出低功耗后软复位；RST 自清零，等它清完才能碰其它寄存器。 */
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

    /* ---- 7.2：收发缓冲（kmalloc，.bss.hi identity，DMA 直投） ---- */
    g_rx_ring  = kmalloc(RX_BUF_ALLOC);
    g_rx_frame = kmalloc(TX_BUF_SIZE);
    g_tx_build = kmalloc(TX_BUF_SIZE);
    int ok = (g_rx_ring && g_rx_frame && g_tx_build);
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

    /* 清残留中断状态 → 使能收发（注意 TE|RE = 0x0C，见文件头勘误） */
    outw(REG(R_ISR), 0x7FFF);
    outb(REG(R_CR), CR_TE | CR_RE);

    /* 中断路由：IDT 门 + PIC 掩码，最后才开 IMR（防半初始化状态进中断） */
    if (irq_route() != 0) {
        dmesg_write("RTL8139: unusual IRQ line, run without interrupts");
        /* 无中断时收包不可用，但 MAC/发送仍可工作；不视为致命 */
    }
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
uint32_t rtl8139_arp_replied(void)  { return g_arp_replied; }
uint32_t rtl8139_icmp_replied(void) { return g_icmp_replied; }
uint32_t rtl8139_irq_count(void)    { return g_irq_count; }

void rtl8139_rx_debug(uint32_t *status, uint32_t *len, uint32_t *capr) {
    if (status) *status = g_dbg_seen ? g_dbg_status : 0;
    if (len)    *len    = g_dbg_seen ? g_dbg_len : 0;
    if (capr)   *capr   = g_dbg_seen ? g_dbg_capr : 0;
}
