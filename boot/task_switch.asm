; ============================================================
; task_switch.asm - 抢占式任务切换（步骤 6a）
;
; void switch_to(uint32_t *old_esp_save, uint32_t new_esp)
;
; 为什么只保存这 4 个寄存器：
;   1) cdecl 约定：ebp/ebx/esi/edi 由被调者保存；eax/ecx/edx 调用者自己负责
;   2) 切换点一定在 IRQ0 上下文（或 task_yield，它同样只在内核态被调用），
;      被中断任务的 eax/ecx/edx 已由 irq0 桩的 pusha 保存在它的栈上，
;      switch_to 不需要重复保存
;
; "任务从哪继续"的全部秘密就在最后的 ret：
;   返回地址由 call switch_to 压在旧任务栈上，ret 弹出后，
;   新任务沿着它自己当初的调用链返回到 irq0 桩，再 iret。
;
; 新任务首次被切到时，栈顶是 task.c 的 stack_init 摆好的那一帧：
;   edi/esi/ebx/ebp = 0，返回地址 = 任务函数入口。
; ============================================================
BITS 32

global switch_to

switch_to:
    mov eax, [esp + 4]          ; &old->esp
    mov edx, [esp + 8]          ; new->esp

    push ebp
    push ebx
    push esi
    push edi

    mov [eax], esp              ; 旧任务：记下切换点
    mov esp, edx                ; 新任务：换栈

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ============================================================
; task_irq_trampoline - 新任务的"第一次出场"
;
; 抢占切换发生在 IRQ0 桩内部（cli; pusha; call irq0_handler; popa; sti; iret）。
; 老任务切回来时，ret 一路回到 irq0_handler 再走完 popa/sti/iret，正常退出中断。
;
; 但**新任务没有中断帧**——它的栈是 task.c 手工摆的。如果让它直接 ret 到
; 任务函数，就永远走不到 popa/sti/iret：中断标志一直是关的（桩里 cli 过），
; PIT 不再触发，整个系统冻住。
;
; 所以新任务栈上的 switch_to 返回地址指向这里，由本 trampoline 补上
; 中断退出序列：popa 弹出（手工摆的）8 个寄存器，iret 弹出 EIP/CS/EFLAGS，
; 从而进入任务函数，并把 IF 置 1（EFLAGS 里预置 0x202）。
; 本段必须与 irq0 桩的 popa/sti/iret 序列保持一致。
; ============================================================
global task_irq_trampoline
task_irq_trampoline:
    popa
    sti
    iret
