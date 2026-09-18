; boot/kernel_entry.asm
[bits 32]
[extern kernel_main]
[extern irq0_handler]
[extern irq1_handler]
[extern irq12_handler]
[extern irq11_handler]
[extern isr_dispatch]
[extern syscall_handler]
[extern __bss_start]
[extern __bss_end]
[extern __hbss_start]
[extern __hbss_end]

global _start
global irq0
global irq1
global irq12
global irq11
global idt_flush
global isr_stub_table
global gdt_flush
global tss_flush
global syscall_entry
global enter_usermode
global g_kernel_esp_save
global g_kernel_eflags
global g_user_exited

_start:
    mov esp, 0x90000

    ; ÇåÁã .bss ¶Î£¨ÄÚºË¾µÏñÖ»¼ÓÔØ 64KB£¬³¬³ö²¿·Ö±£³Ö BIOS ²ÐÁô£¬±ØÐëÏÔÊ½ÇåÁã£©
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; zero .bss.hi (high memory at 1MB+, read-only FS driver buffers)
    mov edi, __hbss_start
    mov ecx, __hbss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kernel_main
    cli
    hlt

; IRQ0 ÖÐ¶ÏÈë¿Ú£¨PIT ¶¨Ê±Æ÷£©
irq0:
    cli
    pusha
    call irq0_handler
    popa
    sti
    iret

; IRQ1 ÖÐ¶ÏÈë¿Ú
irq1:
    cli
    pusha
    call irq1_handler
    popa
    sti
    iret

; IRQ12 ÖÐ¶ÏÈë¿Ú£¨Êó±ê£©
irq12:
    cli
    pusha
    call irq12_handler
    popa
    sti
    iret

; IRQ11 entry (RTL8139 NIC). Gate 0x8E clears IF on entry; iret restores
; it from the saved EFLAGS, so no explicit sti here (no reentry window).
irq11:
    cli
    pusha
    call irq11_handler
    popa
    iret

; ¼ÓÔØ IDT µÄº¯Êý
; ²ÎÊý£ºuint32_t idt_ptr£¨Ö¸Ïò idt_ptr ½á¹¹µÄÖ¸Õë£©
idt_flush:
    mov eax, [esp + 4]    ; »ñÈ¡²ÎÊý£¨idt_ptr µØÖ·£©
    lidt [eax]            ; ¼ÓÔØ IDT
    ret

; ==================== CPU Òì³£×®£¨ÏòÁ¿ 0-31£©====================
; Õ»²¼¾Ö£¨isr_common ÖÐ£¬´Ó esp ÏòÉÏ£©£º
;   [esp..esp+31]  pusha Çø£¨edi esi ebp espÕ¼Î» ebx edx ecx eax£©
;   [esp+32]=ÏòÁ¿ºÅ  [esp+36]=´íÎóÂë(ÎÞÂëÏòÁ¿µæ 0)
;   [esp+40]=EIP  [esp+44]=CS  [esp+48]=EFLAGS
; ring0 Í¬¼¶´¥·¢£¬ÎÞ ESP/SS Ñ¹Õ»£»C ²à isr_regs_t Óë´Ë²¼¾ÖÒ»ÖÂ¡£
%macro ISR_STUB_ERR 1
isr_stub_%1:
    cli
    push dword %1            ; ÏòÁ¿ºÅ£¨´íÎóÂë CPU ÒÑÑ¹£©
    jmp isr_common
%endmacro

%macro ISR_STUB_NOERR 1
isr_stub_%1:
    cli
    push dword 0            ; µæ 0 ´íÎóÂë£¬Óë´øÂëÏòÁ¿²¼¾Ö¶ÔÆë
    push dword %1
    jmp isr_common
%endmacro

