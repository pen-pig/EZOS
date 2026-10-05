; boot/mbr_usb.asm - hybrid MBR for the USB stick image (tools/make_usb_image.py)
;
; Why this exists: a bare custom boot sector (our boot/boot.asm) at LBA 0 has
; neither a BPB nor a partition table. Plenty of real BIOSes refuse to boot
; such a stick (they want a BPB to size the media, and an *active* partition
; entry to accept it as USB-HDD), and no UEFI firmware will ever find it
; because UEFI only looks at partitions holding /EFI/BOOT/BOOTIA32.EFI.
;
; Layout of usb-image.bin:
;   LBA 0     this MBR: BPB + active EFI partition entry + chainload code
;   LBA 1..   kernel (same bytes as in os-image.bin, so boot/boot.asm keeps
;             loading it from LBA 1 and needs no change at all)
;   LBA 1024  copy of the real boot sector (boot/boot.asm, org 0x7C00)
;   LBA 2048  FAT32 EFI system partition with \EFI\BOOT\BOOTIA32.EFI
;
; TWO TRAPS this code is written around - both found the hard way on QEMU
; (SeaBIOS 1.16), and both apply to real BIOSes too:
;
; 1. DO NOT point INT13h AH=42h at 0x7C00 while you are still executing from
;    0x7C00. The BIOS loads us there, so an extended read whose destination
;    covers our own code never returns: SeaBIOS wedges the moment the transfer
;    starts overwriting the sector it is running. Reading the same LBA to
;    0x8000 works fine, so it is the self-overlap, not the LBA. Fix: relocate
;    this sector to 0x0600 first, then read the VBR into 0x7C00.
;
; 2. Interrupts must be enabled around INT13h. SeaBIOS hands over with IF=0 and
;    its ATA PIO transfer parks waiting for the disk IRQ, so the call hangs.
;    STI before touching the disk, and leave IF set for the boot sector.
;
; Comments are ASCII on purpose - the historical GBK->UTF-8 damage in some
; files in this repo turned Chinese comments into mojibake.
[org 0x0600]
[bits 16]

VBR_LBA equ 1024                ; where the real boot sector lives (patched)
BOOT_DRIVE_SAVE equ 0x0500      ; scratch byte below 0x7C00 (never overwritten)

start:
    jmp short reloc
    nop

oem_name:   db "EZOSMBR "

; ---------- BPB (0x0B .. 0x23) ----------
bps:            dw 512
sec_per_clus:   db 1
reserved_sec:   dw 1
num_fats:       db 1
root_entries:   dw 0            ; 0 -> FAT32 style BPB
total_sec16:    dw 0
media_desc:     db 0xF8
fat_size16:     dw 0
sec_per_track:  dw 63
num_heads:      dw 16
hidden_sec:     dd 0
total_sec32:    dd 0            ; patched by tools/make_usb_image.py

; ---------- extended BPB (0x24 .. 0x3D) ----------
drive_num:      db 0x80
reserved_bpb:   db 0
boot_sig:       db 0x29
volume_id:      dd 0x1BADB002
volume_label:   db "EZOS USB   "
fs_type:        db "FAT32   "

times 0x5A-($-$$) db 0

; ---------- relocation stub (runs from 0x7C00, position-independent) ----------
; Only immediate operands and registers here: until the far jump below lands we
; are executing at 0x7C00 while assembled for 0x0600, so any absolute memory
; reference would point at the wrong copy.
reloc:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [BOOT_DRIVE_SAVE], dl   ; DL = drive the BIOS booted us from
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    jmp 0x0000:main             ; continue from the relocated copy

; ---------- main: now safe to use absolute addresses and the disk ----------
main:
    sti                         ; trap 2: INT13h needs the disk IRQ
%ifdef DEBUG_MARK
    mov al, '1'
    call dbg
    mov al, dl
    call dbg
%endif
    mov bp, 3                   ; 3 attempts, reset between them
.retry:
    mov si, dap
    mov ah, 0x42                ; extended read (LBA) -> 0000:7C00
    mov dl, [BOOT_DRIVE_SAVE]
%ifdef DEBUG_MARK
    mov al, 'X'
    call dbg
%endif
    int 0x13
%ifdef DEBUG_MARK
    pushf
    mov al, 'R'
    call dbg
    mov al, ah
    call dbg
    popf
%endif
    jnc .loaded
    dec bp
    jz .fail
    xor ah, ah                  ; reset disk system, then retry
    int 0x13
    jmp .retry

.loaded:
    mov dl, [BOOT_DRIVE_SAVE]   ; the boot sector re-reads DL for its own loads
%ifdef DEBUG_MARK
    mov al, 'L'
    call dbg
    jmp $                       ; debug: stop instead of jumping to the VBR
%endif
    jmp 0000:0x7C00

.fail:
    mov si, err_msg
    call print_str
    jmp $

print_str:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    xor bx, bx                  ; page 0, else the char lands on an invisible page
    int 0x10
    jmp print_str
.done:
    ret

err_msg:        db "EZOS: cannot read boot sector", 0

dbg:
    ; Write AL to the QEMU debug console (port 0xE9). Cheaper and far more
    ; reliable than OCR-ing the VGA screen while debugging a boot sector.
    push dx
    push ax
    mov dx, 0xE9
    out dx, al
    pop ax
    pop dx
    ret

; Disk Address Packet for INT13h AH=42h (VBR_LBA -> 0000:7C00, 1 sector)
dap:            db 0x10
                db 0x00
dap_sectors:    dw 1
dap_offset:     dw 0x7C00
dap_segment:    dw 0x0000
dap_lba:        dd VBR_LBA
                dd 0

times 446-($-$$) db 0

; ---------- partition table (entry 1 patched by the tool) ----------
;   status/CHS/type/CHS/LBA-start/LBA-count; entries 2-4 stay zero.
part1_status:   db 0x80                         ; active (legacy BIOS wants it)
part1_chs0:     db 0x00, 0x01, 0x00            ; CHS start = 0/0/1
part1_type:     db 0xEF                         ; EFI System Partition
part1_chs1:     db 0x00, 0x00, 0x00            ; CHS end (LBA is authoritative)
part1_lba:      dd 2048                         ; patched
part1_sectors:  dd 0                            ; patched

times 510-($-$$) db 0
                dw 0xAA55
