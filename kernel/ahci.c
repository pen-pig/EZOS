/*
 * ahci.c - AHCI 1.3 SATA 磁盘驱动（真机点亮 H1b）
 *
 * 关键阶段全部经 dmesg_write 打点（真机串口诊断通道，见 H1a）：
 *   控制器发现/版本/端口数 -> 每实现端口的探测结果 -> 首例读/写成功。
 *
 * 不可信字段上界检查（红线）：CAP.NP、PI 位图、port 序号在用作下标/循环
 * 上界前一律夹紧到 [0, AHCI_MAX_PORTS] / 32；PRDT 长度恒为 1。
 *
 * 缓冲全部静态落在 .bss（19MB 区，identity 映射 == 物理地址），AHCI 直接拿
 * 内核地址当物理地址用，无需 bounce。轮询 CI 位 + 超时 fail closed，不接 IRQ。
 */
#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "dmesg.h"

/* ---------- 寄存器偏移（相对 ABAR） ---------- */
#define GHC_CAP   0x00
#define GHC_GHC   0x04
#define GHC_IS    0x08
#define GHC_PI    0x0C
#define GHC_VS    0x10
#define GHC_CAP2  0x24
#define GHC_BOQC  0x28

/* 端口寄存器（相对端口基址 0x100 + port*0x80） */
#define PX_CLB  0x00
#define PX_CLBU 0x04
#define PX_FB   0x08
#define PX_FBU  0x0C
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SCTL 0x2C
#define PX_SERR 0x30
#define PX_CI   0x38

/* 位定义 */
#define GHC_AE  (1u << 31)   /* AHCI Enable */
#define GHC_HR  (1u << 0)    /* HBA Reset */
#define GHC_IE  (1u << 1)    /* Interrupt Enable */

#define PXCMD_ST   (1u << 0)  /* Start */
#define PXCMD_SUD  (1u << 1)  /* Spin-Up Device */
#define PXCMD_FRE  (1u << 4)  /* FIS Receive Enable */
#define PXCMD_FR   (1u << 14) /* FIS Receive Running */
#define PXCMD_CR   (1u << 15) /* Command List Running */

#define SSTS_DET(p) ((p) & 0x0Fu)
#define TFD_ERR     (1u << 0)
#define TFD_BSY     (1u << 7)

#define SIG_ATA 0x00000101u   /* SATA 直连 ATA 设备（硬盘） */
#define SIG_ATAPI 0xEB140101u

#define AHCI_CMD_TIMEOUT 2000000u

/* ---------- DMA 缓冲（.bss 19MB 区，identity 映射 == 物理地址） ---------- */
static uint8_t g_cl[AHCI_MAX_PORTS][1024] __attribute__((aligned(1024)));
static uint8_t g_fb[AHCI_MAX_PORTS][256]  __attribute__((aligned(256)));
static uint8_t g_ct[AHCI_MAX_PORTS][256]  __attribute__((aligned(128)));
static uint8_t g_id[AHCI_MAX_PORTS][512];

static volatile uint32_t *g_abar;
static uint8_t  g_online[AHCI_MAX_PORTS];
static uint8_t  g_lba48[AHCI_MAX_PORTS];
static uint8_t  g_port_count;
static int      g_first_read_done;
static int      g_first_write_done;

/* ---------- 寄存器访问（ABAR 已被映射到虚拟==物理的高地址空间） ---------- */
static inline uint32_t ar(uint32_t off) { return g_abar[off >> 2]; }
static inline void     aw(uint32_t off, uint32_t v) { g_abar[off >> 2] = v; }

/* ---------- klog 打点 ---------- */
static void aklog(const char *s) { dmesg_write(s); }

/* label + 8 位十六进制值（调试 AHCI 寄存器） */
static void aklog_hex(const char *pre, uint32_t v, const char *suf) {
    char buf[64];
    int i = 0;
    while (*pre && i < 40) buf[i++] = *pre++;
    static const char hx[] = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) {
        if (i >= 48) break;
        buf[i++] = hx[(v >> s) & 0xF];
    }
    while (*suf && i < 60) buf[i++] = *suf++;
    buf[i] = 0;
    dmesg_write(buf);
}

