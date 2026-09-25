/*
 * serial.c - COM1 (0x3F8) 16550 UART 调试输出
 *
 * 设计要点：
 *  - 只在 serial_init() 做一次环回自检（MCR loopback：写入的字节应从
 *    接收端原样读回）。真机无串口/被占时自检失败，serial_ready 保持 0，
 *    后续 serial_putc 变成无害 no-op——绝不因串口缺失拖死启动。
 *  - 输出走 LSR.THRRE 忙等。串口只在 init/日志/panic 路径使用，
 *    忙等可接受；115200 下一行 ~60 字节 <1ms（FIFO 开启后更短）。
 *  - 不做接收：调试输出是单向的，收包路径等真机联网调试再说。
 */
#include "serial.h"
#include "port.h"

/* 16550 寄存器偏移（DLAB=0/1） */
#define COM1            0x3F8
#define REG_DATA        0   /* RBR/THR (DLAB=0) */
#define REG_IER         1   /* 中断允许 / DLAB=1 时除数低字节 */
#define REG_FCR         2   /* FIFO 控制（写） */
#define REG_LCR         3   /* 线路控制；DLAB 位 = 0x80 */
#define REG_MCR         4   /* MODEM 控制；LOOP 位 = 0x10 */
#define REG_LSR         5   /* 线路状态 */
#define DLL_REG         0   /* DLAB=1: 除数低 */
#define DLM_REG         1   /* DLAB=1: 除数高 */

#define LSR_THRRE       0x20    /* THR 空，可写 */
#define LSR_DATA        0x01    /* 收到数据 */

#define BAUD_DIVISOR    1       /* 115200 = 115200/divisor */

static int g_serial_ok = 0;

/* 带超时的寄存器等待：约 100000 次轮询上限，防真机硬件挂死启动 */
static int lsr_wait(unsigned char mask) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(COM1 + REG_LSR) & mask) != 0) return 1;
    }
    return 0;
}

void serial_init(void) {
    g_serial_ok = 0;

    /* 关中断，配置波特率（DLAB=1） */
    outb(COM1 + REG_IER, 0x00);
    outb(COM1 + REG_LCR, 0x80);
    outb(COM1 + DLL_REG, (unsigned char)(BAUD_DIVISOR & 0xFF));
    outb(COM1 + DLM_REG, (unsigned char)((BAUD_DIVISOR >> 8) & 0xFF));

    /* 8 数据位、无校验、1 停止位，清 DLAB */
    outb(COM1 + REG_LCR, 0x03);

    /* 开 FIFO：清空 + 14 字节阈值 */
    outb(COM1 + REG_FCR, 0xC7);

    /* 环回自检：MODEM 控制器 LOOP 位，发 0xAE 应原样收回 */
    outb(COM1 + REG_MCR, 0x10 | 0x03);          /* LOOP | DTR|RTS */
    outb(COM1 + REG_DATA, 0xAE);
    if (!lsr_wait(LSR_DATA)) {
        outb(COM1 + REG_MCR, 0x00);
        return;                                  /* 无硬件：保持不可用 */
    }
    if (inb(COM1 + REG_DATA) != 0xAE) {
        outb(COM1 + REG_MCR, 0x00);
        return;                                  /* 回读不符：坏 UART */
    }
    outb(COM1 + REG_MCR, 0x03);                  /* 退出环回，正常 DTR|RTS */

    g_serial_ok = 1;
}

void serial_putc(char c) {
    if (!g_serial_ok) return;
    if (c == '\n') {
        if (!lsr_wait(LSR_THRRE)) return;
        outb(COM1 + REG_DATA, '\r');
    }
    if (!lsr_wait(LSR_THRRE)) return;
    outb(COM1 + REG_DATA, (unsigned char)c);
}

void serial_write(const char *s) {
    if (!s || !g_serial_ok) return;
    while (*s) serial_putc(*s++);
}

int serial_ready(void) {
    return g_serial_ok;
}
