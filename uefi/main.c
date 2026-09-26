#include "efi.h"

/* GOP 协�?? GUID: {9042a9de-23dc-4a38-96fb-7aded080516a} */
static const EFI_GUID gEfiGraphicsOutputProtocolGuid = {
  0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}
};

/* 内核镜像固定大小（build.ninja: kernel.bin = pad-to 507904 kernel_raw.bin�? */
#define KERNEL_SIZE  507904u
#define KERNEL_LOAD  0x10000u   /* boot/boot.asm: KERNEL_OFFSET equ 0x10000 */

/* ---- 串口（QEMU �? 0x3F8 即�??�?�? ISA UART，接�? -serial�? ---- */
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
static void serial_dec(uint32_t v) {
  char buf[12]; int i = 0;
  if (v == 0) { serial_putc('0'); return; }
  while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i > 0) serial_putc(buf[--i]);
}
static void serial_hex(uint32_t v) {
  static const char h[] = "0123456789abcdef";
  serial_putc('0'); serial_putc('x');
  for (int s = 28; s >= 0; s -= 4) serial_putc(h[(v >> s) & 0xF]);
}

/* 屏幕用的 UTF-16 字面量（不用 L""，避�? wchar_t 宽度歧义�? */
static const UINT16 msg16[] = {
  'E', 'Z', 'E', 'F', 'I', ':', ' ', 'h', 'e', 'l', 'l', 'o', '\r', '\n', 0
};
static const UINT16 kernel_path[] = {
  'k','e','r','n','e','l','.','b','i','n',0
};

/* 内核图形层（kernel/gfx.c:139-142）约定的帧缓冲参数区物理布局�?
 *   0x5000  uint32  LFB 物理地址�?0=�? LFB，回�? VGA 0x13�?
 *   0x5004  uint16  XRES / 0x5006  uint16  YRES / 0x5008  uint8  BPP
 * 扩展（供 U3）：0x5009 uint8 PixelFormat / 0x500A uint16 保留 / 0x500C uint32 stride
 * 0x5010  uint32  UEFI �?动魔�? 0x55454649（U3 �?其判定跳�? VBE 探测�?
 * UEFI IA32 �?动阶段为 1:1 映射，可直接按物理地�?写入�? */
#define GFX_INFO_PHYS 0x5000u
#define UEFI_MAGIC_PHYS 0x5010u
/* U3: memory map handoff (kernel/pmm.c pmm_apply_uefi_map reads it)
 *   0x5020: uint32 map_size / +0x04 desc_size / +0x08 desc_version / +0x0C data phys
 *   0x5100-0x5FFF: EFI_MEMORY_DESCRIPTOR array (truncated at 0x5FFF) */
#define UEFI_MMAP_HDR_PHYS 0x5020u
#define UEFI_MMAP_PHYS     0x5100u
#define UEFI_MMAP_MAX      (0x6000u - 0x5100u)

