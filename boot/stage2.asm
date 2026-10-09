; boot/stage2.asm —— 二级加载器：把内核搬到 1MB 以上，解除低 1MB 窗口
;
; 为什么存在：BIOS 的 INT13h AH=42h 用 seg:off 寻址，目的地址必须落在低
; 1MB，而 0xA0000 起是 VGA aperture（不是平坦 RAM）——所以"引导扇区直接读
; 内核"的上限被硬件钉死在 0x10000..0xA0000 = 576KB。内核迟早会超过它
; （CI 的 gcc13 -O2 已经 561KB，只剩 28KB）。
;
; 流程（实模式读盘 + 保护模式搬运，交替进行）：
;   1) INT13h 把一批（32KB）读到低地址 bounce 缓冲（BIOS 只能写这里）。
;   2) 进保护模式，用 32 位平坦段 rep movsd 搬到 KERNEL_DST（19MB）。
;   3) 退回实模式并**彻底还原 CPU 状态** → 下一批。
;   4) 搬完再一次进保护模式，设栈，跳 KERNEL_DST。
;
; ★★ 回实模式必须做全的三件事（少一件就是"第二批读盘卡死"，都是实测）★★
;   1) **先**切到 16 位代码段（base=STAGE2_LOAD、limit 0xFFFF、D=0），**再**
;      清 CR0.PE。反过来的话，清 PE 那一刻 CS 缓存还是 32 位描述符，紧跟着
;      的远跳会被按 32 位解码，机器直接跑飞。
;   2) **再跳一次**，把 CS 的"可见值"从保护模式选择子换回实模式段值
;      （STAGE2_SEG）。刚从 16 位代码段进来时 CS 的可见值是 CODE16_SEG
;      (0x18)，而缓存里 base 是对的（STAGE2_LOAD）。看起来一切正常 —— 直到
;      下一次 `int`：CPU 压栈的 CS 是 0x18，`iret` 时已经回到实模式，CPU 按
;      "段值"解释它，base 变成 0x18*16 = 0x180，下一条指令就飞到 0x180+IP
;      那片垃圾里（抓到过 CS=0018/EIP=0x18F、线性 0x30F 全是 0x00 0x53 0xff
;      → 无限 #UD）。
;   3) 逐个重载 ds/es/fs/gs/ss，并把 BIOS 原来的 GDTR 还回去。让段寄存器
;      缓存回到真正的实模式（limit 0xFFFF），BIOS 的 16/32 位切换才有干净
;      的起点。
;
; ★ 为什么不用 unreal mode（少切几次 CR0，看着更省事，但别用）：
;   unreal 让 CPU 留在"寄存器看着是实模式、缓存里却是 4GB 限长段"的混合状态，
;   而 BIOS 的 INT13h 内部要在 16 位与 32 位之间来回切换。实测：搬到第 11 批
;   机器就重启/跑飞；关中断、还原 GDTR、还原 ES/FS/GS、改小搬运块、换 bounce
;   地址 —— 全试过，都没用。跟 BIOS 打交道就给它一个干净的实模式状态。
;
; ★ [org STAGE2_LOAD] 让 NASM 把**标签**解析成 disp = label - org，所以运行时
;   ds 必须等于 STAGE2_LOAD>>4。踩过：ds=0 时 lgdt 装进去的是 BIOS 残留字节，
;   紧接着 `mov es,bx` 直接 #GP。`equ` 出来的绝对常量（如 KERNEL_SECTORS_FIELD）
;   不受 org 影响，读引导扇区里的字段要显式用 es=0。
;
; 由 boot/boot.asm 从 STAGE2_LBA 读入 STAGE2_LOAD 后跳过来；入口 DL=启动盘。

; nasm 只在 CWD 与 -i 目录里找 include，不按被 include 文件所在目录找，
; 所以写相对仓库根的路径（ninja 与 ci/build.sh 都从仓库根执行）。
; 必须在 [org] 之前：org 要用到 STAGE2_LOAD。
%include "boot/layout.inc"

[bits 16]
[org STAGE2_LOAD]

STAGE2_SEG equ (STAGE2_LOAD >> 4)

