; boot/boot.asm
[org 0x7c00]
[bits 16]

KERNEL_OFFSET equ 0x10000     ; 内核加载地址

; ===== VBE 信息传递地址 =====
VBE_INFO   equ 0x6000         ; VBE 控制器信息缓冲区（512 字节）
VBE_MODE   equ 0x6200         ; VBE 模式信息缓冲区（256 字节）
VBE_RESULT equ 0x5000         ; 传递给内核的 VBE 结果
;   [0x5000] dword framebuffer 物理地址
;   [0x5004] word  屏幕宽度
;   [0x5006] word  屏幕高度
;   [0x5008] byte  bpp
;   [0x5009] byte  vbe_ok (1=成功)
;   [0x500A] word  pitch (每行字节数)

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl

    mov si, MSG_LOADING
    call print_string_16

    mov bx, 0x0000
    mov ax, 0x1000
    mov es, ax
    mov dh, 96                ; 读取 96 个扇区（48KB）
    mov dl, [BOOT_DRIVE]
    call disk_load

    call enable_a20
    call detect_vbe           ; VBE 检测：设置最佳图形模式并传递 framebuffer 信息

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_start

[bits 16]
print_string_16:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp print_string_16
.done:
    ret

disk_load:
    push dx
    mov ah, 0x02
    mov al, dh
    mov ch, 0x00
    mov dh, 0x00
    mov cl, 0x02
    int 0x13
    jc disk_error
    pop dx
    cmp dh, al
    jne disk_error
    ret

disk_error:
    mov si, MSG_DISK_ERROR
    call print_string_16
    jmp $

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

; ============ VBE 检测与设置 ============
; 通过 int 0x10 VBE BIOS 调用检测 VBE 支持，选择最佳分辨率
; 优先 1024x768x32bpp，其次 800x600x32bpp、640x480x32bpp，
; 再尝试 24bpp 模式，全部失败则 vbe_ok=0（内核回退 VGA 0x13）
detect_vbe:
    push es
    push ds
    xor ax, ax
    mov es, ax                ; ES=0，确保 VBE 缓冲区在 0x6000/0x6200，不覆盖内核
    ; 初始化结果
    mov dword [VBE_RESULT], 0
    mov word [VBE_RESULT + 4], 0
    mov word [VBE_RESULT + 6], 0
    mov byte [VBE_RESULT + 8], 0
    mov byte [VBE_RESULT + 9], 0
    mov word [VBE_RESULT + 10], 0
    ; 检测 VBE 支持 (AX=0x4F00)
    mov ax, 0x4F00
    mov di, VBE_INFO
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe
    cmp word [VBE_INFO], 0x4156      ; "VA"
    jne .no_vbe
    cmp word [VBE_INFO + 2], 0x4553  ; "ES"
    jne .no_vbe
    ; 遍历候选模式表
    mov si, vbe_mode_table
.try_mode:
    mov cx, [si]              ; 模式号
    cmp cx, 0xFFFF
    je .no_vbe                ; 表结束
    ; 获取模式信息 (AX=0x4F01)
    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .next_mode
    ; ModeAttributes: bit0=支持, bit4=图形, bit7=线性帧缓冲
    mov bx, [VBE_MODE]
    test bl, 0x91
    jz .next_mode
    ; 检查分辨率
    mov ax, [VBE_MODE + 0x12]
    cmp ax, [si + 2]
    jne .next_mode
    mov ax, [VBE_MODE + 0x14]
    cmp ax, [si + 4]
    jne .next_mode
    ; 检查 bpp
    mov al, [VBE_MODE + 0x19]
    cmp al, [si + 6]
    jne .next_mode
    ; 检查 PhysBasePtr 非零
    mov eax, [VBE_MODE + 0x28]
    test eax, eax
    jz .next_mode
    ; 设置模式 (AX=0x4F02, BX = mode | 0x4000 线性帧缓冲)
    mov bx, cx
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .next_mode
    ; 成功！保存结果
    mov eax, [VBE_MODE + 0x28]
    mov [VBE_RESULT], eax
    mov ax, [VBE_MODE + 0x12]
    mov [VBE_RESULT + 4], ax
    mov ax, [VBE_MODE + 0x14]
    mov [VBE_RESULT + 6], ax
    mov al, [VBE_MODE + 0x19]
    mov [VBE_RESULT + 8], al
    mov byte [VBE_RESULT + 9], 1
    mov ax, [VBE_MODE + 0x10]
    mov [VBE_RESULT + 10], ax
    jmp .done
.next_mode:
    add si, 8
    jmp .try_mode
.no_vbe:
    mov byte [VBE_RESULT + 9], 0
.done:
    pop ds
    pop es
    ret

vbe_mode_table:
    dw 0x122, 1024, 768, 32   ; 1024x768x32bpp
    dw 0x121, 800, 600, 32    ; 800x600x32bpp
    dw 0x120, 640, 480, 32    ; 640x480x32bpp
    dw 0x118, 1024, 768, 24   ; 1024x768x24bpp
    dw 0x115, 800, 600, 24    ; 800x600x24bpp
    dw 0x112, 640, 480, 24    ; 640x480x24bpp
    dw 0xFFFF

BOOT_DRIVE db 0
MSG_LOADING db 'Loading kernel...', 13, 10, 0
MSG_DISK_ERROR db 'Disk read error!', 13, 10, 0

gdt_start:

gdt_null:
    dd 0x0
    dd 0x0

gdt_code:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0

gdt_data:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

[bits 32]
protected_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    call KERNEL_OFFSET
    jmp $

times 510-($-$$) db 0
dw 0xaa55
