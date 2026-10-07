/*
 * nvme.c - NVMe 1.x 块设备驱动（真机点亮 A2）
 *
 * 关键阶段全部经 dmesg_write 打点（真机串口诊断通道，见 H1a），前缀一律
 * "NVME:" / "NVME-IO:" —— E2E 按前缀收行，改文案前先改测试。
 *
 * 初始化时序（NVMe 规范 3.1 起控制器初始化，顺序不能乱）：
 *   1. 读 CAP（64 位）取 MQES/DSTRD/TO/CSS/MPSMIN/MPSMAX
 *   2. CC.EN=0 -> 等 CSTS.RDY=0           （禁用控制器）
 *   3. AQA / ASQ / ACQ                     （队列深度与基址，必须 4KB 对齐）
 *   4. CC = IOCQES/IOSQES/MPS/CSS/EN=1    （配置并使能）
 *   5. 等 CSTS.RDY=1                       （超时用 CAP.TO * 500ms）
 *   6. Identify Controller(CNS=1) -> NN；Identify Namespace(CNS=0) -> NSZE/LBADS
 *   7. Create I/O CQ(0x05) + Create I/O SQ(0x01)
 *
 * 完成队列的相位位（Phase Tag）是整个驱动最容易错的地方：
 *   CQE 的 DW3 bit16 = P。控制器每次绕回就翻转 P，所以"有没有新完成项"
 *   不是看"非零"，而是看 **P == 当前期望相位**。期望相位从 1 开始
 *   （CQE 初始全 0，控制器第一轮填 P=1），每绕回一次翻转。
 *   判错会导致要么永远等不到、要么把上一轮的旧完成当成本轮的。
 *
 * 状态判据（照 Linux 的做法）：status = DW3[31:17]，**全 0 才算成功**；
 * 只看低 11 位（SCT+SC）是不对的。
 *
 * 轮询而非中断：与 AHCI/网络栈一致（不接 MSI-X），命令超时即 fail closed。
 */
#include "nvme.h"
#include "pci.h"
#include "paging.h"
#include "dmesg.h"
#include "isr.h"          /* g_pit_ticks：1ms 真实时间基准（nvme_init 在 pit_init+sti 之后） */

/* ---------- 控制器寄存器偏移（相对 BAR 基址） ---------- */
#define NV_CAP     0x00u    /* 64 位：Controller Capabilities */
#define NV_VS      0x08u    /* Version */
#define NV_INTMS   0x0Cu    /* Interrupt Mask Set */
#define NV_INTMC   0x10u    /* Interrupt Mask Clear */
#define NV_CC      0x14u    /* Controller Configuration */
#define NV_CSTS    0x1Cu    /* Controller Status */
#define NV_AQA     0x24u    /* Admin Queue Attributes */
#define NV_ASQ     0x28u    /* Admin SQ Base Address（64 位） */
#define NV_ACQ     0x30u    /* Admin CQ Base Address（64 位） */
#define NV_DB_BASE 0x1000u  /* Doorbell 区起点：SQ0TDBL */

/* CC 位 */
#define CC_EN      (1u << 0)
#define CC_CSS_NVM (0u << 4)    /* bits 6:4 = 0（NVM 命令集） */
#define CC_MPS_4K  (0u << 7)    /* bits 10:7 = 0（4KB 内存页） */
#define CC_AMS_RR  (0u << 11)   /* bits 13:11 = 0（轮转仲裁） */
#define CC_IOSQES_64B (6u << 16)/* bits 19:16 = 6 -> 2^6 = 64 字节 */
#define CC_IOCQES_16B (4u << 20)/* bits 23:20 = 4 -> 2^4 = 16 字节 */

/* CSTS 位 */
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)    /* Controller Fatal Status */

/* 管理命令 opcode（走 Admin 队列） */
#define OPC_CREATE_IO_SQ 0x01u
#define OPC_CREATE_IO_CQ 0x05u
#define OPC_IDENTIFY     0x06u

/* NVM 命令 opcode（走 I/O 队列）。注意 OPC_NVM_WRITE 与 OPC_CREATE_IO_SQ
 * 同为 0x01：管理与 NVM 是**两套独立的 opcode 空间**，靠队列区分，不是笔误。 */
#define OPC_NVM_WRITE    0x01u
#define OPC_NVM_READ     0x02u

