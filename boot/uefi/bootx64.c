#include <efi.h>
#include <efilib.h>

#include "../../include/kpeformat.h"
#include "../include/bootproto.h"

typedef void(__attribute__((sysv_abi)) *KERNEL_ENTRY_FN)(BOOT_INFO *BootInfo);

typedef VOID(EFIAPI *EFI_STALL_FN)(UINTN Microseconds);
#define BOOT_VERBOSE_DEFAULT TRUE

static BOOLEAN gBootVerbose = BOOT_VERBOSE_DEFAULT;

#define BOOT_VERBOSE_PRINT(...) \
    do {                        \
        if (gBootVerbose) {     \
            Print(__VA_ARGS__); \
        }                       \
    } while (0)

#define BOOT_COM1_BASE 0x3F8u
#define BOOT_GLYPH_W 8u
#define BOOT_GLYPH_H 16u
#define BOOT_GLYPH_ADV 9u

static __inline VOID BootIoOut8(UINT16 Port, UINT8 Value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static __inline UINT8 BootIoIn8(UINT16 Port) {
    UINT8 Value;
    __asm__ __volatile__("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

static VOID BootSerialInit(void) {
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 1u), 0x00u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 3u), 0x80u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 0u), 0x01u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 1u), 0x00u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 3u), 0x03u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 2u), 0xC7u);
    BootIoOut8((UINT16)(BOOT_COM1_BASE + 4u), 0x03u);
}

static VOID BootSerialWriteChar(char Ch) {
    UINTN Spin = 0u;
    while ((BootIoIn8((UINT16)(BOOT_COM1_BASE + 5u)) & 0x20u) == 0u) {
        Spin++;
        if (Spin > 2000000u) {
            return;
        }
    }
    BootIoOut8(BOOT_COM1_BASE, (UINT8)Ch);
}

static VOID BootSerialWriteAscii(const char *Text) {
    UINTN Index = 0u;
    if (Text == NULL) {
        return;
    }
    while (Text[Index] != 0) {
        if (Text[Index] == '\n') {
            BootSerialWriteChar('\r');
        }
        BootSerialWriteChar(Text[Index]);
        Index++;
    }
}

static VOID BootSerialWriteHex64(UINT64 Value) {
    UINTN Shift;
    BootSerialWriteAscii("0x");
    for (Shift = 0u; Shift < 16u; Shift++) {
        UINTN NibbleShift = (15u - Shift) * 4u;
        UINT8 Nibble = (UINT8)((Value >> NibbleShift) & 0xFu);
        char Ch = (char)((Nibble < 10u) ? ('0' + Nibble) : ('A' + (Nibble - 10u)));
        BootSerialWriteChar(Ch);
    }
}

static VOID BootSerialWriteHex8(UINT8 Value) {
    char High;
    char Low;
    High = (char)(((Value >> 4) < 10u) ? ('0' + (Value >> 4)) : ('A' + ((Value >> 4) - 10u)));
    Low = (char)(((Value & 0x0Fu) < 10u) ? ('0' + (Value & 0x0Fu)) : ('A' + ((Value & 0x0Fu) - 10u)));
    BootSerialWriteChar(High);
    BootSerialWriteChar(Low);
}

static VOID BootSerialDumpBytes(const char *Prefix, const UINT8 *Data, UINTN Count) {
    UINTN Index;
    if (Prefix != NULL) {
        BootSerialWriteAscii(Prefix);
    }
    for (Index = 0u; Index < Count; Index++) {
        BootSerialWriteHex8(Data[Index]);
        if (Index + 1u < Count) {
            BootSerialWriteChar(' ');
        }
    }
    BootSerialWriteAscii("\n");
}

static VOID BootCallKernel(UINT64 EntryAddress, BOOT_INFO *BootInfo) {
    __asm__ __volatile__(
        "cli\n\t"
        "movq %0, %%rdi\n\t"
        "call *%1\n\t"
        :
        : "r"(BootInfo), "r"(EntryAddress)
        : "rdi", "memory");
}

typedef struct BOOT_MEMORY_SNAPSHOT {
    EFI_MEMORY_DESCRIPTOR *MapBuffer;
    UINTN MapBufferSize;
    UINTN MapSize;
    UINTN MapKey;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;
    PHYSICAL_MEMORY_RANGE *Ranges;
    UINTN RangeCapacity;
    UINTN RangeCount;
} BOOT_MEMORY_SNAPSHOT;

static VOID BootSleepUs(UINTN Microseconds) {
    EFI_STALL_FN Stall;

    if (gBS == NULL || gBS->Stall == NULL) {
        return;
    }

    Stall = (EFI_STALL_FN)gBS->Stall;
    Stall(Microseconds);
}

static EFI_GRAPHICS_OUTPUT_PROTOCOL *BootLocateGop(void) {
    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

    Gop = NULL;
    if (gBS == NULL) {
        return NULL;
    }

    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    if (EFI_ERROR(Status) || Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
        return NULL;
    }
    return Gop;
}