/* �?易内存拷贝（�? libc�? */
static void my_memcpy(volatile void *dst, const void *src, UINTN n) {
  volatile uint8_t *d = (volatile uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (UINTN i = 0; i < n; ++i) d[i] = s[i];
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST) {
  (void)ImageHandle;

  ST->ConOut->OutputString(ST->ConOut, (UINT16 *)msg16);
  serial_puts("EZEFI: hello\r\n");

  EFI_BOOT_SERVICES *BS = ST->BootServices;

  /* ===== GOP 帧缓冲信�?（U2b/U2b-ext�? ===== */
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
  EFI_STATUS st = BS->LocateProtocol(
      (EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, 0, (void **)&gop);
  if (st == EFI_SUCCESS && gop != 0 && gop->Mode != 0) {
    int rgb565_idx = -1;
    UINT32 maxmode = gop->Mode->MaxMode;
    for (UINT32 i = 0; i < maxmode; ++i) {
      UINTN sz = 0;
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
      if (gop->QueryMode(gop, i, &sz, &info) == EFI_SUCCESS && info != 0) {
        serial_puts("EZEFI:mode ");
        serial_dec(i); serial_putc(' ');
        serial_dec(info->HorizontalResolution); serial_putc('x');
        serial_dec(info->VerticalResolution);
        serial_puts(" fmt="); serial_dec((uint32_t)info->PixelFormat);
        serial_puts(" stride="); serial_dec(info->PixelsPerScanLine);
        if (info->PixelFormat == PixelBitMask) {
          serial_puts(" mask=R="); serial_hex(info->PixelInformation.RedMask);
          serial_puts("/G="); serial_hex(info->PixelInformation.GreenMask);
          serial_puts("/B="); serial_hex(info->PixelInformation.BlueMask);
          if (info->PixelInformation.RedMask == 0xF800u &&
              info->PixelInformation.GreenMask == 0x07E0u &&
              info->PixelInformation.BlueMask == 0x001Fu) rgb565_idx = (int)i;
        }
        serial_puts("\r\n");
      }
    }
    int use_rgb565 = 0;
    if (rgb565_idx >= 0 && gop->SetMode(gop, (UINT32)rgb565_idx) == EFI_SUCCESS)
      use_rgb565 = 1;

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = m->Info;
    uint32_t w   = info->HorizontalResolution;
    uint32_t h   = info->VerticalResolution;
    uint32_t fmt = (uint32_t)info->PixelFormat;
    uint32_t fb  = (uint32_t)(m->FrameBufferBase & 0xFFFFFFFFu);
    uint32_t stride = info->PixelsPerScanLine;
    uint8_t  bpp = use_rgb565 ? (uint8_t)16 : (uint8_t)32;

    serial_puts("EZEFI:gop ");
    serial_dec(w); serial_putc('x'); serial_dec(h);
    serial_puts(" fmt="); serial_dec(fmt);
    serial_puts(" fb="); serial_hex(fb);
    serial_puts(" stride="); serial_dec(stride);
    serial_puts("\r\n");

    if (fmt < (uint32_t)PixelBltOnly) {
      *(volatile uint32_t *)GFX_INFO_PHYS        = fb;
      *(volatile uint16_t *)(GFX_INFO_PHYS + 4)  = (uint16_t)w;
      *(volatile uint16_t *)(GFX_INFO_PHYS + 6)  = (uint16_t)h;
      *(volatile uint8_t  *)(GFX_INFO_PHYS + 8)  = bpp;
      *(volatile uint8_t  *)(GFX_INFO_PHYS + 9)  = (uint8_t)fmt;
      *(volatile uint16_t *)(GFX_INFO_PHYS + 10) = (uint16_t)0;
      *(volatile uint32_t *)(GFX_INFO_PHYS + 12) = stride;
      serial_puts("EZEFI:gop write@0x5000 ok\r\n");
    }
  } else {
    serial_puts("EZEFI:gop FAIL loc\r\n");
  }

  /* ===== U2c：加载内核并跳转 ===== */

  /* 1) 找到�?动�?��?�的文件系统 */
  EFI_LOADED_IMAGE_PROTOCOL *li = 0;
  st = ((EFI_OPEN_PROTOCOL)BS->OpenProtocol)(ImageHandle,
      (EFI_GUID *)&gEfiLoadedImageProtocolGuid, (void **)&li,
      ImageHandle, 0, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (st != EFI_SUCCESS || li == 0) {
    serial_puts("EZEFI:kernel no loaded-image\r\n");
    return EFI_SUCCESS;
  }
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
  st = ((EFI_OPEN_PROTOCOL)BS->OpenProtocol)(li->DeviceHandle,
      (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, (void **)&fs,
      ImageHandle, 0, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (st != EFI_SUCCESS || fs == 0) {
    serial_puts("EZEFI:kernel no fs\r\n");
    return EFI_SUCCESS;
  }
  EFI_FILE_PROTOCOL *root = 0;
  if (fs->OpenVolume(fs, &root) != EFI_SUCCESS || root == 0) {
    serial_puts("EZEFI:kernel no root\r\n");
    return EFI_SUCCESS;
  }
  EFI_FILE_PROTOCOL *kf = 0;
  st = root->Open(root, &kf, (CHAR16 *)kernel_path, EFI_FILE_MODE_READ, 0);
  if (st != EFI_SUCCESS || kf == 0) {
    serial_puts("EZEFI:kernel open fail\r\n");
    return EFI_SUCCESS;
  }

  /* 2) 读内核到临时缓冲（AllocatePages 不能指定地址），�? memcpy �? 0x10000 */
  EFI_PHYSICAL_ADDRESS tmp = 0;
  st = ((EFI_ALLOCATE_PAGES)BS->AllocatePages)(AllocateAnyPages, EfiLoaderData,
      (KERNEL_SIZE + 4095) / 4096 + 1, &tmp);
  if (st != EFI_SUCCESS || tmp == 0) {
    serial_puts("EZEFI:kernel alloc fail\r\n");
    return EFI_SUCCESS;
  }
  void *buf = (void *)(UINTN)tmp;
  UINTN read_size = KERNEL_SIZE;
  st = kf->Read(kf, &read_size, buf);
  if (st != EFI_SUCCESS || read_size != KERNEL_SIZE) {
    serial_puts("EZEFI:kernel read fail size=");
    serial_dec((uint32_t)read_size);
    serial_puts("\r\n");
    return EFI_SUCCESS;
  }
  my_memcpy((void *)KERNEL_LOAD, buf, KERNEL_SIZE);
  serial_puts("EZEFI:kernel size=");
  serial_dec((uint32_t)read_size);
  serial_puts(" dst=0x10000 ok\r\n");

  /* 3) UEFI �?动魔数（�? U3 内核判定�? */
  *(volatile uint32_t *)UEFI_MAGIC_PHYS = UEFI_MAGIC;
  serial_puts("EZEFI:uefi magic@0x5010 ok\r\n");

  /* 4) 取内存映射（�?步只打印，不传�?�给内核�? */
  UINTN map_size = 0, map_key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  ((EFI_GET_MEMORY_MAP)BS->GetMemoryMap)(&map_size, 0, &map_key, &desc_size, &desc_ver);
  void *mmap = 0;
  UINTN mmap_cap = map_size + desc_size * 2;   /* buffer capacity (for EBS retries, U3) */
  st = ((EFI_ALLOCATE_POOL)BS->AllocatePool)(EfiLoaderData, mmap_cap, &mmap);
  if (st != EFI_SUCCESS || mmap == 0) {
    serial_puts("EZEFI:map alloc fail\r\n");
    return EFI_SUCCESS;
  }
  st = ((EFI_GET_MEMORY_MAP)BS->GetMemoryMap)(&map_size, mmap, &map_key, &desc_size, &desc_ver);
  if (st != EFI_SUCCESS) {
    serial_puts("EZEFI:map get fail\r\n");
    return EFI_SUCCESS;
  }
  serial_puts("EZEFI:map n=");
  serial_dec((uint32_t)(map_size / desc_size));
  serial_puts(" key=0x"); serial_hex((uint32_t)map_key);
  serial_puts(" descsize="); serial_dec((uint32_t)desc_size);
  serial_puts("\r\n");

  /* 5) ExitBootServices（失败用�? MapKey 重试，最�? 3 次） */
  int ebs_ok = 0;
  for (int attempt = 0; attempt < 3 && !ebs_ok; ++attempt) {
    st = ((EFI_EXIT_BOOT_SERVICES)BS->ExitBootServices)(ImageHandle, map_key);
    if (st == EFI_SUCCESS) {
      ebs_ok = 1;
    } else {
      /* MapKey �?能已变，重新取一�? */
      /* U3: MapKey may have changed - re-fetch the FULL map into the buffer
       * so the copy handed to the kernel matches the post-EBS state */
      UINTN cap_in = mmap_cap;
      if (((EFI_GET_MEMORY_MAP)BS->GetMemoryMap)(&cap_in, mmap, &map_key, &desc_size, &desc_ver)
          != EFI_SUCCESS)
        break;
      map_size = cap_in;
    }
  }
  if (!ebs_ok) {
    serial_puts("EZEFI:ebs fail\r\n");
    return EFI_SUCCESS;
  }
  serial_puts("EZEFI:ebs ok\r\n");

  /* 5b) U3: hand the final memory map to the kernel (copy @0x5100, header @0x5020) */
  {
    uint32_t copy = (map_size < (UINTN)UEFI_MMAP_MAX) ? (uint32_t)map_size
                                                      : (uint32_t)UEFI_MMAP_MAX;
    my_memcpy((void *)UEFI_MMAP_PHYS, mmap, copy);
    *(volatile uint32_t *)(UEFI_MMAP_HDR_PHYS + 0x00) = (uint32_t)map_size;
    *(volatile uint32_t *)(UEFI_MMAP_HDR_PHYS + 0x04) = (uint32_t)desc_size;
    *(volatile uint32_t *)(UEFI_MMAP_HDR_PHYS + 0x08) = (uint32_t)desc_ver;
    *(volatile uint32_t *)(UEFI_MMAP_HDR_PHYS + 0x0C) = UEFI_MMAP_PHYS;
    serial_puts("EZEFI:mmap handoff size=");
    serial_dec((uint32_t)map_size);
    serial_puts(" copied=");
    serial_dec(copy);
    serial_puts("\r\n");
  }

  /* 6) 关中�?�?7) 跳转到内核入�? 0x10000（内核自己�?�栈/�? bss/建页�?�? */
  __asm__ __volatile__("cli");
  typedef void (*entry_t)(void);
  ((entry_t)KERNEL_LOAD)();

  /* 不应返回 */
  return EFI_SUCCESS;
}
