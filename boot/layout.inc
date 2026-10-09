; boot/layout.inc —— 镜像布局的唯一事实来源
;
; 被 boot/boot.asm 与 boot/stage2.asm 同时 %include；Python 侧
;（tools/make_image.py、tools/make_usb_image.py）从这里解析同名常量，
; 避免"同一个数字散落多处、换台机器才暴露"的老毛病。
;
; 为什么需要二级加载器：BIOS 的 INT13h AH=42h 用 seg:off 寻址（20 位），
; 目标地址必须落在低 1MB，而且 0xA0000 起是 VGA aperture（不是平坦 RAM），
; 于是"引导扇区直接读内核"的上限被钉死在 0x10000..0xA0000 = 576KB。
; 内核迟早会超过它（CI 的 gcc13 -O2 已到 561KB，只剩 28KB）。
; 解法：引导扇区只负责把一个小的 stage2 读进低内存，stage2 进保护模式
; 把内核分批搬到 1MB 以上 —— BIOS 只写低地址的 bounce 缓冲，高地址由
; 32 位地址前缀写入，上限不再由 BIOS 决定。

STAGE2_LOAD       equ 0x10000      ; stage2 在内存里的位置（低 1MB，BIOS 能写）
STAGE2_LBA        equ 1            ; stage2 在镜像里的起始扇区
STAGE2_SECTORS    equ 4            ; stage2 固定 4 扇区（2KB），留足余量

KERNEL_LBA        equ 5            ; 内核起始扇区 = STAGE2_LBA + STAGE2_SECTORS
KERNEL_DST        equ 0x01300000   ; 内核目标地址：19MB（同 linker.ld）
                                  ; 16-19MB 是 GUI 背缓冲，19MB 之后原是 .bss
                                  ; （已挪到 22MB）。identity 映射覆盖 0-32MB。
KERNEL_MAX_BYTES  equ 0x00300000   ; 3MB：镜像窗口 19MB..22MB
KERNEL_MAX_SECTORS equ (KERNEL_MAX_BYTES / 512)

; 引导扇区尾部 0x1FC（= 0x7DFC）的双字节是内核扇区数，由 make_image.py 打补丁。
; stage2 从这里读真实大小；裸 boot.bin（未打补丁）读到 0 时兜底成上限。
KERNEL_SECTORS_FIELD equ 0x7DFC

BOUNCE_SEG        equ 0x7000       ; 低地址 bounce 缓冲段（线性 0x70000）
BOUNCE_LIN        equ (BOUNCE_SEG * 16)
STAGE1_STACK      equ 0x7C00       ; 加载阶段栈（向下生长，不碰 0x7C00 的引导扇区）
BATCH_SECTORS     equ 64           ; 每批 64 扇区 = 32KB（必须 < 64KB：ds 段限长）
