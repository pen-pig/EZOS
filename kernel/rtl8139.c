/*
 * rtl8139.c - Realtek RTL8139 网卡驱动（阶段 7.1：设备初始化）
 *
 * 寄存器资料来源：Realtek RTL8139D datasheet。
 * 本阶段覆盖：PCI 认领 → COMMAND 开 IO 解码 + BusMaster →
 * CONFIG1 退出低功耗 → CR.RST 软复位（轮询自清，带超时）→ IDR0-5 读 MAC。
 *
 * 不覆盖（后续阶段）：RBSTART 接收环、TSD/TSAD 发送描述符、IMR/ISR 中断。
 *
 * 安全边界：COMMAND 寄存器用 32 位读-改-写（低 16 位 COMMAND、高 16 位
 * STATUS），保留 STATUS 原值——W1C 位写 0 不会误清。只对认领到的
 * 10EC:8139 写配置空间，不碰其它设备。
 */
#include "rtl8139.h"
#include "pci.h"
#include "port.h"
#include "dmesg.h"

#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

/* 寄存器偏移（相对 IO base） */
#define R_IDR0    0x00    /* MAC 地址字节 0-5（上电后即为 EEPROM 有效值） */
#define R_CR      0x37    /* 命令寄存器：bit1 TE, bit2 RE, bit3 RST */
#define R_CONFIG1 0x52    /* 配置 1：写 0 退出低功耗（清 LWAKE/PMEn） */

#define CR_RST    0x10

/* 软复位的轮询上限：datasheet 要求等 RST 自清后再碰其它寄存器。
 * 真机上约 1ms；100000 次端口读足够且仍是有界等待。 */
#define RST_POLL_MAX 100000

static uint8_t  g_present = 0;
static uint16_t g_io_base = 0;
static uint8_t  g_irq     = 0;
static uint8_t  g_mac[6];

static const pci_device_t *find_device(void) {
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (d->vendor_id == RTL8139_VENDOR && d->device_id == RTL8139_DEVICE)
            return d;
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

    /* 开 IO 空间解码 + Bus Master（后续收发 DMA 必需）。 */
    uint32_t cmd = pci_read_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_dword(d->bus, d->dev, d->func, PCI_REG_COMMAND, cmd);

    /* 退出低功耗后软复位；RST 自清零，等它清完才能读 IDR。 */
    outb((uint16_t)(g_io_base + R_CONFIG1), 0x00);
    outb((uint16_t)(g_io_base + R_CR), CR_RST);
    {
        int timeout = RST_POLL_MAX;
        while ((inb((uint16_t)(g_io_base + R_CR)) & CR_RST) != 0) {
            if (--timeout <= 0) {
                dmesg_write("RTL8139: reset timeout (CR.RST never cleared)");
                g_present = 0;
                return -1;
            }
        }
    }

    for (int i = 0; i < 6; i++)
        g_mac[i] = inb((uint16_t)(g_io_base + R_IDR0 + i));

    g_present = 1;
    return 0;
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