/* 队列深度（必须是 2 的幂更省事，但不是硬性要求；上限取 CAP.MQES+1） */
#define NV_ADMIN_DEPTH 16u
#define NV_IO_DEPTH    32u

/* 真实时间上限（毫秒）。MMIO 读一次的成本在不同机器上差一个数量级，
 * 一律用 g_pit_ticks 计时，绝不拿"迭代次数"当时间（AHCI 踩过）。 */
#define NV_READY_MS   2000u   /* CSTS.RDY 起停上限 */
#define NV_ADMIN_MS   2000u   /* 管理命令完成上限 */
#define NV_IO_MS      3000u   /* IO 命令完成上限（慢盘/真机留足） */

/* ---------- 静态缓冲（.bss 19MB 区：identity 映射 == 物理地址） ----------
 * 队列基址必须按内存页（4KB）对齐，所以每条队列都单独占一个对齐块。 */
static uint8_t g_asq[4096] __attribute__((aligned(4096)));  /* Admin SQ: 16*64 */
static uint8_t g_acq[4096] __attribute__((aligned(4096)));  /* Admin CQ: 16*16 */
static uint8_t g_iosq[4096] __attribute__((aligned(4096))); /* I/O  SQ: 32*64 */
static uint8_t g_iocq[4096] __attribute__((aligned(4096))); /* I/O  CQ: 32*16 */
static uint8_t g_ident[4096] __attribute__((aligned(4096)));/* Identify 返回 */
static uint8_t g_data[4096] __attribute__((aligned(4096))); /* 数据 bounce（单页） */

static volatile uint32_t *g_bar;      /* MMIO 基址（虚拟 == 物理） */
static uint32_t g_dstrd;              /* Doorbell 步长因子（CAP[35:32]） */
static uint32_t g_db_sq1;             /* I/O SQ doorbell 偏移 */
static uint32_t g_db_cq1;             /* I/O CQ doorbell 偏移 */
static uint16_t g_cid;                /* 命令标识符（自增） */

static uint32_t g_sq_tail;            /* Admin SQ tail */
static uint32_t g_cq_head;            /* Admin CQ head */
static uint8_t  g_cq_phase;           /* Admin CQ 期望相位（从 1 开始） */

static uint32_t g_io_sq_tail;
static uint32_t g_io_cq_head;
static uint8_t  g_io_cq_phase;

/* 已认领的 namespace：ns 下标 -> NSID / 容量（扇区数，uint32 截断） */
static uint32_t g_ns_nsid[NVME_MAX_NS];
static uint32_t g_ns_sect[NVME_MAX_NS];
static uint8_t  g_ns_n;

static int      g_ready;              /* 控制器已 enable 且队列就绪 */

/* ---------- 小工具 ---------- */
static void n_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = v;
}

static void n_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void n_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}
static void n_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12]; int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}
static void n_dec64(char *b, int *n, int lim, uint64_t v) {
    char t[24]; int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + (v % 10)); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}
static void n_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

/* ---------- 寄存器访问 ---------- */
static inline uint32_t nr(uint32_t off) { return g_bar[off >> 2]; }
static inline void     nw(uint32_t off, uint32_t v) { g_bar[off >> 2] = v; }
static inline uint64_t nr64(uint32_t off) {
    uint64_t lo = (uint64_t)nr(off);
    uint64_t hi = (uint64_t)nr(off + 4u);
    return lo | (hi << 32);
}
static inline void nw64(uint32_t off, uint64_t v) {
    nw(off, (uint32_t)(v & 0xFFFFFFFFu));
    nw(off + 4u, (uint32_t)(v >> 32));
}

/* ---------- 队列提交 / 完成 ---------- */

/* 往 SQ 尾部放一条 64 字节命令并敲 doorbell。
 * cmd[] 共 16 个 dword：
 *   [0]  CDW0（OPC 低字节 + CID 高 16 位）  [1]  NSID
 *   [6:7] PRP1（64 位物理地址）             [8:9] PRP2
 *   [10..15] CDW10..CDW15
 * 返回本次使用的 cid。 */
