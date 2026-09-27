/*
 * usbenum.c - USB 设备枚举与描述符解析（真机点亮 H2-2c）
 *
 * 流程与边界见 usbenum.h。这里只补充实现层面的要点：
 *  - 本模块不碰 UHCI 寄存器，全部经 uhci_control_xfer() / uhci_port_reset() /
 *    uhci_hc_start() 三个原语完成，保持"传输层 / 枚举层"分层。
 *  - 每步失败都打印一条带端口号的失败行并放弃该端口（fail closed），
 *    绝不在没拿到真实描述符的情况下报成功——E2E 靠"插/不插设备结果相反"
 *    来证明这一点。
 *  - 描述符解析把"长度字段"当不可信输入：先夹紧再当下标；解析循环遇到
 *    len<2 或越界立即跳出，杜绝死循环与越界读。
 *  - 没有 sprintf，行缓冲 128 字节、所有写入以上界为准，末尾保 '\0'。
 */
#include "usbenum.h"
#include "uhci.h"
#include "isr.h"
#include "dmesg.h"

static usb_dev_t g_dev[USBENUM_MAX_DEV];
static int       g_dev_n = 0;

/* ---------- 手写格式化（与 uhci.c 同风格） ---------- */
static void e_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

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

/* 终结行缓冲并写进 dmesg（H1a 起 dmesg 全量镜像 COM1） */
static void e_emit(char *b, int n, int lim) {
    if (n < lim) b[n] = 0; else b[lim] = 0;
    dmesg_write(b);
}

/* 统一失败行：USB-ENUM: port<n> <stage> failed act=<k> */
static void e_fail(int port, const char *stage, int act) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: port");
    e_dec(line, &n, lim, (uint32_t)port);
    e_str(line, &n, lim, " ");
    e_str(line, &n, lim, stage);
    e_str(line, &n, lim, " failed act=");
    e_dec(line, &n, lim, (uint32_t)act);
    e_emit(line, n, lim);
}

/* 真实时间忙等（g_pit_ticks 由 IRQ0 驱动，1ms/格） */
static void e_delay_ms(uint32_t ms) {
    uint32_t t0 = g_pit_ticks;
    while ((uint32_t)(g_pit_ticks - t0) < ms) { }
}

/* 设备描述符打印行 */
static void e_log_dev(int port, const usb_dev_t *d) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: port");
    e_dec(line, &n, lim, (uint32_t)port);
    e_str(line, &n, lim, " addr=");
    e_dec(line, &n, lim, (uint32_t)d->addr);
    e_str(line, &n, lim, " vid=0x");
    e_hex(line, &n, lim, (uint32_t)d->vid, 4);
    e_str(line, &n, lim, " pid=0x");
    e_hex(line, &n, lim, (uint32_t)d->pid, 4);
    e_str(line, &n, lim, " cls=");
    e_hex(line, &n, lim, (uint32_t)d->cls, 2);
    e_str(line, &n, lim, " sub=");
    e_hex(line, &n, lim, (uint32_t)d->sub, 2);
    e_str(line, &n, lim, " proto=");
    e_hex(line, &n, lim, (uint32_t)d->proto, 2);
    e_str(line, &n, lim, " mps0=");
    e_dec(line, &n, lim, (uint32_t)d->mps0);
    e_str(line, &n, lim, " ncfg=");
    e_dec(line, &n, lim, (uint32_t)d->ncfg);
    e_emit(line, n, lim);
}

/* 配置描述符打印行 */
static void e_log_cfg(const usb_dev_t *d) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: cfg value=");
    e_dec(line, &n, lim, (uint32_t)d->cfg_value);
    e_str(line, &n, lim, " total=");
    e_dec(line, &n, lim, (uint32_t)d->cfg_total);
    e_str(line, &n, lim, " nif=");
    e_dec(line, &n, lim, (uint32_t)d->nif);
    e_emit(line, n, lim);
}

