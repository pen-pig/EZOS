/*
 * ehci.c - EHCI（USB 2.0 高速）主机控制器驱动（真机点亮 A1 第一阶段）
 *
 * 为什么先做 EHCI（真机视角）：
 *   - UHCI 只到 12Mbps。U 盘、高速键鼠走 EHCI 才有 480Mbps。
 *   - 目标真机（荣耀）部分 USB 口后面根本没有 companion UHCI，
 *     只有把 EHCI 自己跑起来才能在这些口上用设备。
 *   - 真机上 FS/LS 设备会被路由回 companion，所以"键盘鼠标仍然好使"
 *     不依赖本模块；本模块负责的是**高速**那半边。
 *
 * 寄存器布局（EHCI 1.0 spec 第 2 章）
 *   能力寄存器（BAR 基址起）：
 *     0x00 CAPLENGTH(8)  0x02 HCIVERSION(16)  0x04 HCSPARAMS(32)
 *     0x08 HCCPARAMS(32) 0x0C HCSP-PORTROUTE(32)
 *   操作寄存器（基址 = BAR + CAPLENGTH）：
 *     0x00 USBCMD  0x04 USBSTS  0x08 USBINTR  0x0C FRINDEX
 *     0x10 CTRLDSSEGMENT  0x14 PERIODICLISTBASE  0x18 ASYNCLISTADDR
 *     0x40 CONFIGFLAG      0x44 + 4*n PORTSC[n]
 *
 * USBCMD：bit0 RS（运行） bit1 HCRESET bit3:2 帧列表大小 bit4 PSE
 *         bit5 ASE（异步调度使能） bit6 IAAD（异步推进门铃）
 * USBSTS：bit0 USBINT bit2 PCD bit4 HSE bit5 IAA bit12 HCHalted
 *         bit14 PSS bit15 ASS（异步调度状态）
 * PORTSC：bit0 CCS bit1 CSC(W1C) bit2 PE bit3 PEDC(W1C) bit4 OCA
 *         bit5 OCC(W1C) bit6 FPR bit7 SUSPEND bit8 PR
 *         bit11:10 LineStatus（0=SE0 高速空闲 1=J 全速 2=K 低速）
 *         bit12 PP（端口供电） bit13 PO（归属 companion）
 *
 * 异步调度模型（本步采用"停-挂-跑"三段式，确定性优先）：
 *   常驻 head QH（H=1）自环；每笔传输时把传输 QH 插进环里，
 *   跑完再摘下。插/摘都在 ASE=0（异步调度已停）时做，绕开 HC 对 QH
 *   覆盖区（overlay）的缓存竞态——比 Linux 的 IAAD 门铃慢一点，但
 *   教学 OS 里"绝不出现偶发错包"比带宽重要得多。
 *
 * 红线落实：
 *   - 手写格式化（内核无 sprintf）：行缓冲 128 字节，末尾保 '\0'。
 *   - 不可信字段上界：HCSPARAMS 的 N_PORTS 夹到 [0,15]；EECP 偏移夹到
 *     [0x40,0xFF]；BAR 只收 32 位 MMIO（64 位 BAR 在 32 位无 PAE 下
 *     映射不到 4GB 以上，fail closed 并打明确日志）。
 *   - 每一步忙等都有毫秒上界（g_pit_ticks 基准），超时 fail closed。
 *   - 中途失败回滚：映射失败/复位失败立即放弃本控制器，不留下半初始化状态。
 */
#include "ehci.h"
#include "pci.h"
#include "paging.h"
#include "dmesg.h"
#include "isr.h"
#include "types.h"

/* ---------- 能力寄存器偏移（相对 BAR 基址） ---------- */
#define EH_CAPLENGTH   0x00u
#define EH_HCIVERSION  0x02u
#define EH_HCSPARAMS   0x04u
#define EH_HCCPARAMS   0x08u

/* ---------- 操作寄存器偏移（相对 op base = BAR + CAPLENGTH） ---------- */
#define EH_USBCMD      0x00u
#define EH_USBSTS      0x04u
#define EH_USBINTR     0x08u
#define EH_FRINDEX     0x0Cu
#define EH_CTRLDSSEG   0x10u
#define EH_PERIODIC    0x14u
#define EH_ASYNC       0x18u
#define EH_CONFIGFLAG  0x40u
#define EH_PORTSC      0x44u

/* USBCMD 位 */
#define CMD_RS        0x00000001u   /* Run/Stop */
#define CMD_HCRESET   0x00000002u   /* Host Controller Reset */
#define CMD_PSE       0x00000010u   /* Periodic Schedule Enable */
#define CMD_ASE       0x00000020u   /* Asynchronous Schedule Enable */

