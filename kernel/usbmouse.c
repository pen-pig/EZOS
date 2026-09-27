/*
 * usbmouse.c - USB HID 鼠标：boot 报告解析 + 轮询（真机点亮 H2-2e）
 *
 * 报告格式（USB HID boot interface，鼠标协议）：
 *   [0]     按键位图：bit0 左键 bit1 右键 bit2 中键（bit3..7 保留/填充）
 *   [1]     X 位移，int8（正 = 向右）
 *   [2]     Y 位移，int8（正 = 向下）
 *   [3]     Wheel 位移，int8（可选，只有 >=4 字节的报告才认）
 *
 * 与 PS/2 的坐标差异必须在这一层吃掉：PS/2 的 Y 正值是"向上"，而 USB boot
 * 报告的 Y 正值是"向下"。统一成屏幕坐标（dy>0 向下）后再交给 mouse 层，
 * 这样 GUI 侧完全不需要知道指针来自哪条总线。
 *
 * 红线落实：
 *  - 不可信输入上界：报告不足 3 字节直接丢弃；缓冲固定 8 字节，mps 夹紧。
 *  - 轮询绝不挂死：uhci 层有超时，这里额外做 5ms 节流 + **IF=0 直接返回**
 *    （IF=0 时 g_pit_ticks 不前进，uhci 的超时等待会变成永久挂死）。
 *  - 与 PS/2 共用状态：注入走 mouse_inject_report()，由 mouse.c 负责关中断
 *    保护，本模块不直接碰指针坐标。
 *  - 静默原则：NAK（没有新报告）是常态，绝不打印；诊断行只在真的收到报告
 *    或真的有位移/按键时打，并对"移动事件"再做 250ms 节流防刷屏。
 */
#include "usbmouse.h"
#include "usbenum.h"
#include "uhci.h"
#include "mouse.h"
#include "isr.h"
#include "irqflags.h"
#include "dmesg.h"
#include "types.h"

#define USBMOU_THROTTLE_MS 5u      /* 轮询节流：鼠标 interval 通常 10ms */
#define USBMOU_REPORT_MIN  3u      /* boot 鼠标报告最小 3 字节 */
#define USBMOU_REPORT_LEN  8u      /* 缓冲上界 */
#define USBMOU_EMIT_MS     250u    /* 移动诊断行节流 */

static const usb_dev_t *g_mou = NULL;   /* 认领到的鼠标（指向 usbenum 的静态表） */
static uint32_t g_last_poll = 0;        /* 上次真正轮询的 tick */
static uint32_t g_last_emit = 0;        /* 上次打移动行的 tick */
static int      g_last_btn = 0;         /* 上次按键状态（变化才算事件） */
static int      g_got_report = 0;       /* 至少收到过一个合法报告 */

/* ---------- 手写格式化（内核无 sprintf） ---------- */
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

/* 有符号十进制（位移是 int8，可能为负） */
static void m_sdec(char *b, int *n, int lim, int v) {
    uint32_t u = (v < 0) ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v;
    if (v < 0) m_str(b, n, lim, "-");
    m_dec(b, n, lim, u);
}

static void m_emit(char *b, int n, int lim) {
    if (n < lim) b[n] = 0; else b[lim] = 0;
    dmesg_write(b);
}

/* ---------- 入口：认领鼠标并切 boot 协议 ---------- */
void usbmouse_init(void) {
    int n = usbenum_device_count();

    g_mou = NULL;
    g_got_report = 0;
    g_last_btn = 0;

    for (int i = 0; i < n; i++) {
        const usb_dev_t *d = usbenum_get(i);
        if (!d) continue;
        /* boot 鼠标的判据：HID 接口 + subclass=1(boot) + protocol=2(鼠标)，
         * 且枚举阶段已 SET_CONFIGURATION 成功、找到了中断 IN 端点。
         * 注意 hid_ep_mps 可能小于 8，真正的报告长度以实际 actlen 为准。 */
        if (d->hid_if >= 0 && d->hid_sub == 1u && d->hid_proto == 2u &&
            d->hid_ep != 0u && d->configured) {
            g_mou = d;
            break;
        }
    }

    mouse_usb_set_present(g_mou ? 1 : 0);

    if (!g_mou) {
        dmesg_write("USB-MOU: no HID boot mouse");
        return;
    }

    /*
     * SET_PROTOCOL(boot=0)：让设备用固定的 boot 报告格式（buttons/dx/dy）。
     * 失败不致命（多数鼠标默认就是 boot 格式），但要如实打印。
     *
     * **本驱动故意不发 SET_IDLE**：boot 协议下设备的默认 idle 就是"无限"
     *（变化即报告），与 SET_IDLE(0) 等价，而实测 QEMU 的 usb-mouse 在收到
     * 显式 SET_IDLE 之后就不再在中断端点上报（中断 IN 恒 NAK，控制端点的
     * GET_REPORT 也拿不到数据）——不发 SET_IDLE 反而稳定收到报告。真机上
     * 不发同样安全：默认 idle=无限是所有 boot 鼠标的出厂行为。
     */
    uint8_t ifnum = (uint8_t)(g_mou->hid_if & 0xFFu);
    uint8_t sp[8] = {0x21, 0x0B, 0x00, 0x00, ifnum, 0x00, 0x00, 0x00};
    int rp = uhci_control_xfer(g_mou->io, g_mou->addr, 0, sp, 0, NULL, 0,
                               g_mou->lowspeed, NULL);

    {
        char line[128];
        int n2 = 0;
        int lim = (int)sizeof(line) - 1;
        m_str(line, &n2, lim, "USB-MOU: boot mouse addr=");
        m_dec(line, &n2, lim, (uint32_t)g_mou->addr);
        m_str(line, &n2, lim, " ep=");
        m_dec(line, &n2, lim, (uint32_t)g_mou->hid_ep);
        m_str(line, &n2, lim, " mps=");
        m_dec(line, &n2, lim, (uint32_t)g_mou->hid_ep_mps);
        m_str(line, &n2, lim, " int=");
        m_dec(line, &n2, lim, (uint32_t)g_mou->hid_ep_interval);
        m_str(line, &n2, lim, (rp == 0) ? " proto=ok" : " proto=fail");
        m_emit(line, n2, lim);
    }
}