; ´íÎóÂë¹æÔò£¨Intel SDM Vol.3 6.13£©£º½öÏòÁ¿ 8/10/11/12/13/14/17/30 ÓÉ CPU Ñ¹Èë
; ´íÎóÂë£¬ÆäÓà±ØÐëµæ 0 ²ÅÄÜÓë´øÂëÏòÁ¿¹²ÓÃÍ¬Ò»Õ»²¼¾Ö¡£
;   #DE #DB NMI #BP #OF #BR #UD #NM | #DF | #TS #NP #SS #GP #PF | #MF #AC #MC #XM
;   0   1   2   3   4   5   6   7   | 8   | 10  11  12  13  14   | 16  17  18  19
ISR_STUB_NOERR 0            ; #DE ³ýÁã
ISR_STUB_NOERR 1            ; #DB µ÷ÊÔ
ISR_STUB_NOERR 2            ; NMI
ISR_STUB_NOERR 3            ; #BP ¶Ïµã£¨ÏÝÚå£¬ÎÞ´íÎóÂë£©
ISR_STUB_NOERR 4            ; #OF into Òç³ö
ISR_STUB_NOERR 5            ; #BR bnd Ô½½ç
ISR_STUB_NOERR 6            ; #UD ·Ç·¨Ö¸Áî
ISR_STUB_NOERR 7            ; #NM Éè±¸²»¿ÉÓÃ
ISR_STUB_ERR   8            ; #DF Ë«ÖØ¹ÊÕÏ£¨´íÎóÂëºã 0£©
ISR_STUB_NOERR 9            ; Ð­´¦ÀíÆ÷¶ÎÒç³ö£¨±£Áô£©
ISR_STUB_ERR   10           ; #TS ÎÞÐ§ TSS
ISR_STUB_ERR   11           ; #NP ¶Î²»´æÔÚ
ISR_STUB_ERR   12           ; #SS Õ»¶Î¹ÊÕÏ
ISR_STUB_ERR   13           ; #GP Í¨ÓÃ±£»¤
ISR_STUB_ERR   14           ; #PF È±Ò³£¨CR2=¹ÊÕÏµØÖ·£©
ISR_STUB_NOERR 15           ; ±£Áô
ISR_STUB_NOERR 16           ; #MF x87 ¸¡µã
ISR_STUB_ERR   17           ; #AC ¶ÔÆë¼ì²é£¨´íÎóÂëºã 0£©
ISR_STUB_NOERR 18           ; #MC »úÆ÷¼ì²é
ISR_STUB_NOERR 19           ; #XM SIMD ¸¡µã
ISR_STUB_NOERR 20           ; #VE ÐéÄâ»¯
ISR_STUB_NOERR 21
ISR_STUB_NOERR 22
ISR_STUB_NOERR 23
ISR_STUB_NOERR 24
ISR_STUB_NOERR 25
ISR_STUB_NOERR 26
ISR_STUB_NOERR 27
ISR_STUB_NOERR 28
ISR_STUB_NOERR 29
ISR_STUB_ERR   30           ; #SX °²È«Òì³£
ISR_STUB_NOERR 31

isr_common:
    pusha                    ; [esp]=pusha Çø 32B
    mov eax, esp             ; eax -> isr_regs_t »ùÖ·£¨º¬ÏòÁ¿/´íÎóÂë/EIP/CS/EFLAGS£©
    push eax
    call isr_dispatch
    add esp, 4
    popa
    add esp, 8               ; µ¯ÏòÁ¿ºÅ + ´íÎóÂë/µæÎ»
    iret

