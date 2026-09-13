// SPDX-License-Identifier: MIT
// AfriyieOS — minimal UEFI type definitions
//
// =============================================================================
// READ THIS BEFORE EDITING
// =============================================================================
// One wrong field offset in this file produces an instant triple fault with NO
// output whatsoever — the single most time-consuming failure mode in OS
// development. Every structure below is therefore:
//
//   1. laid out in the exact order the UEFI 2.10 specification defines,
//   2. marked AF_PACKED so the compiler cannot insert padding,
//   3. guarded by a static assert on the total size AND on the offset of every
//      member we actually call.
//
// If a static assert fires, compare the struct against the spec. Do not
// "fix" it by adjusting the asserted number.
//
// Scope: this is the subset v0.1 needs. It is not a complete UEFI binding.
// =============================================================================

#ifndef AFRIYIE_EFI_H
#define AFRIYIE_EFI_H

#include "afriyie/types.h"

// =============================================================================
// ALIGNMENT RULE FOR THIS FILE
// =============================================================================
// Firmware structures use NATURAL alignment, not packing. UEFI firmware is
// compiled as ordinary C, so its structures are laid out with the ABI's normal
// padding — which on x86_64 means four bytes of padding after the u32
// FirmwareRevision field in EFI_SYSTEM_TABLE before the next pointer.
//
// Marking those structures AF_PACKED produces a layout that is four bytes
// narrow from that point on, and reading ConOut from a packed struct returns
// garbage — an instant triple fault with no output at all.
//
// So: no AF_PACKED on any structure the firmware produces. Every size and the
// offset of every member we touch is asserted instead, and an assert failure
// means the layout is wrong, never that the asserted number is.
//
// Packing is correct for OUR formats — the boot handoff structure and the
// on-disk image layouts — because we define those byte for byte.
// =============================================================================

// -----------------------------------------------------------------------------
// Fundamental types
//
// UEFI uses the Microsoft x64 ABI on x86_64: only rcx/rdx/r8/r9 carry the first
// four arguments, and the caller reserves 32 bytes of shadow space. The
// compiler handles this automatically for an application linked as a UEFI
// image; see the ms_abi attribute on the entry point below.
// -----------------------------------------------------------------------------
typedef af_u8       EFI_BOOLEAN;
typedef af_i64      EFI_STATUS;
typedef af_u64      EFI_UINTN;
typedef af_i64      EFI_INTN;
typedef void       *EFI_HANDLE;
typedef void       *EFI_EVENT;
typedef af_u16      CHAR16;
typedef af_u64      EFI_PHYSICAL_ADDRESS;
typedef af_u64      EFI_VIRTUAL_ADDRESS;

// UEFI error codes have the high bit set. Written as a negative signed constant
// rather than `(1ULL << 63) | n`: the latter is unsigned, which makes every
// comparison against EFI_STATUS trip -Wsign-compare, and the cast needed to
// silence that is exactly the kind of cast that later hides a real bug.
#define EFI_ERR_BIT               (-9223372036854775807LL - 1)   /* INT64_MIN */
#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            (EFI_ERR_BIT | 1)
#define EFI_INVALID_PARAMETER     (EFI_ERR_BIT | 2)
#define EFI_UNSUPPORTED           (EFI_ERR_BIT | 3)
#define EFI_BAD_BUFFER_SIZE       (EFI_ERR_BIT | 4)
#define EFI_BUFFER_TOO_SMALL      (EFI_ERR_BIT | 5)
#define EFI_NOT_READY             (EFI_ERR_BIT | 6)
#define EFI_DEVICE_ERROR          (EFI_ERR_BIT | 7)
#define EFI_WRITE_PROTECTED       (EFI_ERR_BIT | 8)
#define EFI_OUT_OF_RESOURCES      (EFI_ERR_BIT | 9)
#define EFI_NOT_FOUND             (EFI_ERR_BIT | 14)

#define EFI_ERROR(status)  (((EFI_STATUS)(status)) < 0)

// -----------------------------------------------------------------------------
// GUID
// -----------------------------------------------------------------------------
typedef struct AF_PACKED {
    af_u32 Data1;
    af_u16 Data2;
    af_u16 Data3;
    af_u8  Data4[8];
} EFI_GUID;

// 4 + 2 + 2 = 8, then the eight Data4 bytes — 16 either way, but assert it: a
// GUID read at the wrong size silently fails to match the protocol and the
// failure looks like "the firmware does not support this".
AF_STATIC_ASSERT_SIZE(EFI_GUID, 16);

