; boot/boot.asm
[org 0x7c00]
[bits 16]

KERNEL_OFFSET equ 0x10000     ; �ں˼��ص�ַ
KERNEL_SECTORS equ 512        ; �ں�����������256KB���� build �ű� --pad-to 262144 ��Ӧ��

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
    ; ��չ��ѭ����AH=42h�����ܶ� KERNEL_SECTORS ������ÿ����� 64 ������
    ; ���ⵥ�� DAP ���� BIOS ���ơ�
    ; ��ڣ�DL = ��������
    mov bx, KERNEL_SECTORS      ; ʣ��������
    xor ecx, ecx                  ; �Ѷ�������
.load_loop:
    test bx, bx
    jz .done
    ; ���ζ�ȡ�� = min(bx, 64)
    mov ax, bx
    cmp ax, 64
    jle .batch_ok
    mov ax, 64
.batch_ok:
    mov word [dap_sectors], ax
    ; LBA = 1 + cx������ boot ������
    mov eax, ecx
    inc eax
    mov dword [dap_lba], eax
    ; �ε�ַ = (0x10000 + cx*512) >> 4��ƫ�� = 0
    mov eax, ecx
    shl eax, 9
    add eax, 0x10000
    shr eax, 4
    mov word [dap_segment], ax
    mov word [dap_offset], 0
    ; ���� BIOS
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error
    ; �����Ѷ�/ʣ��
    mov ax, word [dap_sectors]
    add cx, ax
    sub bx, ax
    jmp .load_loop
.done:
    ret

dap:
    db 0x10                  ; DAP �ṹ��С
    db 0x00                  ; ����
dap_sectors:
    dw 0                     ; ����������
dap_offset:
    dw 0                     ; ƫ��
dap_segment:
    dw 0                     ; �Σ����� = segment<<4 + offset��
dap_lba:
    dq 0                     ; ��ʼ LBA����̬���㣬���� boot ������

disk_error:
    mov si, MSG_DISK_ERROR
    call print_string_16
    jmp $

; ------------------------------------------------------------------
; set_vbe: VBE ��ֱ�������Ӧ̽�⣨16bpp LFB��
;   1) 0x4F00 ��� VBE BIOS ���� + 'VESA' ǩ��
;   2) 0x4F01 ���ֱ��ʴӴ�С����̽���׼ 16bpp VBE ģʽ��
;      1280x1024(0x11A) -> 1024x768(0x117) -> 800x600(0x115)
;      -> 640x480(0x110)��ÿ��ģʽҪ��
;      attributes bit7 (LFB) ��λ��XRES/YRES ���㡢BPP==16��
;      PhysBasePtr ���㣻���е�һ����Ϊ�����÷ֱ��ʡ�
;   success: 0x5000 д��ṹ { dword LFB; word XRES; word YRES; byte BPP }
;   failure: 0x5000 д�� 0���ں˻��� VGA 0x13 320x200��
;   NOTE: ���ⲻ���� 0x4F02 ����ģʽ�������ı�ģʽ���ں� shell ʹ�ã�
;         ���������ı������ LFB ƽ���ϲ��ɼ���gfx_init �ڱ���ģʽ
;         ͨ�� VBE_DISPI �Ĵ�����̽��ֱ��ʼ��
;   buffers: VBEInfoBlock / mode info ����ʵģʽ 0x6000
;            ��mode info ��д��Ḳ�� VBEInfoBlock���ް���
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

; ��׼ VBE 16bpp ģʽ�������ֱ��ʴӴ�С���У�0 ��β
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
