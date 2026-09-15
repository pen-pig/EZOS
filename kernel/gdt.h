/*
 * gdt.h - 全局描述符表（步骤 4：为 ring3 + 系统调用铺路）
 *
 * 引导阶段（boot.asm）只建了 3 项：null / 内核代码 / 内核数据，全部 DPL=0，
 * 因此"没有 TSS、进不了用户态"。内核启动后在这里重建 GDT：
 *
 *   0x00  null
 *   0x08  内核代码  DPL=0  0x9A
 *   0x10  内核数据  DPL=0  0x92
 *   0x18  用户代码  DPL=3  0xFA      -> ring3 选择子 0x1B
 *   0x20  用户数据  DPL=3  0xF2      -> ring3 选择子 0x23
 *   0x28  TSS       DPL=0  0x89      -> ltr 用 0x28（RPL 必须为 0）
 *
 * TSS 的作用只有一个：提供 ring0 栈（esp0/ss0）。ring3 执行 int 0x80 或
 * 收到中断时，CPU 从这里取 esp0 切换栈——没有它就没法安全地从用户态陷入内核。
 * 这里不做硬件任务切换（不使用 TSS 的任务切换功能）。
 */
#ifndef GDT_H
#define GDT_H

#include "types.h"

#define GDT_KCODE_SEG   0x08u
#define GDT_KDATA_SEG   0x10u
#define GDT_UCODE_SEG   0x18u
#define GDT_UDATA_SEG   0x20u
#define GDT_TSS_SEG     0x28u

/* gdt.c 的 tss.ss0 用主内核数据段；本别名仅用于强调"陷入栈也是内核数据段"。
 * （与 GDT_KDATA_SEG 同值 0x10，避免两处不同值引起误解。） */
#define GDT_KERNEL_STACK_SEG GDT_KDATA_SEG

/* ring3 选择子 = 段选择子 | RPL(3)。iret 到用户态时 CS/SS 必须用这两个值，
 * 否则 CPL 与 RPL 不匹配会直接 #GP。 */
#define SEL_UCODE       (GDT_UCODE_SEG | 3u)     /* 0x1B */
#define SEL_UDATA       (GDT_UDATA_SEG | 3u)     /* 0x23 */

/* 内核栈：TSS.esp0 指向这里。与 kernel_entry.asm / boot.asm 的 esp 初值一致。 */
#define GDT_KERNEL_STACK 0x90000u

/* ring3 陷入专用内核栈（16KB @ 物理 9MB，范围 0x8FC000-0x900000）。
 *
 * 为什么不直接用主内核栈（0x90000）：用户态运行期间 IRQ/异常随时会陷入，
 * CPU 按 TSS.esp0 切栈。若 esp0 = 0x90000（主栈顶），irq0 的
 * SS/ESP/EFLAGS/CS/EIP + pusha 会覆写 _start→kernel_main→shell_run
 * 调用链的活跃栈帧（0x8FFE0-0x8FFFC 正是 kernel_main 返回地址）。
 * "当前恰好无害"依赖帧布局运气，生产级不可接受——陷入栈必须与
 * 主调用链物理隔离。
 *
 * 位置依据：4MB-8MB 是用户虚拟空间（步骤 5 起 ELF 装在这里，其中的页会被
 * 重新映射到 pmm 分配的物理页），陷入栈必须落在用户空间之外——否则用户页
 * 覆盖该虚拟地址后，CPU 陷入时会把现场写进用户页。
 *   2MB 页表池 | 3MB pagetest scratch | 4-8MB 用户空间 | **9MB 本栈**
 *   | 10-16MB pmm 物理页池 | 16MB GUI 背缓冲
 * 需 ≥8KB 深度（中断链 + syscall_handler + panic 路径 C 帧之和的保守值）。 */
#define SYSCALL_TRAP_STACK_TOP  0x00900000u
#define SYSCALL_TRAP_STACK_SIZE 0x00004000u      /* 16KB */

/* 重建 GDT 并加载 TSS。只能在启动时调用一次（之后段寄存器已切到新表）。 */
void gdt_init(void);

/* 更新 TSS.esp0（将来有进程后，每次任务切换都要改） */
void tss_set_kernel_stack(uint32_t esp0);

#endif
