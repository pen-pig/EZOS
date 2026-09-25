#include "efi.h"

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
  /* 等待发送保持寄存器空 (LSR bit5) */
  while ((inb(0x3FD) & 0x20) == 0) { }
  outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char *s) {
  for (; *s; ++s) serial_putc(*s);
}

/* 屏幕用的 UTF-16 字面量（不用 L""，避免 wchar_t 宽度歧义） */
static const UINT16 msg16[] = {
  'E', 'Z', 'E', 'F', 'I', ':', ' ', 'h', 'e', 'l', 'l', 'o', '\r', '\n', 0
};

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST) {
  (void)ImageHandle;
  /* 1) 屏幕输出 */
  ST->ConOut->OutputString(ST->ConOut, (UINT16 *)msg16);
  /* 2) 串口输出，便于自动化抓取 */
  serial_puts("EZEFI: hello\r\n");
  return EFI_SUCCESS;
}
