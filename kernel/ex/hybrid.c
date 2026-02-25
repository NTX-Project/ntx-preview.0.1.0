#include "../../hal/inc/hal.h"
#include "../include/kstatus.h"
#include "../include/lpc.h"
#include "../include/nk.h"

static NK_SERVICE_ENDPOINT gExIpcServiceEndpoint;

KSTATUS ExInitializeHybridServices(void) {
    NK_SERVICE_ENDPOINT *LpcEndpoint;
    NK_MESSAGE Message;
    KSTATUS Status;

    Status = NkServiceEndpointInitialize(&gExIpcServiceEndpoint, "ex.control", 0x2000u, 0u);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = LpcGetSystemEndpoint(&LpcEndpoint);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Message.Opcode = LPC_OPCODE_BIND_ENDPOINT;
    Message.Length = 4u;
    Message.Payload[0] = 'E';
    Message.Payload[1] = 'X';
    Message.Payload[2] = 'C';
    Message.Payload[3] = 'T';

    Status = NkServiceSend(LpcEndpoint, &Message);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    HalWriteDebugString("[EX] hybrid service endpoint online (microkernel-style IPC edge)\n");
    return KSTATUS_SUCCESS;
}
