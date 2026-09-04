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

    /* 镜像（text+data）受 boot.asm KERNEL_SECTORS=512 限制，最大 256KB */
    ASSERT(. - 0x10000 <= 0x40000, "kernel image exceeds 256KB limit")

    .bss : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }

    /* bss 未初始化数据不计入镜像，但内存中不得侵入 0x90000 的栈区 */
    ASSERT(__bss_end <= 0x90000, "kernel bss exceeds stack area")
}
