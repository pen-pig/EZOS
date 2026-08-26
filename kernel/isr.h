#ifndef ISR_H
#define ISR_H

#include "types.h"

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ12 44

void isr_install(void);
void irq_install(void);
void pit_init(void);

/* PIT 1000Hz 系统时钟 tick 计数（每 tick = 1ms），供启动日志真实计时 */
extern volatile uint32_t g_pit_ticks;

#endif