/* USBSTS 位 */
#define STS_ASS       0x00008000u   /* bit15 异步调度状态 */
#define STS_PSS       0x00004000u   /* bit14 周期调度状态 */
#define STS_HCH       0x00001000u   /* bit12 HCHalted */
#define STS_HSE       0x00000010u   /* bit4 Host System Error */
#define STS_PCD       0x00000004u   /* bit2 Port Change Detect */

/* PORTSC 位 */
#define PS_CCS        0x00000001u   /* bit0  Current Connect Status */
#define PS_CSC        0x00000002u   /* bit1  Connect Status Change (W1C) */
#define PS_PE         0x00000004u   /* bit2  Port Enabled */
#define PS_PEDC       0x00000008u   /* bit3  Port Enable/Disable Change (W1C) */
#define PS_OCC        0x00000020u   /* bit5  Over-current Change (W1C) */
#define PS_PR         0x00000100u   /* bit8  Port Reset */
#define PS_LS         0x00000C00u   /* bit11:10 Line Status */
#define PS_PP         0x00001000u   /* bit12 Port Power */
#define PS_PO         0x00002000u   /* bit13 Port Owner（1=归 companion） */
/* 写 1 清零的位：写 PORTSC 时必须从读回值里剔除，否则会把状态变化清掉 */
#define PS_RWC        (PS_CSC | PS_PEDC | PS_OCC)

/* qTD token 位（EHCI 3.5.3） */
#define QTD_ACTIVE    0x00000080u   /* bit7  */
#define QTD_HALT      0x00000040u   /* bit6  */
#define QTD_DBE       0x00000020u   /* bit5  数据缓冲错误 */
#define QTD_BABBLE    0x00000010u   /* bit4  */
#define QTD_XACT      0x00000008u   /* bit3  事务错误（CRC/超时/STALL...） */
#define QTD_MMF       0x00000004u   /* bit2  漏掉微帧 */
#define QTD_PING      0x00000001u   /* bit0  */
#define QTD_FATAL     (QTD_HALT | QTD_DBE | QTD_BABBLE | QTD_XACT)
#define QTD_PID_OUT   0x00000000u   /* bit9:8 = 00 */
#define QTD_PID_IN    0x00000100u   /* bit9:8 = 01 */
#define QTD_PID_SETUP 0x00000200u   /* bit9:8 = 10 */
#define QTD_CERR3     0x00000C00u   /* bit11:10 = 3 次重试 */
#define QTD_IOC       0x00008000u   /* bit15 完成时中断（我们轮询，但置上无害） */
#define QTD_DT        0x80000000u   /* bit31 DATA1 */

/* QH info1 位（EHCI 3.6.2） */
#define QH_EPS_HIGH   0x00002000u   /* bit13:12 = 10b 高速 */
#define QH_H          0x00008000u   /* bit15 回收链表头 */
#define QH_MPL(m)     ((uint32_t)(m) << 16)   /* bit26:16 最大包长 */
#define QH_C          0x08000000u   /* bit27 控制端点 */
#define QH_MULT1      0x40000000u   /* bit31:30 = 1（每微帧 1 笔事务） */

/* 链表指针：bit0 = T（终止），bit4:1 = 类型（01b = QH -> 数值 0x2） */
#define LINK_QH(p)    (((uint32_t)(unsigned long)(p)) | 0x2u)
#define LINK_T        0x1u

/* 控制传输缓冲上界（与 UHCI 侧一致，配置描述符需要 >64） */
#define EHCI_XFER_MAX 256
/* 各阶段超时（毫秒，g_pit_ticks 基准） */
#define EHCI_TO_RESET   200u
#define EHCI_TO_SCHED   100u
#define EHCI_TO_XFER    1000u

/* ---------- 手写格式化助手（无 sprintf，与 uhci.c 同风格） ---------- */
static void e_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

static void e_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

static void e_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

/* ---------- 控制器表（静态，无动态分配） ---------- */
typedef struct {
    uint32_t cap;      /* 能力寄存器虚拟基址（MMIO，已映射） */
    uint32_t op;       /* 操作寄存器虚拟基址 = cap + CAPLENGTH */
    int      nports;   /* 根口数（HCSPARAMS[3:0]，已夹紧） */
    int      valid;    /* 该条目已成功初始化 */
} ehci_ctl_t;

static ehci_ctl_t g_ctl[EHCI_MAX_CTL];
static int        g_ctl_n = 0;

/* 已登记的高速端口表（供上层枚举消费，与 uhci.c 的端口表同形） */
typedef struct {
    int ctl;     /* 控制器索引 */
    int port;    /* 根口编号 */
} ehci_port_t;
static ehci_port_t g_plist[EHCI_MAX_PORTS];
static int         g_plist_n = 0;

