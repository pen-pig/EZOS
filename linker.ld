ENTRY(_start)

SECTIONS
{
    /* 内核镜像加载地址 = 19MB（同 boot/layout.inc 的 KERNEL_DST）。
     *
     * 为什么搬出低 1MB：BIOS 的 INT13h AH=42h 用 seg:off 寻址（20 位），
     * 目的地址只能落在低 1MB，而 0xA0000 起是 VGA aperture —— 0xA0000-0xBFFFF
     * 是显卡的窗口，文本模式下**不是**线性 flat RAM（0xB8000 只是其中的
     * 文本缓冲页）。于是"引导扇区直接读内核"的上限被硬件钉死在
     * 0x10000..0xA0000 = 576KB。曾经把上界写成 0xB8000（以为中间那 96KB
     * 也是普通内存），本地 504KB 的产物够不着所以一直没暴露；CI 的 gcc13
     * 编出 591KB，镜像尾部一伸进 0xA0000 就 #PF 跑飞（串口刷了 6533 次
     * page fault）。修完上界只剩 28KB 余量，再加功能必然撞线。
     *
     * 现在由 boot/stage2.asm 进保护模式把内核分批搬到 19MB：BIOS 只写
     * 低地址的 bounce 缓冲，高地址由 32 位地址前缀写入。窗口 3MB
     * （19MB..22MB，22MB 起是 .bss），上限不再由 BIOS 的 20 位地址决定。
     *
     * 入口桩必须永远排在最前。
     * stage2 把内核搬到固定的 KERNEL_DST 并直接跳过去，它不读任何 ELF
     * 入口字段（--oformat binary 已经把 ELF 头丢掉了）。因此 KERNEL_DST 处
     * 必须是 _start，而"KERNEL_DST 处是什么"完全由这里 .text 的收集顺序决定。
     *
     * 曾经只写 `*(.text)`：那么 _start 只有在 kernel_entry.o 恰好是链接
     * 列表第一个时才位于 0x8000。build.ninja 的列表确实是这么排的，所以
     * 一直没出问题；但把同样的 .o 按字母序链接时，kernel_entry.o 被排到
     * 中间，0x8000 处成了别的模块 —— 表现为"能跑完开机自检，然后卡死在
     * exec 演示、永远进不了 shell"（内核其实是在错误的位置开始执行的）。
     *
     * 显式点名就不必再依赖链接顺序了；ld 先匹配先消费，下面的 *(.text)
     * 不会再把它收一遍。 */
    . = 0x1300000;
    /* 镜像起点：kernel.c 直接打印这个符号（不再抄常量，抄就会和这里走偏）。 */
    __image_start = .;

    .text : {
        *kernel_entry.o(.text)
        *(.text)
        *(.rodata*)
    }

    .data : {
        *(.data)
    }

    /* 主栈 1MB..2MB（栈顶 0x200000，向下生长）。
     * 2026-10-08 从 0x90000 搬上来：当时它和镜像抢同一块低 1MB，把镜像窗口
     * 压到 496KB，只能靠 -Os 硬压代码。现在镜像已经整个搬到 19MB，主栈留在
     * 1-2MB 纯属惯性 —— 它夹在页表池(2MB)之下，两边都不冲突。
     * 1-2MB 原本是 .bss.hi，已挪到 30MB（见下方）。
     * 见 boot.asm / kernel_entry.asm 的 `mov esp, 0x200000`。
     *
     * 栈水位扫描（kernel.c stack_paint / stack_high_water）的两端：启动时把
     * [__stack_bottom, __stack_top) 涂成 0xA5，之后"第一个不是 0xA5 的字节"
     * 就是历史最低 esp —— 用它量真实栈峰值。为什么需要：栈越界在这里**不崩**，
     * 只是静默写穿相邻的东西（历史 bug：shell.c cmd_ls 的 16.9KB 栈数组覆盖了
     * fs.c 的 ro_cwd，症状是提示符变 [A.TXT]、cat/write 全挂，伪装成文件系统
     * 损坏）。 */
    __data_end = .;
    __stack_bottom = 0x100000;
    __stack_top = 0x200000;

    /* 镜像（text+data）窗口 3MB：KERNEL_DST(19MB) ~ 22MB(.bss 起点)。
     * 上限不再是 BIOS 的 20 位地址，而是"这一段空档有多大"——16-19MB 是 GUI
     * 背缓冲、22MB 起是 .bss，中间正好 3MB。要再扩就得挪 .bss（现在占
     * 22-24MB，30MB 之前还有余量）或挪 GUI 背缓冲，不是改一个常量就完事。
     *
     * 同一上限在 tools/make_image.py 里校验一次（MAX_SECTORS，从
     * boot/layout.inc 解析）。镜像大小本身是动态的，扇区数写进引导扇区
     * 0x1FC，由 boot/stage2.asm 读。 */
    ASSERT(. - 0x1300000 <= 0x300000, "kernel image exceeds 3MB window (19MB..22MB)")

    /* .bss 与镜像解耦：放到 22MB（镜像窗口 19-22MB 之后、32MB identity
     * 上界之前）。原因：bss 跟着镜像会把文件预算吃光（低 1MB 时代
     * file+bss 合计只有 ~501KB）。
     * 22MB 处 identity 映射覆盖、启动早期（尚未开分页、PM 平坦段）可直接
     * 访问，kernel_entry.asm 的清零循环走符号无需改动。NOBITS 不进镜像文件。 */
    .bss 0x01600000 (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }

    /* bss 上界 30MB：给 22-30MB 区间留出 8MB 余量，同时绝不触及
     * 32MB identity 映射上界。 */
    ASSERT(__bss_end <= 0x01E00000, "kernel bss exceeds 30MB limit")

    /* 高内存 bss：放只读 FS 驱动的大块缓冲。NOBITS 不进镜像；
     * kernel_entry.asm 启动时一并清零。
     *
     * 原本在 1MB 起，现在 1-2MB 让给主栈（见上面的 __stack_bottom），于是挪到
     * 30MB —— .bss（19-30MB）之后的空档，正好贴着 32MB identity 上界，
     * 2MB 空间够用（实测占用 ~1.97MB）。 */
    .bss.hi 0x1E00000 (NOLOAD) : {
        __hbss_start = .;
        *(.bss.hi)
        __hbss_end = .;
    }
    ASSERT(__hbss_end <= 0x2000000, "high bss exceeds 32MB identity limit")

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
    /* 大型纯数据缓冲（NOLOAD，启动不清零：内容均由守卫变量保护，
     * 未写前不会被读）：tty 滚回区等。放 9.25-10MB 的空闲带——
     * 陷入栈(9MB-16KB)与任务栈池(0x904000..0x924000)之后、pmm 池(10MB)之前。 */
    .bss.buff 0x00930000 (NOLOAD) : {
        __bbuf_start = .;
        *(.bss.buff)
        __bbuf_end = .;
    }
    ASSERT(__bbuf_end <= 0x00A00000, "buffer pool exceeds pmm area")
}