static void aklog_dec(const char *pre, uint32_t v, const char *suf) {
    char buf[48];
    int i = 0;
    while (*pre && i < 40) buf[i++] = *pre++;
    if (v == 0) buf[i++] = '0';
    else {
        char tmp[11]; int n = 0;
        while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
        while (n > 0) buf[i++] = tmp[--n];
    }
    while (*suf && i < 46) buf[i++] = *suf++;
    buf[i] = 0;
    dmesg_write(buf);
}

/* ---------- 端口起停 ---------- */
static void ahci_stop_port(uint32_t pr) {
    uint32_t c = ar(pr + PX_CMD);
    if (c & PXCMD_ST) {
        aw(pr + PX_CMD, c & ~PXCMD_ST);
        for (uint32_t t = 0; t < 500000u; t++)
            if (!(ar(pr + PX_CMD) & PXCMD_CR)) break;
    }
    c = ar(pr + PX_CMD);
    if (c & PXCMD_FRE) {
        aw(pr + PX_CMD, c & ~PXCMD_FRE);
        for (uint32_t t = 0; t < 500000u; t++)
            if (!(ar(pr + PX_CMD) & PXCMD_FR)) break;
    }
}

/* 通用 COMRESET：SCTL.DET=1 触发 COMRESET，延时后写回 0 恢复正常工作。
 * 链路（SSTS.DET）真正升到 3 需要端口随后被 start（ST=1）+ FIS 接收使能，
 * 故 COMRESET 之后由调用方继续 FRE/ST 流程。延时给足（icount 下虚拟时间）。 */
static void ahci_comreset(uint32_t pr) {
    aw(pr + PX_SERR, ~0u);
    aw(pr + PX_SCTL, 0x00000001u);   /* DET=1：发起 COMRESET */
    for (volatile uint32_t i = 0; i < 3000000u; i++) { }
    aw(pr + PX_SCTL, 0x00000000u);   /* DET=0：恢复，开始链路检测 */
    for (volatile uint32_t i = 0; i < 3000000u; i++) { }
}

