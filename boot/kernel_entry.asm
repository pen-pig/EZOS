; boot/kernel_entry.asm
[bits 32]
[extern kernel_main]
[extern irq0_handler]
[extern irq1_handler]
[extern irq12_handler]
[extern irq11_handler]
[extern irq7s_handler]
[extern irq15s_handler]
[extern isr_dispatch]
[extern syscall_handler]
[extern __bss_start]
[extern __bss_end]
[extern __hbss_start]
[extern __hbss_end]

global _start
global irq0
global irq1
global irq12
global irq11
global irq7s
global irq15s
global idt_flush
global isr_stub_table
global gdt_flush
global tss_flush
global syscall_entry
global enter_usermode
global g_kernel_esp_save
global g_kernel_eflags
global g_syscall_frame
global g_user_exited

_start:
    mov esp, 0x200000

    ; 清零 .bss 段：C 的静态变量都在里面，必须显式清零，不能指望 BIOS 留下的是 0
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; zero .bss.hi (high memory at 1MB+, read-only FS driver buffers)
    mov edi, __hbss_start
    mov ecx, __hbss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kernel_main
    cli
    hlt

; IRQ0 中断入口（PIT 定时器）
irq0:
    cli
    pusha
    call irq0_handler
    popa
    sti
    iret

; IRQ1 中断入口（键盘）
irq1:
    cli
    pusha
    call irq1_handler
    popa
    sti
    iret

; IRQ12 中断入口（鼠标）
irq12:
    cli
    pusha
    call irq12_handler
    popa
    sti
    iret

; IRQ11 entry (RTL8139 NIC). Gate 0x8E clears IF on entry; iret restores
; it from the saved EFLAGS, so no explicit sti here (no reentry window).
irq11:
    cli
    pusha
    call irq11_handler
    popa
    iret

; spurious IRQ7 handlers (vector 39 master / 47 slave): see isr.c
; - the 8259 raises these when an IRQ line withdraws before INTA.
;   Same stub shape as irq11 (gate 0x8E clears IF; iret restores it).
irq7s:
    cli
    pusha
    call irq7s_handler
    popa
    iret

irq15s:
    cli
    pusha
    call irq15s_handler
    popa
    iret

; 加载 IDT 的函数
; 参数：uint32_t idt_ptr（指向 idt_ptr 结构的指针）
idt_flush:
    mov eax, [esp + 4]    ; 取参数（idt_ptr 地址）
    lidt [eax]            ; 加载 IDT
    ret

; ==================== CPU 异常桩（向量 0-31）====================
; 栈布局（isr_common 里，从 esp 往上）：
;   [esp..esp+31]  pusha 区（edi esi ebp esp占位 ebx edx ecx eax）
;   [esp+32]=向量号  [esp+36]=错误码（无码向量垫 0）
;   [esp+40]=EIP  [esp+44]=CS  [esp+48]=EFLAGS
; ring0 同级触发，不压 ESP/SS；C 侧 isr_regs_t 的布局必须与此一致。
%macro ISR_STUB_ERR 1
isr_stub_%1:
    cli
    push dword %1            ; 向量号（错误码已由 CPU 压入）
    jmp isr_common
%endmacro

%macro ISR_STUB_NOERR 1
isr_stub_%1:
    cli
    push dword 0            ; 垫 0 占位，好和有码向量共用同一栈帧
    push dword %1
    jmp isr_common
%endmacro

