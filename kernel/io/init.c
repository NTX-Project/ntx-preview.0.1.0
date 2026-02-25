#include "../../hal/inc/hal.h"
#include "../include/io.h"
#include "../include/nk.h"
#include "../include/ob.h"
#include "../include/ps.h"

#define IO_MAX_FILE_OBJECTS 128u
#define IO_MAX_BOOT_FILES 16u
#define IO_SPAWN_IMAGE_MAX (16u * 1024u * 1024u)

typedef struct IO_BOOT_FILE {
    char Path[64];
    const uint8_t *Data;
    uint64_t Size;
    uint8_t InUse;
} IO_BOOT_FILE;

typedef struct IO_FILE_OBJECT {
    OB_OBJECT_HEADER *Header;
    const uint8_t *Data;
    uint64_t Size;
    uint32_t AccessMask;
} IO_FILE_OBJECT;

static OB_OBJECT_TYPE *gIoFileType;
static IO_FILE_OBJECT gIoFilePool[IO_MAX_FILE_OBJECTS];
static uint8_t gIoFileInUse[IO_MAX_FILE_OBJECTS];
static IO_BOOT_FILE gIoBootFiles[IO_MAX_BOOT_FILES];
static uint8_t gIoSpawnScratch[IO_SPAWN_IMAGE_MAX];
static uint32_t gIoReady;

static void IopHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void IopWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[128];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    IopHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static uint32_t IopStringEquals(const char *Left, const char *Right) {
    uint32_t Index;
    if (Left == 0 || Right == 0) {
        return 0u;
    }

    for (Index = 0u; ; Index++) {
        if (Left[Index] != Right[Index]) {
            return 0u;
        }
        if (Left[Index] == 0) {
            return 1u;
        }
    }
}

static uint32_t IopCopyString(char *Destination, uint32_t DestinationBytes, const char *Source) {
    uint32_t Index;

    if (Destination == 0 || Source == 0 || DestinationBytes == 0u) {
        return 0u;
    }

    for (Index = 0u; Index < DestinationBytes - 1u; Index++) {
        Destination[Index] = Source[Index];
        if (Source[Index] == 0) {
            return 1u;
        }
    }

    Destination[DestinationBytes - 1u] = 0;
    return 0u;
}

static IO_FILE_OBJECT *IopFindFileByHeader(OB_OBJECT_HEADER *Header) {
    uint32_t Index;
    for (Index = 0u; Index < IO_MAX_FILE_OBJECTS; Index++) {
        if (gIoFileInUse[Index] != 0u && gIoFilePool[Index].Header == Header) {
            return &gIoFilePool[Index];
        }
    }
    return 0;
}

static IO_FILE_OBJECT *IopAllocateFileSlot(void) {
    uint32_t Index;
    for (Index = 0u; Index < IO_MAX_FILE_OBJECTS; Index++) {
        if (gIoFileInUse[Index] == 0u) {
            gIoFileInUse[Index] = 1u;
            gIoFilePool[Index].Header = 0;
            gIoFilePool[Index].Data = 0;
            gIoFilePool[Index].Size = 0u;
            gIoFilePool[Index].AccessMask = 0u;
            return &gIoFilePool[Index];
        }
    }
    return 0;
}

static void IopDeleteFileObject(OB_OBJECT_HEADER *Object) {
    uint32_t Index;
    for (Index = 0u; Index < IO_MAX_FILE_OBJECTS; Index++) {
        if (gIoFileInUse[Index] != 0u && gIoFilePool[Index].Header == Object) {
            gIoFileInUse[Index] = 0u;
            gIoFilePool[Index].Header = 0;
            gIoFilePool[Index].Data = 0;
            gIoFilePool[Index].Size = 0u;
            gIoFilePool[Index].AccessMask = 0u;
            return;
        }
    }
}

static const IO_BOOT_FILE *IopFsFatLookupFile(const char *Path) {
    uint32_t Index;
    for (Index = 0u; Index < IO_MAX_BOOT_FILES; Index++) {
        if (gIoBootFiles[Index].InUse != 0u &&
            IopStringEquals(gIoBootFiles[Index].Path, Path) != 0u) {
            return &gIoBootFiles[Index];
        }
    }
    return 0;
}

