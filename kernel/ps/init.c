#include "../../hal/inc/hal.h"
#include "../../include/kpeformat.h"
#include "../include/kiarch.h"
#include "../include/mm.h"
#include "../include/nkassert.h"
#include "../include/ps.h"

#define PS_USER_TOP 0x0000800000000000ull
#define PS_PTE_PRESENT 0x001ull
#define PS_PTE_WRITABLE 0x002ull
#define PS_PTE_USER 0x004ull
#define PS_PTE_NX (1ull << 63)
#define PS_IMAGE_MAX_SIZE (16u * 1024u * 1024u)
#define PS_USER_STACK_SIZE (16u * MM_PAGE_SIZE)

static OB_OBJECT_TYPE *gPsProcessType;
static OB_OBJECT_TYPE *gPsThreadType;

static KPROCESS gPsProcessPool[PS_MAX_PROCESSES];
static uint8_t gPsProcessInUse[PS_MAX_PROCESSES];
static KTHREAD gPsThreadPool[PS_MAX_THREADS];
static uint8_t gPsThreadInUse[PS_MAX_THREADS];
static uint8_t gPsThreadStackPool[PS_MAX_THREADS][16384];

static uint64_t gPsNextProcessId;
static uint64_t gPsNextThreadId;
static KPROCESS *gPsSystemProcess;
static KTHREAD *gPsCurrentThread;
static KTHREAD *gPsRunQueueHead;
static KTHREAD *gPsRunQueueTail;
static volatile uint32_t gPsReschedulePending;
static uint32_t gPsReady;

static void PspThreadEntryThunk(void);
static KSTATUS PspCreateProcessInternal(const char *Name, KPROCESS **OutProcess);
static uint64_t PspAlignUp(uint64_t Value, uint64_t Alignment);
static uint32_t PspFileRangeValid(uint64_t FileSize, uint64_t Offset, uint64_t Size);
static KSTATUS PspApplyUserKpeRelocations(uint64_t LoadedBase, const KPE_NT_HEADERS64 *Nt);

static void PspHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void PspWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[128];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    PspHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static void PspThreadDeleteRoutine(OB_OBJECT_HEADER *Object) {
    (void)Object;
}

static void PspProcessDeleteRoutine(OB_OBJECT_HEADER *Object) {
    (void)Object;
}

static uint64_t PspAlignUp(uint64_t Value, uint64_t Alignment) {
    if (Alignment == 0u) {
        return Value;
    }
    return (Value + Alignment - 1u) & ~(Alignment - 1u);
}

static uint32_t PspFileRangeValid(uint64_t FileSize, uint64_t Offset, uint64_t Size) {
    uint64_t End = Offset + Size;
    if (End < Offset) {
        return 0u;
    }
    return End <= FileSize;
}

