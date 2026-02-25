#include "../../hal/inc/hal.h"
#include "../../boot/include/bootproto.h"
#include "../include/kiarch.h"
#include "../include/kstatus.h"
#include "../include/lpc.h"
#include "../include/mm.h"
#include "../include/nk.h"
#include "../include/nkassert.h"
#include "../include/ps.h"
#include "../include/syscall.h"

typedef struct KE_VALIDATION_COUNTERS {
    volatile uint32_t WorkerAIterations;
    volatile uint32_t WorkerBIterations;
    volatile uint32_t IpcIterations;
} KE_VALIDATION_COUNTERS;

static KE_VALIDATION_COUNTERS gKeValidationCounters;

typedef KSTATUS (*KE_TEST_ROUTINE)(void);

typedef struct KE_TEST_CASE {
    const char *Name;
    KE_TEST_ROUTINE Routine;
} KE_TEST_CASE;

typedef struct KE_IPC_WORKER_CONTEXT {
    uint32_t SendHandle;
    uint32_t RecvHandle;
    uint32_t Iterations;
    uint32_t RoleId;
    volatile uint32_t *OutCompleted;
} KE_IPC_WORKER_CONTEXT;

static uint64_t KeInvokeSyscall(uint64_t Number,
                                uint64_t Arg0,
                                uint64_t Arg1,
                                uint64_t Arg2,
                                uint64_t Arg3,
                                uint64_t Arg4,
                                uint64_t Arg5) {
    register uint64_t R10 __asm__("r10") = Arg3;
    register uint64_t R8 __asm__("r8") = Arg4;
    register uint64_t R9 __asm__("r9") = Arg5;
    __asm__ __volatile__(
        "int $0x80"
        : "+a"(Number)
        : "D"(Arg0), "S"(Arg1), "d"(Arg2), "r"(R10), "r"(R8), "r"(R9)
        : "rcx", "r11", "memory");
    return Number;
}