/* ---------- MMIO 访问 ---------- */
static inline uint32_t e_mmio_rd(int ctl, uint32_t off) {
    return *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].op + off);
}
static inline void e_mmio_wr(int ctl, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].op + off) = v;
}
static inline uint32_t e_cap_rd(int ctl, uint32_t off) {
    return *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + off);
}
static inline uint32_t e_port_rd(int ctl, int port) {
    return e_mmio_rd(ctl, EH_PORTSC + (uint32_t)port * 4u);
}
/* 写 PORTSC：统一剔除 W1C 位，避免把状态变化误清 */
static inline void e_port_wr(int ctl, int port, uint32_t v) {
    e_mmio_wr(ctl, EH_PORTSC + (uint32_t)port * 4u, v & ~PS_RWC);
}

static void e_delay_ms(uint32_t ms) {
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < ms) { }
}

/* =========================================================================
 * 异步调度结构（.bss，32 字节对齐——EHCI 的 QH/qTD 链接指针低 5 位是
 * 类型/T 标志，故必须 32 字节对齐；identity 映射下虚拟地址即物理地址，
 * HC 作为总线主控可直接 DMA 读这块内存。）
 * ========================================================================= */
static volatile uint32_t g_head[12]  __attribute__((aligned(32)));  /* 常驻 head QH */
static volatile uint32_t g_xqh[12]   __attribute__((aligned(32)));  /* 传输 QH */
static volatile uint32_t g_qtd[3][8] __attribute__((aligned(32)));  /* SETUP/DATA/STATUS */
static volatile uint8_t  g_setup[8]  __attribute__((aligned(32)));
static volatile uint8_t  g_rbuf[256] __attribute__((aligned(32)));

/* 周期帧列表（本步为空，全部 T；PSE 开着也不产生流量，为第二阶段中断
 * 传输预留）。1024 项 × 4B = 4KB，必须 4KB 对齐。 */
static volatile uint32_t g_flist[1024] __attribute__((aligned(4096)));

#define EH_PHYS(p)  ((uint32_t)(unsigned long)(p))

/* ---------- 异步调度停/起（确定性优先，见文件头） ---------- */
static int async_stop(int ctl) {
    e_mmio_wr(ctl, EH_USBCMD, e_mmio_rd(ctl, EH_USBCMD) & ~CMD_ASE);
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < EHCI_TO_SCHED) {
        if ((e_mmio_rd(ctl, EH_USBSTS) & STS_ASS) == 0u) return 0;
    }
    dmesg_write("EHCI: async schedule did not stop (timeout)");
    return -1;
}

static int async_start(int ctl) {
    e_mmio_wr(ctl, EH_USBCMD, e_mmio_rd(ctl, EH_USBCMD) | CMD_ASE | CMD_RS);
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < EHCI_TO_SCHED) {
        if ((e_mmio_rd(ctl, EH_USBSTS) & STS_ASS) != 0u) return 0;
    }
    dmesg_write("EHCI: async schedule did not start (timeout)");
    return -1;
}