static uint16_t n_submit(volatile uint32_t *sq, uint32_t depth, uint32_t *tail,
                         uint32_t db, const uint32_t *cmd) {
    uint16_t cid = (uint16_t)((g_cid + 1u) & 0xFFFFu);   /* 0 不是合法 cid */
    g_cid = cid;
    volatile uint32_t *e = &sq[(*tail) * 16u];
    for (int i = 0; i < 16; i++) e[i] = cmd[i];
    e[0] = (cmd[0] & 0x0000FFFFu) | ((uint32_t)cid << 16);
    *tail = (*tail + 1u) % depth;
    nw(db, *tail);
    return cid;
}

/* 等一条完成项（按相位位判定），取回 DW0 与状态。
 * 返回 0 成功（status==0），-1 失败/超时。res 可为 0。 */
static int n_complete(volatile uint32_t *cq, uint32_t depth, uint32_t *head,
                      uint8_t *phase, uint32_t cq_db, uint16_t want_cid,
                      uint32_t ms, uint32_t *res, uint32_t *status_out) {
    uint32_t t0 = g_pit_ticks;
    for (;;) {
        volatile uint32_t *ce = &cq[(*head) * 4u];
        uint32_t dw3 = ce[3];
        if (((dw3 >> 16) & 1u) == (uint32_t)(*phase & 1u)) {
            uint32_t status = (dw3 >> 17) & 0x7FFFu;   /* 全 0 才算成功 */
            uint16_t cid = (uint16_t)(dw3 & 0xFFFFu);
            if (res) *res = ce[0];
            if (status_out) *status_out = status;
            /* 推进 head；绕回则翻转期望相位，并通知控制器回收该项 */
            *head = *head + 1u;
            if (*head >= depth) { *head = 0; *phase = (uint8_t)(*phase ^ 1u); }
            nw(cq_db, *head);
            if (cid != want_cid) return -1;            /* 乱序：fail closed */
            return (status == 0u) ? 0 : -1;
        }
        if ((uint32_t)(g_pit_ticks - t0) >= ms) {
            if (status_out) *status_out = 0xFFFFFFFFu;
            return -1;
        }
    }
}

/* 发一条管理命令并等完成。prp1 可为 0（无数据阶段）。 */
static int n_admin(uint8_t opc, uint32_t nsid, uint64_t prp1,
                   uint32_t cdw10, uint32_t cdw11, uint32_t *res) {
    uint32_t c[16];
    n_memset(c, 0, sizeof(c));
    c[0] = (uint32_t)opc;
    c[1] = nsid;
    c[6] = (uint32_t)(prp1 & 0xFFFFFFFFu);
    c[7] = (uint32_t)(prp1 >> 32);
    c[10] = cdw10;
    c[11] = cdw11;
    uint16_t cid = n_submit((volatile uint32_t *)g_asq, NV_ADMIN_DEPTH,
                            &g_sq_tail, NV_DB_BASE, c);
    return n_complete((volatile uint32_t *)g_acq, NV_ADMIN_DEPTH, &g_cq_head,
                      &g_cq_phase, NV_DB_BASE + (uint32_t)(4u << g_dstrd),
                      cid, NV_ADMIN_MS, res, 0);
}

/* ---------- Identify ---------- */
static uint64_t id_read64(const uint8_t *p, int off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[off + i];
    return v;
}
static uint32_t id_read32(const uint8_t *p, int off) {
    return (uint32_t)id_read64(p, off) & 0xFFFFFFFFu;
}

