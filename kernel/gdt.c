/*
 * gdt.c - GDT 重建 + TSS（步骤 4a）
 *
 * 为什么必须在内核里重建而不是改 boot.asm：
 *   - boot.asm 的 GDT 位于 0x7C00 附近的引导扇区内存中，那块内存不属于内核，
 *     被覆盖的风险不可控；
 *   - TSS 基址需要指向内核 .bss 里的结构体，链接期才知道地址，实模式汇编写不了。
 */
#include "gdt.h"

#define GDT_ENTRIES 6

/* GDT 项（8 字节，packed：必须按 IA-32 手册的位布局，不能让编译器插填充） */
typedef struct {
    uint16_t limit_low;     /* 0..15 */
    uint16_t base_low;      /* 0..15 */
    uint8_t  base_mid;      /* 16..23 */
    uint8_t  access;        /* P|DPL|S|type */
    uint8_t  gran;          /* limit 16..19 | AVL|L|D/B|G */
    uint8_t  base_high;     /* 24..31 */
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/* 32-bit TSS。只用前几项（esp0/ss0）与末尾的 iomap_base；
 * 其余字段保留供硬件任务切换用——本内核不做硬件任务切换，故全部置 0。 */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;    /* >= TSS 限长表示"无 IO 位图" */
} __attribute__((packed)) tss_entry_t;

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;
static tss_entry_t tss;

/* 汇编侧：gdt_flush 远跳重载 CS；tss_flush 执行 ltr */
extern void gdt_flush(uint32_t gdt_ptr_addr);
extern void tss_flush(void);

static void gdt_set_gate(int n, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran) {
    gdt[n].base_low  = (uint16_t)(base & 0xFFFF);
    gdt[n].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    gdt[n].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[n].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[n].gran      = (uint8_t)((limit >> 16) & 0x0F);
    gdt[n].gran     |= (uint8_t)(gran & 0xF0);
    gdt[n].access    = access;
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint32_t)&gdt;

    /* 段基址全 0、限长 4GB、4KB 粒度（gran = 0xCF）：
     * 扁平模型，与分页前的行为一致，虚拟地址 == 线性地址。 */
    gdt_set_gate(0, 0, 0,        0x00, 0x00);   /* null（CPU 要求） */
    gdt_set_gate(1, 0, 0xFFFFF,  0x9A, 0xCF);   /* 内核代码 DPL0: 可执行/可读 */
    gdt_set_gate(2, 0, 0xFFFFF,  0x92, 0xCF);   /* 内核数据 DPL0: 可读写 */
    gdt_set_gate(3, 0, 0xFFFFF,  0xFA, 0xCF);   /* 用户代码 DPL3 */
    gdt_set_gate(4, 0, 0xFFFFF,  0xF2, 0xCF);   /* 用户数据 DPL3 */

    /* TSS：限长 = sizeof(tss)-1，字节粒度（gran = 0x00）。
     * access 0x89 = P=1, DPL=00, S=0(系统段), type=1001(32-bit TSS available) */
    gdt_set_gate(5, (uint32_t)&tss, (uint32_t)sizeof(tss) - 1, 0x89, 0x00);

    /* TSS 内容：esp0 用专用陷入栈（见 gdt.h 注释——不能复用主栈顶 0x90000，
     * 否则 ring3 期间 IRQ 陷入会覆写 kernel_main 活跃栈帧）。
     * iomap_base = sizeof(tss) 表示"IO 位图在限长之外"= 用户态禁止端口 IO。 */
    for (uint32_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.ss0        = GDT_KDATA_SEG;   /* GDT_KDATA_SEG == GDT_KERNEL_STACK_SEG（0x10，见 gdt.h 布局注释） */
    tss.esp0       = SYSCALL_TRAP_STACK_TOP;
    tss.cs         = GDT_KCODE_SEG;
    tss.ss         = GDT_KDATA_SEG;
    tss.ds         = GDT_KDATA_SEG;
    tss.es         = GDT_KDATA_SEG;
    tss.fs         = GDT_KDATA_SEG;
    tss.gs         = GDT_KDATA_SEG;
    tss.iomap_base = (uint16_t)sizeof(tss);

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