; C ¿É¼ûµÄ×®µØÖ·±í£¨panic.c ±éÀú×¢²á IDT 0-31£©
;
; ±ØÐëÖðÏîÏÔÊ½ÁÐ³ö£¬²»ÄÜÐ´³É
;     %assign i 0 / %rep 32 / dd isr_stub_%+i / %assign i i+1 / %endrep
; ¡ª¡ªNASM µÄ %+ Æ´½Ó·¢ÉúÔÚ %assign ±äÁ¿Õ¹¿ªÖ®Ç°£¬»á°Ñ 32 ¸ö±íÏîÈ«²¿½âÎö³É
; Í¬Ò»¸ö·ûºÅ£¨Êµ²âÈ«Ö¸Ïò isr_stub_0£©£¬ÓÚÊÇÈÎºÎÒì³£¶¼ÉÏ±¨ "#DE vector 00"£¬
; ÇÒ isr_regs_t ÕûÌå´íÎ» 4 ×Ö½Ú£¨EIP ¶Áµ½´íÎóÂë¡¢CS ¶Áµ½Õæ EIP¡¢EFLAGS ¶Áµ½
; Õæ CS=0x8£©¡£¸ÃÈ±ÏÝÓÉ test_step3.py µÄÕæ×Ö¿â OCR ¶ÏÑÔÊ×´Î±©Â¶¡ª¡ª
; ²½Öè 1 µÄ"À¶ÆÁÅäÉ«/ÏñËØÍ³¼Æ"Àà´Ö¼ìÍêÈ«·¢ÏÖ²»ÁË¡£
isr_stub_table:
    dd isr_stub_0
    dd isr_stub_1
    dd isr_stub_2
    dd isr_stub_3
    dd isr_stub_4
    dd isr_stub_5
    dd isr_stub_6
    dd isr_stub_7
    dd isr_stub_8
    dd isr_stub_9
    dd isr_stub_10
    dd isr_stub_11
    dd isr_stub_12
    dd isr_stub_13
    dd isr_stub_14
    dd isr_stub_15
    dd isr_stub_16
    dd isr_stub_17
    dd isr_stub_18
    dd isr_stub_19
    dd isr_stub_20
    dd isr_stub_21
    dd isr_stub_22
    dd isr_stub_23
    dd isr_stub_24
    dd isr_stub_25
    dd isr_stub_26
    dd isr_stub_27
    dd isr_stub_28
    dd isr_stub_29
    dd isr_stub_30
    dd isr_stub_31

; ==================== GDT / TSS / ç³»ç»Ÿè°ƒç”¨ / ring3ï¼ˆæ­¥éª? 4ï¼?====================

[section .bss]
; enter_usermode è¿›å…¥æ—¶ä¿å­˜çš„å†…æ ¸ espï¼ˆæŒ‡å‘è¿”å›žåœ°å€ï¼‰ï¼ŒSYS_EXIT æ—¶æ®æ­¤å›žåˆ°è°ƒç”¨è€?
g_kernel_esp_save:  resd 1
; SYS_EXIT ç½? 1ï¼›syscall_entry æ£€æŸ¥æ­¤æ ‡å¿—èµ?"è¿”å›žå†…æ ¸"è·¯å¾„è€Œéž iret å›žç”¨æˆ·æ€?
g_user_exited:      resd 1
; enter_usermode ½øÈëÊ±µÄÄÚºË EFLAGS£ºSYS_EXIT ·µ»ØÂ·¾¶¾Ý´Ë¾«È·»Ö¸´ IF
; £¨²»ÄÜÎÞÌõ¼þ sti¡ª¡ªÄÇÑù»á°Ñ"µ÷ÓÃÕß±¾¾Í¹Ø×ÅÖÐ¶Ï"µÄÉÏÏÂÎÄ´íÎó´ò¿ª£©
g_kernel_eflags:    resd 1

[section .text]

; void gdt_flush(uint32_t gdt_ptr_addr)
; è£…è½½æ–? GDT åŽå¿…é¡»ç”¨è¿œè·³è½¬é‡è½? CSâ€”â€”åªæ”? ds/es/ss ä¸åŠ¨ CS çš„è¯ï¼?
; åŽç»­å–æŒ‡ä»ç”¨æ—§çš„æè¿°ç¬¦ç¼“å­˜å€¼ã€?
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10                 ; å†…æ ¸æ•°æ®æ®?
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload_cs
.reload_cs:
    ret

; void tss_flush(void)
; ltr çš„é€‰æ‹©å­? RPL å¿…é¡»ä¸? 0ï¼Œå¦åˆ? #GPã€?
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