static KSTATUS IopVfsOpen(const char *Path, IO_FILE_OBJECT *FileObject) {
    const IO_BOOT_FILE *FatFile;

    if (Path == 0 || FileObject == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    /* Thin VFS: route all current paths to FAT backend lookup. */
    FatFile = IopFsFatLookupFile(Path);
    if (FatFile == 0) {
        return KSTATUS_NOT_FOUND;
    }

    FileObject->Data = FatFile->Data;
    FileObject->Size = FatFile->Size;
    return KSTATUS_SUCCESS;
}

KSTATUS IoInitialize(void) {
    KSTATUS Status;
    const BOOT_INFO *BootInfo;
    uint32_t Index;

    if (gIoReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }

    for (Index = 0u; Index < IO_MAX_FILE_OBJECTS; Index++) {
        gIoFileInUse[Index] = 0u;
    }
    for (Index = 0u; Index < IO_MAX_BOOT_FILES; Index++) {
        gIoBootFiles[Index].InUse = 0u;
        gIoBootFiles[Index].Path[0] = 0;
        gIoBootFiles[Index].Data = 0;
        gIoBootFiles[Index].Size = 0u;
    }

    Status = ObCreateType("File", IopDeleteFileObject, &gIoFileType);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    gIoReady = 1u;
    BootInfo = NkGetBootInfo();
    if (BootInfo != 0 &&
        (BootInfo->Flags & BOOTINFO_FLAG_INIT_IMAGE_PRESENT) != 0u &&
        BootInfo->InitImageBase != 0u &&
        BootInfo->InitImageSize != 0u) {
        (void)IoRegisterBootFile("\\System\\init.kpe",
                                 (const void *)(uintptr_t)BootInfo->InitImageBase,
                                 BootInfo->InitImageSize);
    }
    HalWriteDebugString("[IO] I/O manager online (VFS + File handles)\n");
    return KSTATUS_SUCCESS;
}

KSTATUS IoRegisterBootFile(const char *Path, const void *Data, uint64_t Size) {
    uint32_t Index;

    if (gIoReady == 0u || Path == 0 || Data == 0 || Size == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0u; Index < IO_MAX_BOOT_FILES; Index++) {
        if (gIoBootFiles[Index].InUse == 0u) {
            if (IopCopyString(gIoBootFiles[Index].Path, sizeof(gIoBootFiles[Index].Path), Path) == 0u) {
                return KSTATUS_INVALID_PARAMETER;
            }
            gIoBootFiles[Index].Data = (const uint8_t *)Data;
            gIoBootFiles[Index].Size = Size;
            gIoBootFiles[Index].InUse = 1u;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

KSTATUS IoOpenPath(const char *Path, uint32_t DesiredAccess, uint32_t *OutHandle) {
    IO_FILE_OBJECT *FileObject;
    OB_OBJECT_HEADER *Header;
    OB_HANDLE_TABLE *HandleTable;
    KSTATUS Status;

    if (gIoReady == 0u || Path == 0 || OutHandle == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }
    if ((DesiredAccess & IO_FILE_ACCESS_ALL) == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    FileObject = IopAllocateFileSlot();
    if (FileObject == 0) {
        return KSTATUS_INIT_FAILED;
    }

    Status = ObCreateObject(gIoFileType, "io.file", IO_FILE_ACCESS_ALL, 0u, &Header);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    FileObject->Header = Header;
    FileObject->AccessMask = DesiredAccess;
    Status = IopVfsOpen(Path, FileObject);
    if (Status != KSTATUS_SUCCESS) {
        (void)ObDereferenceObject(Header);
        return Status;
    }

    HandleTable = ObGetKernelHandleTable();
    Status = ObInsertHandle(HandleTable, Header, DesiredAccess, OutHandle);
    (void)ObDereferenceObject(Header);
    return Status;
}

KSTATUS IoReadFileHandle(uint32_t HandleValue,
                         uint64_t Offset,
                         void *OutBuffer,
                         uint64_t BufferLength,
                         uint64_t *OutBytesRead) {
    OB_HANDLE_TABLE *HandleTable;
    OB_OBJECT_HEADER *Header;
    IO_FILE_OBJECT *FileObject;
    uint64_t Remaining;
    uint64_t ReadLength;
    uint64_t Index;
    KSTATUS Status;

    if (gIoReady == 0u || OutBuffer == 0 || OutBytesRead == 0 || BufferLength == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HandleTable = ObGetKernelHandleTable();
    Status = ObReferenceObjectByHandle(HandleTable,
                                       HandleValue,
                                       IO_FILE_ACCESS_READ,
                                       gIoFileType,
                                       &Header);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    FileObject = IopFindFileByHeader(Header);
    if (FileObject == 0) {
        (void)ObDereferenceObject(Header);
        return KSTATUS_NOT_FOUND;
    }

    if (Offset >= FileObject->Size) {
        *OutBytesRead = 0u;
        (void)ObDereferenceObject(Header);
        return KSTATUS_SUCCESS;
    }

    Remaining = FileObject->Size - Offset;
    ReadLength = (BufferLength < Remaining) ? BufferLength : Remaining;
    for (Index = 0u; Index < ReadLength; Index++) {
        ((uint8_t *)OutBuffer)[Index] = FileObject->Data[Offset + Index];
    }

    *OutBytesRead = ReadLength;
    (void)ObDereferenceObject(Header);
    return KSTATUS_SUCCESS;
}

KSTATUS IoCloseFileHandle(uint32_t HandleValue) {
    OB_HANDLE_TABLE *HandleTable;

    if (gIoReady == 0u || HandleValue == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HandleTable = ObGetKernelHandleTable();
    return ObCloseHandle(HandleTable, HandleValue);
}

KSTATUS IoSpawnProcessFromFileHandle(uint32_t HandleValue,
                                     uint64_t *OutProcessId,
                                     uint64_t *OutEntryAddress) {
    uint64_t Offset = 0u;
    uint64_t Total = 0u;
    uint64_t ReadNow = 0u;
    KSTATUS Status;

    if (gIoReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HalWriteDebugString("[IO] spawn-from-file begin\n");
    IopWriteHexLine("[IO] spawn.handle=", HandleValue);

    for (;;) {
        if (Total >= IO_SPAWN_IMAGE_MAX) {
            return KSTATUS_INVALID_PARAMETER;
        }

        Status = IoReadFileHandle(HandleValue,
                                  Offset,
                                  &gIoSpawnScratch[Total],
                                  IO_SPAWN_IMAGE_MAX - Total,
                                  &ReadNow);
        if (Status != KSTATUS_SUCCESS) {
            IopWriteHexLine("[IO] spawn.read.status=", (uint64_t)(uint32_t)Status);
            return Status;
        }
        if (ReadNow == 0u) {
            break;
        }

        Offset += ReadNow;
        Total += ReadNow;
    }

    if (Total == 0u) {
        HalWriteDebugString("[IO] spawn failed: empty image\n");
        return KSTATUS_NOT_FOUND;
    }

    IopWriteHexLine("[IO] spawn.image.bytes=", Total);
    Status = PsSpawnUserKpeFromBuffer(gIoSpawnScratch, Total, OutProcessId, OutEntryAddress);
    IopWriteHexLine("[IO] spawn.status=", (uint64_t)(uint32_t)Status);
    if (Status == KSTATUS_SUCCESS) {
        if (OutProcessId != 0) {
            IopWriteHexLine("[IO] spawn.pid=", *OutProcessId);
        }
        if (OutEntryAddress != 0) {
            IopWriteHexLine("[IO] spawn.entry=", *OutEntryAddress);
        }
    }
    return Status;
}

void IoDumpFiles(void) {
    uint32_t Index;
    if (gIoReady == 0u) {
        return;
    }

    HalWriteDebugString("[IODBG] boot files begin\n");
    for (Index = 0u; Index < IO_MAX_BOOT_FILES; Index++) {
        if (gIoBootFiles[Index].InUse != 0u) {
            HalWriteDebugString("[IODBG] boot.path=");
            HalWriteDebugString(gIoBootFiles[Index].Path);
            HalWriteDebugString("\n");
            IopWriteHexLine("[IODBG] boot.size=", gIoBootFiles[Index].Size);
        }
    }

    HalWriteDebugString("[IODBG] open file objects begin\n");
    for (Index = 0u; Index < IO_MAX_FILE_OBJECTS; Index++) {
        if (gIoFileInUse[Index] != 0u) {
            IO_FILE_OBJECT *File = &gIoFilePool[Index];
            if (File->Header != 0) {
                IopWriteHexLine("[IODBG] file.id=", File->Header->ObjectId);
                HalWriteDebugString("[IODBG] file.name=");
                HalWriteDebugString(File->Header->Name);
                HalWriteDebugString("\n");
            }
            IopWriteHexLine("[IODBG] file.size=", File->Size);
            IopWriteHexLine("[IODBG] file.access=", File->AccessMask);
        }
    }
    HalWriteDebugString("[IODBG] open file objects end\n");
}