/*
 * 轮询主体：读一份报告并注入。所有提前返回都在这里，外层统一清重入标志。
 */
static void poll_once(const usb_dev_t *d) {
    uint8_t rep[USBMOU_REPORT_LEN];
    int act = 0;
    int mps = (int)d->hid_ep_mps;
    if (mps < (int)USBMOU_REPORT_MIN) mps = (int)USBMOU_REPORT_MIN;
    if (mps > (int)sizeof(rep)) mps = (int)sizeof(rep);

    for (int i = 0; i < (int)sizeof(rep); i++) rep[i] = 0;
    int r = uhci_interrupt_in(d->io, d->addr, d->hid_ep, rep, mps,
                              d->lowspeed, &act);
    if (r != 0) return;                          /* 超时/致命错误：静默 */
    if (act < (int)USBMOU_REPORT_MIN) return;    /* NAK 或报告不完整：静默 */

    if (!g_got_report) {
        char line[96];
        int n = 0;
        int lim = (int)sizeof(line) - 1;
        g_got_report = 1;
        m_str(line, &n, lim, "USB-MOU: report ok len=");
        m_dec(line, &n, lim, (uint32_t)act);   /* 实测长度，不写死 */
        m_emit(line, n, lim);
    }

    int btn = rep[0] & 0x07;
    int dx  = (int)(int8_t)rep[1];
    int dy  = (int)(int8_t)rep[2];
    int dz  = (act >= 4) ? (int)(int8_t)rep[3] : 0;

    /* 静止且按键没变：不是事件，直接返回（避免把 0 位移当事件刷屏） */
    if (dx == 0 && dy == 0 && dz == 0 && btn == g_last_btn) return;
    g_last_btn = btn;

    /*
     * 注入：统一成屏幕坐标交给 mouse 层。
     *   X：USB 正 = 右，屏幕 X 正 = 右 -> 原样
     *   Y：USB 正 = 下，屏幕 Y 正 = 下 -> 原样（mouse 层内部已按屏幕坐标累加）
     * 滚轮：USB 报告正 = 上滚，mouse 层的 wheel 语义是 >0 向上滚 -> 原样
     */
    mouse_inject_report(dx, dy, btn, dz);

    if ((uint32_t)(g_pit_ticks - g_last_emit) < USBMOU_EMIT_MS) return;
    g_last_emit = g_pit_ticks;

    {
        char line[128];
        int n = 0;
        int lim = (int)sizeof(line) - 1;
        m_str(line, &n, lim, "USB-MOU: move dx=");
        m_sdec(line, &n, lim, dx);
        m_str(line, &n, lim, " dy=");
        m_sdec(line, &n, lim, dy);
        m_str(line, &n, lim, " btn=");
        m_dec(line, &n, lim, (uint32_t)btn);
        m_str(line, &n, lim, " x=");
        m_dec(line, &n, lim, (uint32_t)mouse_get_x());
        m_str(line, &n, lim, " y=");
        m_dec(line, &n, lim, (uint32_t)mouse_get_y());
        m_emit(line, n, lim);
    }
}

/* ---------- 轮询入口：节流 + 死锁/重入防线，再跑一次主体 ---------- */
void usbmouse_poll(void) {
    const usb_dev_t *d = g_mou;
    if (!d) return;

    /*
     * 与键盘同一条死锁防线：**中断关着就绝不能轮询**。
     * uhci_interrupt_in() 靠 g_pit_ticks 推进做超时等待（IRQ0 驱动），在
     * IF=0 的上下文里 g_pit_ticks 永不前进 —— 那不是"等 3ms"，而是永久挂死。
     */
    uint32_t flags = irq_save_disable();
    int irq_was_on = (flags != 0u);
    irq_restore(flags);
    if (!irq_was_on) return;

    if ((uint32_t)(g_pit_ticks - g_last_poll) < USBMOU_THROTTLE_MS) return;
    g_last_poll = g_pit_ticks;

    /* 重入保护：诊断行会读 mouse_get_x/y()，而那也是本函数的轮询点。
     * 标志必须在**所有**出口清零，所以主体拆成 poll_once 由这里统一收尾。 */
    static int in_poll = 0;
    if (in_poll) return;
    in_poll = 1;
    poll_once(d);
    in_poll = 0;
}

int usbmouse_present(void) {
    return g_mou ? 1 : 0;
}

int usbmouse_got_report(void) {
    return g_got_report;
}
