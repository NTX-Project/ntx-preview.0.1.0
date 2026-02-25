#include "../../hal/inc/hal.h"
#include "../include/lpc.h"
#include "../include/ob.h"
#include "../include/ps.h"

#define LPC_MAX_PORT_OBJECTS 128u

typedef struct LPC_PORT_OBJECT {
    OB_OBJECT_HEADER *Header;
    NK_PORT Port;
    KTHREAD *WaitHead;
    KTHREAD *WaitTail;
} LPC_PORT_OBJECT;

static NK_SERVICE_ENDPOINT gLpcSystemEndpoint;
static OB_OBJECT_TYPE *gLpcPortType;
static LPC_PORT_OBJECT gLpcPortPool[LPC_MAX_PORT_OBJECTS];
static uint8_t gLpcPortInUse[LPC_MAX_PORT_OBJECTS];
static uint32_t gLpcReady;

static void LpcpHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void LpcpWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[128];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    LpcpHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static LPC_PORT_OBJECT *LpcpFindPortByHeader(OB_OBJECT_HEADER *Header) {
    uint32_t Index;
    for (Index = 0u; Index < LPC_MAX_PORT_OBJECTS; Index++) {
        if (gLpcPortInUse[Index] != 0u && gLpcPortPool[Index].Header == Header) {
            return &gLpcPortPool[Index];
        }
    }
    return 0;
}

static void LpcpDeletePortObject(OB_OBJECT_HEADER *Object) {
    uint32_t Index;
    for (Index = 0u; Index < LPC_MAX_PORT_OBJECTS; Index++) {
        if (gLpcPortInUse[Index] != 0u && gLpcPortPool[Index].Header == Object) {
            gLpcPortInUse[Index] = 0u;
            gLpcPortPool[Index].Header = 0;
            gLpcPortPool[Index].WaitHead = 0;
            gLpcPortPool[Index].WaitTail = 0;
            return;
        }
    }
}

static LPC_PORT_OBJECT *LpcpAllocatePortSlot(void) {
    uint32_t Index;
    for (Index = 0u; Index < LPC_MAX_PORT_OBJECTS; Index++) {
        if (gLpcPortInUse[Index] == 0u) {
            gLpcPortInUse[Index] = 1u;
            gLpcPortPool[Index].Header = 0;
            gLpcPortPool[Index].WaitHead = 0;
            gLpcPortPool[Index].WaitTail = 0;
            return &gLpcPortPool[Index];
        }
    }
    return 0;
}

static void LpcpEnqueueWaiter(LPC_PORT_OBJECT *Port, KTHREAD *Thread) {
    if (Port == 0 || Thread == 0) {
        return;
    }

    Thread->WaitQueueNext = 0;
    if (Port->WaitTail == 0) {
        Port->WaitHead = Thread;
        Port->WaitTail = Thread;
    } else {
        Port->WaitTail->WaitQueueNext = Thread;
        Port->WaitTail = Thread;
    }
}

static KTHREAD *LpcpDequeueWaiter(LPC_PORT_OBJECT *Port) {
    KTHREAD *Thread;
    if (Port == 0) {
        return 0;
    }

    Thread = Port->WaitHead;
    if (Thread == 0) {
        return 0;
    }

    Port->WaitHead = Thread->WaitQueueNext;
    if (Port->WaitHead == 0) {
        Port->WaitTail = 0;
    }
    Thread->WaitQueueNext = 0;
    return Thread;
}

