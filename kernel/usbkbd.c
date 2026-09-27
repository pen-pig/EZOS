/*
 * usbkbd.c - USB HID 键盘：boot 报告解析 + 轮询（真机点亮 H2-2d）
 *
 * 报告格式（USB HID boot interface，键盘协议）：
 *   [0]     修饰键位图：bit0 LCtrl bit1 LShift bit2 LAlt bit3 LGUI
 *                       bit4 RCtrl bit5 RShift bit6 RAlt bit7 RGUI
 *   [1]     保留
 *   [2..7]  键码数组（最多 6 键同时按下，0 = 空位）
 *
 * 判定"新按下"：把当前报告的键码与上一份报告比对，只处理**上一份里没有**的
 * 键码。这样按住不放不会刷出一串重复字符（boot 协议没有 typematic，设备会
 * 周期性重发同一份报告）。
 *
 * 红线落实：
 *  - 不可信输入上界：报告长度不足 8 字节直接丢弃；键码走映射表/switch，
 *    无越界下标。
 *  - 轮询绝不挂死：中断轮询本身有超时（uhci 层），这里额外做 5ms 节流，
 *    键盘的 shell 忙等循环因此最多每 5ms 才真正碰一次硬件。
 *  - 与 PS/2 共用缓冲：注入走 keyboard_inject()，由 keyboard.c 负责关中断
 *    保护，本模块不直接碰缓冲。
 *  - 静默原则：NAK（没有新报告）是每 10ms 发生一次的常态，绝不打印。
 */
#include "usbkbd.h"
#include "usbenum.h"
#include "uhci.h"
#include "keyboard.h"
#include "isr.h"
#include "irqflags.h"
#include "dmesg.h"
#include "types.h"

#define USBKBD_THROTTLE_MS 5u      /* 轮询节流：键盘 interval=10ms */
#define USBKBD_REPORT_LEN  8u      /* boot 协议键盘报告固定 8 字节 */

static const usb_dev_t *g_kbd = NULL;   /* 认领到的键盘（指向 usbenum 的静态表） */
static uint8_t  g_prev[8];              /* 上一份报告（去重用） */
static uint32_t g_last_poll = 0;        /* 上次真正轮询的 tick */
static int      g_got_report = 0;       /* 至少收到过一个合法报告 */

/* ---------- 手写格式化（内核无 sprintf） ---------- */
static void k_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

static void k_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

static void k_hex2(char *b, int *n, int lim, uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    if (*n < lim) b[(*n)++] = hx[(v >> 4) & 0xF];
    if (*n < lim) b[(*n)++] = hx[v & 0xF];
}

static void k_emit(char *b, int n, int lim) {
    if (n < lim) b[n] = 0; else b[lim] = 0;
    dmesg_write(b);
}

/*
 * HID usage ID (Keyboard/Keypad page 0x07) -> 内核键码。
 * 返回 0 = 该键没有可注入的字符（CapsLock / 未知键）。
 */
static int hid_map(uint8_t k, int shift) {
    /* 0x04..0x1D: a..z（HID 键码是"物理键位"，布局无关） */
    if (k >= 0x04u && k <= 0x1Du) {
        char c = (char)('a' + (int)(k - 0x04u));
        return shift ? (int)(c - 'a' + 'A') : (int)c;
    }
    /* 0x1E..0x27: 1..9,0（主键盘数字行，shift 给出符号） */
    if (k >= 0x1Eu && k <= 0x27u) {
        static const char d[]  = "1234567890";
        static const char ds[] = "!@#$%^&*()";
        int i = (int)(k - 0x1Eu);
        return shift ? (int)ds[i] : (int)d[i];
    }
    switch (k) {
    case 0x28u: return (int)'\n';            /* Enter */
    case 0x29u: return 27;                   /* Esc */
    case 0x2Au: return (int)'\b';            /* Backspace */
    case 0x2Bu: return (int)'\t';            /* Tab */
    case 0x2Cu: return (int)' ';             /* Space */
    case 0x2Du: return shift ? '_'  : '-';
    case 0x2Eu: return shift ? '+'  : '=';
    case 0x2Fu: return shift ? '{'  : '[';
    case 0x30u: return shift ? '}'  : ']';
    case 0x31u: return shift ? '|'  : '\\';
    case 0x33u: return shift ? ':'  : ';';
    case 0x34u: return shift ? '"'  : '\'';
    case 0x35u: return shift ? '~'  : '`';
    case 0x36u: return shift ? '<'  : ',';
    case 0x37u: return shift ? '>'  : '.';
    case 0x38u: return shift ? '?'  : '/';
    case 0x39u: return 0;                    /* CapsLock：无字符 */
    case 0x4Fu: return KEY_RIGHT;
    case 0x50u: return KEY_LEFT;
    case 0x51u: return KEY_DOWN;
    case 0x52u: return KEY_UP;
    default: break;
    }
    /* 0x3A..0x45: F1..F12（KEY_F1 = -7，往后递减） */
    if (k >= 0x3Au && k <= 0x45u) return KEY_F1 - (int)(k - 0x3Au);
    return 0;
}

