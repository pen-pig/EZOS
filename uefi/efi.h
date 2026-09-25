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

typedef struct {
  UINT32 Data1;
  UINT16 Data2;
  UINT16 Data3;
  UINT8  Data4[8];
} EFI_GUID;

/* ---- 控制台文本输出协议（OutputString 为第 2 个成员） ---- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  UINT16 *String
);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_TEXT_STRING OutputString;
  void *TestString;
  void *QueryMode;
  void *SetMode;
  void *SetAttribute;
  void *ClearScreen;
  void *SetCursorPosition;
  void *EnableCursor;
  void *Mode;
};

/* ---- Graphics Output Protocol (GOP) ---- */
typedef enum {
  PixelRedGreenBlueReserved8BitPerColor = 0, /* RGB 32bpp */
  PixelBlueGreenRedReserved8BitPerColor = 1, /* BGR 32bpp */
  PixelBitMask                          = 2,
  PixelBltOnly                          = 3,
  PixelFormatMax                        = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
  UINT32 RedMask;
  UINT32 GreenMask;
  UINT32 BlueMask;
  UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
  UINT32 Version;
  UINT32 HorizontalResolution;
  UINT32 VerticalResolution;
  EFI_GRAPHICS_PIXEL_FORMAT PixelFormat; /* 实为整数枚举，占位 4 字节 */
  EFI_PIXEL_BITMASK PixelInformation;
  UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
  UINT32 MaxMode;
  UINT32 Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINTN  SizeOfInfo;
  UINT64 FrameBufferBase;   /* EFI_PHYSICAL_ADDRESS = UINT64 */
  UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GOP_QUERY_MODE)(
  EFI_GRAPHICS_OUTPUT_PROTOCOL        *This,
  UINT32                              ModeNumber,
  UINTN                              *SizeOfInfo,
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);
typedef EFI_STATUS (EFIAPI *EFI_GOP_SET_MODE)(
  EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  UINT32                        ModeNumber
);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
  EFI_GOP_QUERY_MODE QueryMode;
  EFI_GOP_SET_MODE   SetMode;
  void              *Blt;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
  EFI_GUID *Protocol,
  void     *Registration,
  void    **Interface
);

/* ---- Boot Services（仅用到 LocateProtocol，前置字段按 UEFI 规范布局占位） ---- */
typedef struct {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL;
  void *RestoreTPL;
  void *AllocatePages;
  void *FreePages;
  void *GetMemoryMap;
  void *AllocatePool;
  void *FreePool;
  void *CreateEvent;
  void *SetTimer;
  void *WaitForEvent;
  void *SignalEvent;
  void *CloseEvent;
  void *CheckEvent;
  void *InstallProtocolInterface;
  void *ReinstallProtocolInterface;
  void *UninstallProtocolInterface;
  void *HandleProtocol;
  void *Reserved;         /* UEFI 2.0 兼容保留槽，不可省，否则 LocateProtocol 偏移错位 */
  void *RegisterProtocolNotify;
  void *LocateHandle;
  void *LocateDevicePath;
  void *InstallConfigurationTable;
  void *LoadImage;
  void *StartImage;
  void *Exit;
  void *UnloadImage;
  void *ExitBootServices;
  void *GetNextMonotonicCount;
  void *Stall;
  void *SetWatchdogTimer;
  void *ConnectController;
  void *DisconnectController;
  void *OpenProtocol;
  void *CloseProtocol;
  void *OpenProtocolInformation;
  void *ProtocolsPerHandle;
  void *LocateHandleBuffer;
  EFI_LOCATE_PROTOCOL LocateProtocol; /* 第 37 个函数指针，Hdr 之后偏移 168 字节 */
} EFI_BOOT_SERVICES;

/* ---- 系统表 ---- */
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
  EFI_BOOT_SERVICES *BootServices;
  UINTN   NumberOfTableEntries;
  void   *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* EFI_H */