start:
    mov ax, STAGE2_SEG
    mov ds, ax                  ; 标签相对 STAGE2_LOAD 解析，ds 必须跟着走
    mov es, ax
    mov [BOOT_DRIVE], dl
    sgdt [bios_gdtr]            ; 记下 BIOS 的 GDTR / fs / gs，每次从 PM
    mov [bios_fs], fs           ; 回来都要原样还给它
    mov [bios_gs], gs

    ; 栈：0x7C00 向下（引导扇区在 0x7C00，往下是空的）。BIOS 调用要压栈。
    cli
    xor ax, ax
    mov ss, ax
    mov sp, STAGE1_STACK

    ; 真实内核扇区数由 tools/make_image.py 打进引导扇区 0x1FC（es=0 显式指定）。
    ; 0（未打补丁的裸 boot.bin）或超上限 → 兜底成上限（fail closed）。
    xor ax, ax
    mov es, ax
    mov ax, [es:KERNEL_SECTORS_FIELD]
    test ax, ax
    jz .use_max
    cmp ax, KERNEL_MAX_SECTORS
    jbe .have_count
.use_max:
    mov ax, KERNEL_MAX_SECTORS
.have_count:
    mov [sectors_left], ax
    mov ax, STAGE2_SEG
    mov es, ax

    mov dword [dst_ptr], KERNEL_DST
    mov dword [lba_lo], KERNEL_LBA

.read_loop:
    cmp word [sectors_left], 0
    je .done

    ; 本批扇区数 = min(剩余, BATCH_SECTORS)
    mov ax, [sectors_left]
    cmp ax, BATCH_SECTORS
    jbe .batch_ok
    mov ax, BATCH_SECTORS
.batch_ok:
    mov [cur_batch], ax

    ; DAP：LBA = lba_lo，目的 = BOUNCE_SEG:0000
    mov word [dap_sectors], ax
    mov eax, [lba_lo]
    mov dword [dap_lba], eax
    mov dword [dap_lba + 4], 0
    mov word [dap_offset], 0
    mov word [dap_segment], BOUNCE_SEG

    mov byte [retry_left], 3
    mov fs, [bios_fs]
    mov gs, [bios_gs]
.retry:
    mov si, dap
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jnc .read_ok
    dec byte [retry_left]
    jz disk_error
    xor ah, ah                  ; 复位磁盘系统后重试同一批
    int 0x13
    jmp .retry

.read_ok:
    call copy_to_high           ; 进 PM 搬这一批，退回时已还原成干净实模式

    movzx eax, word [cur_batch]
    add [lba_lo], eax           ; LBA 前进
    sub [sectors_left], ax      ; 剩余减少
    mov al, '.'
    call serial_putc
    jmp .read_loop

.done:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    ; 必须 dword：32 位远跳（0x66 EA）。写 `jmp CODE_SEG:pm32` 会按 16 位
    ; 编码，偏移被截成 0x010A —— 跳进 IVT 里去。
    jmp dword CODE_SEG:pm32

; ------------------------------------------------------------------
; copy_to_high：进保护模式把 bounce 缓冲里的一批搬到 [dst_ptr]，再退回实模式。
;   进：lgdt 我们的 GDT + CR0.PE=1 + 32 位远跳
;   搬：32 位平坦段下的 rep movsd（32 位 PM 里完全可靠）
;   退：见文件头的"回实模式必须做全的三件事"
;   返回时 ds=STAGE2_SEG、es/fs/gs/ss=0、CS 可见值 = STAGE2_SEG。
; ------------------------------------------------------------------
[bits 16]
copy_to_high:
    cli
    ; ★ 保存 sp：PM 里必须重装 ss（ss=0 在保护模式下是空选择子，一用栈就
    ; #GP），而 `mov ss, ax` 会顺带把 esp 拉回栈顶 —— 回来时 `ret` 就会弹到
    ; 垃圾地址（表现是"串口打完第一个字符就再没动静"）。记下调用时的 sp，
    ; 退出 PM 前原样写回。
    mov [saved_sp], sp
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword CODE_SEG:.pm_copy