; 错误码规则（Intel SDM Vol.3 6.13）：只有向量 8/10/11/12/13/14/17/30 由 CPU 压入
; 错误码，其余必须垫 0 才能和带码向量共用同一个栈帧。
;   #DE #DB NMI #BP #OF #BR #UD #NM | #DF | #TS #NP #SS #GP #PF | #MF #AC #MC #XM
;   0   1   2   3   4   5   6   7   | 8   | 10  11  12  13  14   | 16  17  18  19
ISR_STUB_NOERR 0            ; #DE 除零
ISR_STUB_NOERR 1            ; #DB 调试
ISR_STUB_NOERR 2            ; NMI
ISR_STUB_NOERR 3            ; #BP 断点（int3，调试用）
ISR_STUB_NOERR 4            ; #OF into 溢出
ISR_STUB_NOERR 5            ; #BR bnd 越界
ISR_STUB_NOERR 6            ; #UD 非法指令
ISR_STUB_NOERR 7            ; #NM 设备不可用（FPU）
ISR_STUB_ERR   8            ; #DF 双重故障（错误码恒 0）
ISR_STUB_NOERR 9            ; 协处理器段溢出（保留）
ISR_STUB_ERR   10           ; #TS 无效 TSS
ISR_STUB_ERR   11           ; #NP 段不存在
ISR_STUB_ERR   12           ; #SS 栈段故障
ISR_STUB_ERR   13           ; #GP 通用保护
ISR_STUB_ERR   14           ; #PF 缺页（CR2=出错的线性地址）
ISR_STUB_NOERR 15           ; 保留
ISR_STUB_NOERR 16           ; #MF x87 浮点异常
ISR_STUB_ERR   17           ; #AC 对齐检查（错误码恒 0）
ISR_STUB_NOERR 18           ; #MC 机器检查
ISR_STUB_NOERR 19           ; #XM SIMD 浮点异常
ISR_STUB_NOERR 20           ; #VE 虚拟化异常
ISR_STUB_NOERR 21
ISR_STUB_NOERR 22
ISR_STUB_NOERR 23
ISR_STUB_NOERR 24
ISR_STUB_NOERR 25
ISR_STUB_NOERR 26
ISR_STUB_NOERR 27
ISR_STUB_NOERR 28
ISR_STUB_NOERR 29
ISR_STUB_ERR   30           ; #SX 安全异常
ISR_STUB_NOERR 31

isr_common:
    pusha                    ; [esp]=pusha 区 32B
    mov eax, esp             ; eax -> isr_regs_t 基址（含向量号/错误码/EIP/CS/EFLAGS）
    push eax
    call isr_dispatch
    add esp, 4
    popa
    add esp, 8               ; 弹掉向量号 + 错误码/垫位
    iret

; C 可见的桩地址表（panic.c 遍历它注册 IDT 0-31）
;
; 必须逐项显式列出，不能写成
;     %assign i 0 / %rep 32 / dd isr_stub_%+i / %assign i i+1 / %endrep
; ——NASM 的 %+ 拼接发生在 %assign 变量展开之前，会把 32 个表项全部解析成
; 同一个符号，实测全都指向 isr_stub_0：任何异常上报都是 "#DE vector 00"，
; 且 isr_regs_t 整体错位 4 字节（EIP 读成了错误码、CS 读到真 EIP、
; EFLAGS 读到真 CS=0x8）。这个缺陷是 test_step3.py 的开机 OCR 首次暴露的——
; 步骤 1 那类“蓝屏配色/像素统计”断言会全部不成立。
isr_stub_table:
    dd isr_stub_0
    dd isr_stub_1
    dd isr_stub_2
    dd isr_stub_3
    dd isr_stub_4
    dd isr_stub_5
    dd isr_stub_6
    dd isr_stub_7
    dd isr_stub_8
    dd isr_stub_9
    dd isr_stub_10
    dd isr_stub_11
    dd isr_stub_12
    dd isr_stub_13
    dd isr_stub_14
    dd isr_stub_15
    dd isr_stub_16
    dd isr_stub_17
    dd isr_stub_18
    dd isr_stub_19
    dd isr_stub_20
    dd isr_stub_21
    dd isr_stub_22
    dd isr_stub_23
    dd isr_stub_24
    dd isr_stub_25
    dd isr_stub_26
    dd isr_stub_27
    dd isr_stub_28
    dd isr_stub_29
    dd isr_stub_30
    dd isr_stub_31

; ==================== GDT / TSS / 系统调用 / ring3 ====================

[section .bss]
; enter_usermode 进入时保存的内核 esp（指向返回地址），SYS_EXIT 时据此回到调用者
g_kernel_esp_save:  resd 1
; SYS_EXIT 置 1；syscall_entry 检查此标志走“返回内核”路径而不是 iret 回用户态
g_user_exited:      resd 1
; enter_usermode 进入时保存的内核 EFLAGS：SYS_EXIT 返回时据此精确恢复 IF
; （不能无条件 sti——那会把“调用者本来关着中断”的情况给打开）
g_kernel_eflags:    resd 1
; 系统调用陷入时压出来的寄存器帧地址（syscall_entry 在 call 之前保存）：
; fork 要靠它复制子进程的寄存器，而这个地址**只能由汇编**给出：以前是在
; syscall_handler 的第一条语句读 esp、再按固定下标算，可那个下标会随 C 编译器
; 生成的 prologue 变化（push 了几个 callee-saved、sub 了多少局部空间），
; 函数一改就默认错位——这就是靠约定办事的代价。
g_syscall_frame:    resd 1

