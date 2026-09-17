ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    /* 入口桩必须永远排在最前。
     *
     * boot.asm 把内核加载到固定的 0x10000 并直接跳过去，它不读任何 ELF
     * 入口字段（--oformat binary 已经把 ELF 头丢掉了）。因此 0x10000 处
     * 必须是 _start，而"0x10000 处是什么"完全由这里 .text 的收集顺序决定。
     *
     * 曾经只写 `*(.text)`：那么 _start 只有在 kernel_entry.o 恰好是链接
     * 列表第一个时才位于 0x10000。build.ninja 的列表确实是这么排的，所以
     * 一直没出问题；但把同样的 .o 按字母序链接时，kernel_entry.o 被排到
     * 中间，0x10000 处成了别的模块 —— 表现为"能跑完开机自检，然后卡死在
     * exec 演示、永远进不了 shell"（内核其实是在错误的位置开始执行的）。
     *
     * 显式点名就不必再依赖链接顺序了；ld 先匹配先消费，下面的 *(.text)
     * 不会再把它收一遍。 */
    .text : {
        *kernel_entry.o(.text)
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