/* ---------- 通用控制传输 ---------- */
int ehci_control_xfer(int ctl, uint8_t addr, uint8_t ep,
                      const uint8_t *setup, int dir_in,
                      uint8_t *buf, int blen, int *actlen) {
    if (actlen) *actlen = 0;
    /* 上界用**静态容量**而非 g_ctl_n：控制器索引在 ehci_setup_one 内部就
     * 要用于端口复位（g_ctl_n 要等 setup 成功之后才自增），用 g_ctl_n 判定
     * 会把初始化期的复位调用全部 fail closed 掉。真正的"能不能用"由
     * valid 标志把关。 */
    if (ctl < 0 || ctl >= EHCI_MAX_CTL || !g_ctl[ctl].valid) return -1;
    if (addr > 127 || ep > 15) return -1;                  /* 不可信上界 */
    if (blen < 0 || blen > EHCI_XFER_MAX) return -1;       /* 缓冲上界 */
    if (blen > 0 && !buf) return -1;

    /* SETUP 包与数据缓冲：IN 先清零避免读到陈旧值，OUT 拷入待发数据 */
    for (int i = 0; i < 8; i++) g_setup[i] = setup[i];
    for (int i = 0; i < blen; i++) g_rbuf[i] = dir_in ? 0 : buf[i];

    uint32_t qtd0 = EH_PHYS(&g_qtd[0]);
    uint32_t qtd1 = EH_PHYS(&g_qtd[1]);
    uint32_t qtd2 = EH_PHYS(&g_qtd[2]);

    /* 清零三个 qTD（含 HC 回写区），避免上一次传输的残留标志 */
    for (int t = 0; t < 3; t++)
        for (int k = 0; k < 8; k++) g_qtd[t][k] = 0;

    /* qTD0 SETUP：PID=SETUP，DATA0（bit31=0），8 字节 */
    g_qtd[0][0] = (blen > 0) ? qtd1 : qtd2;                /* next */
    g_qtd[0][1] = LINK_T;                                  /* alt next */
    g_qtd[0][3] = EH_PHYS(&g_setup[0]);
    g_qtd[0][2] = QTD_PID_SETUP | (8u << 16) | QTD_CERR3 | QTD_ACTIVE;

    /* qTD1 DATA（仅 blen>0 时构建）：DATA1 */
    if (blen > 0) {
        g_qtd[1][0] = qtd2;
        g_qtd[1][1] = LINK_T;
        g_qtd[1][3] = EH_PHYS(&g_rbuf[0]);
        g_qtd[1][2] = (dir_in ? QTD_PID_IN : QTD_PID_OUT)
                    | ((uint32_t)blen << 16)
                    | QTD_DT | QTD_CERR3 | QTD_ACTIVE;
    }

    /* qTD2 STATUS：方向取数据阶段反，零长度，DATA1，完成时中断 */
    g_qtd[2][0] = LINK_T;
    g_qtd[2][1] = LINK_T;
    g_qtd[2][3] = 0;
    g_qtd[2][2] = (dir_in ? QTD_PID_OUT : QTD_PID_IN)
                | (0u << 16)
                | QTD_DT | QTD_CERR3 | QTD_ACTIVE | QTD_IOC;

    /* 传输 QH：控制端点、高速、mps=64（USB2 规范对高速端点 0 的规定）。
     * 不置 QH_DTC：控制端点的 DATA toggle 由 HC 自己按 USB 规则推进
     *（SETUP=DATA0，其后 DATA1）——软件插手反而容易与设备失步。 */
    for (int k = 0; k < 12; k++) g_xqh[k] = 0;
    g_xqh[0] = LINK_QH(&g_head[0]);                        /* 水平链回 head */
    g_xqh[1] = (uint32_t)addr
             | ((uint32_t)ep << 8)
             | QH_EPS_HIGH
             | QH_MPL(64)
             | QH_C;
    g_xqh[2] = QH_MULT1;                                   /* 异步：S/C-mask=0 */
    g_xqh[3] = 0;                                          /* current qTD */
    g_xqh[4] = qtd0;                                       /* next qTD */
    g_xqh[5] = LINK_T;                                     /* alt next */
    g_xqh[6] = 0;                                          /* token（覆盖区） */

    /* 停 -> 挂 QH -> 跑 */
    if (async_stop(ctl) != 0) return -1;
    g_head[0] = LINK_QH(&g_xqh[0]);
    if (async_start(ctl) != 0) {
        g_head[0] = LINK_QH(&g_head[0]);
        return -1;
    }

    /* 轮询完成：任一 qTD 报致命位即提前收手（fail closed） */
    uint32_t t0 = g_pit_ticks;
    int done = 0;
    while ((uint32_t)(g_pit_ticks - t0) < EHCI_TO_XFER) {
        uint32_t s0 = g_qtd[0][2], s1 = g_qtd[1][2], s2 = g_qtd[2][2];
        if (((s0 & QTD_ACTIVE) == 0u) &&
            ((s1 & QTD_ACTIVE) == 0u) &&
            ((s2 & QTD_ACTIVE) == 0u)) { done = 1; break; }
        if ((s0 & QTD_FATAL) || (s1 & QTD_FATAL) || (s2 & QTD_FATAL)) break;
        if (e_mmio_rd(ctl, EH_USBSTS) & (STS_HCH | STS_HSE)) break;
    }

    /* 摘下 QH（同样在 ASE=0 时做），恢复 head 自环 */
    async_stop(ctl);
    g_head[0] = LINK_QH(&g_head[0]);
    async_start(ctl);

    if (!done) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI-CTRL: xfer timeout addr=");
        e_dec(line, &li, lim, (uint32_t)addr);
        e_str(line, &li, lim, " ep=");
        e_dec(line, &li, lim, (uint32_t)ep);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    uint32_t s0 = g_qtd[0][2], s1 = g_qtd[1][2], s2 = g_qtd[2][2];
    if ((s0 & QTD_FATAL) || (s1 & QTD_FATAL) || (s2 & QTD_FATAL)) {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI-CTRL: xfer error TD0=0x");
        e_hex(line, &li, lim, s0, 8);
        e_str(line, &li, lim, " TD1=0x");
        e_hex(line, &li, lim, s1, 8);
        e_str(line, &li, lim, " TD2=0x");
        e_hex(line, &li, lim, s2, 8);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }

    /* 实际长度 = 请求长度 - token 里"还剩多少字节"（bit30:16） */
    int al = 0;
    if (blen > 0) {
        uint32_t remain = (s1 >> 16) & 0x7FFFu;
        al = (int)((uint32_t)blen - remain);
        if (al < 0) al = 0;
        if (al > blen) al = blen;
        if (dir_in && buf) {
            for (int i = 0; i < al; i++) buf[i] = (uint8_t)g_rbuf[i];
        }
    }
    if (actlen) *actlen = al;
    return 0;
}

