; boot/boot.asm
[org 0x7c00]
[bits 16]

KERNEL_OFFSET equ 0x10000     ; 内核加载地址
KERNEL_SECTORS equ 512        ; 内核总扇区数（256KB，与 build 脚本 --pad-to 262144 对应）

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl

    mov si, MSG_LOADING
    call print_string_16

    mov dl, [BOOT_DRIVE]
    call disk_load

    call set_vbe             ; try VBE 640x480x256 LFB, store LFB at 0x5000

    call enable_a20

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
    ; 扩展读循环（AH=42h）：总读 KERNEL_SECTORS 扇区，每批最多 64 扇区，
    ; 避免单次 DAP 超过 BIOS 限制。
    ; 入口：DL = 驱动器号
    mov bx, KERNEL_SECTORS      ; 剩余扇区数
    xor cx, cx                  ; 已读扇区数
.load_loop:
    test bx, bx
    jz .done
    ; 本次读取数 = min(bx, 64)
    mov ax, bx
    cmp ax, 64
    jle .batch_ok
    mov ax, 64
.batch_ok:
    mov word [dap_sectors], ax
    ; LBA = 1 + cx（跳过 boot 扇区）
    mov eax, ecx
    inc eax
    mov dword [dap_lba], eax
    ; 段地址 = (0x10000 + cx*512) >> 4，偏移 = 0
    mov eax, ecx
    shl eax, 9
    add eax, 0x10000
    shr eax, 4
    mov word [dap_segment], ax
    mov word [dap_offset], 0
    ; 调用 BIOS
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error
    ; 更新已读/剩余
    mov ax, word [dap_sectors]
    add cx, ax
    sub bx, ax
    jmp .load_loop
.done:
    ret

dap:
    db 0x10                  ; DAP 结构大小
    db 0x00                  ; 保留
dap_sectors:
    dw 0                     ; 本批扇区数
dap_offset:
    dw 0                     ; 偏移
dap_segment:
    dw 0                     ; 段（物理 = segment<<4 + offset）
dap_lba:
    dq 0                     ; 起始 LBA（动态计算，跳过 boot 扇区）

disk_error:
    mov si, MSG_DISK_ERROR
    call print_string_16
    jmp $

; ------------------------------------------------------------------
; set_vbe: VBE 多分辨率自适应探测（16bpp LFB）
;   1) 0x4F00 检查 VBE BIOS 存在 + 'VESA' 签名
;   2) 0x4F01 按分辨率从大到小依次探测标准 16bpp VBE 模式：
;      1280x1024(0x11A) -> 1024x768(0x117) -> 800x600(0x115)
;      -> 640x480(0x110)。每个模式要求：
;      attributes bit7 (LFB) 置位、XRES/YRES 非零、BPP==16、
;      PhysBasePtr 非零；命中第一个即为最大可用分辨率。
;   success: 0x5000 写入结构 { dword LFB; word XRES; word YRES; byte BPP }
;   failure: 0x5000 写入 0（内核回退 VGA 0x13 320x200）
;   NOTE: 刻意不调用 0x4F02 激活模式，保持文本模式供内核 shell 使用，
;         避免早期文本输出在 LFB 平面上不可见；gfx_init 在保护模式
;         通过 VBE_DISPI 寄存器按探测分辨率激活。
;   buffers: VBEInfoBlock / mode info 共用实模式 0x6000
;            （mode info 后写入会覆盖 VBEInfoBlock，无碍）
; ------------------------------------------------------------------
set_vbe:
    pusha
    push es
    xor ax, ax
    mov es, ax
    ; 1) check VBE BIOS present (function 00h) -> VBEInfoBlock at 0x6000
    mov di, 0x6000
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x004f
    jne .fail
    cmp dword [es:0x6000], 0x41534556   ; 'VESA' signature
    jne .fail
    ; 2) probe modes largest-first (vbe_modes terminated by 0)
    mov si, vbe_modes
.probe:
    mov bx, [si]                        ; candidate VBE mode number
    test bx, bx
    jz .fail                            ; end of list -> no usable mode
    push si
    push bx
    mov di, 0x6000
    mov ax, 0x4f01
    mov cx, bx
    int 0x10
    pop bx
    pop si
    cmp ax, 0x004f
    jne .next
    ; mode attributes bit7 = LFB supported (offset 0x00)
    mov bx, word [es:0x6000]
    test bx, 0x0080
    jz .next
    ; XResolution (0x12) / YResolution (0x14) must be non-zero
    cmp word [es:0x6012], 0
    je .next
    cmp word [es:0x6014], 0
    je .next
    ; BitsPerPixel (0x19) must be 16
    cmp byte [es:0x6019], 16
    jne .next
    ; LFB physical address (PhysBasePtr, offset 0x28) must be non-zero
    mov eax, dword [es:0x6028]
    test eax, eax
    jz .next
    ; success: store { LFB, XRES, YRES, BPP } at 0x5000
    mov dword [0x5000], eax
    mov ax, word [es:0x6012]
    mov word [0x5004], ax
    mov ax, word [es:0x6014]
    mov word [0x5006], ax
    mov byte [0x5008], 16
    pop es
    popa
    ret
.next:
    add si, 2
    jmp .probe
.fail:
    mov dword [0x5000], 0
    mov word [0x5004], 0
    mov word [0x5006], 0
    mov byte [0x5008], 0
    pop es
    popa
    ret

; 标准 VBE 16bpp 模式表，按分辨率从大到小排列，0 结尾
vbe_modes:
    dw 0x011A, 0x0117, 0x0115, 0x0110, 0

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

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
