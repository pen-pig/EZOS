/*
 * user/user.ld - 用户程序链接脚本（步骤 5c）
 *
 * 装载地址必须与 kernel/syscall.h 的 USER_IMAGE_BASE 一致（0x00400000），
 * 否则内核 elf_validate 会以 "entry point outside user image area" 拒绝。
 *
 * .text 起点加 SIZEOF_HEADERS：把 ELF 头/程序头表塞进第一页的头部空隙，
 * 而不是让 ld 为它们单独留出一整页（那会让一个 700 字节的程序变成 5KB，
 * 白占 8 个簇）。
 *
 * 注意不能用 `PHDRS { text PT_LOAD FILEHDR PHDRS; }`：ld 会把段起点往前挪
 * 一页到 0x003ff000，段就落到用户映像区之外，被内核校验直接拒掉。
 *
 * 段布局刻意分成两个 PT_LOAD：
 *   .text(+.rodata)  -> PF_R | PF_X          内核据此映射为只读（W^X）
 *   .data(+.bss)     -> PF_R | PF_W          可写，.bss 由加载器清零
 * 两段之间用 ALIGN(0x1000) 分页，ld 才会生成两个独立的程序头。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x00400000 + SIZEOF_HEADERS;

    .text   : { *(.text*) *(.rodata*) }
    .data   : ALIGN(0x1000) { *(.data*) }
    /* .bss 也要分页对齐：只给 .data 加 ALIGN 时，若程序没有已初始化数据
     * （.data 为空），ld 不会真的推进位置计数器，.bss 会被并进 .text 那个
     * PT_LOAD，段标志变成 RWE —— 既触发 "LOAD segment with RWX permissions"
     * 警告，又让 .text 可写，脚本文档承诺的 W^X 失效。 */
    .bss    : ALIGN(0x1000) { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