/* ---------- 单扇区读写 ---------- */
static int n_io_sector(uint8_t ns, uint32_t lba, uint8_t *buffer, int write) {
    if (!g_ready || ns >= NVME_MAX_NS || ns >= g_ns_n) return -1;
    if (g_ns_sect[ns] == 0u || lba >= g_ns_sect[ns]) return -1;   /* 越界 fail closed */
    if (write) n_memcpy(g_data, buffer, 512);

    uint32_t c[16];
    n_memset(c, 0, sizeof(c));
    c[0] = write ? OPC_NVM_WRITE : OPC_NVM_READ;
    c[1] = g_ns_nsid[ns];
    c[6] = (uint32_t)((uint64_t)(unsigned long)g_data & 0xFFFFFFFFu);
    c[7] = 0;
    c[10] = lba;                 /* CDW10/11 = SLBA（64 位） */
    c[11] = 0;
    c[12] = 0u;                  /* CDW12[15:0] = NLB（0 基）-> 1 个块 */

    uint16_t cid = n_submit((volatile uint32_t *)g_iosq, NV_IO_DEPTH,
                            &g_io_sq_tail, g_db_sq1, c);
    uint32_t status = 0;
    int r = n_complete((volatile uint32_t *)g_iocq, NV_IO_DEPTH, &g_io_cq_head,
                       &g_io_cq_phase, g_db_cq1, cid, NV_IO_MS, 0, &status);
    if (r != 0) {
        char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
        n_str(line, &n, lim, "NVME-IO: ");
        n_str(line, &n, lim, write ? "write" : "read");
        n_str(line, &n, lim, " fail ns=");
        n_dec(line, &n, lim, (uint32_t)ns);
        n_str(line, &n, lim, " lba=");
        n_dec(line, &n, lim, lba);
        n_str(line, &n, lim, " sts=0x");
        n_hex(line, &n, lim, status, 8);
        if (n < lim) line[n] = 0; else line[lim] = 0;
        dmesg_write(line);
        return -1;
    }
    if (!write) n_memcpy(buffer, g_data, 512);
    return 0;
}

int nvme_read_sector(uint8_t ns, uint32_t lba, uint8_t *buffer) {
    return n_io_sector(ns, lba, buffer, 0);
}

int nvme_write_sector(uint8_t ns, uint32_t lba, const uint8_t *buffer) {
    /* 去掉 const：bounce 只读源缓冲，不写它 */
    return n_io_sector(ns, lba, (uint8_t *)(unsigned long)buffer, 1);
}

/* namespace 总逻辑块数（512B）。未上线返回 0（fail closed）。 */
uint32_t nvme_ns_sectors(uint8_t ns) {
    if (!g_ready || ns >= g_ns_n) return 0;
    return g_ns_sect[ns];
}

int nvme_ns_present(uint8_t ns) {
    return (g_ready && ns < g_ns_n && g_ns_sect[ns] > 0u) ? 1 : 0;
}

int nvme_ns_count(void) {
    return (int)g_ns_n;
}

/* ---------- 控制器初始化 ---------- */

/* 找可用的 MMIO BAR。
 * 判据（A2 实测修正）：**64 位 BAR 不等于不可用**——只要高 32 位是 0
 * （即基址落在 4GB 以下），32 位无 PAE 照样能映射；真正要拒绝的是
 * 基址 >= 4GB（32 位分页够不到）。以前按"类型是 64 位就拒绝"写，
 * 会误拒 QEMU 的 NVMe（BAR0 标 64 位但分配在 0xfebf0000）。 */
static int n_find_bar(const pci_device_t *d, uint32_t *bar_out, int *idx_out) {
    for (int b = 0; b < 6; b++) {
        uint32_t v = d->bar[b];
        if (v == 0u) continue;
        if ((v & 0x1u) != 0u) continue;                 /* I/O 空间 BAR */
        uint32_t base = v & 0xFFFFFFF0u;
        if ((v & 0x6u) == 0x4u) {                       /* 64 位 BAR：占两个槽 */
            if (b + 1 >= 6) return -1;
            if (d->bar[b + 1] != 0u) {
                dmesg_write("NVME: MMIO above 4GB unsupported (32-bit paging)");
                return -1;
            }
            *bar_out = base;
            *idx_out = b;
            b++;                                        /* 吃掉高 32 位那个槽 */
            return 0;
        }
        *bar_out = base;
        *idx_out = b;
        return 0;
    }
    return -1;
}

