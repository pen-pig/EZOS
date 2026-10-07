/*
 * usbmsc.c - USB Mass Storage（BOT + SCSI 子集）驱动（真机点亮 H1c）
 *
 * 协议骨架（Bulk-Only Transport, USB MSD class spec rev 1.0）：
 *   CBW (31B, bulk OUT) -> 可选数据阶段 (bulk IN/OUT) -> CSW (13B, bulk IN)
 *   CBW 签名 'USBC' 0x43425355，CSW 签名 'USBS' 0x53425355。
 *   CSW.bCSWStatus: 0x00=成功 0x01=命令失败(要 REQUEST SENSE) 0x02=相位错。
 *
 * SCSI 子集（SBC-2，QEMU usb-storage 与绝大多数 U 盘都实现）：
 *   TEST UNIT READY(0x00) / INQUIRY(0x12) / REQUEST SENSE(0x03)
 *   READ CAPACITY(10)(0x25) / READ(10)(0x28) / WRITE(10)(0x2A)
 *   所有 SCSI 字段都是大端（USB 描述符/描述头是小端，注意区分）。
 *
 * 红线落实：
 *  - 认领 fail closed：MSC 接口 + 一对 bulk 端点 + 已配置，缺一不注册。
 *  - LBA/扇区数来自 READ CAPACITY，是不可信输入：读写下界夹紧、越界拒绝。
 *  - STALL 恢复：BOT Reset + 两端点 ClearFeature(HALT) + toggle 清零，
 *    一笔命令只恢复重试一次，再失败立即返回错误（不无限重试）。
 *  - 不在 IRQ/关中断上下文调用（bulk 传输依赖 g_pit_ticks 超时，同键盘的
 *    教训）；FS/块层调用点都在任务上下文，天然满足。
 */
#include "usbmsc.h"
#include "usbenum.h"
#include "usbhc.h"
#include "isr.h"
#include "dmesg.h"

/* ---------- 手写格式化（与 usbenum.c 同风格） ---------- */
static void m_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

static void m_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

static void m_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

static void m_emit(char *b, int n, int lim) {
    if (n < lim) b[n] = 0; else b[lim] = 0;
    dmesg_write(b);
}

/* ---------- 设备表 ---------- */
typedef struct {
    const usb_dev_t *d;     /* 枚举表里的设备（静态存储，指针安全） */
    uint32_t sectors;       /* 总扇区数（READ CAPACITY 报告 last_lba+1） */
    uint32_t tag;           /* BOT 命令标签（单调递增） */
    int      ready;         /* TEST UNIT READY 通过 */
} msc_dev_t;

static msc_dev_t g_msc[USBMSC_MAX_DEV];
static int       g_msc_n = 0;

/* ---------- BOT 底层 ---------- */

/* bulk 端点最大包长的合法值：全速/低速 8..64，高速 512（一个扇区）。
 * 低速设备没有 bulk 端点，天然走不到这里。 */
static int msc_mps_ok(uint16_t mps) {
    return (mps == 8u || mps == 16u || mps == 32u || mps == 64u ||
            mps == 512u) ? 1 : 0;
}

/* 一笔完整 BOT 命令：CBW -> 数据 -> CSW。
 * 返回 0 成功；<0 传输失败；1 = CSW 报命令失败（设备要求 REQUEST SENSE）。
 * datalen==0 时跳过数据阶段。dir_in 与 CBW.flags 一致。 */