/* ---------- 提交一条 DMA 命令（槽 0，PRDT 长度 1） ---------- */
static int ahci_submit(uint8_t port, uint8_t ata_cmd, int is_write,
                       uint32_t lba, void *buf) {
    if (port >= AHCI_MAX_PORTS) return -1;
    /* 注意：探测路径在置 g_online 之前就要发 IDENTIFY，这里不能按
     * g_online 拦——否则探测自锁（IDENTIFY 永远发不出去，曾踩）。
     * online 门槛由 read/write_sector 的调用方各自把关。 */

    uint8_t *cl = g_cl[port];
    uint8_t *ct = g_ct[port];
    uint32_t pr = 0x100u + (uint32_t)port * 0x80u;

    /* 命令头：清 8 个 dword */
    for (int i = 0; i < 8; i++) ((uint32_t *)cl)[i] = 0;
    /* CFL=5（CFIS 5 dword），W=写标志，PRDTL=1 */
    ((uint32_t *)cl)[0] = 5u | (is_write ? (1u << 7) : 0u) | (1u << 16);
    ((uint32_t *)cl)[2] = (uint32_t)(size_t)ct;   /* CTBA */
    ((uint32_t *)cl)[3] = 0;                       /* CTBAU */

    /* 命令表：清 64 个 dword（256 字节） */
    for (int i = 0; i < 64; i++) ((uint32_t *)ct)[i] = 0;

    /* H2D 寄存器 FIS（20 字节），位于命令表偏移 0 */
    ct[0] = 0x27;                       /* FIS type */
    ct[1] = 0x80;                       /* C=1（命令），PM port=0 */
    ct[2] = ata_cmd;
    ct[3] = 0;                          /* feature low */
    /* H2D FIS 字节映射（AHCI 1.3 / QEMU 同构，曾整体错位一格，曾致扇区数恒为 0）：
     *   [4] LBA 7:0   [5] LBA 15:8  [6] LBA 23:16  [7] device(LBA 27:24)
     *   [8] LBA 31:24 [9] LBA 39:32 [10] LBA 47:40  [11] feature 15:8
     *   [12] 扇区数 7:0  [13] 扇区数 15:8
     * 扇区数必须写 [12]/[13]：写错位置会让每条读写变成 0 扇区（静默不做事）。
     */
    ct[4] = (uint8_t)(lba & 0xFFu);
    ct[5] = (uint8_t)((lba >> 8) & 0xFFu);
    ct[6] = (uint8_t)((lba >> 16) & 0xFFu);
    ct[12] = 1;                         /* 扇区数 7:0 = 1（块接口固定单扇区） */
    ct[13] = 0;                         /* 扇区数 15:8 */
    if (g_lba48[port]) {
        ct[7] = 0x40;                   /* device: LBA 模式 */
        ct[8] = (uint8_t)((lba >> 24) & 0xFFu);
        ct[9] = 0;                      /* LBA 39:32（块接口 lba 为 32 位，恒 0） */
        ct[10] = 0;                     /* LBA 47:40 恒 0 */
        ct[11] = 0;                     /* feature 15:8 */
    } else {
        ct[7] = (uint8_t)(0x40u | ((lba >> 24) & 0x0Fu)); /* LBA28：高 4 位入 device */
        ct[8] = 0;
        ct[9] = 0; ct[10] = 0; ct[11] = 0;
    }

    /* PRDT（命令表偏移 0x80，规范定义；QEMU 硬编码同值）：单条目 512 字节，I=1 */
    uint32_t *prdt = (uint32_t *)(ct + 0x80);
    prdt[0] = (uint32_t)(size_t)buf;
    prdt[1] = 0;
    prdt[2] = 0;
    prdt[3] = (512u - 1u) | (1u << 31);

    /* 结构写好后再发命令（UC MMIO 强序 + 编译器屏障） */
    asm volatile("" ::: "memory");

    /* 清错误状态，等槽 0 空闲 */
    aw(pr + PX_SERR, ~0u);
    aw(pr + PX_IS, ~0u);
    for (uint32_t t = 0; t < AHCI_CMD_TIMEOUT; t++)
        if (!(ar(pr + PX_CI) & 1u)) break;

    asm volatile("" ::: "memory");
    aw(pr + PX_CI, ar(pr + PX_CI) | 1u);   /* 发出命令 */

    /* 轮询 CI 位清零 = 完成；超时 fail closed */
    for (uint32_t t = 0; t < AHCI_CMD_TIMEOUT; t++) {
        if (!(ar(pr + PX_CI) & 1u)) break;
    }
    if (ar(pr + PX_CI) & 1u) return -1;     /* 超时 */

    uint32_t tfd = ar(pr + PX_TFD);
    if (tfd & (TFD_ERR | TFD_BSY)) return -1;
    return 0;
}