; ---- int 0x80 ç³»ç»Ÿè°ƒç”¨å…¥å£ï¼ˆIDT é—? DPL=3ï¼Œç”¨æˆ·æ€å”¯ä¸€åˆæ³•é™·å…¥æ–¹å¼ï¼?----
; è¿›å…¥æ—? CPU å·²æŒ‰ TSS.esp0/ss0 åˆ‡åˆ°å†…æ ¸æ ˆï¼Œå¹¶åŽ‹å…? SS/ESP/EFLAGS/CS/EIPã€?
; å‚æ•°çº¦å®šï¼šeax=è°ƒç”¨å·ï¼Œebx/ecx/edx=å‚æ•°ï¼›è¿”å›žå€¼ç» eax ä¼ å›žç”¨æˆ·æ€ã€?
;
; æ ˆå¸ƒå±€ï¼ˆpusha ä¹‹åŽï¼Œä»Ž esp èµ·ï¼‰ï¼?
;   +0 edi +4 esi +8 ebp +12 esp +16 ebx +20 edx +24 ecx +28 eax
;   +32 gs +36 fs +40 es +44 ds
syscall_entry:
    push ds
    push es
    push fs
    push gs
    pusha
    push edx
    push ecx
    push ebx
    push eax
    mov ax, 0x10                 ; åˆ‡åˆ°å†…æ ¸æ•°æ®æ®µï¼šç”¨æˆ·æ®µé€‰æ‹©å­ä¸èƒ½ç”¨äºŽå†…æ ¸å­˜å?
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call syscall_handler         ; int syscall_handler(num, a1, a2, a3)
    add esp, 16

    ; SYS_EXIT æœ‰ç‹¬ç«‹è¿”å›žè·¯å¾„ï¼šä¸¢å¼ƒç”¨æˆ·æ€ä¸Šä¸‹æ–‡ï¼Œç›´æŽ¥å›žåˆ? enter_usermode çš„è°ƒç”¨è€?
    cmp dword [g_user_exited], 0
    jne .return_to_kernel

    mov [esp + 28], eax          ; å†™å›žè¿”å›žå€¼åˆ° pusha åŒºçš„ eax æ§½ï¼ˆpopa ä¼šæ¢å¤å®ƒï¼?
    popa
    pop gs
    pop fs
    pop es
    pop ds
    iret                         ; è¿”å›ž ring3

.return_to_kernel:
    mov esp, [g_kernel_esp_save]
    mov dword [g_user_exited], 0
    push dword [g_kernel_eflags]  ; exact restore of caller EFLAGS
    popfd
    ret                          ; å›žåˆ° enter_usermode() çš„è°ƒç”¨è€?

; ---- void enter_usermode(uint32_t entry, uint32_t user_esp) ----
; æž„é€? iret å¸§é™æƒåˆ° ring3ã€‚CS/SS å¿…é¡»å¸? RPL=3ï¼?0x1B/0x23ï¼‰ï¼Œ
; å¦åˆ™ CPL ä¸? RPL ä¸åŒ¹é…ä¼š #GPã€?
enter_usermode:
    mov eax, [esp + 4]           ; entry
    mov edx, [esp + 8]           ; user_esp
    mov [g_kernel_esp_save], esp ; esp æ­¤åˆ»æŒ‡å‘è¿”å›žåœ°å€
    mov dword [g_user_exited], 0

    push dword 0x23              ; SS
    push edx                     ; ESP
    pushfd
    pop ecx                      ; NOT ebx: ebx is callee-saved (cdecl)
    mov [g_kernel_eflags], ecx   ; save for exact restore on exit path
    or ecx, 0x200                ; ç½? IFï¼šç”¨æˆ·æ€éœ€è¦èƒ½æ”¶åˆ°ä¸­æ–­
    push ecx                     ; EFLAGS
    push dword 0x1B              ; CS: ç”¨æˆ·ä»£ç æ®? | RPL3
    push dword [esp + 20]        ; EIP = entry arg: 4 pushes => esp-16, so
                                 ; entry sits at [esp+20]. Reload from stack
                                 ; because "mov ax,imm16" clobbers eax low half

    cli                          ; "half-user" window: ring0 running with DPL3
                                 ; data segments - an IRQ here would let C code
                                 ; touch kernel data through them
    mov ax, 0x23                 ; ç”¨æˆ·æ•°æ®æ®? | RPL3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iret                         ; restores user EFLAGS (IF=1)