int nvme_init(void) {
    int ndev = pci_device_count();
    int found = 0;

    for (int i = 0; i < ndev; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        /* NVMe：class 0x01（大容量存储）/ subclass 0x08（Non-Volatile Memory）。
         * prog_if 规范值是 0x02（NVMHCI），真机有报 0x00 的，故只按 class+subclass
         * 认领，prog_if 只打日志不当判据。 */
        if (d->class_code != 0x01u || d->subclass != 0x08u) continue;

        found++;
        if (g_bar) {
            dmesg_write("NVME: additional NVMe controller skipped (only first)");
            continue;
        }

        uint32_t bar = 0;
        int bar_idx = -1;
        if (n_find_bar(d, &bar, &bar_idx) != 0) {
            dmesg_write("NVME: no usable memory BAR, skip controller");
            continue;
        }

        /* 开 MEM 解码 + 总线主控（寄存器与 DMA 都靠这两个位） */
        uint32_t cmd = pci_read_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND);
        cmd |= (uint32_t)(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
        pci_write_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND, cmd);

        /* 映射 8KB（寄存器区 0x000-0xFFF + doorbell 区 0x1000 起）。
         * 按 PAGING_MMIO_FLAGS（PCD/PWT）关缓存——MMIO 寄存器不能用 WB 回写，
         * 否则真机上写操作可能滞留 cache 不落控制器。 */
        uint32_t page = bar & ~0xFFFu;
        for (uint32_t k = 0; k < 2u; k++) {
            if (paging_map(page + k * 0x1000u, page + k * 0x1000u,
                           PAGING_MMIO_FLAGS) != 0) {
                dmesg_write("NVME: paging_map failed, skip controller");
                return -1;
            }
        }
        g_bar = (volatile uint32_t *)(unsigned long)bar;

        {
            char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
            n_str(line, &n, lim, "NVME: ");
            n_hex(line, &n, lim, (uint32_t)d->vendor_id, 4);
            n_str(line, &n, lim, ":");
            n_hex(line, &n, lim, (uint32_t)d->device_id, 4);
            n_str(line, &n, lim, " bar=0x");
            n_hex(line, &n, lim, bar, 8);
            n_str(line, &n, lim, " (BAR");
            n_dec(line, &n, lim, (uint32_t)bar_idx);
            n_str(line, &n, lim, ") progif=0x");
            n_hex(line, &n, lim, (uint32_t)d->prog_if, 2);
            if (n < lim) line[n] = 0; else line[lim] = 0;
            dmesg_write(line);
        }

        /* ---------- 1. CAP / VS ---------- */
        uint64_t cap = nr64(NV_CAP);
        uint32_t mqes = (uint32_t)(cap & 0xFFFFu) + 1u;      /* 队列最大项数 */
        uint32_t to   = (uint32_t)((cap >> 24) & 0xFFu);     /* 超时（500ms 单位） */
        g_dstrd       = (uint32_t)((cap >> 32) & 0xFu);      /* Doorbell 步长 */
        uint32_t css_nvm = (uint32_t)((cap >> 37) & 0x1u);   /* bit37 = NVM 命令集 */
        uint32_t mpsmin  = (uint32_t)((cap >> 48) & 0xFu);
        uint32_t vers = nr(NV_VS);

        {
            char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
            n_str(line, &n, lim, "NVME: ver=");
            n_dec(line, &n, lim, (vers >> 16) & 0xFFFFu);
            n_str(line, &n, lim, ".");
            n_dec(line, &n, lim, (vers >> 8) & 0xFFu);
            n_str(line, &n, lim, " mqes=");
            n_dec(line, &n, lim, mqes);
            n_str(line, &n, lim, " dstrd=");
            n_dec(line, &n, lim, g_dstrd);
            n_str(line, &n, lim, " to=");
            n_dec(line, &n, lim, to);
            if (n < lim) line[n] = 0; else line[lim] = 0;
            dmesg_write(line);
        }

        if (css_nvm == 0u) {
            dmesg_write("NVME: NVM command set unsupported, skip");
            g_bar = 0;
            continue;
        }
        if (mpsmin != 0u) {
            dmesg_write("NVME: memory page size > 4KB unsupported, skip");
            g_bar = 0;
            continue;
        }
        if (g_dstrd > 8u) g_dstrd = 0u;                     /* 不可信上界 */

        /* ---------- 2. 禁用控制器 ---------- */
        if (nr(NV_CSTS) & CSTS_RDY) {
            nw(NV_CC, 0u);
            uint32_t t0 = g_pit_ticks;
            while ((nr(NV_CSTS) & CSTS_RDY) != 0u) {
                if ((uint32_t)(g_pit_ticks - t0) >= NV_READY_MS) break;
            }
        }
        if (nr(NV_CSTS) & CSTS_RDY) {
            dmesg_write("NVME: controller did not disable, skip");
            g_bar = 0;
            continue;
        }

        /* ---------- 3. 队列属性与基址 ---------- */
        n_memset(g_asq, 0, sizeof(g_asq));
        n_memset(g_acq, 0, sizeof(g_acq));
        n_memset(g_iosq, 0, sizeof(g_iosq));
        n_memset(g_iocq, 0, sizeof(g_iocq));
        /* 深度不能超过 CAP.MQES+1（不可信字段，取小者） */
        uint32_t adepth = NV_ADMIN_DEPTH;
        if (mqes < adepth) adepth = mqes;
        if (adepth < 2u) { dmesg_write("NVME: MQES too small"); g_bar = 0; continue; }

        nw(NV_AQA, ((adepth - 1u) << 16) | (adepth - 1u));
        nw64(NV_ASQ, (uint64_t)(unsigned long)g_asq);
        nw64(NV_ACQ, (uint64_t)(unsigned long)g_acq);

        g_sq_tail = 0; g_cq_head = 0; g_cq_phase = 1;       /* 期望相位从 1 起 */
        g_cid = 0;

        /* ---------- 4. 使能 ---------- */
        nw(NV_CC, CC_IOCQES_16B | CC_IOSQES_64B | CC_AMS_RR | CC_MPS_4K |
                  CC_CSS_NVM | CC_EN);

        /* ---------- 5. 等 RDY ---------- */
        {
            uint32_t t0 = g_pit_ticks;
            uint32_t limit = (to > 0u) ? (to * 500u) : 1000u;
            if (limit < NV_READY_MS) limit = NV_READY_MS;
            while ((nr(NV_CSTS) & CSTS_RDY) == 0u) {
                if ((uint32_t)(g_pit_ticks - t0) >= limit) break;
            }
        }
        if ((nr(NV_CSTS) & CSTS_RDY) == 0u) {
            dmesg_write("NVME: controller did not become ready, skip");
            g_bar = 0;
            continue;
        }
        if (nr(NV_CSTS) & CSTS_CFS) {
            dmesg_write("NVME: controller fatal status, skip");
            g_bar = 0;
            continue;
        }
        dmesg_write("NVME: controller ready, admin queue up");

        /* 关中断（全程轮询），避免未接 ISR 的中断线打来 */
        nw(NV_INTMS, 0xFFFFFFFFu);

        /* ---------- 6. Identify Controller（CNS=1） ---------- */
        n_memset(g_ident, 0, sizeof(g_ident));
        if (n_admin(OPC_IDENTIFY, 0u, (uint64_t)(unsigned long)g_ident,
                    1u /* CNS=1 */, 0u, 0) != 0) {
            dmesg_write("NVME: IDENTIFY controller failed, skip");
            g_bar = 0;
            continue;
        }
        uint32_t nn = id_read32(g_ident, 516);   /* Number of Namespaces */
        /* NN 可能是 0xFFFFFFFF（"很多"）这类哨兵值：本驱动最多只认 NVME_MAX_NS
         * 个 namespace，且 NSID 从 1 顺序试探，所以夹到 16 就够——既防哨兵值
         * 把循环放大到上千次 Identify，也不会漏掉真实存在的前几个 NSID。 */
        if (nn > 16u) nn = 16u;
        {
            char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
            n_str(line, &n, lim, "NVME: identify ctrl nn=");
            n_dec(line, &n, lim, nn);
            n_str(line, &n, lim, " vid=0x");
            n_hex(line, &n, lim, (uint32_t)(g_ident[0] | (g_ident[1] << 8)), 4);
            if (n < lim) line[n] = 0; else line[lim] = 0;
            dmesg_write(line);
        }
        if (nn == 0u) {
            dmesg_write("NVME: controller reports 0 namespace(s)");
            break;
        }

        /* ---------- 7. Create I/O CQ + SQ ---------- */
        uint32_t iodepth = NV_IO_DEPTH;
        if (mqes < iodepth) iodepth = mqes;
        if (iodepth < 2u) { dmesg_write("NVME: MQES too small for I/O queue"); break; }

        uint32_t cdw10_cq = ((iodepth - 1u) << 16) | 1u;     /* QID=1, QSIZE */
        uint32_t cdw11_cq = 1u;                              /* PC=1, IEN=0 */
        if (n_admin(OPC_CREATE_IO_CQ, 0u,
                    (uint64_t)(unsigned long)g_iocq, cdw10_cq, cdw11_cq, 0) != 0) {
            dmesg_write("NVME: CREATE_IO_CQ failed, skip");
            break;
        }
        uint32_t cdw10_sq = ((iodepth - 1u) << 16) | 1u;     /* QID=1, QSIZE */
        uint32_t cdw11_sq = (1u << 16) | 1u;                 /* CQID=1, PC=1 */
        if (n_admin(OPC_CREATE_IO_SQ, 0u,
                    (uint64_t)(unsigned long)g_iosq, cdw10_sq, cdw11_sq, 0) != 0) {
            dmesg_write("NVME: CREATE_IO_SQ failed, skip");
            break;
        }

        uint32_t stride = 4u << g_dstrd;
        g_db_sq1 = NV_DB_BASE + 2u * stride;                 /* SQ1TDBL */
        g_db_cq1 = NV_DB_BASE + 3u * stride;                 /* CQ1HDBL */
        g_io_sq_tail = 0; g_io_cq_head = 0; g_io_cq_phase = 1;
        g_ready = 1;

        /* ---------- 8. 逐个 Identify Namespace（CNS=0） ---------- */
        for (uint32_t nsid = 1u; nsid <= nn && g_ns_n < NVME_MAX_NS; nsid++) {
            n_memset(g_ident, 0, sizeof(g_ident));
            if (n_admin(OPC_IDENTIFY, nsid, (uint64_t)(unsigned long)g_ident,
                        0u /* CNS=0 */, 0u, 0) != 0) {
                continue;                                    /* 该 NSID 无效 */
            }
            uint64_t nsze = id_read64(g_ident, 0);           /* 容量（逻辑块数） */
            if (nsze == 0u) continue;                        /* 未分配 */
            uint32_t flbas = (uint32_t)(g_ident[26] & 0x0Fu);/* 当前 LBA 格式下标 */
            uint32_t nlbafl = (uint32_t)g_ident[25] + 1u;    /* LBA 格式个数 */
            if (flbas >= nlbafl) continue;                   /* 不可信下标 */
            uint32_t lbaf = id_read32(g_ident, 128 + (int)flbas * 4);
            uint32_t lbads = (lbaf >> 16) & 0xFFu;           /* log2(逻辑块大小) */
            if (lbads != 9u) {
                char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
                n_str(line, &n, lim, "NVME: nsid=");
                n_dec(line, &n, lim, nsid);
                n_str(line, &n, lim, " lba size != 512 (lbads=");
                n_dec(line, &n, lim, lbads);
                n_str(line, &n, lim, ") skipped");
                if (n < lim) line[n] = 0; else line[lim] = 0;
                dmesg_write(line);
                continue;
            }
            /* 块层用 uint32 lba：超过 4G 扇区（2TB）的部分截断，不溢出 */
            uint32_t sect = (nsze > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)nsze;
            g_ns_nsid[g_ns_n] = nsid;
            g_ns_sect[g_ns_n] = sect;
            {
                char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
                n_str(line, &n, lim, "NVME: ns");
                n_dec(line, &n, lim, (uint32_t)g_ns_n);
                n_str(line, &n, lim, " nsid=");
                n_dec(line, &n, lim, nsid);
                n_str(line, &n, lim, " sectors=");
                n_dec64(line, &n, lim, nsze);
                n_str(line, &n, lim, " lbads=9 ready");
                if (n < lim) line[n] = 0; else line[lim] = 0;
                dmesg_write(line);
            }
            g_ns_n++;
        }
        break;   /* 只初始化第一个控制器 */
    }

    if (found == 0) {
        dmesg_write("NVME: no NVMe controller found");
        return 0;
    }

    if (g_ns_n == 0u) {
        dmesg_write("NVME: 0 namespace(s) registered");
        return 0;
    }

    {
        char line[128]; int n = 0; int lim = (int)sizeof(line) - 1;
        n_str(line, &n, lim, "NVME: ");
        n_dec(line, &n, lim, (uint32_t)g_ns_n);
        n_str(line, &n, lim, " namespace(s) registered as drive ");
        n_dec(line, &n, lim, (uint32_t)NVME_DRIVE_BASE);
        n_str(line, &n, lim, "..");
        n_dec(line, &n, lim, (uint32_t)(NVME_DRIVE_BASE + g_ns_n - 1u));
        if (n < lim) line[n] = 0; else line[lim] = 0;
        dmesg_write(line);
    }
    return 1;
}