[bits 32]
.pm_copy:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    movzx esp, word [saved_sp]

    movzx ecx, word [cur_batch]
    shl ecx, 9                  ; 字节
    shr ecx, 2                  ; 双字数
    mov esi, BOUNCE_LIN
    mov edi, [dst_ptr]
    cld
    rep movsd
    mov [dst_ptr], edi

    movzx esp, word [saved_sp]  ; 回到 call 时的位置，ret 才能弹对

    ; 第 1 件事：先切 16 位代码段（base=STAGE2_LOAD / limit 0xFFFF / D=0），
    ; 再清 PE。当前 CS 是 32 位的，裸 0xEA 会被当 off32+seg 解码，所以必须
    ; 加 0x66 压成 ptr16:16。
    db 0x66, 0xEA
    dw (.rm_return - $$)
    dw CODE16_SEG

[bits 16]
.rm_return:
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax

    ; 第 2 件事：把 CS 可见值换回实模式段值（详见文件头）。此处 CS 的 D 位
    ; 已经是 0，`jmp seg:off` 就是 EA off16 seg16，不需要 0x66。
    jmp STAGE2_SEG:.rm16
.rm16:

    ; 第 3 件事：还回 BIOS 的 GDTR，并逐个重载段寄存器清掉 PM 的缓存
    lgdt [bios_gdtr]
    mov fs, [bios_fs]
    mov gs, [bios_gs]
    xor ax, ax
    mov es, ax
    mov ss, ax                  ; sp 已在 PM 里还原成 saved_sp，这里别再动
    mov ax, STAGE2_SEG
    mov ds, ax
    ret

[bits 32]
pm32:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x200000           ; 主栈 1-2MB（同 linker.ld __stack_top）
    jmp KERNEL_DST

; ------------------------------------------------------------------
[bits 16]
serial_putc:
    push dx
    push ax
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    pop ax
    out dx, al
    pop dx
    ret

disk_error:
    mov al, 'E'
    call serial_putc
    mov si, MSG_DISK_ERROR
    call print_string_16
.hang:
    jmp .hang

print_string_16:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp print_string_16
.done:
    ret

; ---- 数据 ----
BOOT_DRIVE   db 0
sectors_left dw 0
cur_batch    dw 0
retry_left   db 0
dst_ptr      dd 0
lba_lo       dd 0
saved_sp     dw 0
bios_fs      dw 0
bios_gs      dw 0
bios_gdtr    dw 0
             dw 0
             dw 0                ; sgdt 写 6 字节：limit(2) + base(4)
             dw 0
MSG_DISK_ERROR db 'stage2: disk read error', 13, 10, 0

dap:
    db 0x10                     ; DAP 结构大小
    db 0x00                     ; 未用
dap_sectors:
    dw 0                        ; 本批扇区数
dap_offset:
    dw 0                        ; 目的偏移
dap_segment:
    dw 0                        ; 目的段（线性 = segment<<4 + offset）
dap_lba:
    dq 0                        ; 起始 LBA

; ---- GDT ----
gdt_start:
gdt_null:
    dd 0x0
    dd 0x0
gdt_code:                       ; 32 位代码段：base 0 / limit 4GB
    dw 0xffff
    dw 0x0
    db 0x0
    db 10011010b                ; 可执行/可读/代码段
    db 11001111b                ; 4KB 粒度 + 32 位
    db 0x0
gdt_data:                       ; 32 位数据段：base 0 / limit 4GB
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b                ; 可读写数据段
    db 11001111b
    db 0x0
; 16 位代码段：base = STAGE2_LOAD、limit 0xFFFF、D=0。
; 回实模式的中转站 —— 跳进来后 CS 缓存的 base/limit/D 与"实模式 CS=STAGE2_SEG"
; 完全一致，只差 CS 的可见值（那是第 2 件事要补的）。
gdt_code16:
    dw 0xffff                       ; limit 0xFFFF
    dw (STAGE2_LOAD & 0xFFFF)       ; base[15:0]
    db ((STAGE2_LOAD >> 16) & 0xFF) ; base[23:16]
    db 10011010b                    ; 代码段，可读
    db 00000000b                    ; G=0 D=0（16 位）limit[19:16]=0
    db 0x0                          ; base[31:24]
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG   equ gdt_code - gdt_start
DATA_SEG   equ gdt_data - gdt_start
CODE16_SEG equ gdt_code16 - gdt_start

; 固定 STAGE2_SECTORS 个扇区：make_image.py 会按这个值校验。
; 代码写超了 nasm 会在这里报 "TIMES value ... is negative"，等于编译期护栏。
times (STAGE2_SECTORS * 512) - ($ - $$) db 0
