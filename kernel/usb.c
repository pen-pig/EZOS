/*
 * usb.c - USB 主机控制器普查（只读枚举，真机点亮 H2 前置）
 *
 * 设计要点（与约束严格对齐）：
 *  - 只读 PCI 配置空间（pci.c 已枚举好的 g_devices 表），按 class 0x0C +
 *    subclass 0x03 筛 USB 控制器。注意 class 不是 0x0C 的（即使 subclass
 *    是 0x03，例如某些桥）一律不收，避免误报。
 *  - 按 ProgIF 分类：0x00=UHCI、0x10=OHCI、0x20=EHCI、0x30=xHCI、其余
 *    Unknown(0x..)，绝不初始化任何硬件。
 *  - 每个命中设备 klog 一行：
 *        USB: <bus>:<dev>.<func> <VID>:<DID> <类型> bar0=0x... irq=N
 *    并额外 klog 一条汇总：
 *        USB: N controller(s), UHCI x / OHCI y / EHCI z / xHCI w
 *  - 一个都没找到时 klog "USB: no USB controller found"，静默返回。
 *  - 不可信字段上界（红线，参考 ahci.c）：bus/dev/func 来自设备配置空间，
 *    打印前夹紧到合法范围（bus<=255、dev<=31、func<=7），越界跳过该设备；
 *    bar0/irq 同样夹紧，irq==0xFF 或 >=16 视为未接，打印 N/A，fail closed。
 *  - 手写格式化（内核无 sprintf）：行缓冲 128 字节，全部写入以上界为准，
 *    保证结尾留 '\0'，且不越界。
 */
#include "usb.h"
#include "pci.h"
#include "dmesg.h"
#include "types.h"

/* ---------- 缓冲写入助手（手写格式化，避免依赖 printf/sprintf） ---------- */

/* 追加十进制数（v==0 输出 "0"） */
static void app_dec(char *b, int *n, int lim, uint32_t v) {
    char t[12];
    int m = 0;
    if (v == 0) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m && *n < lim) b[(*n)++] = t[--m];
}

/* 追加定长十六进制（digits 个十六进制位，高位补零） */
static void app_hex(char *b, int *n, int lim, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) {
        if (*n < lim) b[(*n)++] = hx[(v >> s) & 0xF];
    }
}

/* 追加字符串（以 lim 为上界，保末尾 '\0'） */
static void app_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

/* ---------- 类型名（按 ProgIF 规范映射） ---------- */
static const char *usb_type_name(uint8_t prog_if, char *unk, int unksz) {
    switch (prog_if) {
    case 0x00: return "UHCI";
    case 0x10: return "OHCI";
    case 0x20: return "EHCI";
    case 0x30: return "xHCI";
    default:
        /* fail closed：未知类型打印 "Unknown(0x..)"，不静默吞掉 */
        {
            static const char hx[] = "0123456789ABCDEF";
            int i = 0;
            const char *p = "Unknown(0x";
            while (*p && i < unksz - 1) unk[i++] = *p++;
            unk[i++] = hx[(prog_if >> 4) & 0xF];
            unk[i++] = hx[prog_if & 0xF];
            if (i < unksz - 1) unk[i++] = ')';
            unk[i] = 0;
            return unk;
        }
    }
}

/* ---------- 普查入口 ---------- */
void usb_scan_log(void) {
    int n = pci_device_count();

    int n_uhci = 0, n_ohci = 0, n_ehci = 0, n_xhci = 0, n_other = 0;
    int found = 0;

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (!d) continue;

        /* 只收 class 0x0C + subclass 0x03；class 不是 0x0C 的绝不误收 */
        if (d->class_code != 0x0C || d->subclass != 0x03) continue;

        /* fail closed：不可信字段上界夹紧，越界就跳过该设备。
         * bus 是 uint8_t 必 <=255；dev（0-31）、func（0-7）才是真有约束的位。 */
        uint8_t bus  = d->bus;
        uint8_t dev  = d->dev;
        uint8_t func = d->func;
        if (dev > 31)   continue;     /* 设备号 0-31 */
        if (func > 7)   continue;     /* 功能号 0-7 */

        uint8_t prog_if = d->prog_if;
        /* 已知类型返回静态串；unknown 类型经 unkbuf 拼出 "Unknown(0x..)" */
        char unkbuf[24];
        const char *type = usb_type_name(prog_if, unkbuf, (int)sizeof(unkbuf));

        /* 计数（按类型） */
        switch (prog_if) {
        case 0x00: n_uhci++; break;
        case 0x10: n_ohci++; break;
        case 0x20: n_ehci++; break;
        case 0x30: n_xhci++; break;
        default:   n_other++; break;
        }
        found++;

        /* bar0：本身就是 32 位寄存器值，打印原始值（低 4 位含类型编码，
         * 比屏蔽地址更能反映寄存器真实状态）。写入严格以上界为准。 */
        uint32_t bar0 = d->bar[0];

        /* irq：BIOS/固件填的 PIC IRQ。0xFF 表示未连接，>=16 超出 PIC
         * 有效范围，二者都视为无效，打印 N/A，避免输出垃圾值。 */
        uint8_t irq = d->intr_line;
        int irq_valid = (irq != 0xFFu) && (irq < 16u);

        /* 组装一行 klog（手写格式化，无 sprintf） */
        char line[128];
        int li = 0;
        int lim = (int)sizeof(line) - 1;
        app_str(line, &li, lim, "USB: ");
        app_dec(line, &li, lim, bus);
        app_str(line, &li, lim, ":");
        app_dec(line, &li, lim, dev);
        app_str(line, &li, lim, ".");
        app_dec(line, &li, lim, func);
        app_str(line, &li, lim, " ");
        app_hex(line, &li, lim, d->vendor_id, 4);
        app_str(line, &li, lim, ":");
        app_hex(line, &li, lim, d->device_id, 4);
        app_str(line, &li, lim, " ");
        app_str(line, &li, lim, type);
        app_str(line, &li, lim, " bar0=0x");
        app_hex(line, &li, lim, bar0, 8);
        app_str(line, &li, lim, " irq=");
        if (irq_valid) app_dec(line, &li, lim, irq);
        else           app_str(line, &li, lim, "N/A");
        if (li < lim) line[li] = 0;
        else          line[lim] = 0;
        dmesg_write(line);
    }

    if (found == 0) {
        dmesg_write("USB: no USB controller found");
        return;
    }

    /* 汇总行 */
    char sum[128];
    int li = 0;
    int lim = (int)sizeof(sum) - 1;
    app_str(sum, &li, lim, "USB: ");
    app_dec(sum, &li, lim, (uint32_t)found);
    app_str(sum, &li, lim, " controller(s), UHCI ");
    app_dec(sum, &li, lim, (uint32_t)n_uhci);
    app_str(sum, &li, lim, " / OHCI ");
    app_dec(sum, &li, lim, (uint32_t)n_ohci);
    app_str(sum, &li, lim, " / EHCI ");
    app_dec(sum, &li, lim, (uint32_t)n_ehci);
    app_str(sum, &li, lim, " / xHCI ");
    app_dec(sum, &li, lim, (uint32_t)n_xhci);
    if (li < lim) sum[li] = 0;
    else          sum[lim] = 0;
    dmesg_write(sum);
}
