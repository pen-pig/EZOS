#include "idt.h"
#include "isr.h"
#include "port.h"
#include "task.h"
#include "net.h"

extern void irq0();
extern void irq1();
extern void irq12();
extern void irq7s();
extern void irq15s();

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

/* PIT 定时器中断处理：真实 tick 计数 + 抢占式调度
 *
 * 顺序必须是"先 EOI、后调度"：schedule() 可能切换出去，本任务要过很久
 * 才被切回来；EOI 若放在后面，PIC 在收到 EOI 前不再投递任何中断——
 * 整个系统的 IRQ 会被这个"睡着了"的任务卡住。
 */
void irq0_handler(void) {
    g_pit_ticks++;
    outb(0x20, 0x20);   /* 主 PIC EOI（必须在 schedule 之前） */
    task_timer_tick();  /* 步骤 8a：到点的定时睡眠者先回 READY，再进调度 */
    net_tick();         /* 步骤 7.4：TCP 重传定时器（内部按 100ms 节流） */
    schedule();
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

    /* spurious IRQ7 (8259 spec): line withdrawn before INTA. With
     * level-triggered PCI IRQs (PIIX ELCR) this happens whenever the
     * NIC deasserts INTA while the CPU still has IF=0 (poll-path RX).
     * Vector 39 = master spurious, 47 = slave spurious. */
    idt_set_gate(39, (uint32_t)irq7s, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15s, 0x08, 0x8E);
}

/* ---- spurious IRQ7 (8259 spec) ----
 * On INTA with no valid request the PIC issues IRQ7 (the slave's goes
 * up through the cascade as vector 0x2F = 47). Distinguish real vs
 * spurious via the in-service register:
 *   real IRQ7/15: normal EOI (slave also needs the master cascade EOI);
 *   spurious: no in-service bit, must NOT EOI that PIC (it would clear
 * a lower-priority in-service interrupt), but a slave spurious still
 * must EOI the master - cascade IRQ2 is genuinely in-service. */
void irq7s_handler(void) {
    outb(0x20, 0x0B);              /* OCW3: read ISR */
    uint8_t isr = inb(0x20);
    if (isr & 0x80)
        outb(0x20, 0x20);          /* real IRQ7: master EOI */
    /* spurious: no EOI */
}

void irq15s_handler(void) {
    outb(0xA0, 0x0B);              /* OCW3: read slave ISR */
    uint8_t isr = inb(0xA0);
    if (isr & 0x80)
        outb(0xA0, 0x20);          /* real IRQ15: slave EOI */
    outb(0x20, 0x20);              /* cascade was in-service: master EOI */
}
