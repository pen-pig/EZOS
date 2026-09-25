#include "efi.h"

/* GOP 协议 GUID: {9042a9de-23dc-4a38-96fb-7aded080516a} */
static const EFI_GUID gEfiGraphicsOutputProtocolGuid = {
  0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}
};

/* ---- 串口（QEMU 下 0x3F8 即第一个 ISA UART，接到 -serial） ---- */
static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void outb(uint16_t port, uint8_t v) {
  __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port));
}
static void serial_putc(char c) {
  while ((inb(0x3FD) & 0x20) == 0) { }
  outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char *s) {
  for (; *s; ++s) serial_putc(*s);
}
/* 十进制无符号 32 位 */
static void serial_dec(uint32_t v) {
  char buf[12]; int i = 0;
  if (v == 0) { serial_putc('0'); return; }
  while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i > 0) serial_putc(buf[--i]);
}
/* 十六进制 32 位，零填充 8 位，带 0x 前缀 */
static void serial_hex(uint32_t v) {
  static const char h[] = "0123456789abcdef";
  serial_putc('0'); serial_putc('x');
  for (int s = 28; s >= 0; s -= 4) serial_putc(h[(v >> s) & 0xF]);
}

/* 屏幕用的 UTF-16 字面量（不用 L""，避免 wchar_t 宽度歧义） */
static const UINT16 msg16[] = {
  'E', 'Z', 'E', 'F', 'I', ':', ' ', 'h', 'e', 'l', 'l', 'o', '\r', '\n', 0
};

/* 内核图形层（kernel/gfx.c:139-142）约定的帧缓冲参数区物理布局：
 *   0x5000  uint32  LFB 物理地址（0=无 LFB，回退 VGA 0x13）
 *   0x5004  uint16  XRES
 *   0x5006  uint16  YRES
 *   0x5008  uint8   BPP（每像素字节数；gfx.c 当前仅接受 16=RGB565）
 * 扩展（供后续 U3 内核参数化使用，gfx.c 暂未读取）：
 *   0x5009  uint8   GOP PixelFormat 枚举值
 *   0x500A  uint16  保留
 *   0x500C  uint32  PixelsPerScanLine（stride，单位像素）
 * UEFI IA32 启动阶段为 1:1 映射，可直接按物理地址写入。 */
#define GFX_INFO_PHYS 0x5000u

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST) {
  (void)ImageHandle;

  /* 1) 屏幕输出 */
  ST->ConOut->OutputString(ST->ConOut, (UINT16 *)msg16);
  /* 2) 串口输出，便于自动化抓取 */
  serial_puts("EZEFI: hello\r\n");

  /* 3) 取 GOP 帧缓冲信息 */
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
  EFI_STATUS st = ST->BootServices->LocateProtocol(
      (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, 0, (void **)&gop);

  if (st != EFI_SUCCESS || gop == 0 || gop->Mode == 0) {
    serial_puts("EZEFI:gop FAIL loc\r\n");
  } else {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = m->Info;
    uint32_t w   = info->HorizontalResolution;
    uint32_t h   = info->VerticalResolution;
    uint32_t fmt = (uint32_t)info->PixelFormat;
    uint32_t fb  = (uint32_t)(m->FrameBufferBase & 0xFFFFFFFFu);
    uint32_t stride = info->PixelsPerScanLine;

    /* 校验行 */
    serial_puts("EZEFI:gop ");
    serial_dec(w); serial_putc('x'); serial_dec(h);
    serial_puts(" fmt="); serial_dec(fmt);
    serial_puts(" fb="); serial_hex(fb);
    serial_puts(" stride="); serial_dec(stride);
    serial_puts("\r\n");

    if (fmt >= (uint32_t)PixelBltOnly) {
      /* PixelBltOnly / Max：无可用线性帧缓冲，不写 */
      serial_puts("EZEFI:gop unsupported fmt\r\n");
    } else {
      /* 写入内核约定的 0x5000 区域（按 gfx.c 真实字段宽度） */
      *(volatile uint32_t *)GFX_INFO_PHYS        = fb;                 /* 0x5000 */
      *(volatile uint16_t *)(GFX_INFO_PHYS + 4)  = (uint16_t)w;        /* 0x5004 */
      *(volatile uint16_t *)(GFX_INFO_PHYS + 6)  = (uint16_t)h;        /* 0x5006 */
      *(volatile uint8_t  *)(GFX_INFO_PHYS + 8)  = (uint8_t)32;        /* 0x5008 BPP=32 */
      *(volatile uint8_t  *)(GFX_INFO_PHYS + 9)  = (uint8_t)fmt;       /* 0x5009 */
      *(volatile uint16_t *)(GFX_INFO_PHYS + 10) = (uint16_t)0;        /* 0x500A 保留 */
      *(volatile uint32_t *)(GFX_INFO_PHYS + 12) = stride;             /* 0x500C */
      serial_puts("EZEFI:gop write@0x5000 ok\r\n");
    }
  }

  return EFI_SUCCESS;
}
