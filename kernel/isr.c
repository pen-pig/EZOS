#include "idt.h"
#include "isr.h"
#include "port.h"

extern void irq0();
extern void irq1();
extern void irq12();

void isr_install(void) {
    /* 暂时无异常处理 */
}

/* PIT 系统时钟 tick 计数（1000Hz，每 tick = 1ms） */
volatile uint32_t g_pit_ticks = 0;

/* PIT channel 0 初始化：rate generator，1000Hz（1193182/1193 ≈ 1000.15Hz） */
void pit_init(void) {
    outb(0x43, 0x36);            /* channel 0, lobyte/hibyte, rate generator, binary */
    outb(0x40, 1193 & 0xFF);
    outb(0x40, (1193 >> 8) & 0xFF);
}

/* PIT 定时器中断处理：真实 tick 计数 + 主 PIC EOI */
void irq0_handler(void) {
    g_pit_ticks++;
    outb(0x20, 0x20);   /* 主 PIC EOI */
}

void irq_install(void) {
    /* 重映射 PIC */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);   /* 主 PIC 起始中断号 0x20 */
    outb(0xA1, 0x28);   /* 从 PIC 起始中断号 0x28 */
    outb(0x21, 0x04);   /* 主 PIC IRQ2 接从 PIC */
    outb(0xA1, 0x02);   /* 从 PIC IRQ9 接主 PIC */
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    /* 允许 IRQ0（PIT）、IRQ1（键盘）与 IRQ2（级联到从 PIC） */
    outb(0x21, 0xF8);   /* 主 PIC: 允许 IRQ0 + IRQ1 + IRQ2 */
    outb(0xA1, 0xEF);   /* 从 PIC: 允许 IRQ4（全局 IRQ12） */

    /* 注册 IRQ0、IRQ1 与 IRQ12 处理函数 */
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
}
