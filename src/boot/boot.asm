; boot/boot.asm
[org 0x7c00]
[bits 16]

KERNEL_OFFSET equ 0x10000     ; 内核加载地址

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