static KSTATUS KeSysDebugPrint(const char *Text) {
    uint64_t Length = 0u;
    const char *Cursor = Text;

    if (Text == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    while (*Cursor != 0) {
        Length++;
        Cursor++;
    }

    return (KSTATUS)(int64_t)KeInvokeSyscall(NkSysDebugPrint,
                                             (uint64_t)(uintptr_t)Text,
                                             Length,
                                             0u,
                                             0u,
                                             0u,
                                             0u);
}

static void KeStoreU32(uint8_t *Buffer, uint32_t Value) {
    Buffer[0] = (uint8_t)(Value & 0xFFu);
    Buffer[1] = (uint8_t)((Value >> 8) & 0xFFu);
    Buffer[2] = (uint8_t)((Value >> 16) & 0xFFu);
    Buffer[3] = (uint8_t)((Value >> 24) & 0xFFu);
}

static uint32_t KeLoadU32(const uint8_t *Buffer) {
    return ((uint32_t)Buffer[0]) |
           (((uint32_t)Buffer[1]) << 8) |
           (((uint32_t)Buffer[2]) << 16) |
           (((uint32_t)Buffer[3]) << 24);
}

static uint32_t KeShouldRunValidationMode(void) {
#if defined(NK_VALIDATE) && (NK_VALIDATE != 0)
    return 1u;
#else
    const BOOT_INFO *BootInfo = NkGetBootInfo();
    if (BootInfo != 0 && (BootInfo->Flags & BOOTINFO_FLAG_VALIDATE) != 0u) {
        return 1u;
    }
    return 0u;
#endif
}

static KSTATUS KeTestSyscall(void) {
    KSTATUS Status = KeSysDebugPrint("[KEVT] test syscall\n");
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    return (KSTATUS)(int64_t)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
}

static void KeValidationWorkerA(void *Context) {
    uint32_t Iteration;
    (void)Context;
    for (Iteration = 0u; Iteration < 4u; Iteration++) {
        HalWriteDebugString("[KEVT] worker A iteration\n");
        gKeValidationCounters.WorkerAIterations++;
        (void)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
    }
    HalWriteDebugString("[KEVT] worker A complete\n");
}

static void KeValidationWorkerB(void *Context) {
    uint32_t Iteration;
    (void)Context;
    for (Iteration = 0u; Iteration < 4u; Iteration++) {
        HalWriteDebugString("[KEVT] worker B iteration\n");
        gKeValidationCounters.WorkerBIterations++;
        (void)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
    }
    HalWriteDebugString("[KEVT] worker B complete\n");
}

static KSTATUS KeTestScheduler(void) {
    KPROCESS *CurrentProcess = PsGetCurrentProcess();
    KTHREAD *WorkerA;
    KTHREAD *WorkerB;
    KSTATUS Status;
    uint32_t Pass;

    if (CurrentProcess == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = PsCreateKernelThread(CurrentProcess, KeValidationWorkerA, 0, &WorkerA);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsCreateKernelThread(CurrentProcess, KeValidationWorkerB, 0, &WorkerB);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsReadyThread(WorkerA);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsReadyThread(WorkerB);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    for (Pass = 0u; Pass < 24u; Pass++) {
        HalTriggerSyntheticTimerTick();
        PsHandlePreemptionPoint();
        (void)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
    }

    if (gKeValidationCounters.WorkerAIterations == 0u ||
        gKeValidationCounters.WorkerBIterations == 0u) {
        return KSTATUS_INIT_FAILED;
    }

    return KSTATUS_SUCCESS;
}

static KSTATUS KeTestVmLazy(void) {
    KPROCESS *CurrentProcess = PsGetCurrentProcess();
    MM_ADDRESS_SPACE *AddressSpace;
    uint64_t MappingBase;
    volatile uint8_t *Bytes;

    if (CurrentProcess == 0 || CurrentProcess->AddressSpaceRoot == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    MappingBase = KeInvokeSyscall(NkSysMapMemory, MM_PAGE_SIZE, 0u, 0u, 0u, 0u, 0u);
    if ((int64_t)MappingBase < 0) {
        return (KSTATUS)(int64_t)MappingBase;
    }

    HalWriteDebugString("[KEVT] touching lazy-mapped page\n");
    Bytes = (volatile uint8_t *)(uintptr_t)MappingBase;
    Bytes[0] = 0x5Au;
    Bytes[MM_PAGE_SIZE - 1u] = 0xA5u;

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)CurrentProcess->AddressSpaceRoot;
    MmDumpAddressTranslation(AddressSpace, MappingBase);
    MmDumpRecentFaults();
    return KSTATUS_SUCCESS;
}

static KSTATUS KeTestVmSection(void) {
    uint32_t SectionHandle = 0u;
    uint64_t VaA = 0u;
    uint64_t VaB = 0u;
    volatile uint8_t *A;
    volatile uint8_t *B;
    uint64_t Result;

    Result = KeInvokeSyscall(NkSysCreateSection,
                             MM_PAGE_SIZE,
                             MM_SECTION_ACCESS_ALL,
                             (uint64_t)(uintptr_t)&SectionHandle,
                             0u,
                             0u,
                             0u);
    if ((int64_t)Result < 0) {
        return (KSTATUS)(int64_t)Result;
    }

    Result = KeInvokeSyscall(NkSysMapSection,
                             SectionHandle,
                             MM_PROT_READ | MM_PROT_WRITE,
                             (uint64_t)(uintptr_t)&VaA,
                             0u,
                             0u,
                             0u);
    if ((int64_t)Result < 0) {
        (void)KeInvokeSyscall(NkSysCloseHandle, SectionHandle, 0u, 0u, 0u, 0u, 0u);
        return (KSTATUS)(int64_t)Result;
    }

    Result = KeInvokeSyscall(NkSysMapSection,
                             SectionHandle,
                             MM_PROT_READ | MM_PROT_WRITE,
                             (uint64_t)(uintptr_t)&VaB,
                             0u,
                             0u,
                             0u);
    if ((int64_t)Result < 0) {
        (void)KeInvokeSyscall(NkSysCloseHandle, SectionHandle, 0u, 0u, 0u, 0u, 0u);
        return (KSTATUS)(int64_t)Result;
    }

    A = (volatile uint8_t *)(uintptr_t)VaA;
    B = (volatile uint8_t *)(uintptr_t)VaB;

    A[0] = 0x34u;
    A[1] = 0x12u;
    if (B[0] != 0x34u || B[1] != 0x12u) {
        (void)KeInvokeSyscall(NkSysCloseHandle, SectionHandle, 0u, 0u, 0u, 0u, 0u);
        return KSTATUS_INIT_FAILED;
    }

    B[MM_PAGE_SIZE - 1u] = 0xA7u;
    if (A[MM_PAGE_SIZE - 1u] != 0xA7u) {
        (void)KeInvokeSyscall(NkSysCloseHandle, SectionHandle, 0u, 0u, 0u, 0u, 0u);
        return KSTATUS_INIT_FAILED;
    }

    (void)KeInvokeSyscall(NkSysCloseHandle, SectionHandle, 0u, 0u, 0u, 0u, 0u);
    return KSTATUS_SUCCESS;
}

static KSTATUS KeTestUserModeSmoke(void) {
    return KiArchUserModeSmokeTest();
}

static void KeIpcWorkerThread(void *Context) {
    KE_IPC_WORKER_CONTEXT *Worker = (KE_IPC_WORKER_CONTEXT *)Context;
    uint32_t Index;

    for (Index = 0u; Index < Worker->Iterations; Index++) {
        uint8_t SendPayload[4];
        uint8_t RecvPayload[4];
        uint64_t Result;
        uint32_t Value;

        Value = (Worker->RoleId << 24) | Index;
        KeStoreU32(SendPayload, Value);

        Result = KeInvokeSyscall(NkSysSend,
                                 Worker->SendHandle,
                                 (uint64_t)(uintptr_t)SendPayload,
                                 4u,
                                 1u,
                                 0u,
                                 0u);
        if ((int64_t)Result < 0) {
            return;
        }

        Result = KeInvokeSyscall(NkSysReceive,
                                 Worker->RecvHandle,
                                 (uint64_t)(uintptr_t)RecvPayload,
                                 4u,
                                 1u,
                                 0u,
                                 0u);
        if ((int64_t)Result < 0 || Result != 4u) {
            return;
        }

        Value = KeLoadU32(RecvPayload);
        (void)Value;

        gKeValidationCounters.IpcIterations++;
        if ((HalReadTimestampCounter() & 1u) != 0u) {
            (void)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
        }
    }

    *Worker->OutCompleted = 1u;
}

static KSTATUS KeTestIpcPingPong(void) {
    KPROCESS *Process = PsGetCurrentProcess();
    KTHREAD *WorkerA;
    KTHREAD *WorkerB;
    KE_IPC_WORKER_CONTEXT CtxA;
    KE_IPC_WORKER_CONTEXT CtxB;
    volatile uint32_t DoneA = 0u;
    volatile uint32_t DoneB = 0u;
    uint64_t PortA;
    uint64_t PortB;
    uint32_t Loops;
    KSTATUS Status;

    if (Process == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    PortA = KeInvokeSyscall(NkSysCreatePort, LPC_PORT_ACCESS_ALL, 0u, 0u, 0u, 0u, 0u);
    if ((int64_t)PortA < 0) {
        return (KSTATUS)(int64_t)PortA;
    }

    PortB = KeInvokeSyscall(NkSysCreatePort, LPC_PORT_ACCESS_ALL, 0u, 0u, 0u, 0u, 0u);
    if ((int64_t)PortB < 0) {
        (void)KeInvokeSyscall(NkSysCloseHandle, PortA, 0u, 0u, 0u, 0u, 0u);
        return (KSTATUS)(int64_t)PortB;
    }

    CtxA.SendHandle = (uint32_t)PortA;
    CtxA.RecvHandle = (uint32_t)PortB;
    CtxA.Iterations = 1024u;
    CtxA.RoleId = 1u;
    CtxA.OutCompleted = &DoneA;

    CtxB.SendHandle = (uint32_t)PortB;
    CtxB.RecvHandle = (uint32_t)PortA;
    CtxB.Iterations = 1024u;
    CtxB.RoleId = 2u;
    CtxB.OutCompleted = &DoneB;

    Status = PsCreateKernelThread(Process, KeIpcWorkerThread, &CtxA, &WorkerA);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsCreateKernelThread(Process, KeIpcWorkerThread, &CtxB, &WorkerB);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsReadyThread(WorkerA);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsReadyThread(WorkerB);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    for (Loops = 0u; Loops < 20000u; Loops++) {
        HalTriggerSyntheticTimerTick();
        PsHandlePreemptionPoint();
        (void)KeInvokeSyscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
        if (DoneA != 0u && DoneB != 0u) {
            break;
        }
    }

    (void)KeInvokeSyscall(NkSysCloseHandle, PortA, 0u, 0u, 0u, 0u, 0u);
    (void)KeInvokeSyscall(NkSysCloseHandle, PortB, 0u, 0u, 0u, 0u, 0u);

    if (DoneA == 0u || DoneB == 0u) {
        return KSTATUS_INIT_FAILED;
    }

    return KSTATUS_SUCCESS;
}

KSTATUS KeRunValidationSuite(void) {
    static const KE_TEST_CASE Tests[] = {
        { "syscall", KeTestSyscall },
        { "scheduler", KeTestScheduler },
        { "vm_lazy", KeTestVmLazy },
        { "vm_section", KeTestVmSection },
        { "user_smoke", KeTestUserModeSmoke },
        { "ipc_pingpong", KeTestIpcPingPong }
    };
    uint32_t Index;
    KSTATUS Status;

    if (KeShouldRunValidationMode() == 0u) {
        return KSTATUS_SUCCESS;
    }

    gKeValidationCounters.WorkerAIterations = 0u;
    gKeValidationCounters.WorkerBIterations = 0u;
    gKeValidationCounters.IpcIterations = 0u;

    HalWriteDebugString("[KEVT] validation mode enabled\n");

    for (Index = 0u; Index < (sizeof(Tests) / sizeof(Tests[0])); Index++) {
        HalWriteDebugString("[KEVT] run ");
        HalWriteDebugString(Tests[Index].Name);
        HalWriteDebugString("\n");

        Status = Tests[Index].Routine();
        if (Status == KSTATUS_NOT_IMPLEMENTED) {
            HalWriteDebugString("[KEVT] skip ");
            HalWriteDebugString(Tests[Index].Name);
            HalWriteDebugString("\n");
            continue;
        }

        NK_ASSERT(Status == KSTATUS_SUCCESS);
        if (Status != KSTATUS_SUCCESS) {
            HalWriteDebugString("[KEVT] fail ");
            HalWriteDebugString(Tests[Index].Name);
            HalWriteDebugString("\n");
            return Status;
        }

        HalWriteDebugString("[KEVT] pass ");
        HalWriteDebugString(Tests[Index].Name);
        HalWriteDebugString("\n");
    }

    return KeSysDebugPrint("[KEVT] validation suite complete\n");
}
