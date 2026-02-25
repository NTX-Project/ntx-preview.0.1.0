#include "../../hal/inc/hal.h"
#include "../include/nk.h"

static const BOOT_INFO *gNkBootInfo;
static uint32_t gNkPortCount;
static uint32_t gNkServiceEndpointCount;

KSTATUS NkInitializeCore(const BOOT_INFO *BootInfo) {
    if (BootInfo == 0 || BootInfo->Magic != BOOTINFO_MAGIC) {
        return KSTATUS_INVALID_PARAMETER;
    }

    gNkBootInfo = BootInfo;
    gNkPortCount = 0;
    gNkServiceEndpointCount = 0;
    HalWriteDebugString("[NK] nucleus core online (trap/sched/ipc baseline)\n");
    (void)gNkBootInfo;
    return KSTATUS_SUCCESS;
}

const BOOT_INFO *NkGetBootInfo(void) {
    return gNkBootInfo;
}

KSTATUS NkPortInitialize(NK_PORT *Port) {
    uint32_t Index;

    if (Port == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < NK_PORT_QUEUE_DEPTH; Index++) {
        Port->Queue[Index].Opcode = 0;
        Port->Queue[Index].Length = 0;
    }
    Port->Head = 0;
    Port->Tail = 0;
    Port->Count = 0;
    gNkPortCount++;
    return KSTATUS_SUCCESS;
}

KSTATUS NkPortSend(NK_PORT *Port, const NK_MESSAGE *Message) {
    if (Port == 0 || Message == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (Message->Length > NK_MESSAGE_PAYLOAD_BYTES) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (Port->Count >= NK_PORT_QUEUE_DEPTH) {
        return KSTATUS_INIT_FAILED;
    }

    Port->Queue[Port->Tail] = *Message;
    Port->Tail = (Port->Tail + 1) % NK_PORT_QUEUE_DEPTH;
    Port->Count++;
    return KSTATUS_SUCCESS;
}

KSTATUS NkPortReceive(NK_PORT *Port, NK_MESSAGE *Message) {
    if (Port == 0 || Message == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (Port->Count == 0) {
        return KSTATUS_INIT_FAILED;
    }

    *Message = Port->Queue[Port->Head];
    Port->Head = (Port->Head + 1) % NK_PORT_QUEUE_DEPTH;
    Port->Count--;
    return KSTATUS_SUCCESS;
}

KSTATUS NkServiceEndpointInitialize(NK_SERVICE_ENDPOINT *Endpoint,
                                    const char *Name,
                                    uint32_t BaseOpcode,
                                    uint32_t Flags) {
    uint32_t Index;
    KSTATUS Status;

    if (Endpoint == 0 || Name == 0 || BaseOpcode == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index <= NK_SERVICE_NAME_MAX; Index++) {
        char Ch = Name[Index];
        Endpoint->Name[Index] = Ch;
        if (Ch == 0) {
            break;
        }
    }

    if (Index > NK_SERVICE_NAME_MAX) {
        Endpoint->Name[NK_SERVICE_NAME_MAX] = 0;
    }

    Endpoint->BaseOpcode = BaseOpcode;
    Endpoint->Flags = Flags;

    Status = NkPortInitialize(&Endpoint->Port);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    gNkServiceEndpointCount++;
    return KSTATUS_SUCCESS;
}

KSTATUS NkServiceSend(NK_SERVICE_ENDPOINT *Endpoint, const NK_MESSAGE *Message) {
    if (Endpoint == 0 || Message == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    return NkPortSend(&Endpoint->Port, Message);
}

KSTATUS NkServiceReceive(NK_SERVICE_ENDPOINT *Endpoint, NK_MESSAGE *Message) {
    if (Endpoint == 0 || Message == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    return NkPortReceive(&Endpoint->Port, Message);
}