/* ---------- 端口探测 ---------- */
static int ahci_probe_port(uint8_t port) {
    uint32_t pr = 0x100u + (uint32_t)port * 0x80u;

    /* 干净起停端口（先清 ST/CR、FRE/FR，再装基址） */
    ahci_stop_port(pr);
    aw(pr + PX_CLB,  (uint32_t)(size_t)g_cl[port]);
    aw(pr + PX_CLBU, 0);
    aw(pr + PX_FB,   (uint32_t)(size_t)g_fb[port]);
    aw(pr + PX_FBU,  0);
    aw(pr + PX_SERR, ~0u);

    /* HBA reset 之后链路默认不会起来，发 COMRESET 让设备重新协商 */
    ahci_comreset(pr);

    /* FIS 接收使能（收签名 FIS 必需），等 FR */
    aw(pr + PX_CMD, ar(pr + PX_CMD) | PXCMD_FRE);
    for (uint32_t t = 0; t < 500000u; t++)
        if (ar(pr + PX_CMD) & PXCMD_FR) break;
    if (!(ar(pr + PX_CMD) & PXCMD_FR)) {
        aklog_dec("AHCI: port ", port, " FIS receive enable failed");
        return 0;
    }
    /* 命令列表启动，等 CR */
    aw(pr + PX_CMD, ar(pr + PX_CMD) | PXCMD_ST);
    for (uint32_t t = 0; t < 500000u; t++)
        if (ar(pr + PX_CMD) & PXCMD_CR) break;
    if (pr == 0x100)
        aklog_hex("AHCI: after ST PXCMD=0x", ar(pr + PX_CMD), "");
    if (!(ar(pr + PX_CMD) & PXCMD_CR)) {
        aklog_dec("AHCI: port ", port, " command start failed");
        return 0;
    }

    /* 等链路就绪：SSTS.DET == 3（设备存在 + 协商完成）。
     * 在 QEMU ich9-ahci 上，DET 升到 3 需要端口已 start（ST=1）+ FIS 接收使能，
     * 故放在 FRE/ST 之后轮询。 */
    uint32_t ssts = 0;
    for (uint32_t t = 0; t < 5000000u; t++) {
        ssts = ar(pr + PX_SSTS);
        if (SSTS_DET(ssts) == 3) break;
    }
    aklog_hex("AHCI: port ", (uint32_t)port, " SSTS=0x");
    aklog_hex("        SSTS=0x", ssts, "");
    if (SSTS_DET(ssts) != 3) {
        aklog_dec("AHCI: port ", port, " absent (SSTS.DET != 3)");
        return 0;
    }

    /* 等签名：ATA 直连 = 0x00000101（FIS 接收使能后设备会回签名 FIS） */
    uint32_t sig = 0;
    for (uint32_t t = 0; t < 1000000u; t++) {
        sig = ar(pr + PX_SIG);
        if (sig != 0) break;
    }
    if (sig != SIG_ATA) {
        if (sig == SIG_ATAPI)
            aklog_dec("AHCI: port ", port, " is ATAPI (unsupported, skip)");
        else
            aklog_dec("AHCI: port ", port, " bad signature, skip");
        return 0;
    }

    /* IDENTIFY DEVICE：验证读 DMA 路径并取型号 */
    if (ahci_submit(port, 0xEC, 0, 0, g_id[port]) != 0) {
        aklog_hex("AHCI: port ", port, " IDENTIFY failed");
        aklog_hex("        TFD=0x", ar(pr + PX_TFD), "");
        aklog_hex("        IS =0x", ar(pr + PX_IS), "");
        aklog_hex("        SERR=0x", ar(pr + PX_SERR), "");
        aklog_hex("        CI =0x", ar(pr + PX_CI), "");
        return 0;
    }

    /* 型号字符串（IDENTIFY 字 27-46，每字字节对交换） */
    char model[42];
    int mi = 0;
    for (int w = 27; w <= 46 && mi < 40; w++) {
        uint8_t b0 = g_id[port][w * 2];
        uint8_t b1 = g_id[port][w * 2 + 1];
        if (mi < 40) model[mi++] = (char)b1;
        if (mi < 40) model[mi++] = (char)b0;
    }
    while (mi > 0 && (model[mi - 1] == ' ' || model[mi - 1] == 0)) mi--;
    model[mi] = 0;

    /* 字 83 bit10：LBA48 支持 */
    uint16_t w83 = (uint16_t)(g_id[port][166] | ((uint16_t)g_id[port][167] << 8));
    g_lba48[port] = (uint8_t)((w83 >> 10) & 1u);

    g_online[port] = 1;
    char line[64];
    int li = 0;
    const char *p = "AHCI: port ";
    while (*p && li < 20) line[li++] = *p++;
    if (port < 10) line[li++] = (char)('0' + port);
    else { line[li++] = (char)('0' + port / 10); line[li++] = (char)('0' + port % 10); }
    p = " online (model: ";
    while (*p && li < 48) line[li++] = *p++;
    for (int k = 0; k < mi && li < 60; k++) line[li++] = model[k];
    p = ")";
    while (*p && li < 62) line[li++] = *p++;
    line[li] = 0;
    dmesg_write(line);
    return 1;
}

/* ---------- 对外接口 ---------- */
int ahci_port_present(uint8_t port) {
    if (port >= AHCI_MAX_PORTS) return 0;
    return g_online[port];
}

uint8_t ahci_port_count(void) { return g_port_count; }

int ahci_read_sector(uint8_t port, uint32_t lba, uint8_t *buffer) {
    if (port >= AHCI_MAX_PORTS || !g_online[port]) return -1;
    uint8_t cmd = g_lba48[port] ? 0x25u : 0xC8u;   /* READ DMA EXT / READ DMA */
    int r = ahci_submit(port, cmd, 0, lba, buffer);
    if (r == 0 && !g_first_read_done) {
        g_first_read_done = 1;
        aklog("AHCI: first read OK");
    }
    return r;
}