/* ---------- 端口复位 + 高速判定 ---------- */
int ehci_port_reset(int ctl, int port) {
    if (ctl < 0 || ctl >= EHCI_MAX_CTL || !g_ctl[ctl].valid) return 0;
    if (port < 0 || port >= g_ctl[ctl].nports) return 0;

    uint32_t sc = e_port_rd(ctl, port);
    if ((sc & PS_CCS) == 0u) return 0;                  /* 空口：什么都不做 */

    /* 置 PR（bit8）并保持 >=50ms（USB2 规范要求根口复位 >=50ms）。
     * 保留 PP（供电）等其它位；写回时剔除 W1C 位由 e_port_wr 统一处理。 */
    e_port_wr(ctl, port, (sc & ~PS_RWC) | PS_PR | PS_PP);
    e_delay_ms(60);
    sc = e_port_rd(ctl, port);
    e_port_wr(ctl, port, (sc & ~PS_RWC & ~PS_PR) | PS_PP);   /* 清 PR */
    /* 等 HC 完成握手：高速设备在复位期间完成 chirp，HC 会置 PE。 */
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < EHCI_TO_RESET) {
        sc = e_port_rd(ctl, port);
        if ((sc & PS_PR) == 0u && ((sc & PS_PE) != 0u)) break;
        if ((sc & PS_CCS) == 0u) break;                  /* 拔走了 */
        e_delay_ms(5);
    }
    e_delay_ms(20);                                      /* 复位后设备稳定期 */
    sc = e_port_rd(ctl, port);

    uint32_t conn = (sc & PS_CCS) ? 1u : 0u;
    uint32_t en   = (sc & PS_PE)  ? 1u : 0u;
    uint32_t ls   = (sc & PS_LS) >> 10;
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: port");
        e_dec(line, &li, lim, (uint32_t)port);
        e_str(line, &li, lim, " post-reset conn=");
        e_dec(line, &li, lim, conn);
        e_str(line, &li, lim, " en=");
        e_dec(line, &li, lim, en);
        e_str(line, &li, lim, " ls=");
        e_dec(line, &li, lim, ls);
        e_str(line, &li, lim, " raw=0x");
        e_hex(line, &li, lim, sc, 8);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    if (!conn) return 0;
    if (!en) {
        /* 复位后端口没被使能 —— 按 EHCI 规范这意味着挂的是全速/低速设备，
         * 必须交还 companion（写 PO=1）。真机上 companion UHCI 随后会枚举它，
         * 键盘鼠标因此不受影响；本控制器不再看这个口。 */
        e_port_wr(ctl, port, (sc & ~PS_RWC) | PS_PO | PS_PP);
        {
            char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
            e_str(line, &li, lim, "EHCI: port");
            e_dec(line, &li, lim, (uint32_t)port);
            e_str(line, &li, lim, " not high-speed (ls=");
            e_dec(line, &li, lim, ls);
            e_str(line, &li, lim, "), released to companion");
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
        return 0;
    }
    return 1;   /* 高速设备，本控制器拥有 */
}

/* ---------- BIOS/OS handoff（真机必需） ---------- *
 * EHCI 的扩展能力链表起点在 HCCPARAMS[15:8]（EECP）。偏移处若为
 * capability id 1（USBLEGSUP），其 bit16 = BIOS Owned Semaphore：
 * 固件/SMM 占着控制器时该位为 1，此时 EHCI 的寄存器被 BIOS 接管，
 * 端口永远不工作。做法：写 bit24（OS Owned Semaphore）=1，等 BIOS 清
 * bit16（规范给固件 1ms，耐心给 200ms），再关掉 SMI 使能位。 */
static void ehci_bios_handoff(int ctl) {
    uint32_t hcc = e_cap_rd(ctl, EH_HCCPARAMS);
    uint32_t eecp = (hcc >> 8) & 0xFFu;
    if (eecp < 0x40u) return;          /* 0 = 没有扩展能力；<0x40 不是合法 PCI 配置空间偏移 */

    uint32_t sem = *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp);
    uint32_t capid = sem & 0xFFu;
    if (capid != 0x01u) return;        /* 不是 USBLEGSUP */

    if ((sem & (1u << 16)) == 0u) {    /* BIOS 没占着，无需交接 */
        /* 顺手关掉 SMI（bit31:16 里的 SMI 使能位），避免真机 SMM 打断 */
        *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp) =
            sem & 0xFFFF0000u & ~(0xFFFFu);
        return;
    }

    /* 请求 OS 所有权 */
    *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp) = sem | (1u << 24);
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < 200u) {
        uint32_t v = *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp);
        if ((v & (1u << 16)) == 0u) break;
        e_delay_ms(5);
    }
    uint32_t after = *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp);
    /* 关掉全部 SMI 使能（低 16 位），保留高 16 位的信号量状态 */
    *(volatile uint32_t *)(unsigned long)(g_ctl[ctl].cap + eecp) = after & 0xFFFF0000u;
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: bios handoff eecp=0x");
        e_hex(line, &li, lim, eecp, 2);
        e_str(line, &li, lim, " bios-owned-now=");
        e_dec(line, &li, lim, (after & (1u << 16)) ? 1u : 0u);
        e_str(line, &li, lim, " os-owned=");
        e_dec(line, &li, lim, (after & (1u << 24)) ? 1u : 0u);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
}

