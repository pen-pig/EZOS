/*
 * panic.c - CPU 异常处理 + panic 屏幕（Linux oops 风格）
 *
 * 32 个异常向量全挂 0x8E ring0 门。触发时：
 *   1) 蓝底白字整屏（经典 BSOD 配色，区别于正常灰底）
 *   2) 异常名 + 向量号 + 错误码（#PF 附 CR2）
 *   3) 寄存器快照（含段寄存器，从汇编侧不可得的部分直接读）
 *   4) panic_set_context 登记的最近上下文（"where it happened"）
 *   5) 停机（cli+hlt 循环）——不尝试恢复，保全现场
 *
 * #BP（int3）为陷阱类：EIP 已越过指令，只打印不停机（shell int3
 * 命令与 system_reboot 用它作可控触发测试）。其余向量一律停机。
 *
 * 不在 panic 路径上做任何堆分配/磁盘 IO——异常处理自身必须零依赖。
 */
#include "panic.h"
#include "idt.h"
#include "tty.h"
#include "port.h"
#include "isr.h"

extern void *isr_stub_table[];    /* kernel_entry.asm */

static const char *vec_name(uint32_t v) {
    switch (v) {
    case 0:  return "#DE Divide Error";
    case 1:  return "#DB Debug";
    case 2:  return "NMI Interrupt";
    case 3:  return "#BP Breakpoint";
    case 4:  return "#OF Overflow";
    case 5:  return "#BR BOUND Range Exceeded";
    case 6:  return "#UD Invalid Opcode";
    case 7:  return "#NM Device Not Available";
    case 8:  return "#DF Double Fault";
    case 9:  return "Coprocessor Segment Overrun";
    case 10: return "#TS Invalid TSS";
    case 11: return "#NP Segment Not Present";
    case 12: return "#SS Stack Fault";
    case 13: return "#GP General Protection";
    case 14: return "#PF Page Fault";
    case 15: return "Reserved";
    case 16: return "#MF x87 FPU Error";
    case 17: return "#AC Alignment Check";
    case 18: return "#MC Machine Check";
    case 19: return "#XM SIMD FP Exception";
    case 20: return "#VE Virtualization";
    default: return "Reserved";
    }
}

static const char *g_panic_ctx = "boot";

void panic_set_context(const char *what) {
    if (what) g_panic_ctx = what;
}

/* 蓝底白字（与正常终端灰底黑字区分，一眼可辨崩溃屏） */
#define PANIC_COLOR 0x1F

static void pputs(const char *s) {
    terminal_setcolor(PANIC_COLOR);
    terminal_writestring(s);
}

static void phex(uint32_t v, int width) {
    terminal_setcolor(PANIC_COLOR);
    static const char h[] = "0123456789ABCDEF";
    char buf[12];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = h[v & 0xF]; v >>= 4; }
    while (n < width) buf[n++] = '0';
    while (n) terminal_putchar(buf[--n]);
}

void isr_register_stubs(void) {
    for (int i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, (uint32_t)isr_stub_table[i], 0x08, 0x8E);
}

static void __attribute__((noreturn)) panic_halt(void) {
    for (;;) asm volatile("cli; hlt");
}