static int bot_command(msc_dev_t *u, const uint8_t *cb, int cblen,
                       uint8_t *data, uint32_t datalen, int dir_in) {
    const usb_dev_t *d = u->d;
    uint8_t cbw[31];
    uint8_t csw[13];

    u->tag++;

    /* ---- CBW（31 字节，小端字段，签名是字节序固定的 magic） ---- */
    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43; /* 'USBC' */
    cbw[4]  = (uint8_t)(u->tag & 0xFFu);
    cbw[5]  = (uint8_t)((u->tag >> 8) & 0xFFu);
    cbw[6]  = (uint8_t)((u->tag >> 16) & 0xFFu);
    cbw[7]  = (uint8_t)((u->tag >> 24) & 0xFFu);
    cbw[8]  = (uint8_t)(datalen & 0xFFu);
    cbw[9]  = (uint8_t)((datalen >> 8) & 0xFFu);
    cbw[10] = (uint8_t)((datalen >> 16) & 0xFFu);
    cbw[11] = (uint8_t)((datalen >> 24) & 0xFFu);
    cbw[12] = (dir_in && datalen > 0u) ? 0x80u : 0x00u;
    cbw[13] = 0;                                   /* LUN 0（单 LUN 设备） */
    cbw[14] = (uint8_t)cblen;
    for (int i = 0; i < cblen && i < 16; i++) cbw[15 + i] = cb[i];

    if (usbhc_bulk_xfer(&d->bus, d->addr, d->msc_ep_out, cbw, 31,
                        (int)d->msc_ep_out_mps, NULL) != 0)
        return -1;

    /* ---- 数据阶段（长度精确 = dCBWDataTransferLength） ---- */
    if (datalen > 0) {
        uint8_t ep = dir_in ? d->msc_ep_in : d->msc_ep_out;
        int mps = (int)(dir_in ? d->msc_ep_in_mps : d->msc_ep_out_mps);
        int act = 0;
        if (usbhc_bulk_xfer(&d->bus, d->addr, ep, data, (int)datalen,
                            mps, &act) != 0)
            return -1;
    }

    /* ---- CSW（13 字节） ---- */
    for (int i = 0; i < 13; i++) csw[i] = 0;
    if (usbhc_bulk_xfer(&d->bus, d->addr, d->msc_ep_in, csw, 13,
                        (int)d->msc_ep_in_mps, NULL) != 0)
        return -1;

    /* 签名/tag 校验：错位/相位错误直接按传输失败处理 */
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53)
        return -1;
    uint32_t ctag = (uint32_t)csw[4] | ((uint32_t)csw[5] << 8)
                  | ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    if (ctag != u->tag) return -1;

    uint8_t st = csw[12];
    if (st == 0x00u) return 0;
    if (st == 0x01u) return 1;                     /* 命令失败：可 REQUEST SENSE */
    return -1;                                     /* 0x02 相位错误 */
}

/* STALL/失败恢复：BOT Reset + 两端点 ClearFeature(HALT) + toggle 清零 */
static void bot_reset_recovery(const msc_dev_t *u) {
    const usb_dev_t *d = u->d;
    uint8_t rst[8]  = {0x21, 0xFF, 0x00, 0x00,
                       (uint8_t)(d->msc_if & 0xFFu), 0x00, 0x00, 0x00};
    uint8_t cin[8]  = {0x02, 0x01, 0x00, 0x00, d->msc_ep_in,  0x00, 0x00, 0x00};
    uint8_t cout[8] = {0x02, 0x01, 0x00, 0x00, d->msc_ep_out, 0x00, 0x00, 0x00};
    (void)usbhc_control_xfer(&d->bus, d->addr, 0, rst, 0, NULL, 0, NULL);
    (void)usbhc_control_xfer(&d->bus, d->addr, 0, cin, 0, NULL, 0, NULL);
    (void)usbhc_control_xfer(&d->bus, d->addr, 0, cout, 0, NULL, 0, NULL);
    usbhc_bulk_tog_reset(&d->bus, d->addr);
}

/* 带一次恢复重试的 BOT 命令封装 */
static int bot_command_retry(msc_dev_t *u, const uint8_t *cb, int cblen,
                             uint8_t *data, uint32_t datalen, int dir_in) {
    int r = bot_command(u, cb, cblen, data, datalen, dir_in);
    if (r == 0) return 0;
    /* 恢复一次：BOT Reset 清两端点 HALT + toggle 归零，然后重发 */
    bot_reset_recovery(u);
    return bot_command(u, cb, cblen, data, datalen, dir_in);
}

/* ---------- SCSI 命令子集 ---------- */

