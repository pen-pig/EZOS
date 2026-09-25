#ifndef EFI_H
#define EFI_H
/* 最小 UEFI 头文件（仅覆盖本步所需类型）。
 * 32 位 IA32 UEFI：UINTN 为 4 字节，CHAR16 为 2 字节(uint16_t)。 */

#include <stdint.h>

typedef uint8_t  BOOLEAN;
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t UINT16;   /* EFI CHAR16 是 UTF-16，2 字节 */
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;
typedef uint32_t UINTN;    /* IA32 下指针/状态为 32 位 */
typedef int32_t  INTN;

typedef void     *EFI_HANDLE;
typedef UINTN     EFI_STATUS;

#define EFI_SUCCESS 0
#define EFIAPI              /* IA32 上 EFIAPI 即 cdecl，gcc 默认即 cdecl */

typedef struct {
  UINT64  Signature;
  UINT32  Revision;
  UINT32  HeaderSize;
  UINT32  CRC32;
  UINT32  Reserved;
} EFI_TABLE_HEADER;

/* 控制台文本输出协议（布局需与规范一致，OutputString 为第 2 个成员） */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  UINT16 *String
);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_TEXT_STRING OutputString;   /* 第 2 个函数指针 */
  void *TestString;
  void *QueryMode;
  void *SetMode;
  void *SetAttribute;
  void *ClearScreen;
  void *SetCursorPosition;
  void *EnableCursor;
  void *Mode;
};

/* 系统表（ConOut 在固定偏移处） */
typedef struct {
  EFI_TABLE_HEADER Hdr;
  UINT16 *FirmwareVendor;
  UINT32  FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  void   *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  void   *StdErr;
  void   *RuntimeServices;
  void   *BootServices;
  UINTN   NumberOfTableEntries;
  void   *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* EFI_H */