static KSTATUS PspAllocateProcessSlot(KPROCESS **OutProcess) {
    uint32_t Index;

    if (OutProcess == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < PS_MAX_PROCESSES; Index++) {
        if (gPsProcessInUse[Index] == 0u) {
            gPsProcessInUse[Index] = 1u;
            *OutProcess = &gPsProcessPool[Index];
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

static KSTATUS PspAllocateThreadSlot(KTHREAD **OutThread) {
    uint32_t Index;

    if (OutThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < PS_MAX_THREADS; Index++) {
        if (gPsThreadInUse[Index] == 0u) {
            gPsThreadInUse[Index] = 1u;
            *OutThread = &gPsThreadPool[Index];
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

static uint32_t PspThreadPoolIndex(KTHREAD *Thread) {
    return (uint32_t)(Thread - &gPsThreadPool[0]);
}

static KSTATUS PspCreateProcessInternal(const char *Name, KPROCESS **OutProcess) {
    KPROCESS *Process;
    MM_ADDRESS_SPACE *AddressSpace;
    KSTATUS Status;
    const char *ObjectName;

    if (OutProcess == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    ObjectName = (Name != 0) ? Name : "proc.user";

    Status = PspAllocateProcessSlot(&Process);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = ObCreateObject(gPsProcessType,
                            ObjectName,
                            OB_ACCESS_ALL,
                            0u,
                            &Process->ObjectHeader);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Process->ProcessId = gPsNextProcessId++;

    Status = MmCreateAddressSpace(Process->ProcessId, &AddressSpace);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }
    Process->AddressSpaceRoot = (uint64_t)(uintptr_t)AddressSpace;

    *OutProcess = Process;
    return KSTATUS_SUCCESS;
}

static KSTATUS PspCreateSystemProcess(void) {
    KSTATUS Status;

    Status = PspCreateProcessInternal("proc.system", &gPsSystemProcess);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    return KSTATUS_SUCCESS;
}

static void PspRunQueuePush(KTHREAD *Thread) {
    NK_ASSERT(Thread != 0);
    NK_ASSERT(Thread->State != PsThreadDead);
    Thread->RunQueueNext = 0;
    if (gPsRunQueueTail == 0) {
        gPsRunQueueHead = Thread;
        gPsRunQueueTail = Thread;
    } else {
        gPsRunQueueTail->RunQueueNext = Thread;
        gPsRunQueueTail = Thread;
    }
}

static KTHREAD *PspRunQueuePop(void) {
    KTHREAD *Thread = gPsRunQueueHead;
    if (Thread == 0) {
        return 0;
    }

    gPsRunQueueHead = Thread->RunQueueNext;
    if (gPsRunQueueHead == 0) {
        gPsRunQueueTail = 0;
    }
    Thread->RunQueueNext = 0;
    return Thread;
}

KSTATUS PsCreateProcess(const char *Name, KPROCESS **OutProcess) {
    if (gPsReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }
    return PspCreateProcessInternal(Name, OutProcess);
}

static KSTATUS PspApplyUserKpeRelocations(uint64_t LoadedBase, const KPE_NT_HEADERS64 *Nt) {
    const KPE_DATA_DIRECTORY *RelocDir;
    uint8_t *Cursor;
    uint8_t *End;
    int64_t Delta;

    Delta = (int64_t)LoadedBase - (int64_t)Nt->OptionalHeader.ImageBase;
    if (Delta == 0) {
        return KSTATUS_SUCCESS;
    }

    if ((Nt->OptionalHeader.DllCharacteristics & KPE_DLLCHAR_DYNAMIC_BASE) == 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    RelocDir = &Nt->OptionalHeader.DataDirectory[KPE_DIR_BASERELOC];
    if (RelocDir->VirtualAddress == 0u || RelocDir->Size < sizeof(KPE_BASE_RELOCATION_BLOCK)) {
        HalWriteDebugString("[PS] reloc missing or too small\n");
        return KSTATUS_ACCESS_DENIED;
    }
    if ((uint64_t)RelocDir->VirtualAddress + (uint64_t)RelocDir->Size > Nt->OptionalHeader.SizeOfImage) {
        HalWriteDebugString("[PS] reloc range invalid\n");
        return KSTATUS_INVALID_PARAMETER;
    }

    Cursor = (uint8_t *)(uintptr_t)(LoadedBase + RelocDir->VirtualAddress);
    End = Cursor + RelocDir->Size;
    while ((uint64_t)(uintptr_t)(Cursor + sizeof(KPE_BASE_RELOCATION_BLOCK)) <= (uint64_t)(uintptr_t)End) {
        KPE_BASE_RELOCATION_BLOCK *Block = (KPE_BASE_RELOCATION_BLOCK *)(void *)Cursor;
        uint32_t EntryCount;
        uint32_t Index;
        uint16_t *Entries;

        if (Block->SizeOfBlock < sizeof(KPE_BASE_RELOCATION_BLOCK)) {
            HalWriteDebugString("[PS] reloc block too small\n");
            return KSTATUS_INVALID_PARAMETER;
        }
        if ((uint64_t)Block->VirtualAddress + MM_PAGE_SIZE > Nt->OptionalHeader.SizeOfImage) {
            HalWriteDebugString("[PS] reloc block VA out of image\n");
            return KSTATUS_INVALID_PARAMETER;
        }
        if ((uint64_t)(uintptr_t)(Cursor + Block->SizeOfBlock) > (uint64_t)(uintptr_t)End) {
            HalWriteDebugString("[PS] reloc block overruns directory\n");
            return KSTATUS_INVALID_PARAMETER;
        }

        EntryCount = (Block->SizeOfBlock - sizeof(KPE_BASE_RELOCATION_BLOCK)) / sizeof(uint16_t);
        Entries = (uint16_t *)(void *)(Cursor + sizeof(KPE_BASE_RELOCATION_BLOCK));

        for (Index = 0u; Index < EntryCount; Index++) {
            uint16_t Entry = Entries[Index];
            uint16_t Type = (uint16_t)(Entry >> 12);
            uint16_t Offset = (uint16_t)(Entry & 0x0FFFu);
            uint32_t PatchRva;
            uint64_t *Patch;

            if (Type == KPE_REL_BASED_ABSOLUTE) {
                continue;
            }
            if (Type != KPE_REL_BASED_DIR64) {
                HalWriteDebugString("[PS] reloc type unsupported\n");
                return KSTATUS_NOT_IMPLEMENTED;
            }

            PatchRva = Block->VirtualAddress + Offset;
            if ((uint64_t)PatchRva + sizeof(uint64_t) > Nt->OptionalHeader.SizeOfImage) {
                HalWriteDebugString("[PS] reloc patch RVA out of image\n");
                return KSTATUS_INVALID_PARAMETER;
            }

            Patch = (uint64_t *)(uintptr_t)(LoadedBase + PatchRva);
            *Patch = (uint64_t)((int64_t)(*Patch) + Delta);
        }

        Cursor += Block->SizeOfBlock;
    }

    return KSTATUS_SUCCESS;
}

KSTATUS PsSpawnUserKpeFromBuffer(const void *ImageBuffer,
                                 uint64_t ImageSize,
                                 uint64_t *OutProcessId,
                                 uint64_t *OutEntryAddress) {
    const uint8_t *Bytes = (const uint8_t *)ImageBuffer;
    const KPE_DOS_HEADER *Dos;
    const KPE_NT_HEADERS64 *Nt;
    const KPE_SECTION_HEADER *Sections;
    KPROCESS *Process;
    KTHREAD *Thread;
    MM_ADDRESS_SPACE *AddressSpace;
    MM_ADDRESS_SPACE *PreviousAddressSpace = 0;
    uint64_t SectionTableOffset;
    uint64_t UserImageBase;
    uint64_t PreferredImageBase;
    uint64_t UserEntry;
    uint64_t UserStackBase;
    uint64_t UserStackTop;
    uint64_t Index;
    KSTATUS Status;

    if (gPsReady == 0u || ImageBuffer == 0 || ImageSize < sizeof(KPE_DOS_HEADER) || ImageSize > PS_IMAGE_MAX_SIZE) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HalWriteDebugString("[PS] user image spawn begin\n");
    PspWriteHexLine("[PS] user.image.size=", ImageSize);

    Dos = (const KPE_DOS_HEADER *)ImageBuffer;
    if (Dos->e_magic != KPE_DOS_MAGIC) {
        return KSTATUS_INVALID_PARAMETER;
    }
    if ((uint64_t)Dos->e_lfanew + sizeof(KPE_NT_HEADERS64) > ImageSize) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Nt = (const KPE_NT_HEADERS64 *)(const void *)(Bytes + Dos->e_lfanew);
    if (Nt->Signature != KPE_SIGNATURE ||
        Nt->FileHeader.Machine != KPE_MACHINE_AUR64 ||
        Nt->OptionalHeader.Magic != KPE_OPT_MAGIC_AUR64 ||
        Nt->OptionalHeader.Subsystem != KPE_SUBSYSTEM_USERLAND ||
        Nt->FileHeader.NumberOfSections == 0u ||
        Nt->OptionalHeader.SizeOfImage == 0u ||
        Nt->OptionalHeader.AddressOfEntryPoint >= Nt->OptionalHeader.SizeOfImage ||
        Nt->OptionalHeader.NumberOfRvaAndSizes > KPE_NUMBEROF_DIRECTORY_ENTRIES) {
        return KSTATUS_INVALID_PARAMETER;
    }

    SectionTableOffset = (uint64_t)Dos->e_lfanew + sizeof(uint32_t) + sizeof(KPE_FILE_HEADER) + Nt->FileHeader.SizeOfOptionalHeader;
    if (SectionTableOffset + ((uint64_t)Nt->FileHeader.NumberOfSections * sizeof(KPE_SECTION_HEADER)) > ImageSize) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Sections = (const KPE_SECTION_HEADER *)(const void *)(Bytes + SectionTableOffset);
    for (Index = 0u; Index < Nt->FileHeader.NumberOfSections; Index++) {
        if (Sections[Index].SizeOfRawData == 0u) {
            continue;
        }
        if (PspFileRangeValid(ImageSize, Sections[Index].PointerToRawData, Sections[Index].SizeOfRawData) == 0u) {
            return KSTATUS_INVALID_PARAMETER;
        }
    }

    Status = PspCreateProcessInternal("proc.user", &Process);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.proc.create.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    PspWriteHexLine("[PS] user.proc.pid=", Process->ProcessId);

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
    PreferredImageBase = Nt->OptionalHeader.ImageBase & ~((uint64_t)0xFFFu);
    if (PreferredImageBase == 0u ||
        PreferredImageBase >= PS_USER_TOP ||
        PreferredImageBase + Nt->OptionalHeader.SizeOfImage < PreferredImageBase ||
        PreferredImageBase + Nt->OptionalHeader.SizeOfImage >= PS_USER_TOP) {
        return KSTATUS_INVALID_PARAMETER;
    }
    AddressSpace->NextUserBase = PreferredImageBase;
    PspWriteHexLine("[PS] user.image.request.base=", PreferredImageBase);
    Status = MmMapAnonymous(AddressSpace,
                            Nt->OptionalHeader.SizeOfImage,
                            MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXECUTE,
                            MM_REGION_FLAG_WIRED,
                            &UserImageBase);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.map.image.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    PspWriteHexLine("[PS] user.image.base=", UserImageBase);

    Status = MmMapAnonymous(AddressSpace,
                            MM_PAGE_SIZE,
                            MM_PROT_READ,
                            MM_REGION_FLAG_GUARD | MM_REGION_FLAG_LAZY,
                            &UserStackBase);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.map.guard.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }

    Status = MmMapAnonymous(AddressSpace,
                            PS_USER_STACK_SIZE,
                            MM_PROT_READ | MM_PROT_WRITE,
                            MM_REGION_FLAG_WIRED,
                            &UserStackBase);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.map.stack.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    UserStackTop = UserStackBase + PS_USER_STACK_SIZE;
    PspWriteHexLine("[PS] user.stack.base=", UserStackBase);
    PspWriteHexLine("[PS] user.stack.top=", UserStackTop);

    {
        KPROCESS *CurrentProcess = PsGetCurrentProcess();
        if (CurrentProcess != 0 && CurrentProcess->AddressSpaceRoot != 0u) {
            PreviousAddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)CurrentProcess->AddressSpaceRoot;
        }
    }

    Status = MmActivateAddressSpace(AddressSpace);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.activate.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }

    {
        uint8_t *Dest = (uint8_t *)(uintptr_t)UserImageBase;
        uint64_t HeaderCopySize = Nt->OptionalHeader.SizeOfHeaders;
        if (HeaderCopySize > Nt->OptionalHeader.SizeOfImage) {
            HeaderCopySize = Nt->OptionalHeader.SizeOfImage;
        }
        if (HeaderCopySize > ImageSize) {
            HeaderCopySize = ImageSize;
        }

        for (Index = 0u; Index < HeaderCopySize; Index++) {
            Dest[Index] = Bytes[Index];
        }

        for (Index = 0u; Index < Nt->FileHeader.NumberOfSections; Index++) {
            uint32_t CopySize = Sections[Index].SizeOfRawData;
            uint8_t *SectionDest = (uint8_t *)(uintptr_t)(UserImageBase + Sections[Index].VirtualAddress);

            if (CopySize == 0u) {
                continue;
            }
            if ((uint64_t)Sections[Index].VirtualAddress + CopySize > Nt->OptionalHeader.SizeOfImage) {
                HalWriteDebugString("[PS] user.section copy out of image\n");
                PspWriteHexLine("[PS] user.section.index=", Index);
                Status = KSTATUS_INVALID_PARAMETER;
                break;
            }

            {
                const uint8_t *SectionSrc = Bytes + Sections[Index].PointerToRawData;
                uint32_t Byte;
                for (Byte = 0u; Byte < CopySize; Byte++) {
                    SectionDest[Byte] = SectionSrc[Byte];
                }
            }
        }
    }

    if (Status == KSTATUS_SUCCESS) {
        Status = PspApplyUserKpeRelocations(UserImageBase, Nt);
    }

    if (PreviousAddressSpace != 0) {
        (void)MmActivateAddressSpace(PreviousAddressSpace);
    }

    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.image.finalize.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }

    for (Index = 0u; Index < Nt->FileHeader.NumberOfSections; Index++) {
        uint32_t EffectiveVirtualSize = Sections[Index].VirtualSize;
        uint64_t SectionBase;
        uint64_t Span;
        uint64_t Cursor;
        uint64_t PteFlags = PS_PTE_USER;

        if (EffectiveVirtualSize == 0u) {
            EffectiveVirtualSize = Sections[Index].SizeOfRawData;
        }
        if (EffectiveVirtualSize == 0u) {
            continue;
        }

        if ((Sections[Index].Characteristics & KPE_SCN_MEM_WRITE) != 0u) {
            PteFlags |= PS_PTE_WRITABLE;
        }
        if ((Sections[Index].Characteristics & KPE_SCN_MEM_EXECUTE) == 0u) {
            PteFlags |= PS_PTE_NX;
        }

        SectionBase = UserImageBase + (uint64_t)Sections[Index].VirtualAddress;
        Span = PspAlignUp((uint64_t)EffectiveVirtualSize, MM_PAGE_SIZE);
        for (Cursor = 0u; Cursor < Span; Cursor += MM_PAGE_SIZE) {
            (void)MmAsProtectPage(AddressSpace,
                                  (SectionBase + Cursor) & ~((uint64_t)0xFFFu),
                                  PteFlags);
        }
    }

    UserEntry = UserImageBase + Nt->OptionalHeader.AddressOfEntryPoint;
    Status = PsCreateUserThread(Process,
                                UserEntry,
                                UserStackTop,
                                0u,
                                0u,
                                &Thread);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.thread.create.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    PspWriteHexLine("[PS] user.entry=", UserEntry);
    PspWriteHexLine("[PS] user.tid=", Thread->ThreadId);

    Status = PsReadyThread(Thread);
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] user.thread.ready.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] user image spawn ready\n");

    if (OutProcessId != 0) {
        *OutProcessId = Process->ProcessId;
    }
    if (OutEntryAddress != 0) {
        *OutEntryAddress = UserEntry;
    }

    return KSTATUS_SUCCESS;
}

KSTATUS PsCreateKernelThread(KPROCESS *Process,
                             void (*StartRoutine)(void *Context),
                             void *StartContext,
                             KTHREAD **OutThread) {
    KTHREAD *Thread;
    KSTATUS Status;

    if (gPsReady == 0u || Process == 0 || OutThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = PspAllocateThreadSlot(&Thread);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = ObCreateObject(gPsThreadType,
                            "thread.kernel",
                            OB_ACCESS_ALL,
                            OB_FLAG_WAITABLE,
                            &Thread->ObjectHeader);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Thread->ThreadId = gPsNextThreadId++;
    Thread->OwnerProcess = Process;
    Thread->State = PsThreadReady;
    Thread->TimeSliceTicks = PS_DEFAULT_TIMESLICE_TICKS;
    Thread->RemainingTicks = Thread->TimeSliceTicks;
    Thread->RunQueueNext = 0;
    Thread->WaitQueueNext = 0;
    Thread->StartRoutine = StartRoutine;
    Thread->StartContext = StartContext;
    Thread->KernelStackTop = (uint64_t)(uintptr_t)&gPsThreadStackPool[PspThreadPoolIndex(Thread)][sizeof(gPsThreadStackPool[0])];
    Thread->IsUserThread = 0u;
    Thread->UserEntry = 0u;
    Thread->UserStackTop = 0u;
    Thread->UserArg0 = 0u;
    Thread->UserArg1 = 0u;
    KiArchInitializeContext(
        &Thread->Context,
        Thread->KernelStackTop,
        PspThreadEntryThunk);

    *OutThread = Thread;
    return KSTATUS_SUCCESS;
}

KSTATUS PsCreateUserThread(KPROCESS *Process,
                           uint64_t UserEntry,
                           uint64_t UserStackTop,
                           uint64_t UserArg0,
                           uint64_t UserArg1,
                           KTHREAD **OutThread) {
    KTHREAD *Thread;
    MM_ADDRESS_SPACE *AddressSpace;
    uint64_t EntryPagePte;
    uint64_t StackPagePte;
    KSTATUS Status;

    if (gPsReady == 0u || Process == 0 || OutThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (UserEntry == 0u || UserStackTop < 16u) {
        return KSTATUS_INVALID_PARAMETER;
    }
    if (UserEntry >= PS_USER_TOP || UserStackTop >= PS_USER_TOP) {
        return KSTATUS_ACCESS_DENIED;
    }

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
    if (AddressSpace == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = MmAsQueryPte(AddressSpace, UserEntry & ~((uint64_t)0xFFFu), &EntryPagePte);
    if (Status != KSTATUS_SUCCESS ||
        (EntryPagePte & PS_PTE_PRESENT) == 0u ||
        (EntryPagePte & PS_PTE_USER) == 0u ||
        (EntryPagePte & PS_PTE_NX) != 0u) {
        PspWriteHexLine("[PS] user.entry.check.status=", (uint64_t)(uint32_t)Status);
        PspWriteHexLine("[PS] user.entry.check.pte=", EntryPagePte);
        return KSTATUS_ACCESS_DENIED;
    }

    Status = MmAsQueryPte(AddressSpace, (UserStackTop - 1u) & ~((uint64_t)0xFFFu), &StackPagePte);
    if (Status != KSTATUS_SUCCESS ||
        (StackPagePte & PS_PTE_PRESENT) == 0u ||
        (StackPagePte & PS_PTE_USER) == 0u ||
        (StackPagePte & PS_PTE_WRITABLE) == 0u) {
        PspWriteHexLine("[PS] user.stack.check.status=", (uint64_t)(uint32_t)Status);
        PspWriteHexLine("[PS] user.stack.check.pte=", StackPagePte);
        return KSTATUS_ACCESS_DENIED;
    }

    Status = PspAllocateThreadSlot(&Thread);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = ObCreateObject(gPsThreadType,
                            "thread.user",
                            OB_ACCESS_ALL,
                            OB_FLAG_WAITABLE,
                            &Thread->ObjectHeader);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Thread->ThreadId = gPsNextThreadId++;
    Thread->OwnerProcess = Process;
    Thread->State = PsThreadReady;
    Thread->TimeSliceTicks = PS_DEFAULT_TIMESLICE_TICKS;
    Thread->RemainingTicks = Thread->TimeSliceTicks;
    Thread->RunQueueNext = 0;
    Thread->WaitQueueNext = 0;
    Thread->StartRoutine = 0;
    Thread->StartContext = 0;
    Thread->KernelStackTop = (uint64_t)(uintptr_t)&gPsThreadStackPool[PspThreadPoolIndex(Thread)][sizeof(gPsThreadStackPool[0])];
    Thread->IsUserThread = 1u;
    Thread->UserEntry = UserEntry;
    Thread->UserStackTop = UserStackTop & ~((uint64_t)0x0Fu);
    Thread->UserArg0 = UserArg0;
    Thread->UserArg1 = UserArg1;

    KiArchInitializeContext(&Thread->Context, Thread->KernelStackTop, PspThreadEntryThunk);
    *OutThread = Thread;
    return KSTATUS_SUCCESS;
}

KSTATUS PsReadyThread(KTHREAD *Thread) {
    if (gPsReady == 0u || Thread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (Thread->State == PsThreadDead) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Thread->State = PsThreadReady;
    PspRunQueuePush(Thread);
    return KSTATUS_SUCCESS;
}

KSTATUS PsDispatch(void) {
    KTHREAD *NextThread;
    KTHREAD *PreviousThread;

    if (gPsReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    NextThread = PspRunQueuePop();
    if (NextThread == 0) {
        return KSTATUS_INIT_FAILED;
    }
    NK_ASSERT(NextThread->OwnerProcess != 0);

    PreviousThread = gPsCurrentThread;
    if (PreviousThread != 0 && PreviousThread->State == PsThreadRunning) {
        PreviousThread->State = PsThreadReady;
        PspRunQueuePush(PreviousThread);
    }

    NextThread->State = PsThreadRunning;
    NextThread->RemainingTicks = NextThread->TimeSliceTicks;
    gPsCurrentThread = NextThread;
    HalSetKernelStackPointer(NextThread->KernelStackTop);
    if (NextThread->OwnerProcess != 0 && NextThread->OwnerProcess->AddressSpaceRoot != 0u) {
        (void)MmActivateAddressSpace((MM_ADDRESS_SPACE *)(uintptr_t)NextThread->OwnerProcess->AddressSpaceRoot);
    }

    if (PreviousThread != 0 && PreviousThread != NextThread) {
        KiArchSwitchContext(&PreviousThread->Context, &NextThread->Context);
    }

    return KSTATUS_SUCCESS;
}

KSTATUS PsYieldThread(void) {
    if (gPsReady == 0u || gPsCurrentThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }
    NK_ASSERT(gPsCurrentThread->State == PsThreadRunning);

    gPsReschedulePending = 0u;
    return PsDispatch();
}

KSTATUS PsExitCurrentThread(uint64_t ExitCode) {
    (void)ExitCode;

    if (gPsReady == 0u || gPsCurrentThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    gPsCurrentThread->State = PsThreadDead;
    gPsCurrentThread = 0;
    return PsDispatch();
}

KSTATUS PsBlockCurrentThread(void) {
    if (gPsReady == 0u || gPsCurrentThread == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    gPsCurrentThread->State = PsThreadBlocked;
    gPsCurrentThread = 0;
    return PsDispatch();
}

void PsWakeThread(KTHREAD *Thread) {
    if (gPsReady == 0u || Thread == 0) {
        return;
    }

    if (Thread->State == PsThreadBlocked) {
        Thread->State = PsThreadReady;
        PspRunQueuePush(Thread);
    }
}

void PsOnTimerTick(void) {
    if (gPsReady == 0u || gPsCurrentThread == 0) {
        return;
    }

    if (gPsCurrentThread->RemainingTicks > 0u) {
        gPsCurrentThread->RemainingTicks--;
    }

    if (gPsCurrentThread->RemainingTicks == 0u) {
        gPsReschedulePending = 1u;
    }
}

void PsHandlePreemptionPoint(void) {
    if (gPsReady == 0u || gPsReschedulePending == 0u) {
        return;
    }

    (void)PsYieldThread();
}

KTHREAD *PsGetCurrentThread(void) {
    return gPsCurrentThread;
}

KPROCESS *PsGetCurrentProcess(void) {
    if (gPsCurrentThread == 0) {
        return 0;
    }
    return gPsCurrentThread->OwnerProcess;
}

void PsDumpSchedulerState(void) {
    KTHREAD *Cursor;
    uint32_t Index;

    if (gPsReady == 0u) {
        return;
    }

    HalWriteDebugString("[PSDBG] scheduler begin\n");
    if (gPsCurrentThread != 0) {
        PspWriteHexLine("[PSDBG] current.tid=", gPsCurrentThread->ThreadId);
        PspWriteHexLine("[PSDBG] current.state=", gPsCurrentThread->State);
        if (gPsCurrentThread->OwnerProcess != 0) {
            PspWriteHexLine("[PSDBG] current.pid=", gPsCurrentThread->OwnerProcess->ProcessId);
        }
    } else {
        HalWriteDebugString("[PSDBG] current=<none>\n");
    }

    Cursor = gPsRunQueueHead;
    while (Cursor != 0) {
        PspWriteHexLine("[PSDBG] runq.tid=", Cursor->ThreadId);
        if (Cursor->OwnerProcess != 0) {
            PspWriteHexLine("[PSDBG] runq.pid=", Cursor->OwnerProcess->ProcessId);
        }
        PspWriteHexLine("[PSDBG] runq.state=", Cursor->State);
        Cursor = Cursor->RunQueueNext;
    }

    for (Index = 0u; Index < PS_MAX_PROCESSES; Index++) {
        if (gPsProcessInUse[Index] != 0u) {
            KPROCESS *Process = &gPsProcessPool[Index];
            PspWriteHexLine("[PSDBG] proc.pid=", Process->ProcessId);
            PspWriteHexLine("[PSDBG] proc.as=", Process->AddressSpaceRoot);
            if (Process->ObjectHeader != 0) {
                HalWriteDebugString("[PSDBG] proc.name=");
                HalWriteDebugString(Process->ObjectHeader->Name);
                HalWriteDebugString("\n");
            }
        }
    }

    for (Index = 0u; Index < PS_MAX_THREADS; Index++) {
        if (gPsThreadInUse[Index] != 0u) {
            KTHREAD *Thread = &gPsThreadPool[Index];
            PspWriteHexLine("[PSDBG] thread.tid=", Thread->ThreadId);
            PspWriteHexLine("[PSDBG] thread.state=", Thread->State);
            PspWriteHexLine("[PSDBG] thread.user=", Thread->IsUserThread);
            if (Thread->OwnerProcess != 0) {
                PspWriteHexLine("[PSDBG] thread.pid=", Thread->OwnerProcess->ProcessId);
            }
        }
    }
    HalWriteDebugString("[PSDBG] scheduler end\n");
}

static void PspThreadEntryThunk(void) {
    for (;;) {
        KTHREAD *CurrentThread = gPsCurrentThread;

        if (CurrentThread == 0) {
            HalHalt();
        }

        if (CurrentThread->IsUserThread != 0u) {
            HalWriteDebugString("[PS] entering user mode\n");
            PspWriteHexLine("[PS] enter.user.rip=", CurrentThread->UserEntry);
            PspWriteHexLine("[PS] enter.user.rsp=", CurrentThread->UserStackTop);
            KiArchEnterUserMode(CurrentThread->UserEntry,
                                CurrentThread->UserStackTop,
                                CurrentThread->UserArg0,
                                CurrentThread->UserArg1);
            HalWriteDebugString("[PS] returned from user mode unexpectedly\n");
        }

        if (CurrentThread->StartRoutine != 0) {
            void (*StartRoutine)(void *Context) = CurrentThread->StartRoutine;
            void *StartContext = CurrentThread->StartContext;

            CurrentThread->StartRoutine = 0;
            CurrentThread->StartContext = 0;
            StartRoutine(StartContext);
        }

        CurrentThread->State = PsThreadDead;
        (void)PsDispatch();
    }
}

KSTATUS PsInitialize(void) {
    KTHREAD *BootstrapThread;
    KSTATUS Status;
    uint32_t Index;

    if (gPsReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }
    HalWriteDebugString("[PS] init begin\n");

    for (Index = 0; Index < PS_MAX_PROCESSES; Index++) {
        gPsProcessInUse[Index] = 0u;
    }

    for (Index = 0; Index < PS_MAX_THREADS; Index++) {
        gPsThreadInUse[Index] = 0u;
    }

    gPsNextProcessId = 1u;
    gPsNextThreadId = 1u;
    gPsRunQueueHead = 0;
    gPsRunQueueTail = 0;
    gPsCurrentThread = 0;
    gPsReschedulePending = 0u;

    Status = ObCreateType("Process", PspProcessDeleteRoutine, &gPsProcessType);
    if (Status != KSTATUS_SUCCESS && Status != KSTATUS_ALREADY_INITIALIZED) {
        PspWriteHexLine("[PS] init type.process.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init type.process ok\n");

    Status = ObCreateType("Thread", PspThreadDeleteRoutine, &gPsThreadType);
    if (Status != KSTATUS_SUCCESS && Status != KSTATUS_ALREADY_INITIALIZED) {
        PspWriteHexLine("[PS] init type.thread.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init type.thread ok\n");

    Status = PspCreateSystemProcess();
    if (Status != KSTATUS_SUCCESS) {
        PspWriteHexLine("[PS] init system.process.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init system.process ok\n");

    /* Internal bootstrap still uses public thread APIs gated by gPsReady. */
    gPsReady = 1u;

    Status = PsCreateKernelThread(gPsSystemProcess, 0, 0, &BootstrapThread);
    if (Status != KSTATUS_SUCCESS) {
        gPsReady = 0u;
        PspWriteHexLine("[PS] init bootstrap.thread.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init bootstrap.thread ok\n");

    Status = PsReadyThread(BootstrapThread);
    if (Status != KSTATUS_SUCCESS) {
        gPsReady = 0u;
        PspWriteHexLine("[PS] init bootstrap.ready.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init bootstrap.ready ok\n");

    HalRegisterTimerTickRoutine(PsOnTimerTick);

    Status = PsDispatch();
    if (Status != KSTATUS_SUCCESS) {
        gPsReady = 0u;
        PspWriteHexLine("[PS] init dispatch.status=", (uint64_t)(uint32_t)Status);
        return Status;
    }
    HalWriteDebugString("[PS] init dispatch ok\n");

    HalWriteDebugString("[PS] process/thread manager online (scheduler v0)\n");
    return KSTATUS_SUCCESS;
}