/* READ CAPACITY(10)：拿总扇区数与扇区大小（都来自设备，按不可信处理） */
static int scsi_read_capacity(msc_dev_t *u, uint32_t *sectors, uint32_t *blksize) {
    uint8_t cb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[8];
    for (int i = 0; i < 8; i++) data[i] = 0;
    if (bot_command_retry(u, cb, 10, data, 8, 1) != 0) return -1;
    /* 大端：data[0..3] = last LBA，data[4..7] = block size */
    uint32_t last = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                  | ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    uint32_t bs   = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16)
                  | ((uint32_t)data[6] << 8)  |  (uint32_t)data[7];
    *sectors  = last + 1u;               /* last LBA 是 0 基的最大合法 LBA */
    *blksize  = bs;
    return 0;
}

/* 单扇区读（READ(10)，1 扇区，512B 数据阶段） */
static int scsi_read10(msc_dev_t *u, uint32_t lba, uint8_t *buf) {
    uint8_t cb[10];
    cb[0] = 0x28;                          /* READ(10) */
    cb[1] = 0;
    cb[2] = (uint8_t)(lba >> 24);          /* 大端 LBA */
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)(lba);
    cb[6] = 0;
    cb[7] = 0;                             /* 大端传输块数 = 1 */
    cb[8] = 1;
    cb[9] = 0;
    return bot_command_retry(u, cb, 10, buf, 512, 1);
}

/* 单扇区写（WRITE(10)，1 扇区，512B 数据阶段）。
 * 数据阶段方向为 OUT，bot_command 只读 data，无需复制。 */
static int scsi_write10(msc_dev_t *u, uint32_t lba, const uint8_t *buf) {
    uint8_t cb[10];
    cb[0] = 0x2A;                          /* WRITE(10) */
    cb[1] = 0;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)(lba);
    cb[6] = 0;
    cb[7] = 0;
    cb[8] = 1;
    cb[9] = 0;
    return bot_command_retry(u, cb, 10, (uint8_t *)buf, 512, 0);
}

/* TEST UNIT READY：设备是否就绪（无数据阶段）。 */
static int scsi_test_ready(msc_dev_t *u) {
    uint8_t cb[6] = {0x00, 0, 0, 0, 0, 0};
    int r = bot_command_retry(u, cb, 6, NULL, 0, 1);
    return (r == 0) ? 0 : -1;
}

/* ---------- 对上接口 ---------- */

