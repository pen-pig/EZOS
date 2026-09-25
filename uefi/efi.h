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
typedef uint64_t EFI_PHYSICAL_ADDRESS;  /* 物理地址（64 位，IA32 下高 32 位为 0） */
typedef uint16_t CHAR16;                /* EFI 字符串元素（UTF-16） */
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

/* ---- 文件/镜像/内存相关协议与 BootServices 调用类型 ---- */
typedef enum { AllocateAnyPages = 0, AllocateMaxAddress = 1, AllocateAddress = 2, MaxAllocateType } EFI_ALLOCATE_TYPE;
typedef enum {
  EfiReservedMemoryType = 0, EfiLoaderCode = 1, EfiLoaderData = 2, EfiBootServicesCode = 3,
  EfiBootServicesData = 4, EfiRuntimeServicesCode = 5, EfiRuntimeServicesData = 6,
  EfiConventionalMemory = 7, EfiUnusableMemory = 8, EfiACPIReclaimMemory = 9,
  EfiACPIMemoryNVS = 10, EfiMemoryMappedIO = 11, EfiMemoryMappedIOPortSpace = 12,
  EfiPalCode = 13, EfiPersistentMemory = 14, EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
  EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
  UINTN *MemoryMapSize, void *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
  EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface,
  EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);

#define EFI_OPEN_PROTOCOL_GET_PROTOCOL   0x00000002
#define EFI_FILE_MODE_READ               0x0000000000000001ULL

typedef struct {
  UINT32 Type;
  UINT32 Pad;
  EFI_PHYSICAL_ADDRESS PhysicalStart;
  UINT64 VirtualStart;
  UINT64 NumberOfPages;
  UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;  /* 仅用于缓冲区大小，本步不逐条解析 */

/* Loaded Image Protocol（本步仅用 DeviceHandle 找到启动设备） */
typedef struct {
  UINT32    Revision;
  EFI_HANDLE ParentHandle;
  void     *SystemTable;
  EFI_HANDLE DeviceHandle;
  /* 之后字段本步不使用 */
} EFI_LOADED_IMAGE_PROTOCOL;

/* Simple File System Protocol */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  UINT64 Revision;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
};

/* File Protocol */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
  EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
  CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
  EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
struct _EFI_FILE_PROTOCOL {
  UINT64 Revision;
  EFI_FILE_OPEN  Open;
  EFI_FILE_CLOSE Close;
  void *Delete;
  EFI_FILE_READ  Read;
  void *Write;
  void *GetPosition;
  void *SetPosition;
  void *GetInfo;
  void *SetInfo;
  void *Flush;
};

/* UEFI 启动魔数：物理 0x5010 写 0x55454649（供 U3 内核判定 UEFI 启动） */
#define UEFI_MAGIC 0x55454649u

/* GUIDs */
static const EFI_GUID gEfiLoadedImageProtocolGuid = {
  0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
  0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

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
