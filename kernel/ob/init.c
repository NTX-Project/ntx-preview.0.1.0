#include "../../hal/inc/hal.h"
#include "../include/ob.h"

static OB_OBJECT_TYPE gObTypePool[OB_MAX_OBJECTS];
static OB_OBJECT_HEADER gObObjectPool[OB_MAX_OBJECTS];
static uint8_t gObTypeInUse[OB_MAX_OBJECTS];
static uint8_t gObObjectInUse[OB_MAX_OBJECTS];
static OB_HANDLE_TABLE gObKernelHandleTable;
static uint64_t gObNextObjectId;
static uint32_t gObReady;

static KSTATUS ObpCopyName(char *Destination, uint32_t DestinationBytes, const char *Source) {
    uint32_t Index;

    if (Destination == 0 || Source == 0 || DestinationBytes < 1u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < (DestinationBytes - 1u); Index++) {
        char Ch = Source[Index];
        Destination[Index] = Ch;
        if (Ch == 0) {
            return KSTATUS_SUCCESS;
        }
    }

    Destination[DestinationBytes - 1u] = 0;
    return KSTATUS_SUCCESS;
}

static void ObpZeroHandleTable(OB_HANDLE_TABLE *HandleTable) {
    uint32_t Index;

    for (Index = 0; Index < OB_MAX_HANDLES; Index++) {
        HandleTable->Entries[Index].HandleValue = 0u;
        HandleTable->Entries[Index].Object = 0;
        HandleTable->Entries[Index].GrantedAccess = 0u;
        HandleTable->Entries[Index].InUse = 0u;
    }
    HandleTable->NextHandle = 4u;
}