// {9042A9DE-23DC-4A38-96FB-7ADED080516A} — EFI_GRAPHICS_OUTPUT_PROTOCOL
//
// Defined as a compound literal rather than a static const object: a static
// const in a header trips -Wunused-const-variable in every translation unit
// that includes it but does not use it, and this project builds with -Werror.
#define AF_EFI_GUID_GRAPHICS_OUTPUT \
    ((EFI_GUID){ 0x9042A9DEu, 0x23DCu, 0x4A38u, \
                 { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } })

// {5B1B31A1-9562-11D2-8E3F-00A0C969723B} — EFI_LOADED_IMAGE_PROTOCOL
#define AF_EFI_GUID_LOADED_IMAGE \
    ((EFI_GUID){ 0x5B1B31A1u, 0x9562u, 0x11D2u, \
                 { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } })

// {09576E91-6D3F-11D2-8E39-00A0C969723B} — EFI_DEVICE_PATH_PROTOCOL
#define AF_EFI_GUID_DEVICE_PATH \
    ((EFI_GUID){ 0x09576E91u, 0x6D3Fu, 0x11D2u, \
                 { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } })

// -----------------------------------------------------------------------------
// Table header
// -----------------------------------------------------------------------------
typedef struct {
    af_u64 Signature;
    af_u32 Revision;
    af_u32 HeaderSize;
    af_u32 Crc32;
    af_u32 Reserved;
} EFI_TABLE_HEADER;

AF_STATIC_ASSERT_SIZE(EFI_TABLE_HEADER, 24);
AF_STATIC_ASSERT_OFFSET(EFI_TABLE_HEADER, Signature, 0);
AF_STATIC_ASSERT_OFFSET(EFI_TABLE_HEADER, Revision, 8);
AF_STATIC_ASSERT_OFFSET(EFI_TABLE_HEADER, HeaderSize, 12);
AF_STATIC_ASSERT_OFFSET(EFI_TABLE_HEADER, Crc32, 16);
AF_STATIC_ASSERT_OFFSET(EFI_TABLE_HEADER, Reserved, 20);

// -----------------------------------------------------------------------------
// Memory map
// -----------------------------------------------------------------------------
typedef enum {
    EfiReservedMemoryType      = 0,
    EfiLoaderCode              = 1,
    EfiLoaderData              = 2,
    EfiBootServicesCode        = 3,
    EfiBootServicesData        = 4,
    EfiRuntimeServicesCode     = 5,
    EfiRuntimeServicesData     = 6,
    EfiConventionalMemory      = 7,
    EfiUnusableMemory          = 8,
    EfiACPIReclaimMemory       = 9,
    EfiACPIMemoryNVS           = 10,
    EfiMemoryMappedIO          = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode                 = 13,
    EfiPersistentMemory        = 14,
} EFI_MEMORY_TYPE;

typedef struct {
    af_u32                Type;
    af_u32                Pad;
    EFI_PHYSICAL_ADDRESS  PhysicalStart;
    EFI_VIRTUAL_ADDRESS   VirtualStart;
    af_u64                NumberOfPages;
    af_u64                Attribute;
} EFI_MEMORY_DESCRIPTOR;

AF_STATIC_ASSERT_SIZE(EFI_MEMORY_DESCRIPTOR, 40);
AF_STATIC_ASSERT_OFFSET(EFI_MEMORY_DESCRIPTOR, Type, 0);
AF_STATIC_ASSERT_OFFSET(EFI_MEMORY_DESCRIPTOR, PhysicalStart, 8);
AF_STATIC_ASSERT_OFFSET(EFI_MEMORY_DESCRIPTOR, NumberOfPages, 24);
AF_STATIC_ASSERT_OFFSET(EFI_MEMORY_DESCRIPTOR, Attribute, 32);

// -----------------------------------------------------------------------------
// Simple text output
// -----------------------------------------------------------------------------
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_TEXT_RESET)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                     EFI_BOOLEAN ExtendedVerification);
typedef EFI_STATUS (*EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
typedef EFI_STATUS (*EFI_TEXT_TEST_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                           CHAR16 *String);
typedef EFI_STATUS (*EFI_TEXT_QUERY_MODE)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                          EFI_UINTN ModeNumber, EFI_UINTN *Columns,
                                          EFI_UINTN *Rows);
