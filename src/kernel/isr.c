#include "idt.h"
#include "isr.h"
#include "port.h"

extern void irq1();
extern void irq12();

void isr_install(void) {
    /* 暂时无异常处理 */
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

    /* 允许 IRQ1（键盘）与 IRQ2（级联到从 PIC） */
    outb(0x21, 0xF9);   /* 主 PIC: 允许 IRQ1 + IRQ2 */
    outb(0xA1, 0xEF);   /* 从 PIC: 允许 IRQ4（全局 IRQ12） */

    /* 注册 IRQ1 与 IRQ12 处理函数 */
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
}
