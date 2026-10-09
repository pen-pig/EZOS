; boot/boot.asm
[org 0x7c00]
[bits 16]

%include "boot/layout.inc"
; 布局常量（STAGE2_LBA / STAGE2_SECTORS / STAGE2_LOAD / KERNEL_DST ...）
; 都来自 layout.inc，Python 侧也解析同一份，不再各写各的。

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

    ; 跳 stage2：CS:IP = 0x1000:0000 = 线性 STAGE2_LOAD。stage2 仍在实模式
    ; （它要调用 INT13h），进保护模式是它搬完内核之后的事。
    mov dl, [BOOT_DRIVE]
    jmp 0x1000:0x0000

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
    ; 只读 stage2（固定 STAGE2_SECTORS 个扇区，从 STAGE2_LBA 到 STAGE2_LOAD）。
    ; 内核交给 stage2 接着读：BIOS INT13h 的目的地址撑死只能落在低 1MB，而
    ; 0xA0000 起是 VGA aperture，于是"引导扇区直接读内核"的上限只有 576KB。
    ; stage2 进保护模式把内核分批搬到 KERNEL_DST（19MB），上限由窗口决定、
    ; 不再是 BIOS 的 20 位地址。见 boot/stage2.asm。
    mov word [dap_sectors], STAGE2_SECTORS
    mov dword [dap_lba], STAGE2_LBA
    mov dword [dap_lba + 4], 0
    mov word [dap_offset], 0
    mov word [dap_segment], (STAGE2_LOAD >> 4)
    mov byte [retry_left], 3
.retry:
    mov si, dap
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jnc .ok
    dec byte [retry_left]
    jz disk_error
    xor ah, ah                  ; 复位磁盘系统后重试
    int 0x13
    jmp .retry
.ok:
    ret

dap:
    db 0x10                  ; DAP 结构大小
    db 0x00                  ; 保留，必须为 0
dap_sectors:
    dw 0                     ; 要读的扇区数
dap_offset:
    dw 0                     ; 目标偏移
dap_segment:
    dw 0                     ; 目标段（线性地址 = segment<<4 + offset）
dap_lba:
    dq 0                     ; 起始 LBA（运行时填，不是 boot 期常量）

disk_error:
    mov si, MSG_DISK_ERROR
    call print_string_16
    jmp $

; ------------------------------------------------------------------
; ------------------------------------------------------------------
; set_vbe: 探测 VBE 的线性帧缓冲（LFB），只认 16bpp：
;   1) 0x4F00 取 VBE BIOS 信息，并校验 'VESA' 签名；
;   2) 0x4F01 按分辨率从大到小逐个探测 16bpp 模式：
;      1280x1024(0x11A) -> 1024x768(0x117) -> 800x600(0x114)
;      -> 640x480(0x111)，每个模式都要求
;      attributes bit7（LFB）置位、XRES/YRES 非零、BPP==16、
;      PhysBasePtr 非零；命中第一个就作为选定分辨率。
;   成功：0x5000 写入 { dword LFB; word XRES; word YRES; byte BPP }
;   失败：0x5000 写 0，内核回退到 VGA 0x13 320x200。
;   注意：这里**不**调 0x4F02 切模式——文本模式还要留给内核 shell，
;        真正的切模式在用户态 gfx_init 里通过 VBE_DISPI 寄存器完成。
;   缓冲：VBEInfoBlock 与 mode info 都放在实模式 0x6000
;        （读 mode info 会覆盖 VBEInfoBlock，那时它已经用完了）。
; ------------------------------------------------------------------
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

; 标准 VESA 16bpp 模式号，按分辨率从大到小排，0 结尾
; standard VESA 16bpp modes, largest first, 0 terminated
; (0x115 is 24bpp and 0x110 is 15bpp - both rejected by BPP==16 check)
vbe_modes:
    dw 0x011A, 0x0117, 0x0114, 0x0111, 0

; A20 enable with triple fallback: BIOS int 15h -> port 0x92 -> keyboard ctrl
; (port 0x92 alone fails on legacy machines whose BIOS gates A20 via KBC)
enable_a20:
    mov ax, 0x2401              ; 1) BIOS: enable A20 (modern machines)
    int 0x15
    jnc .done
    in al, 0x92                 ; 2) fast A20 via chipset port 0x92
    test al, 2                  ;    already enabled?
    jnz .done
    or al, 2                    ;    set A20 only, keep bit0 (fast reset)
    out 0x92, al
    call .kbc_wait              ; 3) keyboard controller fallback (legacy)
    mov al, 0xD1                ;    write output port command
    out 0x64, al
    call .kbc_wait
    mov al, 0xDF                ;    enable A20 + peripherals
    out 0x60, al
.done:
    ret
.kbc_wait:                      ; wait KBC input buffer empty (with timeout)
    xor cx, cx
.kbc_spin:
    in al, 0x64
    test al, 2
    jz .kbc_ok
    loop .kbc_spin
.kbc_ok:
    ret

BOOT_DRIVE db 0
retry_left db 0
MSG_LOADING db 'Loading kernel...', 13, 10, 0
MSG_DISK_ERROR db 'Disk read error!', 13, 10, 0

; GDT 与保护模式入口都在 boot/stage2.asm：引导扇区自己已经不进保护模式了
; （省下的字节正好够放跳转 stage2 的远跳）。


times 508-($-$$) db 0
kernel_sectors: dw 0          ; LE16 sector count, written by tools/make_image.py
dw 0xaa55
