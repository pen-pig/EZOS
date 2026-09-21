; ============================================================
; task_switch.asm - 抢占式任务切换（步骤 6a）
;
; void switch_to(uint32_t *old_esp_save, uint32_t new_esp)
;
; 为什么只保存这 4 个寄存器 + EFLAGS：
;   1) cdecl 约定：ebp/ebx/esi/edi 由被调者保存；eax/ecx/edx 调用者自己负责
;   2) 切换点一定在 IRQ0 上下文（或 task_yield/task_sleep，同样只在
;      内核态被调用），被中断任务的 eax/ecx/edx 已由 irq0 桩的 pusha
;      保存在它的栈上，switch_to 不需要重复保存
;   3) EFLAGS（步骤 8a）：恢复方必须拿到**自己入睡时**的 IF，而不是
;      "切回来时恰好在谁手里的 IF"。没有 popfd 的话，一个在系统调用里
;      （IF=0）睡着的任务可能被 IRQ0 的调度（同样 IF=0）唤醒、又被某个
;      内核线程的 yield（IF=1）唤醒——醒来时的 IF 取决于上一次切换的
;      发起者，等于把中断门语义交给运气。保存/恢复 EFLAGS 后：
;      睡眠者恒以入睡时的 IF 恢复执行，跨睡眠的 IF=0 约定成立。
;
; "任务从哪继续"的全部秘密就在最后的 ret：
;   返回地址由 call switch_to 压在旧任务栈上，ret 弹出后，
;   新任务沿着它自己当初的调用链返回到 irq0 桩，再 iret。
;
; 新任务首次被切到时，栈顶是 task.c 的 stack_init 摆好的那一帧：
;   edi/esi/ebx/ebp、EFLAGS(0x0002, IF=0)、返回地址 = trampoline。
; ============================================================
BITS 32

global switch_to

switch_to:
    mov eax, [esp + 4]          ; &old->esp
    mov edx, [esp + 8]          ; new->esp

    pushfd                      ; EFLAGS（含 IF）——步骤 8a
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
    popfd                       ; 恢复本任务入睡时保存的 EFLAGS/IF
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
    ; iret does NOT reload DS/ES/FS/GS: a user process would
    ; start ring3 with whatever the scheduler had loaded (kernel
    ; selectors, surviving only via stale descriptor caches).
    ; Load the data segments that match the frame's CS RPL so the
    ; first syscall pushes/pops a consistent, valid selector.
    mov eax, [esp + 4]      ; CS from the iret frame
    test al, 3
    jz .ksegs
    mov ax, 0x23            ; user data | RPL3
    jmp .loadsegs
.ksegs:
    mov ax, 0x10            ; kernel data
.loadsegs:
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iret
