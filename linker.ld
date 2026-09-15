ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
        *(.rodata*)
    }

    .data : {
        *(.data)
    }

    /* 镜像（text+data）受 boot.asm KERNEL_SECTORS=768 限制，最大 384KB */
    ASSERT(. - 0x10000 <= 0x60000, "kernel image exceeds 384KB limit")

    .bss : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }

    /* bss 未初始化数据不计入镜像，但内存中不得侵入 0x90000 的栈区 */
    ASSERT(__bss_end <= 0x90000, "kernel bss exceeds stack area")

    /* 高内存 bss（1MB 起，扩展内存）：放只读 FS 驱动的大块缓冲。
     * NOBITS 不进镜像；kernel_entry.asm 启动时一并清零。
     * 上限 1MB（到 2MB），避开 16MB 处的 GUI 背缓冲（gfxwin GW_BB_ADDR）。 */
    .bss.hi 0x100000 (NOLOAD) : {
        __hbss_start = .;
        *(.bss.hi)
        __hbss_end = .;
    }
    ASSERT(__hbss_end <= 0x200000, "high bss exceeds 2MB limit")

    /* 任务内核栈池（步骤 6a）：0x904000 起，到 pmm 物理页池 10MB 之前。
     *
     * 不放 .bss.hi 的原因：那里已经用了 ~1.97MB（上限 2MB），8 个 16KB 栈
     * 会直接顶穿。9MB 陷入栈之后到 10MB 之间有 768KB 空档，正好够用，
     * 且不挤占任何已有区域：
     *   2MB 页表池 | 3MB scratch | 4-8MB 用户空间 | 9MB 陷入栈
     *   | **9.016MB 本池** | 10-16MB pmm 池 | 16MB GUI 背缓冲
     *
     * NOLOAD 且启动代码不清零：栈不需要零初始化，省 128KB 的清零时间。 */
    .bss.kstack 0x00904000 (NOLOAD) : {
        __kstack_start = .;
        *(.bss.kstack)
        __kstack_end = .;
    }
    ASSERT(__kstack_end <= 0x00A00000, "task kernel stacks exceed pool")
}