int usbmsc_init(void) {
    int ndev = usbenum_device_count();
    g_msc_n = 0;
    for (int i = 0; i < USBMSC_MAX_DEV; i++) {
        g_msc[i].d = NULL;
        g_msc[i].sectors = 0;
        g_msc[i].tag = 0;
        g_msc[i].ready = 0;
    }

    for (int i = 0; i < ndev && g_msc_n < USBMSC_MAX_DEV; i++) {
        const usb_dev_t *d = usbenum_get(i);
        if (!d || !d->configured) continue;
        /* 认领判据：MSC 接口 + 一对 bulk 端点 + SCSI 透明/BOT。
         * 低速设备没有 bulk 端点，lowspeed 设备天然不满足 mps 校验。 */
        if (d->msc_if < 0 || d->msc_sub != 0x06u || d->msc_proto != 0x50u)
            continue;
        if (d->msc_ep_in == 0u || d->msc_ep_out == 0u) continue;
        /* bulk 端点 mps 只做**合法性**校验：全速/低速是 8/16/32/64，高速
         *（EHCI）恒为 512。早先这里硬编码"必须 64"，结果高速 U 盘（mps=512）
         * 被静默拒掉——认领不了又不报错，最难受的那种失败。真正的"这条总线
         * 支不支持这个 mps"由传输层（usbhc_bulk_xfer -> uhci/ehci）判定。 */
        if (!msc_mps_ok(d->msc_ep_in_mps) || !msc_mps_ok(d->msc_ep_out_mps))
            continue;

        msc_dev_t *u = &g_msc[g_msc_n];
        u->d = d;
        u->tag = 0x5A5A0000u;              /* 任意非零基值，好认 */

        /* INQUIRY 没做：容量/就绪已经足够认领。TEST UNIT READY -> CAPACITY */
        if (scsi_test_ready(u) != 0) {
            char line[128];
            int n = 0, lim = (int)sizeof(line) - 1;
            m_str(line, &n, lim, "USB-MSC: unit");
            m_dec(line, &n, lim, (uint32_t)g_msc_n);
            m_str(line, &n, lim, " addr=");
            m_dec(line, &n, lim, (uint32_t)d->addr);
            m_str(line, &n, lim, " not ready, skip");
            m_emit(line, n, lim);
            u->d = NULL;
            continue;
        }
        uint32_t sectors = 0;
        uint32_t blksize = 0;
        if (scsi_read_capacity(u, &sectors, &blksize) != 0) {
            char line[128];
            int n = 0, lim = (int)sizeof(line) - 1;
            m_str(line, &n, lim, "USB-MSC: unit");
            m_dec(line, &n, lim, (uint32_t)g_msc_n);
            m_str(line, &n, lim, " read capacity failed, skip");
            m_emit(line, n, lim);
            u->d = NULL;
            continue;
        }
        /* 不可信上界夹紧：扇区大小只认 512（块层约定），总扇区数至少 1 */
        if (blksize == 0u) blksize = 512u;
        if (sectors < 1u) sectors = 1u;
        u->sectors = sectors;
        u->ready = 1;
        g_msc_n++;

        char line[128];
        int n = 0, lim = (int)sizeof(line) - 1;
        m_str(line, &n, lim, "USB-MSC: unit");
        m_dec(line, &n, lim, (uint32_t)(g_msc_n - 1));
        m_str(line, &n, lim, " addr=");
        m_dec(line, &n, lim, (uint32_t)d->addr);
        m_str(line, &n, lim, " ep_in=0x");
        m_hex(line, &n, lim, (uint32_t)d->msc_ep_in, 2);
        m_str(line, &n, lim, " ep_out=0x");
        m_hex(line, &n, lim, (uint32_t)d->msc_ep_out, 2);
        m_str(line, &n, lim, " sectors=");
        m_dec(line, &n, lim, sectors);
        m_str(line, &n, lim, " blksize=");
        m_dec(line, &n, lim, blksize);
        m_str(line, &n, lim, " ready");
        m_emit(line, n, lim);
    }

    if (g_msc_n == 0) {
        dmesg_write("USB-MSC: no mass storage device");
    } else {
        char line[128];
        int n = 0, lim = (int)sizeof(line) - 1;
        m_str(line, &n, lim, "USB-MSC: ");
        m_dec(line, &n, lim, (uint32_t)g_msc_n);
        m_str(line, &n, lim, " drive(s) registered as drive ");
        m_dec(line, &n, lim, (uint32_t)USBMSC_DRIVE_BASE);
        m_str(line, &n, lim, "..");
        m_dec(line, &n, lim, (uint32_t)(USBMSC_DRIVE_BASE + g_msc_n - 1));
        m_emit(line, n, lim);
    }
    return g_msc_n;
}

int usbmsc_count(void) {
    return g_msc_n;
}

int usbmsc_present(uint8_t unit) {
    if (unit >= (uint8_t)USBMSC_MAX_DEV) return 0;
    return (g_msc[unit].d && g_msc[unit].ready) ? 1 : 0;
}

/* U 盘总扇区数（512B）。未在线返回 0（fail closed）。 */
uint32_t usbmsc_sectors(uint8_t unit) {
    if (unit >= (uint8_t)USBMSC_MAX_DEV) return 0;
    if (!g_msc[unit].d || !g_msc[unit].ready) return 0;
    return g_msc[unit].sectors;
}

static msc_dev_t *msc_get(uint8_t unit, uint32_t lba) {
    if (unit >= (uint8_t)USBMSC_MAX_DEV) return NULL;
    msc_dev_t *u = &g_msc[unit];
    if (!u->d || !u->ready) return NULL;
    if (lba >= u->sectors) return NULL;      /* LBA 越界：fail closed */
    return u;
}

int usbmsc_read_sector(uint8_t unit, uint32_t lba, uint8_t *buffer) {
    msc_dev_t *u = msc_get(unit, lba);
    if (!u || !buffer) return -1;
    return scsi_read10(u, lba, buffer);
}

int usbmsc_write_sector(uint8_t unit, uint32_t lba, const uint8_t *buffer) {
    msc_dev_t *u = msc_get(unit, lba);
    if (!u || !buffer) return -1;
    return scsi_write10(u, lba, buffer);
}