static void dump(isr_regs_t *r) {
    /* 与 pusha 顺序相反：eax 在 regs+28 */
    uint32_t cr0, cr2, cr3;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint32_t ds, es, fs, gs, ss;
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    asm volatile("mov %%ss, %0" : "=r"(ss));

    /* 陷入来源判定：CS 的 RPL 就是被打断代码的 CPL。
     * ring3 陷入时 CPU 在 EFLAGS 之上额外压入用户 ESP/SS，此时 useresp/ss
     * 才是被打断的用户栈；ring0 同级异常没有这两个值，读它们只会得到栈上
     * 残留数据——此前 SS 恒显示内核 0x10，排查用户态崩溃时会被误导成内核崩。 */
    int from_user = ((r->cs & 3u) != 0u);
    uint32_t shown_esp = from_user ? r->useresp : r->esp_dummy;
    uint32_t shown_ss  = from_user ? (r->ss & 0xFFFFu) : ss;

    terminal_setcolor(PANIC_COLOR);
    /* 清屏：整屏刷蓝底空格（直写 VGA 缓冲，绕过滚动逻辑） */
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    uint16_t blank = (uint16_t)(' ' | (PANIC_COLOR << 8));
    for (int i = 0; i < 80 * 25; i++) vga[i] = blank;

    terminal_set_cursor(0, 0);
    pputs("EZOS kernel panic\n\n");
    pputs("  exception: ");
    terminal_setcolor(PANIC_COLOR);
    terminal_writestring(vec_name(r->vec));
    pputs(" (vector ");
    phex(r->vec, 2);
    pputs(", err 0x");
    phex(r->err, 8);
    pputs(")\n");
    if (r->vec == 14) {
        pputs("  page fault address (CR2): 0x");
        phex(cr2, 8);
        pputs("\n  cause: ");
        terminal_setcolor(PANIC_COLOR);
        terminal_writestring((r->err & 1u) ? "protection violation" : "page not present");
        pputs(" / ");
        terminal_setcolor(PANIC_COLOR);
        terminal_writestring((r->err & 2u) ? "write" : "read");
        pputs(" / ");
        terminal_setcolor(PANIC_COLOR);
        terminal_writestring((r->err & 4u) ? "user" : "supervisor");
        if (r->err & 8u)  pputs(" / reserved-bit set");
        if (r->err & 16u) pputs(" / instruction fetch");
        pputs("\n");
    }
    pputs("  context: ");
    terminal_setcolor(PANIC_COLOR);
    terminal_writestring(g_panic_ctx);
    pputs("\n\n  registers:\n");
    pputs("    EAX=0x"); phex(r->eax, 8);
    pputs("  EBX=0x"); phex(r->ebx, 8);
    pputs("  ECX=0x"); phex(r->ecx, 8);
    pputs("  EDX=0x"); phex(r->edx, 8);
    pputs("\n    ESI=0x"); phex(r->esi, 8);
    pputs("  EDI=0x"); phex(r->edi, 8);
    pputs("  EBP=0x"); phex(r->ebp, 8);
    pputs("  ESP~0x"); phex(shown_esp, 8);
    pputs("\n    EIP=0x"); phex(r->eip, 8);
    pputs("  CS=0x"); phex(r->cs, 4);
    pputs("  FL=0x"); phex(r->eflags, 8);
    pputs(from_user ? "  [ring3]" : "  [ring0]");
    pputs("\n    DS=0x"); phex(ds, 4);
    pputs(" ES=0x"); phex(es, 4);
    pputs(" FS=0x"); phex(fs, 4);
    pputs(" GS=0x"); phex(gs, 4);
    pputs(" SS=0x"); phex(shown_ss, 4);
    pputs("\n    CR0=0x"); phex(cr0, 8);
    pputs(" CR2=0x"); phex(cr2, 8);
    pputs(" CR3=0x"); phex(cr3, 8);
    pputs("\n\n  System halted. Power off or reboot (QEMU: Ctrl-Alt-G / reset).\n");

    panic_halt();
}

void isr_dispatch(isr_regs_t *regs) {
    if (regs->vec == 3) {
        /* #BP 陷阱：shell 测试命令与 reboot 兜底路径的受控触发。
         * 只报一行提示，不占用整屏（system_reboot 的 int3 属正常流程）。 */
        terminal_writestring("[int3 breakpoint: EIP=0x");
        /* 就地小 hex 打印，避免依赖 print_hex32 可见性 */
        uint32_t v = regs->eip;
        static const char h[] = "0123456789ABCDEF";
        char b[9];
        int n = 0;
        while (n < 8) { b[7 - n] = h[v & 0xF]; v >>= 4; n++; }
        b[8] = 0;
        terminal_writestring(b);
        terminal_writestring("] continuing\n");
        return;
    }
    dump(regs);     /* 其余 31 个向量：全屏现场 + 停机（不返回） */
}
