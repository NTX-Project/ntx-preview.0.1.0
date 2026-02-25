#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

typedef enum NK_SYSCALL_NUMBER {
    NkSysDebugPrint = 0,
    NkSysYield = 1,
    NkSysSend = 2,
    NkSysReceive = 3,
    NkSysMapMemory = 4,
    NkSysExitThread = 5,
    NkSysCreatePort = 6,
    NkSysCloseHandle = 7,
    NkSysCreateSection = 8,
    NkSysMapSection = 9,
    NkSysCreateProcessFromBuffer = 10,
    NkSysOpen = 11,
    NkSysRead = 12,
    NkSysClose = 13,
    NkSysCreateProcessFromFileHandle = 14,
    NkSysReadConsole = 15
} NK_SYSCALL_NUMBER;

uint64_t KiDispatchSystemCall(uint64_t Number,
                              uint64_t Arg0,
                              uint64_t Arg1,
                              uint64_t Arg2,
                              uint64_t Arg3,
                              uint64_t Arg4,
                              uint64_t Arg5);

#endif /* SYSCALL_H */