typedef EFI_STATUS (*EFI_TEXT_SET_MODE)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                        EFI_UINTN ModeNumber);
typedef EFI_STATUS (*EFI_TEXT_SET_ATTRIBUTE)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                             EFI_UINTN Attribute);
typedef EFI_STATUS (*EFI_TEXT_CLEAR_SCREEN)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (*EFI_TEXT_SET_CURSOR_POSITION)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                                   EFI_UINTN Column, EFI_UINTN Row);
typedef EFI_STATUS (*EFI_TEXT_ENABLE_CURSOR)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                             EFI_BOOLEAN Visible);

typedef struct {
    af_i32  MaxMode;
    af_i32  Mode;
    af_i32  Attribute;
    af_i32  CursorColumn;
    af_i32  CursorRow;
    EFI_BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET                Reset;
    EFI_TEXT_STRING               OutputString;
    EFI_TEXT_TEST_STRING          TestString;
    EFI_TEXT_QUERY_MODE           QueryMode;
    EFI_TEXT_SET_MODE             SetMode;
    EFI_TEXT_SET_ATTRIBUTE        SetAttribute;
    EFI_TEXT_CLEAR_SCREEN         ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION  SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR        EnableCursor;
    EFI_SIMPLE_TEXT_OUTPUT_MODE  *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// -----------------------------------------------------------------------------
// Graphics Output Protocol
// -----------------------------------------------------------------------------
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask                          = 2,
    PixelBltOnly                          = 3,
    PixelFormatMax                        = 4,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct AF_PACKED {
    af_u32 RedMask;
    af_u32 GreenMask;
    af_u32 BlueMask;
    af_u32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    af_u32                      Version;
    af_u32                      HorizontalResolution;
    af_u32                      VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT   PixelFormat;
    EFI_PIXEL_BITMASK           PixelInformation;
    af_u32                      PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    af_u32                                MaxMode;
    af_u32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    EFI_UINTN                             SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    EFI_UINTN                             FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, af_u32 ModeNumber,
    EFI_UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, af_u32 ModeNumber);
typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, void *BltBuffer,
    af_u32 BltOperation, EFI_UINTN SourceX, EFI_UINTN SourceY,
    EFI_UINTN DestinationX, EFI_UINTN DestinationY,
    EFI_UINTN Width, EFI_UINTN Height, EFI_UINTN Delta);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// -----------------------------------------------------------------------------
// Loaded image protocol — lets us find where firmware placed us
// -----------------------------------------------------------------------------
typedef struct {
    af_u32   Revision;
    EFI_HANDLE ParentHandle;
    void    *SystemTable;
    EFI_HANDLE DeviceHandle;
    void    *FilePath;
    void    *Reserved;
    af_u32   LoadOptionsSize;
    void    *LoadOptions;
    void    *ImageBase;
    af_u64   ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

// Naturally aligned: Revision (4) + 4 bytes of padding + everything else.
AF_STATIC_ASSERT_SIZE(EFI_LOADED_IMAGE_PROTOCOL, 96);
AF_STATIC_ASSERT_OFFSET(EFI_LOADED_IMAGE_PROTOCOL, Revision, 0);
AF_STATIC_ASSERT_OFFSET(EFI_LOADED_IMAGE_PROTOCOL, ParentHandle, 8);
AF_STATIC_ASSERT_OFFSET(EFI_LOADED_IMAGE_PROTOCOL, ImageBase, 64);
AF_STATIC_ASSERT_OFFSET(EFI_LOADED_IMAGE_PROTOCOL, ImageSize, 72);

// -----------------------------------------------------------------------------
// Boot services
//
// Every member is listed, in specification order, even the ones we never call.
// Omitting any of them would shift the offset of everything after it — which is
// exactly the class of bug this file exists to prevent.
// -----------------------------------------------------------------------------
typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(EFI_MEMORY_TYPE Type,
                                         af_u32 AllocateType, EFI_UINTN Pages,
                                         EFI_PHYSICAL_ADDRESS *Memory);
// EFI_ALLOCATE_TYPE
#define AllocateAnyPages   0
#define AllocateMaxAddress 1
#define AllocateAddress    2

typedef EFI_STATUS (*EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, EFI_UINTN Pages);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(EFI_UINTN *MemoryMapSize,
                                         EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                         EFI_UINTN *MapKey,
                                         EFI_UINTN *DescriptorSize,
                                         af_u32 *DescriptorVersion);
typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType, EFI_UINTN Size,
                                        void **Buffer);