static KSTATUS ObpFindHandleEntry(OB_HANDLE_TABLE *HandleTable,
                                  uint32_t HandleValue,
                                  OB_HANDLE_TABLE_ENTRY **OutEntry) {
    uint32_t Index;

    if (HandleTable == 0 || OutEntry == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < OB_MAX_HANDLES; Index++) {
        OB_HANDLE_TABLE_ENTRY *Entry = &HandleTable->Entries[Index];
        if (Entry->InUse != 0u && Entry->HandleValue == HandleValue) {
            *OutEntry = Entry;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_NOT_FOUND;
}

static void ObpHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void ObpWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[128];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    ObpHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

KSTATUS ObInitialize(void) {
    uint32_t Index;

    if (gObReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }

    for (Index = 0; Index < OB_MAX_OBJECTS; Index++) {
        gObTypeInUse[Index] = 0u;
        gObObjectInUse[Index] = 0u;
    }

    ObpZeroHandleTable(&gObKernelHandleTable);
    gObNextObjectId = 1u;
    gObReady = 1u;

    HalWriteDebugString("[OB] object manager online (type/header/handle skeleton)\n");
    return KSTATUS_SUCCESS;
}

KSTATUS ObCreateType(const char *Name,
                     OB_DELETE_ROUTINE DeleteRoutine,
                     OB_OBJECT_TYPE **OutType) {
    uint32_t Index;
    KSTATUS Status;

    if (gObReady == 0u || Name == 0 || OutType == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < OB_MAX_OBJECTS; Index++) {
        if (gObTypeInUse[Index] == 0u) {
            gObTypeInUse[Index] = 1u;
            gObTypePool[Index].TypeId = Index + 1u;
            Status = ObpCopyName(gObTypePool[Index].Name, OB_MAX_TYPE_NAME + 1u, Name);
            if (Status != KSTATUS_SUCCESS) {
                gObTypeInUse[Index] = 0u;
                return Status;
            }
            gObTypePool[Index].DeleteRoutine = DeleteRoutine;
            *OutType = &gObTypePool[Index];
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

KSTATUS ObCreateObject(const OB_OBJECT_TYPE *Type,
                       const char *Name,
                       uint32_t DefaultAccessMask,
                       uint32_t Flags,
                       OB_OBJECT_HEADER **OutObject) {
    uint32_t Index;
    KSTATUS Status;

    if (gObReady == 0u || Type == 0 || Name == 0 || OutObject == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < OB_MAX_OBJECTS; Index++) {
        if (gObObjectInUse[Index] == 0u) {
            OB_OBJECT_HEADER *Object = &gObObjectPool[Index];

            gObObjectInUse[Index] = 1u;
            Object->ObjectId = gObNextObjectId++;
            Object->Type = Type;
            Object->RefCount = 1u;
            Object->Flags = Flags;
            Object->DefaultAccessMask = DefaultAccessMask;
            Object->WaitState = OB_WAIT_STATE_CLEAR;
            Status = ObpCopyName(Object->Name, OB_MAX_OBJECT_NAME + 1u, Name);
            if (Status != KSTATUS_SUCCESS) {
                gObObjectInUse[Index] = 0u;
                return Status;
            }
            *OutObject = Object;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

KSTATUS ObInsertHandle(OB_HANDLE_TABLE *HandleTable,
                       OB_OBJECT_HEADER *Object,
                       uint32_t GrantedAccess,
                       uint32_t *OutHandle) {
    uint32_t Index;

    if (gObReady == 0u || HandleTable == 0 || Object == 0 || OutHandle == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if ((GrantedAccess & Object->DefaultAccessMask) != GrantedAccess) {
        return KSTATUS_ACCESS_DENIED;
    }

    for (Index = 0; Index < OB_MAX_HANDLES; Index++) {
        OB_HANDLE_TABLE_ENTRY *Entry = &HandleTable->Entries[Index];
        if (Entry->InUse == 0u) {
            Entry->InUse = 1u;
            Entry->Object = Object;
            Entry->GrantedAccess = GrantedAccess;
            Entry->HandleValue = HandleTable->NextHandle;
            HandleTable->NextHandle += 4u;
            Object->RefCount++;
            *OutHandle = Entry->HandleValue;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

KSTATUS ObReferenceObjectByHandle(OB_HANDLE_TABLE *HandleTable,
                                  uint32_t HandleValue,
                                  uint32_t DesiredAccess,
                                  const OB_OBJECT_TYPE *ExpectedType,
                                  OB_OBJECT_HEADER **OutObject) {
    OB_HANDLE_TABLE_ENTRY *Entry;
    KSTATUS Status;

    if (gObReady == 0u || HandleTable == 0 || OutObject == 0 || HandleValue == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = ObpFindHandleEntry(HandleTable, HandleValue, &Entry);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    if ((Entry->GrantedAccess & DesiredAccess) != DesiredAccess) {
        return KSTATUS_ACCESS_DENIED;
    }

    if (ExpectedType != 0 && Entry->Object->Type != ExpectedType) {
        return KSTATUS_NOT_FOUND;
    }

    Entry->Object->RefCount++;
    *OutObject = Entry->Object;
    return KSTATUS_SUCCESS;
}

KSTATUS ObCloseHandle(OB_HANDLE_TABLE *HandleTable, uint32_t HandleValue) {
    OB_HANDLE_TABLE_ENTRY *Entry;
    KSTATUS Status;

    if (gObReady == 0u || HandleTable == 0 || HandleValue == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = ObpFindHandleEntry(HandleTable, HandleValue, &Entry);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = ObDereferenceObject(Entry->Object);
    Entry->InUse = 0u;
    Entry->HandleValue = 0u;
    Entry->Object = 0;
    Entry->GrantedAccess = 0u;
    return Status;
}

KSTATUS ObDereferenceObject(OB_OBJECT_HEADER *Object) {
    uint32_t Index;

    if (gObReady == 0u || Object == 0 || Object->RefCount == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Object->RefCount--;
    if (Object->RefCount != 0u) {
        return KSTATUS_SUCCESS;
    }

    if (Object->Type != 0 && Object->Type->DeleteRoutine != 0) {
        Object->Type->DeleteRoutine(Object);
    }

    for (Index = 0; Index < OB_MAX_OBJECTS; Index++) {
        if (&gObObjectPool[Index] == Object) {
            gObObjectInUse[Index] = 0u;
            Object->Type = 0;
            Object->Flags = 0u;
            Object->DefaultAccessMask = 0u;
            Object->WaitState = OB_WAIT_STATE_CLEAR;
            Object->Name[0] = 0;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_NOT_FOUND;
}

OB_HANDLE_TABLE *ObGetKernelHandleTable(void) {
    if (gObReady == 0u) {
        return 0;
    }
    return &gObKernelHandleTable;
}

void ObDumpObjects(void) {
    uint32_t Index;
    if (gObReady == 0u) {
        return;
    }

    HalWriteDebugString("[OBDBG] objects begin\n");
    for (Index = 0u; Index < OB_MAX_OBJECTS; Index++) {
        if (gObObjectInUse[Index] != 0u) {
            OB_OBJECT_HEADER *Object = &gObObjectPool[Index];
            HalWriteDebugString("[OBDBG] object name=");
            HalWriteDebugString(Object->Name);
            HalWriteDebugString(" type=");
            HalWriteDebugString((Object->Type != 0) ? Object->Type->Name : "<null>");
            HalWriteDebugString("\n");
            ObpWriteHexLine("[OBDBG] object.id=", Object->ObjectId);
            ObpWriteHexLine("[OBDBG] object.ref=", Object->RefCount);
            ObpWriteHexLine("[OBDBG] object.flags=", Object->Flags);
            ObpWriteHexLine("[OBDBG] object.default_access=", Object->DefaultAccessMask);
        }
    }
    HalWriteDebugString("[OBDBG] objects end\n");
}

void ObDumpHandles(void) {
    uint32_t Index;
    if (gObReady == 0u) {
        return;
    }

    HalWriteDebugString("[OBDBG] handles begin\n");
    for (Index = 0u; Index < OB_MAX_HANDLES; Index++) {
        OB_HANDLE_TABLE_ENTRY *Entry = &gObKernelHandleTable.Entries[Index];
        if (Entry->InUse != 0u) {
            ObpWriteHexLine("[OBDBG] handle.value=", Entry->HandleValue);
            ObpWriteHexLine("[OBDBG] handle.granted=", Entry->GrantedAccess);
            if (Entry->Object != 0) {
                HalWriteDebugString("[OBDBG] handle.object.name=");
                HalWriteDebugString(Entry->Object->Name);
                HalWriteDebugString("\n");
                HalWriteDebugString("[OBDBG] handle.object.type=");
                HalWriteDebugString((Entry->Object->Type != 0) ? Entry->Object->Type->Name : "<null>");
                HalWriteDebugString("\n");
                ObpWriteHexLine("[OBDBG] handle.object.id=", Entry->Object->ObjectId);
            }
        }
    }
    HalWriteDebugString("[OBDBG] handles end\n");
}
