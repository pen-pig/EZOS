ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    /* 入口桩必须永远排在最前。
     *
     * boot.asm 把内核加载到固定的 0x8000 并直接跳过去，它不读任何 ELF
     * 入口字段（--oformat binary 已经把 ELF 头丢掉了）。因此 0x8000 处
     * 必须是 _start，而"0x8000 处是什么"完全由这里 .text 的收集顺序决定。
     *
     * 窗口 = 0x8000..0xA0000 = 608KB。两个端点都是硬件给的，不是随手选的：
     *   上界 0xA0000 —— VGA aperture 起点。0xA0000-0xBFFFF 是显卡的窗口，
     *   文本模式下**不是**线性 flat RAM（0xB8000 只是其中的文本缓冲页）。
     *   曾经把上界写成 0xB8000（以为中间那 96KB 也是普通内存），本地 504KB
     *   的产物够不着所以一直没暴露；CI 的 gcc13 编出 591KB，镜像尾部一伸进
     *   0xA0000 就 #PF 跑飞（串口刷了 6533 次 page fault）。
     *   下界 0x10000 —— 不能再低。不是空间问题（0x8000 就能多 32KB），
     *   而是 BOOTIA32.EFI 是仓库外预编译的二进制，里面写死了往 0x10000
     *   搬内核；改 linker.ld 不跟着改它，UEFI 启动必然跑飞。
     *
     * 曾经只写 `*(.text)`：那么 _start 只有在 kernel_entry.o 恰好是链接
     * 列表第一个时才位于 0x8000。build.ninja 的列表确实是这么排的，所以
     * 一直没出问题；但把同样的 .o 按字母序链接时，kernel_entry.o 被排到
     * 中间，0x8000 处成了别的模块 —— 表现为"能跑完开机自检，然后卡死在
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

    /* 主栈搬到 1MB..2MB：以前它占着 0x90000，和镜像抢同一块低 1MB，把
     * 窗口压到 496KB。搬走之后镜像独占 0x10000..0xA0000 = 576KB。
     * 1-2MB 原本是 .bss.hi，已挪到 30MB（见下方）。
     * 栈从 0x200000 向下生长，1MB 空间（原来只有 ~21KB）。
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

    /* 镜像（text+data）窗口 576KB：0x10000(内核) ~ 0xA0000(VGA aperture)。
     * 以前主栈占着 0x90000、和镜像抢同一块 512KB，只能靠 -Os 硬压，换个
     * gcc 就撞线（Ubuntu gcc13 -O2 编出 579KB > 496KB 上限）。现在主栈搬去
     * 1MB，窗口 576KB，比原来多 80KB（代价是 -O2 产物要靠 -fno-align-* 压
     * 掉代码对齐填充，见 build.ninja）。
     * 同一上限在 tools/make_image.py 里校验一次（MAX_SECTORS=1152）——
     * 镜像大小本身是动态的，扇区数写进引导扇区 0x1FC。 */
    ASSERT(. - 0x10000 <= 0x90000, "kernel image exceeds 576KB window")

    /* .bss 与镜像解耦：放到 19MB（16MB GUI 背缓冲 ~18.6MB 结束之后、
     * 32MB identity 上界之前）。原因：低内存 0x10000-0x90000 被镜像+栈
     * 挤满（file+bss 合计只有 ~501KB），bss 跟着镜像会把文件预算吃光；
     * 而 2-4MB 虚拟段是 exec 装载的 PD[1] 暂存区、1-2MB 是 .bss.hi，
     * 都不能碰。19MB 处 identity 映射覆盖、启动早期（尚未开分页、PM
     * 平坦段）可直接访问，kernel_entry.asm 的清零循环走符号无需改动。
     * NOBITS 不进镜像文件。 */
    .bss 0x01300000 (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }

    /* bss 上界 30MB：给 19-30MB 区间留出 11MB 余量，同时绝不触及
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