static UINT32 BootMakePixel(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT8 R, UINT8 G, UINT8 B) {
    if (Gop->Mode->Info->PixelFormat == 1u) {
        return (UINT32)(((UINT32)R << 16) | ((UINT32)G << 8) | (UINT32)B);
    }
    return (UINT32)(((UINT32)B << 16) | ((UINT32)G << 8) | (UINT32)R);
}

static VOID BootDrawPixel(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT32 X, UINT32 Y, UINT32 Pixel) {
    UINT32 *Fb;
    UINT32 Pitch;
    if (X >= Gop->Mode->Info->HorizontalResolution || Y >= Gop->Mode->Info->VerticalResolution) {
        return;
    }
    Fb = (UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
    Pitch = Gop->Mode->Info->PixelsPerScanLine;
    Fb[(Y * Pitch) + X] = Pixel;
}

static VOID BootFillRect(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT32 X, UINT32 Y, UINT32 W, UINT32 H, UINT32 Pixel) {
    UINT32 Row;
    UINT32 Col;
    for (Row = 0; Row < H; Row++) {
        for (Col = 0; Col < W; Col++) {
            BootDrawPixel(Gop, X + Col, Y + Row, Pixel);
        }
    }
}

static UINT8 BootGlyphRow5x7(char Ch, UINT32 Row) {
    switch (Ch) {
        case 'A': { static const UINT8 R[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return R[Row]; }
        case 'B': { static const UINT8 R[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return R[Row]; }
        case 'D': { static const UINT8 R[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return R[Row]; }
        case 'E': { static const UINT8 R[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return R[Row]; }
        case 'L': { static const UINT8 R[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return R[Row]; }
        case 'N': { static const UINT8 R[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return R[Row]; }
        case 'O': { static const UINT8 R[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return R[Row]; }
        case 'P': { static const UINT8 R[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return R[Row]; }
        case 'R': { static const UINT8 R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return R[Row]; }
        case 'T': { static const UINT8 R[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return R[Row]; }
        case 'U': { static const UINT8 R[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return R[Row]; }
        case 'V': { static const UINT8 R[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return R[Row]; }
        case 'W': { static const UINT8 R[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return R[Row]; }
        case 'X': { static const UINT8 R[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return R[Row]; }
        case 'Y': { static const UINT8 R[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return R[Row]; }
        case ' ': return 0x00;
        default:  return 0x00;
    }
}

static UINT8 BootGlyphRow8x16(char Ch, UINT32 Row) {
    UINT32 SrcRow;
    UINT8 SrcBits;

    if (Ch >= 'a' && Ch <= 'z') {
        Ch = (char)(Ch - 'a' + 'A');
    }

    if (Row == 0u || Row >= 15u) {
        return 0u;
    }

    SrcRow = (Row - 1u) >> 1;
    SrcBits = (UINT8)(BootGlyphRow5x7(Ch, SrcRow) & 0x1Fu);
    return (UINT8)(SrcBits << 1);
}

static UINT32 BootTextWidth(const char *Text, UINT32 Scale) {
    UINT32 Count = 0u;
    while (Text[Count] != 0) {
        Count++;
    }
    if (Count == 0u) {
        return 0u;
    }
    return Count * (BOOT_GLYPH_ADV * Scale);
}

static VOID BootDrawTextChars(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                              UINT32 X,
                              UINT32 Y,
                              const char *Text,
                              UINT32 CharCount,
                              UINT32 Scale,
                              UINT32 Pixel) {
    UINT32 ChIndex = 0u;
    while (Text[ChIndex] != 0 && ChIndex < CharCount) {
        char Ch = Text[ChIndex];
        UINT32 Row;
        for (Row = 0u; Row < BOOT_GLYPH_H; Row++) {
            UINT8 Bits = BootGlyphRow8x16(Ch, Row);
            UINT32 Col;
            for (Col = 0u; Col < BOOT_GLYPH_W; Col++) {
                if ((Bits & (UINT8)(1u << (BOOT_GLYPH_W - 1u - Col))) != 0u) {
                    BootFillRect(Gop,
                                 X + (ChIndex * BOOT_GLYPH_ADV + Col) * Scale,
                                 Y + Row * Scale,
                                 Scale,
                                 Scale,
                                 Pixel);
                }
            }
        }
        ChIndex++;
    }
}

static VOID BootDrawText(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                         UINT32 X,
                         UINT32 Y,
                         const char *Text,
                         UINT32 Scale,
                         UINT32 Pixel) {
    UINT32 Count = 0u;
    while (Text[Count] != 0) {
        Count++;
    }
    BootDrawTextChars(Gop, X, Y, Text, Count, Scale, Pixel);
}

static UINT32 BootTextChars(const char *Text) {
    UINT32 Count = 0u;
    while (Text[Count] != 0) {
        Count++;
    }
    return Count;
}

static VOID BootClearFramebufferEarly(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = BootLocateGop();
    if (Gop == NULL) {
        return;
    }
    BootFillRect(Gop,
                 0u,
                 0u,
                 Gop->Mode->Info->HorizontalResolution,
                 Gop->Mode->Info->VerticalResolution,
                 BootMakePixel(Gop, 0x00u, 0x00u, 0x00u));
}

static VOID BootShowSplash(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
    const char *Main = "NOVA TARANTULA";
    const char *Sub = "POWERED BY NTX";
    UINT32 Width;
    UINT32 Height;
    UINT32 MainScale;
    UINT32 SubScale;
    UINT32 MainW;
    UINT32 SubW;
    UINT32 MainH;
    UINT32 SubH;
    UINT32 MainX;
    UINT32 MainY;
    UINT32 SubX;
    UINT32 SubY;
    UINT32 Bg;
    UINT32 MainColor;
    UINT32 MainChars;
    UINT32 StepChars;
    UINT32 Step;
    static const UINT8 Fade[] = { 0x30u, 0x50u, 0x70u, 0x90u, 0xB0u, 0xD0u };

    Gop = BootLocateGop();
    if (Gop == NULL) {
        Print(L"Nova Tarantula\r\n");
        Print(L"Powered by NTX\r\n");
        return;
    }

    Width = Gop->Mode->Info->HorizontalResolution;
    Height = Gop->Mode->Info->VerticalResolution;
    MainScale = 8u;
    SubScale = 4u;
    MainW = BootTextWidth(Main, MainScale);
    while (MainW > (Width - 40u) && MainScale > 3u) {
        MainScale--;
        MainW = BootTextWidth(Main, MainScale);
    }
    SubW = BootTextWidth(Sub, SubScale);
    MainH = BOOT_GLYPH_H * MainScale;
    SubH = BOOT_GLYPH_H * SubScale;
    MainX = (Width > MainW) ? ((Width - MainW) / 2u) : 0u;
    MainY = (Height / 2u) - MainH;
    SubX = (Width > SubW) ? ((Width - SubW) / 2u) : 0u;
    SubY = MainY + MainH + 24u;

    Bg = BootMakePixel(Gop, 0x00u, 0x00u, 0x00u);
    {
        UINT32 ShadowColor = BootMakePixel(Gop, 0x18u, 0x24u, 0x38u);
        UINT32 SubShadowColor = BootMakePixel(Gop, 0x10u, 0x18u, 0x28u);
        UINT32 ShadowDxMain = (MainScale > 1u) ? (MainScale / 2u) : 1u;
        UINT32 ShadowDyMain = (MainScale > 1u) ? (MainScale / 2u) : 1u;
        UINT32 ShadowDxSub = (SubScale > 1u) ? (SubScale / 2u) : 1u;
        UINT32 ShadowDySub = (SubScale > 1u) ? (SubScale / 2u) : 1u;
        MainColor = BootMakePixel(Gop, 0xEAu, 0xEAu, 0xEAu);
        MainChars = BootTextChars(Main);

        BootFillRect(Gop, 0u, 0u, Width, Height, Bg);
        for (StepChars = 1u; StepChars <= MainChars; StepChars++) {
            BootFillRect(Gop, MainX, MainY, MainW + MainScale + 2u, MainH + MainScale + 2u, Bg);
            BootDrawTextChars(Gop, MainX + ShadowDxMain, MainY + ShadowDyMain, Main, StepChars, MainScale, ShadowColor);
            BootDrawTextChars(Gop, MainX, MainY, Main, StepChars, MainScale, MainColor);
            BootSleepUs(35000u);
        }

        for (Step = 0u; Step < (sizeof(Fade) / sizeof(Fade[0])); Step++) {
            UINT32 SubColor = BootMakePixel(Gop, Fade[Step], Fade[Step], Fade[Step]);
            BootFillRect(Gop, SubX, SubY, SubW + SubScale + 2u, SubH + SubScale + 2u, Bg);
            BootDrawText(Gop, SubX + ShadowDxSub, SubY + ShadowDySub, Sub, SubScale, SubShadowColor);
            BootDrawText(Gop, SubX, SubY, Sub, SubScale, SubColor);
            BootSleepUs(85000u);
        }
    }

    /* Keep splash visible long enough to avoid a flash-like transition. */
    BootSleepUs(1800000u);
    if (gBootVerbose) {
        Print(L"[BOOT] verbose mode enabled (default)\r\n");
    }
}

static UINT64 AlignUp64(UINT64 Value, UINT64 Alignment) {
    if (Alignment == 0) {
        return Value;
    }
    return (Value + Alignment - 1) & ~(Alignment - 1);
}

static BOOLEAN FileRangeValid(UINTN FileSize, UINT64 Offset, UINT64 Size) {
    if (Offset > (UINT64)FileSize) {
        return FALSE;
    }
    if (Size > ((UINT64)FileSize - Offset)) {
        return FALSE;
    }
    return TRUE;
}

static EFI_STATUS OpenRootVolume(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **RootOut) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs;

    Status = gBS->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = gBS->HandleProtocol(
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&SimpleFs);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    return SimpleFs->OpenVolume(SimpleFs, RootOut);
}

static EFI_STATUS ReadWholeFile(
    EFI_FILE_PROTOCOL *Root,
    CHAR16 *Path,
    VOID **BufferOut,
    UINTN *SizeOut) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *File;
    EFI_FILE_INFO *Info;
    UINTN InfoSize;
    UINTN ReadSize;
    VOID *FileBuffer;

    Status = Root->Open(Root, &File, Path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
    Info = AllocatePool(InfoSize);
    if (Info == NULL) {
        File->Close(File);
        return EFI_OUT_OF_RESOURCES;
    }

    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (Status == EFI_BUFFER_TOO_SMALL) {
        FreePool(Info);
        Info = AllocatePool(InfoSize);
        if (Info == NULL) {
            File->Close(File);
            return EFI_OUT_OF_RESOURCES;
        }
        Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
    }

    if (EFI_ERROR(Status)) {
        FreePool(Info);
        File->Close(File);
        return Status;
    }

    ReadSize = (UINTN)Info->FileSize;
    FileBuffer = AllocatePool(ReadSize);
    if (FileBuffer == NULL) {
        FreePool(Info);
        File->Close(File);
        return EFI_OUT_OF_RESOURCES;
    }

    Status = File->Read(File, &ReadSize, FileBuffer);
    File->Close(File);
    FreePool(Info);
    if (EFI_ERROR(Status)) {
        FreePool(FileBuffer);
        return Status;
    }

    *BufferOut = FileBuffer;
    *SizeOut = ReadSize;
    return EFI_SUCCESS;
}

static BOOLEAN ValidateKpeImage(
    VOID *ImageFileBuffer,
    UINTN ImageFileSize,
    KPE_NT_HEADERS64 **NtOut,
    KPE_SECTION_HEADER **SectionsOut) {
    KPE_DOS_HEADER *Dos;
    KPE_NT_HEADERS64 *Nt;
    KPE_SECTION_HEADER *Sections;
    UINTN SectionTableOffset;
    UINTN Index;

    if (ImageFileSize < sizeof(KPE_DOS_HEADER)) {
        return FALSE;
    }

    Dos = (KPE_DOS_HEADER *)ImageFileBuffer;
    if (Dos->e_magic != KPE_DOS_MAGIC) {
        return FALSE;
    }

    if ((UINT64)Dos->e_lfanew + sizeof(KPE_NT_HEADERS64) > ImageFileSize) {
        return FALSE;
    }

    Nt = (KPE_NT_HEADERS64 *)((UINT8 *)ImageFileBuffer + Dos->e_lfanew);
    if (Nt->Signature != KPE_SIGNATURE) {
        return FALSE;
    }

    if (Nt->FileHeader.Machine != KPE_MACHINE_AUR64) {
        return FALSE;
    }

    if (Nt->FileHeader.NumberOfSections == 0) {
        return FALSE;
    }

    if (Nt->FileHeader.SizeOfOptionalHeader < sizeof(KPE_OPTIONAL_HEADER64)) {
        return FALSE;
    }

    if (Nt->OptionalHeader.Magic != KPE_OPT_MAGIC_AUR64) {
        return FALSE;
    }

    if (Nt->OptionalHeader.Subsystem != KPE_SUBSYSTEM_NATIVE_KERNEL) {
        return FALSE;
    }

    if (Nt->OptionalHeader.SectionAlignment == 0 || Nt->OptionalHeader.FileAlignment == 0) {
        return FALSE;
    }

    if (Nt->OptionalHeader.AddressOfEntryPoint >= Nt->OptionalHeader.SizeOfImage) {
        return FALSE;
    }

    if (Nt->OptionalHeader.NumberOfRvaAndSizes > KPE_NUMBEROF_DIRECTORY_ENTRIES) {
        return FALSE;
    }

    SectionTableOffset = (UINTN)Dos->e_lfanew +
        sizeof(UINT32) +
        sizeof(KPE_FILE_HEADER) +
        Nt->FileHeader.SizeOfOptionalHeader;

    if ((UINT64)SectionTableOffset + ((UINT64)Nt->FileHeader.NumberOfSections * sizeof(KPE_SECTION_HEADER)) > ImageFileSize) {
        return FALSE;
    }

    Sections = (KPE_SECTION_HEADER *)((UINT8 *)ImageFileBuffer + SectionTableOffset);
    for (Index = 0; Index < Nt->FileHeader.NumberOfSections; Index++) {
        UINT64 VirtualSpan;

        if (Sections[Index].VirtualAddress >= Nt->OptionalHeader.SizeOfImage) {
            return FALSE;
        }

        VirtualSpan = AlignUp64(Sections[Index].VirtualSize, Nt->OptionalHeader.SectionAlignment);
        if ((UINT64)Sections[Index].VirtualAddress + VirtualSpan > Nt->OptionalHeader.SizeOfImage) {
            return FALSE;
        }

        if (Sections[Index].SizeOfRawData == 0) {
            continue;
        }

        if (!FileRangeValid(ImageFileSize, Sections[Index].PointerToRawData, Sections[Index].SizeOfRawData)) {
            return FALSE;
        }
    }

    *NtOut = Nt;
    *SectionsOut = Sections;
    return TRUE;
}

static VOID *ImageRvaToPointer(
    EFI_PHYSICAL_ADDRESS ImageBase,
    UINT64 ImageSize,
    UINT32 Rva,
    UINT32 Size) {
    UINT64 End;

    End = (UINT64)Rva + (UINT64)Size;
    if (End > ImageSize) {
        return NULL;
    }

    return (VOID *)(UINTN)(ImageBase + Rva);
}

static EFI_STATUS ApplyKpeBaseRelocations(
    EFI_PHYSICAL_ADDRESS LoadedImageBase,
    KPE_NT_HEADERS64 *Nt) {
    KPE_DATA_DIRECTORY *RelocDir;
    UINT8 *Cursor;
    UINT8 *End;
    INT64 Delta;

    if (Nt == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Delta = (INT64)((INT64)LoadedImageBase - (INT64)Nt->OptionalHeader.ImageBase);
    if (Delta == 0) {
        return EFI_SUCCESS;
    }

    RelocDir = &Nt->OptionalHeader.DataDirectory[KPE_DIR_BASERELOC];
    if (RelocDir->VirtualAddress == 0 || RelocDir->Size < sizeof(KPE_BASE_RELOCATION_BLOCK)) {
        return EFI_UNSUPPORTED;
    }

    Cursor = (UINT8 *)ImageRvaToPointer(
        LoadedImageBase,
        Nt->OptionalHeader.SizeOfImage,
        RelocDir->VirtualAddress,
        RelocDir->Size);
    if (Cursor == NULL) {
        return EFI_LOAD_ERROR;
    }

    End = Cursor + RelocDir->Size;
    while ((Cursor + sizeof(KPE_BASE_RELOCATION_BLOCK)) <= End) {
        KPE_BASE_RELOCATION_BLOCK *Block;
        UINT32 EntryCount;
        UINT32 Index;
        UINT16 *Entries;

        Block = (KPE_BASE_RELOCATION_BLOCK *)Cursor;
        if (Block->SizeOfBlock < sizeof(KPE_BASE_RELOCATION_BLOCK)) {
            return EFI_LOAD_ERROR;
        }

        if ((Cursor + Block->SizeOfBlock) > End) {
            return EFI_LOAD_ERROR;
        }

        EntryCount = (Block->SizeOfBlock - sizeof(KPE_BASE_RELOCATION_BLOCK)) / sizeof(UINT16);
        Entries = (UINT16 *)(Cursor + sizeof(KPE_BASE_RELOCATION_BLOCK));

        for (Index = 0; Index < EntryCount; Index++) {
            UINT16 Entry;
            UINT16 Type;
            UINT16 Offset;
            UINT32 PatchRva;
            UINT64 *PatchValue;

            Entry = Entries[Index];
            Type = (UINT16)(Entry >> 12);
            Offset = (UINT16)(Entry & 0x0FFFu);

            if (Type == KPE_REL_BASED_ABSOLUTE) {
                continue;
            }

            if (Type != KPE_REL_BASED_DIR64) {
                return EFI_UNSUPPORTED;
            }

            PatchRva = Block->VirtualAddress + Offset;
            PatchValue = (UINT64 *)ImageRvaToPointer(
                LoadedImageBase,
                Nt->OptionalHeader.SizeOfImage,
                PatchRva,
                sizeof(UINT64));
            if (PatchValue == NULL) {
                return EFI_LOAD_ERROR;
            }

            *PatchValue = (UINT64)((INT64)(*PatchValue) + Delta);
        }

        Cursor += Block->SizeOfBlock;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS LoadKpeImage(
    VOID *ImageFileBuffer,
    UINTN ImageFileSize,
    EFI_PHYSICAL_ADDRESS *ImageBaseOut,
    UINT64 *ImageSizeOut,
    UINT64 *EntryAddressOut,
    INT64 *RelocationDeltaOut) {
    EFI_STATUS Status;
    KPE_NT_HEADERS64 *Nt;
    KPE_SECTION_HEADER *Sections;
    EFI_PHYSICAL_ADDRESS ImageBase;
    UINTN Pages;
    UINTN Index;

    if (!ValidateKpeImage(ImageFileBuffer, ImageFileSize, &Nt, &Sections)) {
        return EFI_LOAD_ERROR;
    }

    Pages = EFI_SIZE_TO_PAGES((UINTN)Nt->OptionalHeader.SizeOfImage);
    ImageBase = (EFI_PHYSICAL_ADDRESS)Nt->OptionalHeader.ImageBase;
    Status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, Pages, &ImageBase);
    if (EFI_ERROR(Status)) {
        if ((Nt->OptionalHeader.DllCharacteristics & KPE_DLLCHAR_DYNAMIC_BASE) == 0) {
            return Status;
        }

        ImageBase = 0;
        Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderCode, Pages, &ImageBase);
        if (EFI_ERROR(Status)) {
            return Status;
        }
    }

    SetMem((VOID *)(UINTN)ImageBase, (UINTN)Nt->OptionalHeader.SizeOfImage, 0);

    if (Nt->OptionalHeader.SizeOfHeaders > 0) {
        UINTN HeaderCopySize = Nt->OptionalHeader.SizeOfHeaders;
        if (HeaderCopySize > ImageFileSize) {
            HeaderCopySize = ImageFileSize;
        }
        CopyMem((VOID *)(UINTN)ImageBase, ImageFileBuffer, HeaderCopySize);
    }

    for (Index = 0; Index < Nt->FileHeader.NumberOfSections; Index++) {
        UINT8 *Dest;
        UINT8 *Src;
        UINT32 CopySize;
        UINT32 ClearSize;

        Dest = (UINT8 *)(UINTN)(ImageBase + Sections[Index].VirtualAddress);

        CopySize = Sections[Index].SizeOfRawData;
        if (CopySize > 0) {
            Src = (UINT8 *)ImageFileBuffer + Sections[Index].PointerToRawData;
            CopyMem(Dest, Src, CopySize);
        }

        ClearSize = 0;
        if (Sections[Index].VirtualSize > CopySize) {
            ClearSize = Sections[Index].VirtualSize - CopySize;
        }
        if (ClearSize > 0) {
            SetMem(Dest + CopySize, ClearSize, 0);
        }
    }

    Status = ApplyKpeBaseRelocations(ImageBase, Nt);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    *ImageBaseOut = ImageBase;
    *ImageSizeOut = Nt->OptionalHeader.SizeOfImage;
    *EntryAddressOut = (UINT64)(ImageBase + Nt->OptionalHeader.AddressOfEntryPoint);
    if (RelocationDeltaOut != NULL) {
        *RelocationDeltaOut = (INT64)((INT64)ImageBase - (INT64)Nt->OptionalHeader.ImageBase);
    }
    return EFI_SUCCESS;
}

static VOID CaptureVideoInfo(BOOT_VIDEO_INFO *VideoOut) {
    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

    SetMem(VideoOut, sizeof(*VideoOut), 0);
    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    if (EFI_ERROR(Status) || Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
        return;
    }

    VideoOut->FrameBufferBase = Gop->Mode->FrameBufferBase;
    VideoOut->FrameBufferSize = (UINT32)Gop->Mode->FrameBufferSize;
    VideoOut->Width = Gop->Mode->Info->HorizontalResolution;
    VideoOut->Height = Gop->Mode->Info->VerticalResolution;
    VideoOut->PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;
    VideoOut->PixelFormat = Gop->Mode->Info->PixelFormat;
}

static EFI_STATUS SnapshotMemoryMap(BOOT_MEMORY_SNAPSHOT *Snapshot) {
    EFI_STATUS Status;
    UINTN ProbedMapSize;
    UINTN RangeCapacity;

    SetMem(Snapshot, sizeof(*Snapshot), 0);

    ProbedMapSize = 0;
    Status = gBS->GetMemoryMap(
        &ProbedMapSize,
        NULL,
        &Snapshot->MapKey,
        &Snapshot->DescriptorSize,
        &Snapshot->DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        return Status;
    }

    Snapshot->MapBufferSize = ProbedMapSize + (Snapshot->DescriptorSize * 16);
    Snapshot->MapBuffer = AllocatePool(Snapshot->MapBufferSize);
    if (Snapshot->MapBuffer == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    RangeCapacity = Snapshot->MapBufferSize / Snapshot->DescriptorSize;
    Snapshot->Ranges = AllocatePool(sizeof(PHYSICAL_MEMORY_RANGE) * RangeCapacity);
    if (Snapshot->Ranges == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    Snapshot->RangeCapacity = RangeCapacity;

    Snapshot->MapSize = Snapshot->MapBufferSize;
    Status = gBS->GetMemoryMap(
        &Snapshot->MapSize,
        Snapshot->MapBuffer,
        &Snapshot->MapKey,
        &Snapshot->DescriptorSize,
        &Snapshot->DescriptorVersion);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    return EFI_SUCCESS;
}

static VOID ConvertMemoryMap(BOOT_MEMORY_SNAPSHOT *Snapshot) {
    UINTN Offset;
    UINTN Index;

    Snapshot->RangeCount = 0;
    for (Offset = 0; Offset < Snapshot->MapSize; Offset += Snapshot->DescriptorSize) {
        EFI_MEMORY_DESCRIPTOR *Descriptor;
        PHYSICAL_MEMORY_RANGE *Range;

        Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Snapshot->MapBuffer + Offset);
        if (Snapshot->RangeCount >= Snapshot->RangeCapacity) {
            break;
        }

        Index = Snapshot->RangeCount++;
        Range = &Snapshot->Ranges[Index];
        Range->Base = Descriptor->PhysicalStart;
        Range->Length = EFI_PAGES_TO_SIZE(Descriptor->NumberOfPages);
        Range->Type = (UINT32)Descriptor->Type;
        Range->Attributes = (UINT32)(Descriptor->Attribute & 0xFFFFFFFFu);
    }
}

static VOID PopulateBootInfo(
    BOOT_INFO *BootInfo,
    EFI_PHYSICAL_ADDRESS LoaderBase,
    EFI_PHYSICAL_ADDRESS KernelBase,
    UINT64 KernelSize,
    UINT64 KernelEntry,
    VOID *InitImageBuffer,
    UINTN InitImageSize,
    BOOT_MEMORY_SNAPSHOT *Snapshot,
    BOOLEAN VerboseBoot) {
    SetMem(BootInfo, sizeof(*BootInfo), 0);
    BootInfo->Magic = BOOTINFO_MAGIC;
    BootInfo->Version = BOOTINFO_VERSION;
    BootInfo->Size = sizeof(*BootInfo);
    BootInfo->FirmwareType = 1;
    BootInfo->LoaderBase = LoaderBase;
    BootInfo->KernelBase = KernelBase;
    BootInfo->KernelSize = KernelSize;
    BootInfo->KernelEntry = KernelEntry;
    BootInfo->InitImageBase = (UINT64)(UINTN)InitImageBuffer;
    BootInfo->InitImageSize = (UINT64)InitImageSize;
    if (InitImageBuffer != NULL && InitImageSize != 0u) {
        BootInfo->Flags |= BOOTINFO_FLAG_INIT_IMAGE_PRESENT;
    }
    if (VerboseBoot != FALSE) {
        BootInfo->Flags |= BOOTINFO_FLAG_VERBOSE_BOOT;
    }
    BootInfo->MemoryMap = (UINT64)(UINTN)Snapshot->Ranges;
    BootInfo->MemoryMapEntryCount = (UINT32)Snapshot->RangeCount;
    BootInfo->MemoryMapEntrySize = sizeof(PHYSICAL_MEMORY_RANGE);
    CaptureVideoInfo(&BootInfo->Video);
}

static EFI_STATUS ExitBootServicesRobust(EFI_HANDLE ImageHandle,
                                         BOOT_INFO *BootInfo,
                                         BOOT_MEMORY_SNAPSHOT *Snapshot) {
    EFI_STATUS Status = EFI_INVALID_PARAMETER;
    UINTN Attempt;

    if (Snapshot == NULL || BootInfo == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    /* UEFI requirement: no boot-services calls between GetMemoryMap and ExitBootServices. */
    for (Attempt = 0u; Attempt < 4u; Attempt++) {
        Snapshot->MapSize = Snapshot->MapBufferSize;
        Status = gBS->GetMemoryMap(
            &Snapshot->MapSize,
            Snapshot->MapBuffer,
            &Snapshot->MapKey,
            &Snapshot->DescriptorSize,
            &Snapshot->DescriptorVersion);
        if (Status == EFI_BUFFER_TOO_SMALL) {
            UINTN NewBufferSize;
            EFI_MEMORY_DESCRIPTOR *NewMapBuffer;
            PHYSICAL_MEMORY_RANGE *NewRanges;
            UINTN NewRangeCapacity;

            NewBufferSize = Snapshot->MapSize + (Snapshot->DescriptorSize * 16u);
            NewMapBuffer = AllocatePool(NewBufferSize);
            if (NewMapBuffer == NULL) {
                return EFI_OUT_OF_RESOURCES;
            }

            NewRangeCapacity = NewBufferSize / Snapshot->DescriptorSize;
            NewRanges = AllocatePool(sizeof(PHYSICAL_MEMORY_RANGE) * NewRangeCapacity);
            if (NewRanges == NULL) {
                FreePool(NewMapBuffer);
                return EFI_OUT_OF_RESOURCES;
            }

            if (Snapshot->MapBuffer != NULL) {
                FreePool(Snapshot->MapBuffer);
            }
            if (Snapshot->Ranges != NULL) {
                FreePool(Snapshot->Ranges);
            }

            Snapshot->MapBuffer = NewMapBuffer;
            Snapshot->MapBufferSize = NewBufferSize;
            Snapshot->Ranges = NewRanges;
            Snapshot->RangeCapacity = NewRangeCapacity;
            continue;
        }
        if (EFI_ERROR(Status)) {
            return Status;
        }

        ConvertMemoryMap(Snapshot);
        BootInfo->MemoryMapEntryCount = (UINT32)Snapshot->RangeCount;

        Status = gBS->ExitBootServices(ImageHandle, Snapshot->MapKey);
        if (!EFI_ERROR(Status)) {
            return EFI_SUCCESS;
        }

        if (Status != EFI_INVALID_PARAMETER) {
            return Status;
        }
    }

    return Status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Root;
    VOID *KernelBuffer;
    UINTN KernelFileSize;
    VOID *InitBuffer;
    UINTN InitFileSize;
    EFI_PHYSICAL_ADDRESS KernelBase;
    UINT64 KernelImageSize;
    UINT64 KernelEntry;
    INT64 KernelRelocationDelta;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    BOOT_INFO *BootInfo;
    BOOT_MEMORY_SNAPSHOT Snapshot;

    InitializeLib(ImageHandle, SystemTable);
    BootSerialInit();
    BootSerialWriteAscii("[BL] efi_main entered\n");
    BOOT_VERBOSE_PRINT(L"NTX Boot: UEFI boot begin\r\n");

    Status = OpenRootVolume(ImageHandle, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: root volume open failed: %r\r\n", Status);
        return Status;
    }
    BOOT_VERBOSE_PRINT(L"AsterLoader: root volume open ok\r\n");

    Status = ReadWholeFile(Root, L"\\kernel\\ntxkrnl.kpe", &KernelBuffer, &KernelFileSize);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: kernel file read failed: %r\r\n", Status);
        return Status;
    }
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel file bytes=%u\r\n", KernelFileSize);

    InitBuffer = NULL;
    InitFileSize = 0;
    Status = ReadWholeFile(Root, L"\\System\\init.kpe", &InitBuffer, &InitFileSize);
    if (EFI_ERROR(Status)) {
        InitBuffer = NULL;
        InitFileSize = 0;
        Print(L"AsterLoader: init.kpe not found, continuing without init\r\n");
    } else {
        Print(L"AsterLoader: init.kpe loaded (%u bytes)\r\n", (UINTN)InitFileSize);
    }

    BOOT_VERBOSE_PRINT(L"AsterLoader: stage load kernel image\r\n");
    Status = LoadKpeImage(KernelBuffer,
                          KernelFileSize,
                          &KernelBase,
                          &KernelImageSize,
                          &KernelEntry,
                          &KernelRelocationDelta);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: kernel load failed: %r\r\n", Status);
        return Status;
    }
    BOOT_VERBOSE_PRINT(L"AsterLoader: stage kernel image ready\r\n");
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel image bytes=%u\r\n", (UINTN)KernelImageSize);
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel base=%r\r\n", (EFI_STATUS)KernelBase);
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel entry=%r\r\n", (EFI_STATUS)KernelEntry);
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel entry offset=%r\r\n", (EFI_STATUS)(KernelEntry - (UINT64)KernelBase));
    BOOT_VERBOSE_PRINT(L"AsterLoader: kernel reloc delta=%r\r\n", (EFI_STATUS)KernelRelocationDelta);
    if (KernelEntry < (UINT64)KernelBase || KernelEntry >= ((UINT64)KernelBase + KernelImageSize)) {
        Print(L"AsterLoader: kernel entry out of range\r\n");
        return EFI_LOAD_ERROR;
    }

    BOOT_VERBOSE_PRINT(L"AsterLoader: stage loader protocol\r\n");
    Status = gBS->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: image protocol missing: %r\r\n", Status);
        return Status;
    }

    BootInfo = AllocatePool(sizeof(BOOT_INFO));
    if (BootInfo == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    BOOT_VERBOSE_PRINT(L"AsterLoader: stage snapshot memory map\r\n");
    Status = SnapshotMemoryMap(&Snapshot);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: memory map snapshot failed: %r\r\n", Status);
        return Status;
    }
    ConvertMemoryMap(&Snapshot);
    BOOT_VERBOSE_PRINT(L"AsterLoader: stage boot info ready\r\n");
    BOOT_VERBOSE_PRINT(L"AsterLoader: mmap entries=%u\r\n", Snapshot.RangeCount);

    PopulateBootInfo(
        BootInfo,
        (EFI_PHYSICAL_ADDRESS)(UINTN)LoadedImage->ImageBase,
        KernelBase,
        KernelImageSize,
        KernelEntry,
        InitBuffer,
        InitFileSize,
        &Snapshot,
        gBootVerbose);

    /* Visual sequence: firmware splash first, then Nova splash right before kernel handoff. */
    BootClearFramebufferEarly();
    BootShowSplash();

    BOOT_VERBOSE_PRINT(L"AsterLoader: stage exit boot services\r\n");
    BootSerialWriteAscii("[BL] about to ExitBootServices\n");
    Status = ExitBootServicesRobust(ImageHandle, BootInfo, &Snapshot);
    if (EFI_ERROR(Status)) {
        Print(L"AsterLoader: exit boot services failed: %r\r\n", Status);
        return Status;
    }
    BootSerialWriteAscii("[BL] ExitBootServices OK\n");

    BootSerialWriteAscii("[BL] jumping kernel entry=");
    BootSerialWriteHex64(BootInfo->KernelEntry);
    BootSerialWriteAscii(" boot_info=");
    BootSerialWriteHex64((UINT64)(UINTN)BootInfo);
    BootSerialWriteAscii("\n");
    BootSerialDumpBytes("[BL] entry bytes: ", (const UINT8 *)(UINTN)BootInfo->KernelEntry, 16u);
    BootCallKernel(BootInfo->KernelEntry, BootInfo);
    BootSerialWriteAscii("[BL] kernel returned unexpectedly\n");

    for (;;) {
        __asm__ __volatile__("hlt");
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
