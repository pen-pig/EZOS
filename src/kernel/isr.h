#ifndef ISR_H
#define ISR_H

#include "types.h"

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ12 44

void isr_install(void);
void irq_install(void);

#endif
