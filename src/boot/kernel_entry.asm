; boot/kernel_entry.asm
[bits 32]
[extern kernel_main]
[extern irq1_handler]
[extern irq12_handler]

global _start
global irq1
global irq12
global idt_flush

_start:
    mov esp, 0x90000
    call kernel_main
    cli
    hlt

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