/* ---------- 入口：认领键盘并切 boot 协议 ---------- */
void usbkbd_init(void) {
    int n = usbenum_device_count();

    g_kbd = NULL;
    g_got_report = 0;
    for (int i = 0; i < (int)sizeof(g_prev); i++) g_prev[i] = 0;

    for (int i = 0; i < n; i++) {
        const usb_dev_t *d = usbenum_get(i);
        if (!d) continue;
        /* boot 键盘的判据：HID 接口 + subclass=1(boot) + protocol=1(键盘)，
         * 且枚举阶段已 SET_CONFIGURATION 成功、找到了中断 IN 端点。 */
        if (d->hid_if >= 0 && d->hid_sub == 1u && d->hid_proto == 1u &&
            d->hid_ep != 0u && d->configured) {
            g_kbd = d;
            break;
        }
    }

    if (!g_kbd) {
        dmesg_write("USB-KBD: no HID boot keyboard");
        return;
    }

    /* SET_PROTOCOL(boot=0) + SET_IDLE(0)：让设备用固定的 8 字节 boot 报告。
     * 两者失败都不致命（多数设备默认就是 boot 格式），但要如实打印。 */
    uint8_t ifnum = (uint8_t)(g_kbd->hid_if & 0xFFu);
    uint8_t sp[8] = {0x21, 0x0B, 0x00, 0x00, ifnum, 0x00, 0x00, 0x00};
    uint8_t si[8] = {0x21, 0x0A, 0x00, 0x00, ifnum, 0x00, 0x00, 0x00};
    int rp = uhci_control_xfer(g_kbd->io, g_kbd->addr, 0, sp, 0, NULL, 0,
                               g_kbd->lowspeed, NULL);
    int ri = uhci_control_xfer(g_kbd->io, g_kbd->addr, 0, si, 0, NULL, 0,
                               g_kbd->lowspeed, NULL);

    {
        char line[128];
        int n2 = 0;
        int lim = (int)sizeof(line) - 1;
        k_str(line, &n2, lim, "USB-KBD: boot kbd addr=");
        k_dec(line, &n2, lim, (uint32_t)g_kbd->addr);
        k_str(line, &n2, lim, " ep=");
        k_hex2(line, &n2, lim, (uint32_t)g_kbd->hid_ep);
        k_str(line, &n2, lim, " mps=");
        k_dec(line, &n2, lim, (uint32_t)g_kbd->hid_ep_mps);
        k_str(line, &n2, lim, " int=");
        k_dec(line, &n2, lim, (uint32_t)g_kbd->hid_ep_interval);
        k_str(line, &n2, lim, (rp == 0) ? " proto=ok" : " proto=fail");
        k_str(line, &n2, lim, (ri == 0) ? " idle=ok" : " idle=fail");
        k_emit(line, n2, lim);
    }
}

/* ---------- 轮询：读一份报告，注入新按下的键 ---------- */
void usbkbd_poll(void) {
    const usb_dev_t *d = g_kbd;
    if (!d) return;

    /*
     * 关键前置检查：**中断关着就绝不能轮询**。
     * uhci_interrupt_in() 靠 g_pit_ticks 推进做超时等待（IRQ0 驱动），在
     * IF=0 的上下文（task_lock 临界区、IRQ 处理函数内部）里 g_pit_ticks 永远
     * 不前进 —— 那不是"等 3ms"，而是**永久挂死**。shell 正是在 task_lock()
     * 里读键盘的，所以这个检查不是防御性装饰，是死锁的唯一防线。
     */
    uint32_t flags = irq_save_disable();
    int irq_was_on = (flags != 0u);
    irq_restore(flags);
    if (!irq_was_on) return;

    /* 节流：既符合设备 interval，也避免 shell 忙等循环把 UHCI 轮询跑满 */
    uint32_t now = g_pit_ticks;
    if ((uint32_t)(now - g_last_poll) < USBKBD_THROTTLE_MS) return;
    g_last_poll = now;

    uint8_t rep[8];
    int act = 0;
    int mps = (int)d->hid_ep_mps;
    if (mps < 1) mps = 8;
    if (mps > (int)sizeof(rep)) mps = (int)sizeof(rep);

    for (int i = 0; i < (int)sizeof(rep); i++) rep[i] = 0;
    int r = uhci_interrupt_in(d->io, d->addr, d->hid_ep, rep, mps,
                              d->lowspeed, &act);
    if (r != 0) return;              /* 超时/致命错误：静默（下一轮再来） */
    if (act < (int)USBKBD_REPORT_LEN) return;   /* NAK 或报告不完整：静默 */

    if (!g_got_report) {
        char line[96];
        int n = 0;
        int lim = (int)sizeof(line) - 1;
        g_got_report = 1;
        k_str(line, &n, lim, "USB-KBD: report ok len=");
        k_dec(line, &n, lim, (uint32_t)act);   /* 实测长度，不写死 8 */
        k_emit(line, n, lim);
    }

    uint8_t mod = rep[0];
    int shift = (mod & 0x22u) ? 1 : 0;    /* bit1 LShift / bit5 RShift */

    for (int i = 2; i < (int)USBKBD_REPORT_LEN; i++) {
        uint8_t code = rep[i];
        if (code == 0u) continue;
        /* 去重：只处理上一份报告里没有的键码 */
        int seen = 0;
        for (int j = 2; j < (int)USBKBD_REPORT_LEN; j++) {
            if (g_prev[j] == code) { seen = 1; break; }
        }
        if (seen) continue;

        int key = hid_map(code, shift);
        {
            char line[128];
            int n = 0;
            int lim = (int)sizeof(line) - 1;
            k_str(line, &n, lim, "USB-KBD: key=");
            k_dec(line, &n, lim, (uint32_t)key);
            k_str(line, &n, lim, " mod=");
            k_hex2(line, &n, lim, (uint32_t)mod);
            k_emit(line, n, lim);
        }
        if (key != 0) keyboard_inject(key);
    }

    for (int i = 0; i < (int)USBKBD_REPORT_LEN; i++) g_prev[i] = rep[i];
}

int usbkbd_present(void) {
    return g_kbd ? 1 : 0;
}

int usbkbd_got_report(void) {
    return g_got_report;
}
