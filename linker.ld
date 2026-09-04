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
}
