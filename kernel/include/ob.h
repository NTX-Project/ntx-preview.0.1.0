#ifndef OB_H
#define OB_H

#include <stdint.h>

#include "kstatus.h"

#define OB_MAX_OBJECTS 256u
#define OB_MAX_HANDLES 512u
#define OB_MAX_TYPE_NAME 31u
#define OB_MAX_OBJECT_NAME 31u

#define OB_ACCESS_READ 0x00000001u
#define OB_ACCESS_WRITE 0x00000002u
#define OB_ACCESS_EXECUTE 0x00000004u
#define OB_ACCESS_DUPLICATE 0x00000008u
#define OB_ACCESS_ALL 0xFFFFFFFFu

#define OB_FLAG_WAITABLE 0x00000001u
#define OB_WAIT_STATE_CLEAR 0u
#define OB_WAIT_STATE_SIGNALED 1u

typedef struct OB_OBJECT_TYPE OB_OBJECT_TYPE;
typedef struct OB_OBJECT_HEADER OB_OBJECT_HEADER;

typedef void (*OB_DELETE_ROUTINE)(OB_OBJECT_HEADER *Object);

struct OB_OBJECT_TYPE {
    uint32_t TypeId;
    char Name[OB_MAX_TYPE_NAME + 1u];
    OB_DELETE_ROUTINE DeleteRoutine;
};

struct OB_OBJECT_HEADER {
    uint64_t ObjectId;
    const OB_OBJECT_TYPE *Type;
    uint32_t RefCount;
    uint32_t Flags;
    uint32_t DefaultAccessMask;
    uint32_t WaitState;
    char Name[OB_MAX_OBJECT_NAME + 1u];
};

typedef struct OB_HANDLE_TABLE_ENTRY {
    uint32_t HandleValue;
    OB_OBJECT_HEADER *Object;
    uint32_t GrantedAccess;
    uint32_t InUse;
} OB_HANDLE_TABLE_ENTRY;

typedef struct OB_HANDLE_TABLE {
    OB_HANDLE_TABLE_ENTRY Entries[OB_MAX_HANDLES];
    uint32_t NextHandle;
} OB_HANDLE_TABLE;

KSTATUS ObInitialize(void);
KSTATUS ObCreateType(const char *Name,
                     OB_DELETE_ROUTINE DeleteRoutine,
                     OB_OBJECT_TYPE **OutType);
KSTATUS ObCreateObject(const OB_OBJECT_TYPE *Type,
                       const char *Name,
                       uint32_t DefaultAccessMask,
                       uint32_t Flags,
                       OB_OBJECT_HEADER **OutObject);
KSTATUS ObInsertHandle(OB_HANDLE_TABLE *HandleTable,
                       OB_OBJECT_HEADER *Object,
                       uint32_t GrantedAccess,
                       uint32_t *OutHandle);
KSTATUS ObReferenceObjectByHandle(OB_HANDLE_TABLE *HandleTable,
                                  uint32_t HandleValue,
                                  uint32_t DesiredAccess,
                                  const OB_OBJECT_TYPE *ExpectedType,
                                  OB_OBJECT_HEADER **OutObject);
KSTATUS ObCloseHandle(OB_HANDLE_TABLE *HandleTable, uint32_t HandleValue);
KSTATUS ObDereferenceObject(OB_OBJECT_HEADER *Object);
OB_HANDLE_TABLE *ObGetKernelHandleTable(void);
void ObDumpObjects(void);
void ObDumpHandles(void);

#endif /* OB_H */