/* 接口打印行：USB-ENUM: if<n> cls=03 sub=01 proto=01 hid */
static void e_log_if(int ifnum, uint8_t cls, uint8_t sub, uint8_t proto, int is_hid) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: if");
    e_dec(line, &n, lim, (uint32_t)ifnum);
    e_str(line, &n, lim, " cls=");
    e_hex(line, &n, lim, (uint32_t)cls, 2);
    e_str(line, &n, lim, " sub=");
    e_hex(line, &n, lim, (uint32_t)sub, 2);
    e_str(line, &n, lim, " proto=");
    e_hex(line, &n, lim, (uint32_t)proto, 2);
    if (is_hid) e_str(line, &n, lim, " HID");
    e_emit(line, n, lim);
}

/* 端点打印行：USB-ENUM: if<n> ep=81 attr=03 mps=8 int=10 */
static void e_log_ep(int ifnum, uint8_t ep, uint8_t attr, uint16_t mps, uint8_t iv) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: if");
    e_dec(line, &n, lim, (uint32_t)ifnum);
    e_str(line, &n, lim, " ep=");
    e_hex(line, &n, lim, (uint32_t)ep, 2);
    e_str(line, &n, lim, " attr=");
    e_hex(line, &n, lim, (uint32_t)attr, 2);
    e_str(line, &n, lim, " mps=");
    e_dec(line, &n, lim, (uint32_t)mps);
    e_str(line, &n, lim, " int=");
    e_dec(line, &n, lim, (uint32_t)iv);
    e_emit(line, n, lim);
}

/* HID 描述符打印行 */
static void e_log_hid(uint16_t bcd, uint8_t country, uint16_t rlen) {
    char line[128];
    int n = 0;
    int lim = (int)sizeof(line) - 1;
    e_str(line, &n, lim, "USB-ENUM: hid bcd=0x");
    e_hex(line, &n, lim, (uint32_t)bcd, 4);
    e_str(line, &n, lim, " country=");
    e_dec(line, &n, lim, (uint32_t)country);
    e_str(line, &n, lim, " rlen=");
    e_dec(line, &n, lim, (uint32_t)rlen);
    e_emit(line, n, lim);
}

/*
 * 解析配置描述符：顺序扫描 bLength/bDescriptorType 链表。
 * 只认三类：配置(0x02)、接口(0x04)、端点(0x05)。
 * 取第一个 HID 接口（class 0x03）及其第一个中断 IN 端点。
 */