static KSTATUS LpcpReferencePortByHandle(uint32_t HandleValue,
                                         uint32_t DesiredAccess,
                                         OB_OBJECT_HEADER **OutHeader,
                                         LPC_PORT_OBJECT **OutPort) {
    OB_HANDLE_TABLE *HandleTable;
    OB_OBJECT_HEADER *Header;
    LPC_PORT_OBJECT *Port;
    KSTATUS Status;

    if (OutHeader == 0 || OutPort == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HandleTable = ObGetKernelHandleTable();
    if (HandleTable == 0 || gLpcPortType == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = ObReferenceObjectByHandle(HandleTable,
                                       HandleValue,
                                       DesiredAccess,
                                       gLpcPortType,
                                       &Header);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Port = LpcpFindPortByHeader(Header);
    if (Port == 0) {
        (void)ObDereferenceObject(Header);
        return KSTATUS_NOT_FOUND;
    }

    *OutHeader = Header;
    *OutPort = Port;
    return KSTATUS_SUCCESS;
}

KSTATUS LpcInitialize(void) {
    KSTATUS Status;
    uint32_t Index;

    if (gLpcReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }

    for (Index = 0u; Index < LPC_MAX_PORT_OBJECTS; Index++) {
        gLpcPortInUse[Index] = 0u;
    }

    Status = NkServiceEndpointInitialize(&gLpcSystemEndpoint, "lpc.system", LPC_OPCODE_BASE, 0u);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = ObCreateType("LpcPort", LpcpDeletePortObject, &gLpcPortType);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    gLpcReady = 1u;
    HalWriteDebugString("[LPC] local port manager online\n");
    return KSTATUS_SUCCESS;
}

KSTATUS LpcGetSystemEndpoint(NK_SERVICE_ENDPOINT **OutEndpoint) {
    if (OutEndpoint == 0 || gLpcReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    *OutEndpoint = &gLpcSystemEndpoint;
    return KSTATUS_SUCCESS;
}

KSTATUS LpcCreatePortHandle(uint32_t DesiredAccess, uint32_t *OutHandle) {
    LPC_PORT_OBJECT *Port;
    OB_OBJECT_HEADER *Header;
    OB_HANDLE_TABLE *HandleTable;
    KSTATUS Status;

    if (gLpcReady == 0u || OutHandle == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Port = LpcpAllocatePortSlot();
    if (Port == 0) {
        return KSTATUS_INIT_FAILED;
    }

    Status = ObCreateObject(gLpcPortType,
                            "lpc.port",
                            LPC_PORT_ACCESS_ALL,
                            OB_FLAG_WAITABLE,
                            &Header);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = NkPortInitialize(&Port->Port);
    if (Status != KSTATUS_SUCCESS) {
        (void)ObDereferenceObject(Header);
        return Status;
    }

    Port->Header = Header;
    HandleTable = ObGetKernelHandleTable();
    Status = ObInsertHandle(HandleTable, Header, DesiredAccess, OutHandle);
    (void)ObDereferenceObject(Header);
    return Status;
}

KSTATUS LpcSendByHandle(uint32_t HandleValue, const NK_MESSAGE *Message, uint32_t Wait) {
    OB_OBJECT_HEADER *Header;
    LPC_PORT_OBJECT *Port;
    KTHREAD *Waiter;
    KSTATUS Status;

    (void)Wait;

    if (gLpcReady == 0u || Message == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = LpcpReferencePortByHandle(HandleValue, LPC_PORT_ACCESS_SEND, &Header, &Port);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    HalDisableInterrupts();
    Status = NkPortSend(&Port->Port, Message);
    if (Status == KSTATUS_SUCCESS) {
        Waiter = LpcpDequeueWaiter(Port);
        if (Waiter != 0) {
            PsWakeThread(Waiter);
        }
        HalEnableInterrupts();
        (void)ObDereferenceObject(Header);
        return KSTATUS_SUCCESS;
    }

    HalEnableInterrupts();
    (void)ObDereferenceObject(Header);
    return KSTATUS_WOULD_BLOCK;
}

KSTATUS LpcReceiveByHandle(uint32_t HandleValue, NK_MESSAGE *OutMessage, uint32_t Wait) {
    KTHREAD *Current;

    if (gLpcReady == 0u || OutMessage == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Current = PsGetCurrentThread();
    if (Current == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (;;) {
        OB_OBJECT_HEADER *Header;
        LPC_PORT_OBJECT *Port;
        KSTATUS Status;

        Status = LpcpReferencePortByHandle(HandleValue, LPC_PORT_ACCESS_RECV, &Header, &Port);
        if (Status != KSTATUS_SUCCESS) {
            return Status;
        }

        HalDisableInterrupts();
        Status = NkPortReceive(&Port->Port, OutMessage);
        if (Status == KSTATUS_SUCCESS) {
            HalEnableInterrupts();
            (void)ObDereferenceObject(Header);
            return KSTATUS_SUCCESS;
        }

        if (Wait == 0u) {
            HalEnableInterrupts();
            (void)ObDereferenceObject(Header);
            return KSTATUS_WOULD_BLOCK;
        }

        LpcpEnqueueWaiter(Port, Current);
        HalEnableInterrupts();
        (void)ObDereferenceObject(Header);

        Status = PsBlockCurrentThread();
        if (Status != KSTATUS_SUCCESS) {
            return Status;
        }
    }
}

KSTATUS LpcCloseHandle(uint32_t HandleValue) {
    OB_HANDLE_TABLE *HandleTable;

    HandleTable = ObGetKernelHandleTable();
    if (HandleTable == 0 || HandleValue == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    return ObCloseHandle(HandleTable, HandleValue);
}

void LpcDumpState(void) {
    uint32_t Index;
    if (gLpcReady == 0u) {
        return;
    }

    HalWriteDebugString("[LPCDBG] ports begin\n");
    for (Index = 0u; Index < LPC_MAX_PORT_OBJECTS; Index++) {
        if (gLpcPortInUse[Index] != 0u) {
            LPC_PORT_OBJECT *Port = &gLpcPortPool[Index];
            HalWriteDebugString("[LPCDBG] port.name=");
            if (Port->Header != 0) {
                HalWriteDebugString(Port->Header->Name);
                HalWriteDebugString("\n");
                LpcpWriteHexLine("[LPCDBG] port.id=", Port->Header->ObjectId);
            } else {
                HalWriteDebugString("<null>\n");
            }
            LpcpWriteHexLine("[LPCDBG] queue.count=", Port->Port.Count);
            LpcpWriteHexLine("[LPCDBG] queue.head=", Port->Port.Head);
            LpcpWriteHexLine("[LPCDBG] queue.tail=", Port->Port.Tail);
        }
    }
    HalWriteDebugString("[LPCDBG] ports end\n");
}
