#ifndef EFILIB_H
#define EFILIB_H

#include "efi.h"

extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;

#define gST ST
#define gBS BS

void InitializeLib(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
UINTN Print(const CHAR16 *Format, ...);
VOID *AllocatePool(UINTN Size);
VOID FreePool(VOID *Buffer);
VOID SetMem(VOID *Buffer, UINTN Size, UINT8 Value);
VOID CopyMem(VOID *Destination, const VOID *Source, UINTN Size);

#endif /* EFILIB_H */