/* ---------- 单控制器初始化 ---------- */
static int ehci_setup_one(const pci_device_t *d, int idx) {
    /* 找 32 位 MMIO BAR（bit0=0 表示内存；bit2:1=00b 表示 32 位）。
     * 64 位 BAR（bit2:1=10b）在 32 位无 PAE 下够不到 4GB 以上，
     * fail closed 并明确打日志，绝不静默跳过。 */
    uint32_t bar = 0;
    int bar_idx = -1;
    for (int b = 0; b < 6; b++) {
        uint32_t v = d->bar[b];
        if (v == 0u) continue;
        if ((v & 0x1u) != 0u) continue;               /* I/O 空间，不是 EHCI 的 */
        if ((v & 0x6u) != 0x0u) {
            dmesg_write("EHCI: 64-bit MMIO BAR unsupported (32-bit paging)");
            return -1;
        }
        bar = v & 0xFFFFFFF0u;
        bar_idx = b;
        break;
    }
    if (bar_idx < 0) {
        dmesg_write("EHCI: no memory BAR found, skip");
        return -1;
    }

    /* 开 MEM 解码 + 总线主控（EHCI 的寄存器与 DMA 都靠这两个位） */
    uint32_t cmd = pci_read_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND);
    cmd |= (uint32_t)(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    pci_write_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND, cmd);

    /* 映射 MMIO：BAR 常在 32MB identity 之外，必须显式映射。
     * 按 PAGING_MMIO_FLAGS（PCD/PWT）关缓存——MMIO 寄存器不能用 WB 回写，
     * 否则真机上写操作可能滞留在 cache 里不落到控制器。
     * 端口寄存器最坏 0x44+4*15=0x80，加能力区不超过 1KB；BAR 未页对齐时
     * 多映射一页，保证窗口完整落在映射内。 */
    uint32_t page = bar & ~0xFFFu;
    uint32_t npages = ((bar & 0xFFFu) == 0u) ? 1u : 2u;
    for (uint32_t i = 0; i < npages; i++) {
        if (paging_map(page + i * 0x1000u, page + i * 0x1000u,
                       PAGING_MMIO_FLAGS) != 0) {
            dmesg_write("EHCI: paging_map failed, skip controller");
            return -1;
        }
    }

    g_ctl[idx].cap = bar;
    uint32_t caplen = *(volatile uint8_t *)(unsigned long)(bar + EH_CAPLENGTH);
    if (caplen < 0x10u || caplen > 0x40u) caplen = 0x10u;   /* 不可信上界 */
    g_ctl[idx].op = bar + caplen;

    uint32_t hcs = e_cap_rd(idx, EH_HCSPARAMS);
    uint32_t hcc = e_cap_rd(idx, EH_HCCPARAMS);
    int nports = (int)(hcs & 0x0Fu);
    if (nports > EHCI_MAX_PORTS) nports = EHCI_MAX_PORTS;
    g_ctl[idx].nports = nports;

    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: bar=0x");
        e_hex(line, &li, lim, bar, 8);
        e_str(line, &li, lim, " (BAR");
        e_dec(line, &li, lim, (uint32_t)bar_idx);
        e_str(line, &li, lim, ") irq=");
        if (d->intr_line != 0xFFu && d->intr_line < 16u)
            e_dec(line, &li, lim, (uint32_t)d->intr_line);
        else
            e_str(line, &li, lim, "N/A");
        e_str(line, &li, lim, " caplen=0x");
        e_hex(line, &li, lim, caplen, 2);
        e_str(line, &li, lim, " nports=");
        e_dec(line, &li, lim, (uint32_t)nports);
        e_str(line, &li, lim, " hcc=0x");
        e_hex(line, &li, lim, hcc, 8);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    ehci_bios_handoff(idx);

    /* ---------- 控制器复位（HCRESET） ----------
     * 复位会把 USBCMD/USBSTS/端口状态全部带回上电值；后续必须重新
     * 配置帧列表/异步环/CONFIGFLAG，否则"调度不跑 + 端口不使能"。 */
    e_mmio_wr(idx, EH_USBCMD, CMD_HCRESET);
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < EHCI_TO_RESET) {
        if ((e_mmio_rd(idx, EH_USBCMD) & CMD_HCRESET) == 0u) break;
        e_delay_ms(2);
    }
    {
        uint32_t sts = e_mmio_rd(idx, EH_USBSTS);
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: reset done, USBSTS=0x");
        e_hex(line, &li, lim, sts, 8);
        e_str(line, &li, lim, " HCHALTED=");
        e_dec(line, &li, lim, (sts & STS_HCH) ? 1u : 0u);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* ---------- 建立调度骨架 ---------- */
    e_mmio_wr(idx, EH_USBINTR, 0u);                    /* 不用中断，纯轮询 */
    e_mmio_wr(idx, EH_CTRLDSSEG, 0u);                  /* 32 位地址，无高段 */

    /* 周期帧列表：全 T（本步无周期流量），为第二阶段中断传输预留 */
    for (int k = 0; k < 1024; k++) g_flist[k] = LINK_T;
    e_mmio_wr(idx, EH_PERIODIC, EH_PHYS(&g_flist[0]));

    /* 异步环头：H=1（回收链表头），自环，token 置 HALT 防 HC 执行陈旧 qTD */
    for (int k = 0; k < 12; k++) g_head[k] = 0;
    g_head[0] = LINK_QH(&g_head[0]);                   /* 水平链指向自己 */
    g_head[1] = QH_H;
    g_head[2] = QH_MULT1;
    g_head[4] = LINK_T;                                /* next qTD = T */
    g_head[5] = LINK_T;                                /* alt next = T */
    g_head[6] = QTD_HALT;                              /* 覆盖区：halted */
    e_mmio_wr(idx, EH_ASYNC, EH_PHYS(&g_head[0]));

    /* CONFIGFLAG=1：把根口路由到 EHCI 自己（为 0 时端口归 companion，
     * 表现就是"PORTSC 永远读不到连接"）。必须在 RS=1 之前写。 */
    e_mmio_wr(idx, EH_CONFIGFLAG, 0x1u);

    /* 起调度：RS=1 + PSE=1（周期列表空，跑着无害）+ ASE=1 */
    e_mmio_wr(idx, EH_USBCMD, CMD_RS | CMD_PSE | CMD_ASE);
    e_delay_ms(5);
    {
        uint32_t sts = e_mmio_rd(idx, EH_USBSTS);
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: schedule started, USBSTS=0x");
        e_hex(line, &li, lim, sts, 8);
        e_str(line, &li, lim, " ASS=");
        e_dec(line, &li, lim, (sts & STS_ASS) ? 1u : 0u);
        e_str(line, &li, lim, " PSS=");
        e_dec(line, &li, lim, (sts & STS_PSS) ? 1u : 0u);
        e_str(line, &li, lim, " async=0x");
        e_hex(line, &li, lim, EH_PHYS(&g_head[0]), 8);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    /* 标记有效必须**在端口路由之前**：ehci_port_reset 会校验 valid，
     * 否则复位调用全部被 fail closed 拦掉（表现为"设备插着却一个口都不复位"）。 */
    g_ctl[idx].valid = 1;

    /* ---------- 端口供电（HCSPARAMS bit4 = PPC） ---------- */
    if (hcs & (1u << 4)) {
        for (int p = 0; p < nports; p++)
            e_port_wr(idx, p, PS_PP);
        e_delay_ms(20);
    }

    /* ---------- 端口路由 ---------- */
    for (int p = 0; p < nports; p++) {
        uint32_t sc = e_port_rd(idx, p);
        /* 每个口都打一行原始值：真机上"设备插着但读不到连接"的头号线索
         * 就在这里（供电位 PP、归属位 PO、连接位 CCS 一眼可判）。 */
        {
            char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
            e_str(line, &li, lim, "EHCI: scan port");
            e_dec(line, &li, lim, (uint32_t)p);
            e_str(line, &li, lim, " raw=0x");
            e_hex(line, &li, lim, sc, 8);
            e_str(line, &li, lim, " hcs=0x");
            e_hex(line, &li, lim, hcs, 8);
            if (li < lim) line[li] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
        if ((sc & PS_CCS) == 0u) continue;              /* 空口，跳过 */
        if (sc & PS_PO) continue;                       /* 已归 companion */
        int hs = ehci_port_reset(idx, p);
        if (hs && g_plist_n < EHCI_MAX_PORTS) {
            g_plist[g_plist_n].ctl = idx;
            g_plist[g_plist_n].port = p;
            g_plist_n++;
        }
    }

    g_ctl[idx].valid = 1;
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: controller ready, high-speed port(s)=");
        e_dec(line, &li, lim, (uint32_t)g_plist_n);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
    return 0;
}

/* ---------- 最小控制传输探针（证据，非枚举全链路） ---------- */
static void ehci_control_probe(void) {
    if (g_plist_n <= 0) {
        dmesg_write("EHCI: 0 high-speed device(s) attached");
        return;
    }
    int ctl = g_plist[0].ctl;
    int port = g_plist[0].port;

    /* 1) GET_DESCRIPTOR(Device, len18) @ addr0 */
    uint8_t req[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};
    uint8_t dbuf[64];
    int act = 0;
    int r = ehci_control_xfer(ctl, 0, 0, req, 1, dbuf, 18, &act);
    if (r == 0 && act >= 8) {
        uint16_t bcd = (uint16_t)(dbuf[2] | (dbuf[3] << 8));
        uint16_t vid = (uint16_t)(dbuf[8] | (dbuf[9] << 8));
        uint16_t pid = (uint16_t)(dbuf[10] | (dbuf[11] << 8));
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI-CTRL: GET_DESCRIPTOR dev @0 ok len=");
        e_dec(line, &li, lim, (uint32_t)act);
        e_str(line, &li, lim, " port=");
        e_dec(line, &li, lim, (uint32_t)port);
        e_str(line, &li, lim, " bcdUSB=0x");
        e_hex(line, &li, lim, (uint32_t)bcd, 4);
        e_str(line, &li, lim, " VID=0x");
        e_hex(line, &li, lim, (uint32_t)vid, 4);
        e_str(line, &li, lim, " PID=0x");
        e_hex(line, &li, lim, (uint32_t)pid, 4);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    } else {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI-CTRL: GET_DESCRIPTOR @0 failed act=");
        e_dec(line, &li, lim, (uint32_t)act);
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
        return;
    }

    /* 2) SET_ADDRESS(1)（无数据阶段；状态阶段为 IN） */
    uint8_t sa[8] = {0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    int rs = ehci_control_xfer(ctl, 0, 0, sa, 0, NULL, 0, NULL);
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI-CTRL: SET_ADDRESS(1) ");
        e_str(line, &li, lim, (rs == 0) ? "ok" : "fail");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
    e_delay_ms(10);      /* SET_ADDRESS 后 >=2ms 才能用新地址 */

    /* 3) GET_DESCRIPTOR @ addr1（确认新地址生效） */
    uint8_t req2[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};
    uint8_t dbuf2[64];
    int act2 = 0;
    int r2 = ehci_control_xfer(ctl, 1, 0, req2, 1, dbuf2, 18, &act2);
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        if (r2 == 0 && act2 >= 8) {
            uint16_t vid = (uint16_t)(dbuf2[8] | (dbuf2[9] << 8));
            uint16_t pid = (uint16_t)(dbuf2[10] | (dbuf2[11] << 8));
            e_str(line, &li, lim, "EHCI-CTRL: GET_DESCRIPTOR @1 ok VID=0x");
            e_hex(line, &li, lim, (uint32_t)vid, 4);
            e_str(line, &li, lim, " PID=0x");
            e_hex(line, &li, lim, (uint32_t)pid, 4);
            e_str(line, &li, lim, " len=");
            e_dec(line, &li, lim, (uint32_t)act2);
        } else {
            e_str(line, &li, lim, "EHCI-CTRL: GET_DESCRIPTOR @1 failed act=");
            e_dec(line, &li, lim, (uint32_t)act2);
        }
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
}

/* ---------- 入口 ---------- */
void ehci_init(void) {
    int n = pci_device_count();
    int found = 0;
    int handled = 0;

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        /* 只收 EHCI：class 0x0C / subclass 0x03 / prog_if 0x20 */
        if (d->class_code != 0x0Cu || d->subclass != 0x03u || d->prog_if != 0x20u)
            continue;
        found++;
        if (handled) {
            dmesg_write("EHCI: additional EHCI controller skipped this step");
            continue;
        }
        if (g_ctl_n >= EHCI_MAX_CTL) continue;
        handled = 1;
        if (ehci_setup_one(d, g_ctl_n) == 0) g_ctl_n++;
    }

    if (found == 0) {
        dmesg_write("EHCI: no EHCI controller found");
        return;
    }
    {
        char line[128]; int li = 0; int lim = (int)sizeof(line) - 1;
        e_str(line, &li, lim, "EHCI: ");
        e_dec(line, &li, lim, (uint32_t)found);
        e_str(line, &li, lim, " EHCI controller(s) found, initialized first");
        if (li < lim) line[li] = 0; else line[lim] = 0;
        dmesg_write(line);
    }

    e_delay_ms(100);      /* 端口复位后设备稳定期（枚举前必须等） */
    ehci_control_probe();
}

int ehci_port_count(void) {
    return g_plist_n;
}

int ehci_port_get(int i, int *ctl_out, int *port_out) {
    if (i < 0 || i >= g_plist_n) return -1;
    if (ctl_out)  *ctl_out  = g_plist[i].ctl;
    if (port_out) *port_out = g_plist[i].port;
    return 0;
}
