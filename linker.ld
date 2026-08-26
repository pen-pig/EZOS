ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
    }

    .data : {
        *(.data)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }
}