static void parse_config(const uint8_t *c, int n, usb_dev_t *d) {
    int off = 0;
    int cur_if = -1;
    while (off + 2 <= n) {
        uint8_t len  = c[off];
        uint8_t type = c[off + 1];
        if (len < 2) break;              /* 防死循环 / 畸形描述符 */
        if (off + len > n) break;        /* 越界即停，不读半截 */
        if (type == 0x04 && len >= 9) {
            cur_if = (int)c[off + 2];
            uint8_t cls   = c[off + 5];
            uint8_t sub   = c[off + 6];
            uint8_t proto = c[off + 7];
            int is_hid = (cls == 0x03u);
            if (is_hid && d->hid_if < 0) {
                d->hid_if    = cur_if;
                d->hid_sub   = sub;
                d->hid_proto = proto;
            }
            /* H1c：Mass Storage 接口（class 0x08）同样只记第一个 */
            if (cls == 0x08u && d->msc_if < 0) {
                d->msc_if    = cur_if;
                d->msc_sub   = sub;
                d->msc_proto = proto;
            }
            e_log_if(cur_if, cls, sub, proto, is_hid);
        } else if (type == 0x05 && len >= 7 && cur_if >= 0) {
            uint8_t  ep   = c[off + 2];
            uint8_t  attr = c[off + 3];
            uint16_t mps  = (uint16_t)(c[off + 4] | (c[off + 5] << 8));
            uint8_t  iv   = c[off + 6];
            e_log_ep(cur_if, ep, attr, mps, iv);
            /* 中断 IN（bit7=1 且 attr&3==3）才给 HID 用；只取第一个 */
            if (d->hid_ep == 0u && cur_if == d->hid_if &&
                (ep & 0x80u) != 0u && (attr & 0x03u) == 0x03u) {
                d->hid_ep          = ep;
                d->hid_ep_mps      = mps;
                d->hid_ep_interval = iv;
            }
            /* bulk 端点（attr&3==2）给 MSC 用：IN/OUT 各记第一个 */
            if (cur_if == d->msc_if && (attr & 0x03u) == 0x02u) {
                if ((ep & 0x80u) != 0u && d->msc_ep_in == 0u) {
                    d->msc_ep_in     = ep;
                    d->msc_ep_in_mps = mps;
                } else if ((ep & 0x80u) == 0u && d->msc_ep_out == 0u) {
                    d->msc_ep_out     = ep;
                    d->msc_ep_out_mps = mps;
                }
            }
        } else if (type == 0x21 && len >= 9 && cur_if >= 0 && cur_if == d->hid_if) {
            /* HID 类描述符（0x21）是配置描述符的一部分，紧跟在接口描述符后。
             * 报告描述符长度从这里取——不要单独发 GET_DESCRIPTOR(0x21)：
             * QEMU 的 HID 设备只响应 0x22（报告描述符本体），对 0x21 会 STALL。
             * 布局：[0]len [1]0x21 [2:3]bcdHID [4]country [5]numDesc
             *       [6]下级类型(0x22) [7:8]wReportLength(LE) */
            uint16_t bcd  = (uint16_t)(c[off + 2] | (c[off + 3] << 8));
            d->hid_rep_len = (uint16_t)(c[off + 7] | (c[off + 8] << 8));
            e_log_hid(bcd, c[off + 4], d->hid_rep_len);
        }
        off += len;
    }
}

