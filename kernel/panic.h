/*
 * panic.h - CPU 异常处理 + panic 屏幕
 *
 * 依赖（步骤 1a，boot/kernel_entry.asm）：
 *   - 32 个异常桩 isr_stub_0..31（0x8E 门，ring0）
 *   - isr_common：pusha 后调 isr_dispatch(regs)，返回后 popa/iret
 *   - isr_stub_table[]：桩地址表，C 侧遍历挂 IDT
 *
 * 栈布局（ring0 同级触发，无 ESP/SS 切换压栈）：
 *   regs+0..31  pusha（edi,esi,ebp,esp占位,ebx,edx,ecx,eax）
 *   regs+32     向量号（桩压）
 *   regs+36     错误码（无码向量垫 0）
 *   regs+40     EIP（CPU 压）
 *   regs+44     CS
 *   regs+48     EFLAGS
 */
#ifndef PANIC_H
#define PANIC_H

#include "types.h"

typedef struct {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* 0..31 */
    uint32_t vec;                                            /* 32 */
    uint32_t err;                                            /* 36 */
    uint32_t eip, cs, eflags;                                /* 40.. */
    /* 52/56：只有 (cs & 3) != 0（即 ring3 陷入、发生特权切换）时 CPU 才会压入。
     * ring0 同级异常不压 ESP/SS，这两个字段是栈上残留数据，不可读。 */
    uint32_t useresp, ss;
} isr_regs_t;

/* 挂 IDT 0-31（idt_init 之后、irq_install 之前调用一次） */
void isr_register_stubs(void);

/* 汇编 isr_common 调入；不直接使用 */
void isr_dispatch(isr_regs_t *regs);

/* panic 上下文记录（供异常屏显示"出事时在干什么"） */
void panic_set_context(const char *what);

#endif
