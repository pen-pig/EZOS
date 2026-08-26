; boot/kernel_entry.asm
[bits 32]
[extern kernel_main]
[extern irq0_handler]
[extern irq1_handler]
[extern irq12_handler]
[extern __bss_start]
[extern __bss_end]

global _start
global irq0
global irq1
global irq12
global idt_flush

_start:
    mov esp, 0x90000

    ; 清零 .bss 段（内核镜像只加载 64KB，超出部分保持 BIOS 残留，必须显式清零）
    mov edi, __bss_start
    mov ecx, __bss_end
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

; IRQ1 中断入口
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

; 加载 IDT 的函数
; 参数：uint32_t idt_ptr（指向 idt_ptr 结构的指针）
idt_flush:
    mov eax, [esp + 4]    ; 获取参数（idt_ptr 地址）
    lidt [eax]            ; 加载 IDT
    ret