/* 单个端口的完整枚举。成功返回 0 并填 *out。 */
static int enum_one(uint16_t io, int port, int ls, uint8_t addr, usb_dev_t *out) {
    uint8_t buf[256];
    int act = 0;

    for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = 0;
    out->io       = io;
    out->port     = (uint8_t)port;
    out->addr     = addr;
    out->lowspeed = (uint8_t)(ls ? 1 : 0);
    out->hid_if   = -1;
    out->msc_if   = -1;

    /* 0) 端口复位：设备回到默认态（地址 0）。未连设备直接 fail closed 返回。 */
    if (!uhci_port_reset(io, port)) {
        e_fail(port, "port reset", 0);
        return -1;
    }
    /* GRESET/端口复位后 HC 的 CF/RS 可能已被清掉，传输前必须重新起调度 */
    if (uhci_hc_start(io) != 0) {
        e_fail(port, "hc start", 0);
        return -1;
    }

    /* 1) 设备描述符前 8 字节：只为拿 bMaxPacketSize0（低速恒为 8） */
    {
        uint8_t r8[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
        act = 0;
        if (uhci_control_xfer(io, 0, 0, r8, 1, buf, 8, ls, &act) != 0 || act < 8) {
            e_fail(port, "GET_DESCRIPTOR(dev,8)", act);
            return -1;
        }
        out->mps0 = buf[7];
        if (out->mps0 < 8u) out->mps0 = 8u;      /* 规范最小 8；异常值夹紧 */
        if (out->mps0 > 64u) out->mps0 = 64u;    /* 端点 0 合法上界 */
    }

    /* 2) 完整设备描述符（18 字节） */
    {
        uint8_t r18[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00};
        act = 0;
        if (uhci_control_xfer(io, 0, 0, r18, 1, buf, 18, ls, &act) != 0 || act < 18) {
            e_fail(port, "GET_DESCRIPTOR(dev,18)", act);
            return -1;
        }
        out->vid   = (uint16_t)(buf[8]  | (buf[9]  << 8));
        out->pid   = (uint16_t)(buf[10] | (buf[11] << 8));
        out->cls   = buf[4];
        out->sub   = buf[5];
        out->proto = buf[6];
        out->ncfg  = buf[17];
        e_log_dev(port, out);
    }

    /* 3) SET_ADDRESS：之后按规范等 >=2ms 设备才真正切到新地址 */
    {
        uint8_t sa[8] = {0x00, 0x05, addr, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (uhci_control_xfer(io, 0, 0, sa, 0, NULL, 0, ls, NULL) != 0) {
            e_fail(port, "SET_ADDRESS", 0);
            return -1;
        }
        e_delay_ms(10);
    }

    /* 4) 用新地址重读设备描述符——地址确实生效的最直接证据 */
    {
        uint8_t r18[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00};
        uint8_t chk[32];
        act = 0;
        for (int i = 0; i < (int)sizeof(chk); i++) chk[i] = 0;
        if (uhci_control_xfer(io, addr, 0, r18, 1, chk, 18, ls, &act) != 0 || act < 18) {
            e_fail(port, "GET_DESCRIPTOR(dev)@addr", act);
            return -1;
        }
        uint16_t vid = (uint16_t)(chk[8] | (chk[9] << 8));
        uint16_t pid = (uint16_t)(chk[10] | (chk[11] << 8));
        if (vid != out->vid || pid != out->pid) {
            e_fail(port, "addr verify", act);
            return -1;
        }
    }

    /* 5) 配置描述符前 9 字节：拿 wTotalLength / bConfigurationValue / 接口数 */
    uint16_t total = 9;
    {
        uint8_t c9[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00};
        uint8_t c[16];
        act = 0;
        for (int i = 0; i < (int)sizeof(c); i++) c[i] = 0;
        if (uhci_control_xfer(io, addr, 0, c9, 1, c, 9, ls, &act) != 0 || act < 9) {
            e_fail(port, "GET_DESCRIPTOR(cfg,9)", act);
            return -1;
        }
        total = (uint16_t)(c[2] | (c[3] << 8));
        out->cfg_value = c[5];
        out->nif       = c[4];
        /* 不可信上界：wTotalLength 至少 9（配置头自身），至多取 sizeof(buf) */
        if (total < 9u) total = 9u;
        if (total > (uint16_t)sizeof(buf)) total = (uint16_t)sizeof(buf);
    }

    /* 6) 完整配置描述符并解析接口/端点 */
    {
        uint8_t cf[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00,
                         (uint8_t)(total & 0xFFu), (uint8_t)((total >> 8) & 0xFFu)};
        act = 0;
        if (uhci_control_xfer(io, addr, 0, cf, 1, buf, (int)total, ls, &act) != 0 || act < 9) {
            e_fail(port, "GET_DESCRIPTOR(cfg,full)", act);
            return -1;
        }
        out->cfg_total = (uint16_t)act;
        e_log_cfg(out);
        parse_config(buf, act, out);
    }

    /* 7) SET_CONFIGURATION：让设备进入配置态（端点可用） */
    {
        uint8_t sc[8] = {0x00, 0x09, out->cfg_value, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (uhci_control_xfer(io, addr, 0, sc, 0, NULL, 0, ls, NULL) != 0) {
            e_fail(port, "SET_CONFIGURATION", 0);
            return -1;
        }
        out->configured = 1;
        dmesg_write("USB-ENUM: SET_CONFIGURATION ok");
    }

    /* 8) HID 报告描述符本体（0x22，经接口请求）。长度已在第 6 步从配置里的
     * 0x21 描述符拿到，这里把报告取回内存——为下一步（H2-2d 报告解析）铺路，
     * 本步只校验"能读回多少字节"这一条证据。失败不影响枚举结论（设备已配置
     * 成功），但必须打印失败行，绝不静默。 */
    if (out->hid_if >= 0 && out->hid_rep_len > 0) {
        uint16_t want = out->hid_rep_len;
        if (want > 255u) want = 255u;                  /* TD maxlen 11 位上界 */
        if (want > (uint16_t)sizeof(buf)) want = (uint16_t)sizeof(buf);
        uint8_t rr[8] = {0x81, 0x06, 0x00, 0x22,
                         (uint8_t)(out->hid_if & 0xFFu), 0x00,
                         (uint8_t)(want & 0xFFu), 0x00};
        act = 0;
        if (uhci_control_xfer(io, addr, 0, rr, 1, buf, (int)want, ls, &act) != 0
            || act < 4) {
            e_fail(port, "GET_DESCRIPTOR(report)", act);
        } else {
            char line[128];
            int n = 0;
            int lim = (int)sizeof(line) - 1;
            e_str(line, &n, lim, "USB-ENUM: hid report ok len=");
            e_dec(line, &n, lim, (uint32_t)act);
            e_emit(line, n, lim);
        }
    }

    return 0;
}

/* ---------- 入口 ---------- */
void usbenum_init(void) {
    int nports = uhci_port_count();

    g_dev_n = 0;
    for (int i = 0; i < USBENUM_MAX_DEV; i++) {
        usb_dev_t *d = &g_dev[i];
        d->io = 0; d->port = 0; d->addr = 0; d->lowspeed = 0;
        d->vid = 0; d->pid = 0; d->cls = 0; d->sub = 0; d->proto = 0;
        d->mps0 = 0; d->ncfg = 0; d->cfg_value = 0; d->cfg_total = 0;
        d->nif = 0; d->hid_if = -1; d->hid_sub = 0; d->hid_proto = 0;
        d->hid_ep = 0; d->hid_ep_mps = 0; d->hid_ep_interval = 0;
        d->hid_rep_len = 0; d->configured = 0;
        d->msc_if = -1; d->msc_sub = 0; d->msc_proto = 0;
        d->msc_ep_in = 0; d->msc_ep_in_mps = 0;
        d->msc_ep_out = 0; d->msc_ep_out_mps = 0;
    }

    if (nports <= 0) {
        dmesg_write("USB-ENUM: no connected port, skip");
    } else {
        char line[128];
        int n = 0;
        int lim = (int)sizeof(line) - 1;
        e_str(line, &n, lim, "USB-ENUM: begin ports=");
        e_dec(line, &n, lim, (uint32_t)nports);
        e_emit(line, n, lim);

        for (int i = 0; i < nports && g_dev_n < USBENUM_MAX_DEV; i++) {
            uint16_t io = 0;
            int port = 0;
            int ls = 0;
            if (uhci_port_get(i, &io, &port, &ls) != 0) continue;
            uint8_t addr = (uint8_t)(g_dev_n + 1);   /* 地址 1..127 顺序分配 */
            if (enum_one(io, port, ls, addr, &g_dev[g_dev_n]) == 0)
                g_dev_n++;
        }
    }

    {
        char line[128];
        int n = 0;
        int lim = (int)sizeof(line) - 1;
        /* 前缀必须带 "USB-ENUM:"：E2E 按前缀收集诊断行（别处也可能出现
         * "device(s) enumerated" 字样，没有前缀就会收集错行）。 */
        e_str(line, &n, lim, "USB-ENUM: ");
        e_dec(line, &n, lim, (uint32_t)g_dev_n);
        e_str(line, &n, lim, " device(s) enumerated");
        e_emit(line, n, lim);
    }
}

int usbenum_device_count(void) {
    return g_dev_n;
}

const usb_dev_t *usbenum_get(int i) {
    if (i < 0 || i >= g_dev_n) return NULL;
    return &g_dev[i];
}
