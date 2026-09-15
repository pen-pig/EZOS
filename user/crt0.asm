; ============================================================
; user/crt0.asm - 用户程序运行时的启动代码（步骤 5c）
;
; ELF 入口 _start 由链接脚本定位到 0x00400000。内核 exec 在切换
; 到 ring3 之前把参数按 System V i386 约定压在用户栈上：
;
;       esp -> argc
;              argv[0]
;              argv[1]
;              ...
;              NULL            (argv 终止)
;              NULL            (envp 终止，暂无环境)
;
; _start 只做三件事：取出 argc/argv -> 调 umain -> 用返回值调 _exit。
; 系统调用走 int 0x80，调用号在 eax，参数依次 ebx/ecx/edx，返回值 eax。
; ============================================================
BITS 32

section .text

global _start
extern umain

_start:
    pop     eax                 ; argc
    mov     edx, esp            ; edx = &argv[0]
    push    edx
    push    eax
    call    umain
    add     esp, 8
    push    eax                 ; umain 的返回值作为退出码
    call    _exit
    ; _exit 不返回；即便异常返回（比如内核拒收）也只能停在这里，
    ; 绝不能落进 umain 的机器码里乱跑。
.hang:
    jmp     .hang

; ---- void _exit(int code) : SYS_EXIT，永不返回 ----
global _exit
_exit:
    mov     eax, 2              ; SYS_EXIT
    mov     ebx, [esp + 4]
    int     0x80
    jmp     _exit               ; 内核若拒收，原地重试而不是乱跑

; ---- int write(int fd, const void *buf, unsigned n) : SYS_WRITE ----
global write
write:
    push    ebx                 ; ebx 是被调用者保存寄存器
    mov     eax, 1              ; SYS_WRITE
    mov     ebx, [esp + 8]      ; fd
    mov     ecx, [esp + 12]     ; buf
    mov     edx, [esp + 16]     ; n
    int     0x80
    pop     ebx
    ret

; ---- int read(int fd, void *buf, unsigned n) : SYS_READ ----
global read
read:
    push    ebx
    mov     eax, 0              ; SYS_READ
    mov     ebx, [esp + 8]
    mov     ecx, [esp + 12]
    mov     edx, [esp + 16]
    int     0x80
    pop     ebx
    ret

; ---- int open(const char *name, int flags) : SYS_OPEN ----
global open
open:
    push    ebx
    mov     eax, 5              ; SYS_OPEN
    mov     ebx, [esp + 8]      ; name
    mov     ecx, [esp + 12]     ; flags
    int     0x80
    pop     ebx
    ret

; ---- int close(int fd) : SYS_CLOSE ----
global close
close:
    push    ebx
    mov     eax, 6              ; SYS_CLOSE
    mov     ebx, [esp + 8]      ; fd
    int     0x80
    pop     ebx
    ret

; ---- int lseek(int fd, int off, int whence) : SYS_LSEEK ----
global lseek
lseek:
    push    ebx
    mov     eax, 19             ; SYS_LSEEK
    mov     ebx, [esp + 8]      ; fd
    mov     ecx, [esp + 12]     ; off
    mov     edx, [esp + 16]     ; whence
    int     0x80
    pop     ebx
    ret