int ahci_write_sector(uint8_t port, uint32_t lba, const uint8_t *buffer) {
    if (port >= AHCI_MAX_PORTS || !g_online[port]) return -1;
    uint8_t cmd = g_lba48[port] ? 0x35u : 0xCAu;   /* WRITE DMA EXT / WRITE DMA */
    int r = ahci_submit(port, cmd, 1, lba, (void *)buffer);
    if (r == 0 && !g_first_write_done) {
        g_first_write_done = 1;
        aklog("AHCI: first write OK");
    }
    return r;
}

int ahci_init(void) {
    const pci_device_t *dev = 0;

    /* 1) PCI 扫描 class 0x0106（SATA AHCI）：class 0x01, subclass 0x06 */
    int npci = pci_device_count();
    for (int i = 0; i < npci; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x01 && d->subclass == 0x06) { dev = d; break; }
    }
    /* 回退：已知 AHCI 控制器 PCIID（BIOS 把它设成兼容模式时 class 可能不符） */
    if (!dev) {
        static const struct { uint16_t v; uint16_t d; } known[] = {
            { 0x8086, 0x2922 }, { 0x8086, 0x2829 }, { 0x8086, 0x1E02 },
            { 0x8086, 0x8C02 }, { 0x1B4B, 0x9230 }, { 0x1CC4, 0x8630 },
        };
        for (int i = 0; i < npci; i++) {
            const pci_device_t *d = pci_get_device(i);
            if (!d) continue;
            for (int k = 0; k < 6; k++)
                if (d->vendor_id == known[k].v && d->device_id == known[k].d) {
                    dev = d; break;
                }
            if (dev) break;
        }
    }
    if (!dev) {
        aklog("AHCI: no SATA/AHCI controller found, skipping");
        return 0;
    }

    /* 2) 打开 PCI 内存解码 + 总线主控（DMA 必需） */
    uint32_t cmd = pci_read_dword(dev->bus, dev->dev, dev->func, PCI_REG_COMMAND);
    cmd |= (uint32_t)PCI_CMD_MEM_SPACE | (uint32_t)PCI_CMD_BUS_MASTER;
    pci_write_dword(dev->bus, dev->dev, dev->func, PCI_REG_COMMAND, cmd);

    /* 3) 映射 ABAR（BAR5，内存 BAR）。ABAR 通常在高物理地址，需 map 进内核空间。
     *    选 virt = phys（高地址不与 0-32MB identity 区冲突），uncacheable。 */
    uint32_t bar = dev->bar[5];
    if (bar & 0x1u) {                    /* bit0=1 说明是 IO BAR，AHCI 必为 mem */
        aklog("AHCI: BAR5 is not a memory BAR, abort");
        return -1;
    }
    uint32_t pa = bar & 0xFFFFFFF0u;
    uint32_t page_pa = pa & 0xFFFFF000u;
    for (int i = 0; i < 4; i++) {        /* 最多 4 页，覆盖 <=32 端口的寄存器区 */
        uint32_t pg = page_pa + (uint32_t)i * 4096u;
        if (paging_map(pg, pg, PAGING_MMIO_FLAGS) != 0) {
            aklog("AHCI: ABAR map failed, abort");
            return -1;
        }
    }
    g_abar = (volatile uint32_t *)(size_t)pa;

    /* 诊断：确认 ABAR 物理地址与页表映射确实覆盖端口寄存器区 */
    {
        uint32_t qp = 0, qf = 0;
        int q1 = paging_query(pa, &qp, &qf);
        int q2 = paging_query(pa + 0x128, &qp, &qf);
        aklog_hex("AHCI: ABAR pa=0x", pa, "");
        aklog_hex("AHCI: GHC pg phys=0x", q1 == 0 ? qp : 0, q1 == 0 ? " ok" : " UNMAPPED");
        aklog_hex("AHCI: SSTS pg phys=0x", q2 == 0 ? qp : 0, q2 == 0 ? " ok" : " UNMAPPED");
        aklog_hex("AHCI: reread CAP=0x", ar(GHC_CAP), "");
        aklog_hex("AHCI: SIG@0x124=0x", ar(0x124), "");
        aklog_hex("AHCI: SSTS@0x128=0x", ar(0x128), "");
    }

    /* 4) BIOS/OS 接管握手（若 BIOS 仍持有控制器） */
    uint32_t boqc = ar(GHC_BOQC);
    if ((boqc & 1u) && !(boqc & 2u)) {
        aw(GHC_BOQC, boqc | 1u);
        for (uint32_t t = 0; t < 200000u; t++)
            if (ar(GHC_BOQC) & 2u) break;
    }

    /* 5) AHCI Enable + HBA Reset */
    if (!(ar(GHC_GHC) & GHC_AE)) {
        aw(GHC_GHC, ar(GHC_GHC) | GHC_AE);
    }
    if (!(ar(GHC_GHC) & GHC_AE)) {
        aklog("AHCI: cannot set AE (AHCI Enable), abort");
        return -1;
    }
