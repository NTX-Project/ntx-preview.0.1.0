#ifndef LPC_H
#define LPC_H

#include "kstatus.h"
#include "nk.h"

#define LPC_OPCODE_BASE 0x1100u
#define LPC_OPCODE_BIND_ENDPOINT (LPC_OPCODE_BASE + 1u)
#define LPC_OPCODE_CONNECT_PORT (LPC_OPCODE_BASE + 2u)
#define LPC_PORT_ACCESS_SEND 0x00000002u
#define LPC_PORT_ACCESS_RECV 0x00000001u
#define LPC_PORT_ACCESS_ALL (LPC_PORT_ACCESS_SEND | LPC_PORT_ACCESS_RECV)

KSTATUS LpcInitialize(void);
KSTATUS LpcGetSystemEndpoint(NK_SERVICE_ENDPOINT **OutEndpoint);
KSTATUS LpcCreatePortHandle(uint32_t DesiredAccess, uint32_t *OutHandle);
KSTATUS LpcSendByHandle(uint32_t HandleValue, const NK_MESSAGE *Message, uint32_t Wait);
KSTATUS LpcReceiveByHandle(uint32_t HandleValue, NK_MESSAGE *OutMessage, uint32_t Wait);
KSTATUS LpcCloseHandle(uint32_t HandleValue);
void LpcDumpState(void);

#endif /* LPC_H */
