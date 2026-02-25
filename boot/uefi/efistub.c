#include <stdarg.h>
#include <stdint.h>

#include "../include/efi.h"
#include "../include/efilib.h"

EFI_SYSTEM_TABLE *ST;
EFI_BOOT_SERVICES *BS;

EFI_GUID gEfiLoadedImageProtocolGuid = { 0x5B1B31A1u, 0x9562u, 0x11D2u, { 0x8Eu, 0x3Fu, 0x00u, 0xA0u, 0xC9u, 0x69u, 0x72u, 0x3Bu } };
EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22u, 0x6459u, 0x11D2u, { 0x8Eu, 0x39u, 0x00u, 0xA0u, 0xC9u, 0x69u, 0x72u, 0x3Bu } };
EFI_GUID gEfiFileInfoGuid = { 0x09576E92u, 0x6D3Fu, 0x11D2u, { 0x8Eu, 0x39u, 0x00u, 0xA0u, 0xC9u, 0x69u, 0x72u, 0x3Bu } };
EFI_GUID gEfiGraphicsOutputProtocolGuid = { 0x9042A9DEu, 0x23DCu, 0x4A38u, { 0x96u, 0xFBu, 0x7Au, 0xDEu, 0xD0u, 0x80u, 0x51u, 0x6Au } };

static VOID EfiOutput(const CHAR16 *Text) {
    if (ST != 0 && ST->ConOut != 0 && ST->ConOut->OutputString != 0 && Text != 0) {
        ST->ConOut->OutputString(ST->ConOut, Text);
    }
}

static VOID EfiWriteHexStatus(EFI_STATUS Status) {
    CHAR16 Buffer[20];
    static const CHAR16 Hex[] = { L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7', L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F' };
    UINT32 Index;

    Buffer[0] = L'0';
    Buffer[1] = L'x';
    for (Index = 0u; Index < 16u; Index++) {
        UINT32 Shift = (15u - Index) * 4u;
        Buffer[2u + Index] = Hex[(Status >> Shift) & 0xFu];
    }
    Buffer[18] = 0;
    EfiOutput(Buffer);
}

static VOID EfiWriteUnsigned(UINT64 Value) {
    CHAR16 Buffer[32];
    UINT32 Pos = 0u;
    UINT32 Start;

    if (Value == 0u) {
        CHAR16 Zero[2];
        Zero[0] = L'0';
        Zero[1] = 0;
        EfiOutput(Zero);
        return;
    }

    while (Value != 0u && Pos < (UINT32)(sizeof(Buffer) / sizeof(Buffer[0]) - 1u)) {
        Buffer[Pos++] = (CHAR16)(L'0' + (Value % 10u));
        Value /= 10u;
    }

    for (Start = 0u; Start < Pos / 2u; Start++) {
        CHAR16 Tmp = Buffer[Start];
        Buffer[Start] = Buffer[Pos - 1u - Start];
        Buffer[Pos - 1u - Start] = Tmp;
    }
    Buffer[Pos] = 0;
    EfiOutput(Buffer);
}

void InitializeLib(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;
    ST = SystemTable;
    BS = (SystemTable != 0) ? SystemTable->BootServices : 0;
}

UINTN Print(const CHAR16 *Format, ...) {
    va_list Args;
    const CHAR16 *Cursor;
    UINTN Written = 0u;

    if (Format == 0) {
        return 0u;
    }

    va_start(Args, Format);
    Cursor = Format;
    while (*Cursor != 0) {
        if (Cursor[0] == L'%' && Cursor[1] == L'r') {
            EFI_STATUS Status = va_arg(Args, EFI_STATUS);
            EfiWriteHexStatus(Status);
            Cursor += 2;
            continue;
        }
        if (Cursor[0] == L'%' && Cursor[1] == L'u') {
            UINTN Value = va_arg(Args, UINTN);
            EfiWriteUnsigned((UINT64)Value);
            Cursor += 2;
            continue;
        }

        {
            CHAR16 One[2];
            One[0] = *Cursor;
            One[1] = 0;
            EfiOutput(One);
            Written++;
        }
        Cursor++;
    }
    va_end(Args);
    return Written;
}

VOID *AllocatePool(UINTN Size) {
    VOID *Buffer = 0;
    if (BS == 0 || BS->AllocatePool == 0) {
        return 0;
    }
    if (BS->AllocatePool(EfiLoaderData, Size, &Buffer) != EFI_SUCCESS) {
        return 0;
    }
    return Buffer;
}

VOID FreePool(VOID *Buffer) {
    if (BS != 0 && BS->FreePool != 0 && Buffer != 0) {
        (void)BS->FreePool(Buffer);
    }
}

VOID SetMem(VOID *Buffer, UINTN Size, UINT8 Value) {
    UINT8 *Dst = (UINT8 *)Buffer;
    UINTN Index;
    for (Index = 0; Index < Size; Index++) {
        Dst[Index] = Value;
    }
}

VOID CopyMem(VOID *Destination, const VOID *Source, UINTN Size) {
    UINT8 *Dst = (UINT8 *)Destination;
    const UINT8 *Src = (const UINT8 *)Source;
    UINTN Index;
    for (Index = 0; Index < Size; Index++) {
        Dst[Index] = Src[Index];
    }
}
