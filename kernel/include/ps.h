#ifndef PS_H
#define PS_H

#include <stdint.h>

#include "../arch/aur64/aur64.h"
#include "kstatus.h"
#include "ob.h"

#define PS_MAX_PROCESSES 32u
#define PS_MAX_THREADS 128u
#define PS_DEFAULT_TIMESLICE_TICKS 4u

typedef enum PS_THREAD_STATE {
    PsThreadReady = 0,
    PsThreadRunning = 1,
    PsThreadBlocked = 2,
    PsThreadDead = 3
} PS_THREAD_STATE;

typedef struct KPROCESS KPROCESS;
typedef struct KTHREAD KTHREAD;

struct KPROCESS {
    OB_OBJECT_HEADER *ObjectHeader;
    uint64_t ProcessId;
    uint64_t AddressSpaceRoot;
};

struct KTHREAD {
    OB_OBJECT_HEADER *ObjectHeader;
    uint64_t ThreadId;
    KPROCESS *OwnerProcess;
    AUR64_CONTEXT Context;
    PS_THREAD_STATE State;
    uint32_t TimeSliceTicks;
    uint32_t RemainingTicks;
    KTHREAD *RunQueueNext;
    KTHREAD *WaitQueueNext;
    void (*StartRoutine)(void *Context);
    void *StartContext;
    uint64_t KernelStackTop;
    uint8_t IsUserThread;
    uint64_t UserEntry;
    uint64_t UserStackTop;
    uint64_t UserArg0;
    uint64_t UserArg1;
};

KSTATUS PsInitialize(void);
KSTATUS PsCreateKernelThread(KPROCESS *Process,
                             void (*StartRoutine)(void *Context),
                             void *StartContext,
                             KTHREAD **OutThread);
KSTATUS PsCreateProcess(const char *Name, KPROCESS **OutProcess);
KSTATUS PsSpawnUserKpeFromBuffer(const void *ImageBuffer,
                                 uint64_t ImageSize,
                                 uint64_t *OutProcessId,
                                 uint64_t *OutEntryAddress);
KSTATUS PsCreateUserThread(KPROCESS *Process,
                           uint64_t UserEntry,
                           uint64_t UserStackTop,
                           uint64_t UserArg0,
                           uint64_t UserArg1,
                           KTHREAD **OutThread);
KSTATUS PsReadyThread(KTHREAD *Thread);
KSTATUS PsDispatch(void);
KSTATUS PsYieldThread(void);
KSTATUS PsExitCurrentThread(uint64_t ExitCode);
KSTATUS PsBlockCurrentThread(void);
void PsWakeThread(KTHREAD *Thread);
void PsOnTimerTick(void);
void PsHandlePreemptionPoint(void);
KTHREAD *PsGetCurrentThread(void);
KPROCESS *PsGetCurrentProcess(void);
void PsDumpSchedulerState(void);

#endif /* PS_H */
