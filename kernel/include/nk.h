#ifndef NK_H
#define NK_H

#include <stdint.h>

#include "../../boot/include/bootproto.h"
#include "kstatus.h"

#define NK_PORT_QUEUE_DEPTH 32u
#define NK_MESSAGE_PAYLOAD_BYTES 64u
#define NK_SERVICE_NAME_MAX 31u

typedef struct NK_MESSAGE {
    uint32_t Opcode;
    uint32_t Length;
    uint8_t Payload[NK_MESSAGE_PAYLOAD_BYTES];
} NK_MESSAGE;

typedef struct NK_PORT {
    NK_MESSAGE Queue[NK_PORT_QUEUE_DEPTH];
    uint32_t Head;
    uint32_t Tail;
    uint32_t Count;
} NK_PORT;

typedef struct NK_SERVICE_ENDPOINT {
    char Name[NK_SERVICE_NAME_MAX + 1u];
    uint32_t BaseOpcode;
    uint32_t Flags;
    NK_PORT Port;
} NK_SERVICE_ENDPOINT;

KSTATUS NkInitializeCore(const BOOT_INFO *BootInfo);
const BOOT_INFO *NkGetBootInfo(void);
KSTATUS NkPortInitialize(NK_PORT *Port);
KSTATUS NkPortSend(NK_PORT *Port, const NK_MESSAGE *Message);
KSTATUS NkPortReceive(NK_PORT *Port, NK_MESSAGE *Message);
KSTATUS NkServiceEndpointInitialize(NK_SERVICE_ENDPOINT *Endpoint,
                                    const char *Name,
                                    uint32_t BaseOpcode,
                                    uint32_t Flags);
KSTATUS NkServiceSend(NK_SERVICE_ENDPOINT *Endpoint, const NK_MESSAGE *Message);
KSTATUS NkServiceReceive(NK_SERVICE_ENDPOINT *Endpoint, NK_MESSAGE *Message);

#endif /* NK_H */