[section .text]

; void gdt_flush(uint32_t gdt_ptr_addr)
; 装载新 GDT 后必须用远跳转重载 CS——只改 ds/es/ss 不动 CS 的话，
; 后续取指用的还是旧的描述符缓存值。
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10                 ; 内核数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload_cs
.reload_cs:
    ret

; void tss_flush(void)
; ltr 的选择子 RPL 必须为 0，否则 #GP。
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

; ---- int 0x80 系统调用入口（IDT 门 DPL=3，用户态唯一合法陷入方式）----
; 进入时 CPU 已按 TSS.esp0/ss0 切到内核栈，并压入 SS/ESP/EFLAGS/CS/EIP。
; 参数约定：eax=调用号，ebx/ecx/edx=参数；返回值经 eax 传回用户态。
;
; 栈布局（pusha 之后，从 esp 起）：
;   +0 edi +4 esi +8 ebp +12 esp +16 ebx +20 edx +24 ecx +28 eax
;   +32 gs +36 fs +40 es +44 ds
syscall_entry:
    push ds
    push es
    push fs
    push gs
    pusha
    push edx
    push ecx
    push ebx
    push eax
    ; frame base for fork: +0 eax +4 ebx +8 ecx +12 edx, +16..+44 pusha
    ; area (edi..eax), +64 EIP +68 CS +72 EFLAGS +76 ESP +80 SS.
    ; Must come from ASM: indexing from a C function's esp depends on the
    ; compiler-generated prologue and silently shifts when the function changes.
    mov [g_syscall_frame], esp
    mov ax, 0x10                 ; 切到内核数据段：用户段选择子不能用于内核存取
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call syscall_handler         ; int syscall_handler(num, a1, a2, a3)
    add esp, 16

    ; SYS_EXIT 有独立返回路径：丢弃用户态上下文，直接回到 enter_usermode 的调用者
    cmp dword [g_user_exited], 0
    jne .return_to_kernel

    mov [esp + 28], eax
    popa
    pop gs
    pop fs
    pop es
    pop ds
    ; defensive: when returning to ring3 force user data segments.
    ; A task may have started with stale/null selectors; the pops
    ; above would restore them verbatim and the next user memory
    ; access faults. CS sits right after EIP in the iret frame.
    ; EDX is scratch (cdecl) - EAX must survive: it is the return value.
    mov edx, [esp + 4]
    test dl, 3
    jz .sys_ring0_out
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx
.sys_ring0_out:
    iret

.return_to_kernel:
    mov esp, [g_kernel_esp_save]
    mov dword [g_user_exited], 0
    push dword [g_kernel_eflags]  ; exact restore of caller EFLAGS
    popfd
    ret                          ; 回到 enter_usermode() 的调用者

; ---- void enter_usermode(uint32_t entry, uint32_t user_esp) ----
; 构造 iret 帧降权到 ring3。CS/SS 必须带 RPL=3（0x1B/0x23），
; 否则 CPL 与 RPL 不匹配会 #GP。
enter_usermode:
    mov eax, [esp + 4]           ; entry
    mov edx, [esp + 8]           ; user_esp
    mov [g_kernel_esp_save], esp ; esp 此刻指向返回地址
    mov dword [g_user_exited], 0

    push dword 0x23              ; SS
    push edx                     ; ESP
    pushfd
    pop ecx                      ; NOT ebx: ebx is callee-saved (cdecl)
    mov [g_kernel_eflags], ecx   ; save for exact restore on exit path
    or ecx, 0x200                ; 置 IF：用户态需要能收到中断
    push ecx                     ; EFLAGS
    push dword 0x1B              ; CS: 用户代码段 | RPL3
    push dword [esp + 20]        ; EIP = entry arg: 4 pushes => esp-16, so
                                 ; entry sits at [esp+20]. Reload from stack
                                 ; because "mov ax,imm16" clobbers eax low half

    cli                          ; "half-user" window: ring0 running with DPL3
                                 ; data segments - an IRQ here would let C code
                                 ; touch kernel data through them
    mov ax, 0x23                 ; 用户数据段 | RPL3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iret                         ; restores user EFLAGS (IF=1)