#if 0
    aw(GHC_GHC, ar(GHC_GHC) | GHC_HR);
    for (uint32_t t = 0; t < 1000000u; t++)
        if (!(ar(GHC_GHC) & GHC_HR)) break;
    if (ar(GHC_GHC) & GHC_HR) {
        aklog("AHCI: HBA reset timeout, abort");
        return -1;
    }
    aw(GHC_GHC, ar(GHC_GHC) | GHC_AE);   /* reset 后 AE 可能被清，重设 */
#endif

    /* 诊断：HBA reset + AE 之后，端口寄存器区是否仍可访问 */
    {
        aklog_hex("AHCI: post-reset SIG@0x124=0x", ar(0x124), "");
        aklog_hex("AHCI: post-reset PXCMD@0x118=0x", ar(0x118), "");
        aklog_hex("AHCI: post-reset SSTS@0x128=0x", ar(0x128), "");
    }

    /* 6) 统计端口并打印控制器信息 */
    uint32_t cap = ar(GHC_CAP);
    uint32_t vs  = ar(GHC_VS);
    uint32_t pi  = ar(GHC_PI);
    uint32_t nports = (cap & 0x1Fu) + 1u;     /* CAP.NP = 端口数-1 */
    if (nports > 32u) nports = 32u;
    uint32_t implemented = 0;
    for (uint32_t p = 0; p < nports; p++)
        if (pi & (1u << p)) implemented++;

    aklog_hex("AHCI: CAP=0x", cap, "");
    aklog_hex("AHCI: PI =0x", pi, "");
    aklog_dec("AHCI: nports=", nports, "");

    {
        char line[64];
        int li = 0;
        const char *p = "AHCI: controller ";
        while (*p && li < 16) line[li++] = *p++;
        char vid[8]; int vi = 0;
        uint16_t v = dev->vendor_id, dd = dev->device_id;
        /* 简单十六进制 */
        static const char hx[] = "0123456789ABCDEF";
        vid[vi++] = hx[(v >> 12) & 0xF]; vid[vi++] = hx[(v >> 8) & 0xF];
        vid[vi++] = hx[(v >> 4) & 0xF]; vid[vi++] = hx[v & 0xF];
        vid[vi++] = ':';
        vid[vi++] = hx[(dd >> 12) & 0xF]; vid[vi++] = hx[(dd >> 8) & 0xF];
        vid[vi++] = hx[(dd >> 4) & 0xF]; vid[vi++] = hx[dd & 0xF];
        for (int k = 0; k < vi && li < 28; k++) line[li++] = vid[k];
        p = " ver ";
        while (*p && li < 36) line[li++] = *p++;
        line[li++] = (char)('0' + ((vs >> 16) & 0xF));
        line[li++] = '.';
        line[li++] = (char)('0' + ((vs >> 12) & 0xF));
        line[li++] = (char)('0' + ((vs >> 8) & 0xF));
        p = " ports=";
        while (*p && li < 50) line[li++] = *p++;
        line[li++] = (char)('0' + (nports % 10));
        p = " impl=";
        while (*p && li < 60) line[li++] = *p++;
        line[li++] = (char)('0' + (implemented % 10));
        line[li] = 0;
        dmesg_write(line);
    }

    /* 7) 逐端口探测 */
    g_port_count = 0;
    for (uint8_t port = 0; port < (uint8_t)nports && port < AHCI_MAX_PORTS; port++) {
        if (!(pi & (1u << port))) continue;
        aklog_dec("AHCI: enter probe port ", port, " (PI set)");
        if (ahci_probe_port(port)) g_port_count++;
    }

    aklog_dec("AHCI: ", (uint32_t)g_port_count, " port(s) online");
    return g_port_count > 0 ? 1 : 0;
}