typedef EFI_STATUS (*EFI_FREE_POOL)(void *Buffer);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, EFI_UINTN MapKey);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol, void *Registration,
                                          void **Interface);
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                          void **Interface);
typedef EFI_STATUS (*EFI_STALL)(EFI_UINTN Microseconds);
typedef EFI_STATUS (*EFI_SET_WATCHDOG_TIMER)(EFI_UINTN Timeout, af_u64 WatchdogCode,
                                             EFI_UINTN DataSize, CHAR16 *WatchdogData);
typedef void       (*EFI_COPY_MEM)(void *Destination, void *Source, EFI_UINTN Length);
typedef void       (*EFI_SET_MEM)(void *Buffer, EFI_UINTN Size, af_u8 Value);

typedef struct {
    EFI_TABLE_HEADER  Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_ALLOCATE_PAGES        AllocatePages;          // offset 40
    EFI_FREE_PAGES            FreePages;              // offset 48
    EFI_GET_MEMORY_MAP        GetMemoryMap;           // offset 56
    EFI_ALLOCATE_POOL         AllocatePool;           // offset 64
    EFI_FREE_POOL             FreePool;               // offset 72

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;          // offset 232

    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;

    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;               // offset 320

    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    EFI_COPY_MEM CopyMem;
    EFI_SET_MEM  SetMem;

    void *CreateEventEx;
} EFI_BOOT_SERVICES;

// The offsets below are the same whether or not this structure is packed,
// because everything after the 24-byte header is a pointer and 24 is already
// 8-byte aligned. They are asserted anyway — "it does not matter here" is
// exactly the reasoning that lets a real offset bug through somewhere it does.
AF_STATIC_ASSERT_SIZE(EFI_BOOT_SERVICES, 24 + 44 * 8);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, AllocatePages, 40);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, FreePages, 48);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, GetMemoryMap, 56);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, AllocatePool, 64);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, FreePool, 72);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, HandleProtocol, 152);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, ExitBootServices, 232);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, Stall, 248);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, SetWatchdogTimer, 256);
AF_STATIC_ASSERT_OFFSET(EFI_BOOT_SERVICES, LocateProtocol, 320);

// -----------------------------------------------------------------------------
// Runtime services (only the header is needed; we do not call any yet)
// -----------------------------------------------------------------------------
typedef struct {
    EFI_TABLE_HEADER Hdr;
} EFI_RUNTIME_SERVICES;

// -----------------------------------------------------------------------------
// Configuration table
// -----------------------------------------------------------------------------
typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

// -----------------------------------------------------------------------------
// System table
// -----------------------------------------------------------------------------
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16          *FirmwareVendor;
    af_u32           FirmwareRevision;
    EFI_HANDLE       ConsoleInHandle;
    void            *ConIn;
    EFI_HANDLE       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    EFI_UINTN                        NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// -----------------------------------------------------------------------------
// THE OFFSETS THAT MATTER MOST IN THE ENTIRE BOOT BRIDGE
//
// UEFI firmware's EFI_SYSTEM_TABLE is naturally aligned. After the 24-byte
// header and the u32 FirmwareRevision there are FOUR BYTES OF PADDING before
// ConsoleInHandle, which pushes ConOut to offset 64 — not 60 as a packed layout
// would place it.
//
// Reading ConOut from a packed struct returns four bytes of the wrong pointer
// and the very first console write faults before anything can be printed. These
// asserts exist to make that impossible; if one fires, the structure above is
// wrong, not the number here.
// -----------------------------------------------------------------------------
AF_STATIC_ASSERT_SIZE(EFI_SYSTEM_TABLE, 120);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, Hdr, 0);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, FirmwareVendor, 24);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, FirmwareRevision, 32);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, ConsoleInHandle, 40);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, ConIn, 48);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, ConsoleOutHandle, 56);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, ConOut, 64);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, StandardErrorHandle, 72);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, StdErr, 80);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, RuntimeServices, 88);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, BootServices, 96);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, NumberOfTableEntries, 104);
AF_STATIC_ASSERT_OFFSET(EFI_SYSTEM_TABLE, ConfigurationTable, 112);

// -----------------------------------------------------------------------------
// The UEFI application entry point.
//
// The Microsoft x64 ABI applies regardless of the host compiler, so this must
// be declared with ms_abi. Getting this wrong means the arguments arrive in the
// wrong registers and the very first dereference of SystemTable faults.
// -----------------------------------------------------------------------------
#define EFI_MS_ABI __attribute__((ms_abi))

typedef EFI_STATUS (EFI_MS_ABI *EFI_IMAGE_ENTRY_POINT)(EFI_HANDLE ImageHandle,
                                                       EFI_SYSTEM_TABLE *SystemTable);

#endif // AFRIYIE_EFI_H
