#include "../../hal/inc/hal.h"
#include "../include/mm.h"
#include "../include/nk.h"
#include "../include/nkassert.h"
#include "../include/ob.h"
#include "../include/ps.h"

#define MM_BITMAP_WORD_BITS 64u
#define MM_BITMAP_WORD_COUNT (MM_MAX_TRACKED_PAGES / MM_BITMAP_WORD_BITS)

#define EFI_LOADER_CODE 1u
#define EFI_LOADER_DATA 2u
#define EFI_BOOT_SERVICES_CODE 3u
#define EFI_BOOT_SERVICES_DATA 4u
#define EFI_CONVENTIONAL_MEMORY 7u

#define MM_USER_TOP 0x0000800000000000ull
#define MM_USER_BASE 0x0000000040000000ull

#define MM_PTE_PRESENT 0x001ull
#define MM_PTE_WRITABLE 0x002ull
#define MM_PTE_USER 0x004ull
#define MM_PTE_LARGE 0x080ull
#define MM_PTE_SOFT_COW 0x200ull
#define MM_PTE_NX (1ull << 63)
#define MM_PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

#define MM_PHYSMAP_BASE 0xFFFF900000000000ull

#define MM_PF_ERROR_PRESENT 0x1ull
#define MM_PF_ERROR_WRITE 0x2ull
#define MM_PF_ERROR_USER 0x4ull
#define MM_PF_ERROR_INSTR 0x10ull

typedef struct MM_FAULT_TRACE {
    uint64_t FaultAddress;
    uint64_t InstructionPointer;
    uint64_t ErrorCode;
    uint64_t ProcessId;
    uint64_t AddressSpaceId;
    int64_t Outcome;
} MM_FAULT_TRACE;

typedef struct MM_SECTION_OBJECT {
    OB_OBJECT_HEADER *Header;
    uint32_t PageCount;
    uint64_t Pages[MM_MAX_SECTION_PAGES];
} MM_SECTION_OBJECT;

static uint64_t gMmPageBitmap[MM_BITMAP_WORD_COUNT];
static uint32_t gMmPageRefCount[MM_MAX_TRACKED_PAGES];
static uint32_t gMmTrackedPages;
static uint32_t gMmNextScanPage;
static uint32_t gMmReady;
static MM_ADDRESS_SPACE gMmAddressSpacePool[MM_MAX_ADDRESS_SPACES];
static uint8_t gMmAddressSpaceInUse[MM_MAX_ADDRESS_SPACES];
static uint64_t gMmNextAddressSpaceId;
static uint64_t gMmKernelRootTablePhysical;
static OB_OBJECT_TYPE *gMmSectionType;
static MM_SECTION_OBJECT gMmSectionPool[MM_MAX_SECTION_OBJECTS];
static uint8_t gMmSectionInUse[MM_MAX_SECTION_OBJECTS];
static MM_FAULT_TRACE gMmFaultTrace[MM_FAULT_TRACE_DEPTH];
static uint32_t gMmFaultTraceNext;
static uint64_t gMmPhysmapBase;
static uint64_t gMmPhysmapBytes;
static uint32_t gMmPhysmapReady;
static void MmpOnPageFault(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer);
static uint64_t MmpProtectionToPte(uint32_t Protection, uint32_t RegionFlags);
static MM_VA_REGION *MmpFindRegion(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress);
static MM_SECTION_OBJECT *MmpFindSectionByHeader(OB_OBJECT_HEADER *Header);
static void MmpDeleteSectionObject(OB_OBJECT_HEADER *Object);
static KSTATUS MmpCloneKernelRootTable(void);

static uint32_t MmpIsUserAddress(uint64_t VirtualAddress) {
    return (VirtualAddress >= MM_USER_BASE && VirtualAddress < MM_USER_TOP) ? 1u : 0u;
}

static void MmpSetPageAllocated(uint32_t PageIndex) {
    uint32_t Word = PageIndex / MM_BITMAP_WORD_BITS;
    uint32_t Bit = PageIndex % MM_BITMAP_WORD_BITS;
    gMmPageBitmap[Word] |= (((uint64_t)1u) << Bit);
}

static void MmpSetPageFree(uint32_t PageIndex) {
    uint32_t Word = PageIndex / MM_BITMAP_WORD_BITS;
    uint32_t Bit = PageIndex % MM_BITMAP_WORD_BITS;
    gMmPageBitmap[Word] &= ~(((uint64_t)1u) << Bit);
}

static uint32_t MmpIsPageAllocated(uint32_t PageIndex) {
    uint32_t Word = PageIndex / MM_BITMAP_WORD_BITS;
    uint32_t Bit = PageIndex % MM_BITMAP_WORD_BITS;
    return (gMmPageBitmap[Word] & (((uint64_t)1u) << Bit)) != 0u;
}

static uint64_t MmpAlignUp(uint64_t Value, uint64_t Alignment) {
    if (Alignment == 0u) {
        return Value;
    }
    return (Value + Alignment - 1u) & ~(Alignment - 1u);
}

static uint64_t MmpAlignDown(uint64_t Value, uint64_t Alignment) {
    if (Alignment == 0u) {
        return Value;
    }
    return Value & ~(Alignment - 1u);
}

static uint64_t MmpAlignSizeToPage(uint64_t Size) {
    if (Size == 0u) {
        return MM_PAGE_SIZE;
    }
    return MmpAlignUp(Size, MM_PAGE_SIZE);
}

static uint32_t MmpIsCanonicalAddress(uint64_t Va) {
    uint64_t Top = Va >> 48;
    return (Top == 0u) || (Top == 0xFFFFu);
}

static uint64_t MmpReadCr3(void) {
    uint64_t Value;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(Value));
    return Value;
}

static void MmpWriteCr3(uint64_t Value) {
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(Value) : "memory");
}

static void MmpInvalidatePage(uint64_t VirtualAddress) {
    __asm__ __volatile__("invlpg (%0)" : : "r"((void *)(uintptr_t)VirtualAddress) : "memory");
}

static uint64_t *MmpPhysToVirtBootstrapTable(uint64_t PhysicalAddress) {
    return (uint64_t *)(uintptr_t)(PhysicalAddress & MM_PTE_ADDR_MASK);
}

static uint64_t *MmpPhysToVirtTable(uint64_t PhysicalAddress) {
    if (gMmPhysmapReady != 0u) {
        return (uint64_t *)(uintptr_t)(gMmPhysmapBase + (PhysicalAddress & MM_PTE_ADDR_MASK));
    }
    return MmpPhysToVirtBootstrapTable(PhysicalAddress);
}

static uint8_t *MmpPhysToVirtPage(uint64_t PhysicalAddress) {
    if (gMmPhysmapReady != 0u) {
        return (uint8_t *)(uintptr_t)(gMmPhysmapBase + (PhysicalAddress & MM_PTE_ADDR_MASK));
    }
    return (uint8_t *)(uintptr_t)(PhysicalAddress & MM_PTE_ADDR_MASK);
}

static KSTATUS MmpAllocPageInternal(uint64_t *OutPhysicalAddress, uint32_t InitialRefCount) {
    uint32_t ScanCount;
    uint32_t Page;

    if (OutPhysicalAddress == 0 || gMmTrackedPages == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (ScanCount = 0; ScanCount < gMmTrackedPages; ScanCount++) {
        Page = (gMmNextScanPage + ScanCount) % gMmTrackedPages;
        if (MmpIsPageAllocated(Page) == 0u) {
            MmpSetPageAllocated(Page);
            gMmPageRefCount[Page] = InitialRefCount;
            gMmNextScanPage = (Page + 1u) % gMmTrackedPages;
            *OutPhysicalAddress = (uint64_t)Page * MM_PAGE_SIZE;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

static KSTATUS MmpBuildKernelPhysmap(void) {
    uint64_t MappedBytes;
    uint64_t Offset;
    uint64_t *Pml4;
    uint64_t Pml4Index;
    uint64_t PdptPa;
    uint64_t *Pdpt;

    if (gMmKernelRootTablePhysical == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    MappedBytes = MmpAlignUp((uint64_t)gMmTrackedPages * MM_PAGE_SIZE, 0x200000u);
    gMmPhysmapBase = MM_PHYSMAP_BASE;
    gMmPhysmapBytes = MappedBytes;

    Pml4 = MmpPhysToVirtBootstrapTable(gMmKernelRootTablePhysical);
    Pml4Index = (gMmPhysmapBase >> 39) & 0x1FFu;

    if (MmpAllocPageInternal(&PdptPa, 1u) != KSTATUS_SUCCESS) {
        return KSTATUS_INIT_FAILED;
    }
    Pdpt = MmpPhysToVirtBootstrapTable(PdptPa);
    for (Offset = 0; Offset < 512u; Offset++) {
        Pdpt[Offset] = 0u;
    }
    Pml4[Pml4Index] = PdptPa | MM_PTE_PRESENT | MM_PTE_WRITABLE;

    PdptPa = Pml4[Pml4Index] & MM_PTE_ADDR_MASK;
    Pdpt = MmpPhysToVirtBootstrapTable(PdptPa);

    for (Offset = 0; Offset < MappedBytes; Offset += 0x40000000ull) {
        uint64_t Va = gMmPhysmapBase + Offset;
        uint64_t L3 = (Va >> 30) & 0x1FFu;
        uint64_t PdPa;
        uint64_t *Pd;
        uint64_t Inner;

        if ((Pdpt[L3] & MM_PTE_PRESENT) == 0u) {
            if (MmpAllocPageInternal(&PdPa, 1u) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            Pd = MmpPhysToVirtBootstrapTable(PdPa);
            for (Inner = 0; Inner < 512u; Inner++) {
                Pd[Inner] = 0u;
            }
            Pdpt[L3] = PdPa | MM_PTE_PRESENT | MM_PTE_WRITABLE;
        }

        PdPa = Pdpt[L3] & MM_PTE_ADDR_MASK;
        Pd = MmpPhysToVirtBootstrapTable(PdPa);
        for (Inner = 0; Inner < 0x40000000ull && (Offset + Inner) < MappedBytes; Inner += 0x200000ull) {
            uint64_t Va2 = Va + Inner;
            uint64_t L2 = (Va2 >> 21) & 0x1FFu;
            uint64_t Pa = Offset + Inner;
            Pd[L2] = Pa | MM_PTE_PRESENT | MM_PTE_WRITABLE | MM_PTE_LARGE | MM_PTE_NX;
        }
    }

    MmpWriteCr3(MmpReadCr3());
    gMmPhysmapReady = 1u;
    HalWriteDebugString("[MM] physmap online\n");
    return KSTATUS_SUCCESS;
}

static KSTATUS MmpCloneKernelRootTable(void) {
    uint64_t CurrentRootPa;
    uint64_t NewRootPa;
    uint64_t *CurrentRoot;
    uint64_t *NewRoot;
    uint32_t Index;

    CurrentRootPa = MmpReadCr3() & MM_PTE_ADDR_MASK;
    if (CurrentRootPa == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (MmpAllocPageInternal(&NewRootPa, 1u) != KSTATUS_SUCCESS) {
        return KSTATUS_INIT_FAILED;
    }

    CurrentRoot = MmpPhysToVirtBootstrapTable(CurrentRootPa);
    NewRoot = MmpPhysToVirtBootstrapTable(NewRootPa);
    for (Index = 0u; Index < 512u; Index++) {
        NewRoot[Index] = CurrentRoot[Index];
    }

    gMmKernelRootTablePhysical = NewRootPa;
    MmpWriteCr3(NewRootPa);
    return KSTATUS_SUCCESS;
}

static uint32_t MmpIsRangeUsable(const PHYSICAL_MEMORY_RANGE *Range) {
    if (Range == 0) {
        return 0u;
    }

    if (Range->Type == EFI_CONVENTIONAL_MEMORY ||
        Range->Type == EFI_BOOT_SERVICES_CODE ||
        Range->Type == EFI_BOOT_SERVICES_DATA ||
        Range->Type == EFI_LOADER_CODE ||
        Range->Type == EFI_LOADER_DATA) {
        return 1u;
    }

    return 0u;
}

static void MmpMarkRange(uint64_t Base, uint64_t Length, uint32_t Allocated) {
    uint64_t Start;
    uint64_t End;
    uint64_t Page;

    if (Length == 0u || gMmTrackedPages == 0u) {
        return;
    }

    Start = MmpAlignUp(Base, MM_PAGE_SIZE);
    End = MmpAlignDown(Base + Length, MM_PAGE_SIZE);
    if (End <= Start) {
        return;
    }

    for (Page = Start / MM_PAGE_SIZE; Page < (End / MM_PAGE_SIZE); Page++) {
        if (Page >= (uint64_t)gMmTrackedPages) {
            break;
        }

        if (Allocated != 0u) {
            MmpSetPageAllocated((uint32_t)Page);
        } else {
            MmpSetPageFree((uint32_t)Page);
        }
    }
}

static void MmpMarkUsableFromBootInfo(const BOOT_INFO *BootInfo) {
    const uint8_t *Cursor;
    uint32_t Index;

    if (BootInfo == 0 || BootInfo->MemoryMap == 0 || BootInfo->MemoryMapEntrySize == 0u) {
        return;
    }

    Cursor = (const uint8_t *)(uintptr_t)BootInfo->MemoryMap;
    for (Index = 0; Index < BootInfo->MemoryMapEntryCount; Index++) {
        const PHYSICAL_MEMORY_RANGE *Range = (const PHYSICAL_MEMORY_RANGE *)Cursor;
        if (MmpIsRangeUsable(Range) != 0u) {
            MmpMarkRange(Range->Base, Range->Length, 0u);
        }
        Cursor += BootInfo->MemoryMapEntrySize;
    }
}

static void MmpReserveBootCriticalRanges(const BOOT_INFO *BootInfo) {
    uint64_t MapBytes;

    if (BootInfo == 0) {
        return;
    }

    MmpMarkRange(0u, 0x100000u, 1u);
    MmpMarkRange(BootInfo->KernelBase, BootInfo->KernelSize, 1u);
    MmpMarkRange(BootInfo->LoaderBase, 0x200000u, 1u);
    if ((BootInfo->Flags & BOOTINFO_FLAG_INIT_IMAGE_PRESENT) != 0u &&
        BootInfo->InitImageBase != 0u &&
        BootInfo->InitImageSize != 0u) {
        MmpMarkRange(BootInfo->InitImageBase, BootInfo->InitImageSize, 1u);
    }
    MmpMarkRange((uint64_t)(uintptr_t)BootInfo, sizeof(*BootInfo), 1u);

    MapBytes = (uint64_t)BootInfo->MemoryMapEntryCount * (uint64_t)BootInfo->MemoryMapEntrySize;
    MmpMarkRange(BootInfo->MemoryMap, MapBytes, 1u);
}

static void MmpRetainPage(uint64_t PhysicalAddress) {
    uint64_t Page = PhysicalAddress / MM_PAGE_SIZE;
    if (Page < MM_MAX_TRACKED_PAGES && gMmPageRefCount[Page] != 0u) {
        gMmPageRefCount[Page]++;
    }
}

static void MmpReleasePage(uint64_t PhysicalAddress) {
    uint64_t Page = PhysicalAddress / MM_PAGE_SIZE;
    if (Page >= MM_MAX_TRACKED_PAGES || gMmPageRefCount[Page] == 0u) {
        return;
    }

    gMmPageRefCount[Page]--;
    if (gMmPageRefCount[Page] == 0u) {
        MmpSetPageFree((uint32_t)Page);
    }
}

static void MmpRecordFault(MM_ADDRESS_SPACE *AddressSpace,
                           uint64_t FaultAddress,
                           uint64_t ErrorCode,
                           uint64_t InstructionPointer,
                           KSTATUS Outcome) {
    MM_FAULT_TRACE *Entry = &gMmFaultTrace[gMmFaultTraceNext % MM_FAULT_TRACE_DEPTH];
    KPROCESS *Process = PsGetCurrentProcess();

    Entry->FaultAddress = FaultAddress;
    Entry->InstructionPointer = InstructionPointer;
    Entry->ErrorCode = ErrorCode;
    Entry->ProcessId = (Process != 0) ? Process->ProcessId : 0u;
    Entry->AddressSpaceId = (AddressSpace != 0) ? AddressSpace->AddressSpaceId : 0u;
    Entry->Outcome = (int64_t)Outcome;

    gMmFaultTraceNext++;
}

static void MmpHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    int32_t Index;
    for (Index = 0; Index < 16; Index++) {
        uint32_t Shift = (uint32_t)((15 - Index) * 4);
        Out[Index] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void MmpWriteHexLine(const char *Prefix, uint64_t Value) {
    char Hex[17];
    char Buffer[96];
    uint32_t Pos = 0;
    uint32_t i = 0;

    while (Prefix[i] != 0 && Pos + 1 < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }

    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    MmpHex64(Value, Hex);
    for (i = 0; i < 16 && Pos + 1 < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static void MmpWriteAccessType(uint64_t ErrorCode) {
    HalWriteDebugString("[MMFLT] access=");
    HalWriteDebugString((ErrorCode & MM_PF_ERROR_INSTR) != 0u ? "exec " : "");
    HalWriteDebugString((ErrorCode & MM_PF_ERROR_WRITE) != 0u ? "write " : "read ");
    HalWriteDebugString((ErrorCode & MM_PF_ERROR_USER) != 0u ? "user\n" : "kernel\n");
}

static void MmpDumpRegion(const MM_VA_REGION *Region) {
    if (Region == 0) {
        HalWriteDebugString("[MMFLT] region=<none>\n");
        return;
    }

    MmpWriteHexLine("[MMFLT] region.base=", Region->Base);
    MmpWriteHexLine("[MMFLT] region.size=", Region->Size);
    MmpWriteHexLine("[MMFLT] region.type=", Region->Type);
    MmpWriteHexLine("[MMFLT] region.prot=", Region->Protection);
    MmpWriteHexLine("[MMFLT] region.flags=", Region->Flags);
    MmpWriteHexLine("[MMFLT] region.commit=", Region->CommittedPages);
    MmpWriteHexLine("[MMFLT] region.backing=", Region->BackingPhysicalBase);
}

static KSTATUS MmpWalkToPte(MM_ADDRESS_SPACE *AddressSpace,
                            uint64_t VirtualAddress,
                            uint32_t CreateMissing,
                            uint64_t **OutPte) {
    uint64_t *Table;
    uint64_t Entry;
    uint64_t NextTablePa;
    uint64_t Indexes[4];
    uint32_t Level;

    if (AddressSpace == 0 || OutPte == 0 || MmpIsCanonicalAddress(VirtualAddress) == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }
    NK_ASSERT(AddressSpace->RootTablePhysical != 0u);

    Indexes[0] = (VirtualAddress >> 39) & 0x1FFu;
    Indexes[1] = (VirtualAddress >> 30) & 0x1FFu;
    Indexes[2] = (VirtualAddress >> 21) & 0x1FFu;
    Indexes[3] = (VirtualAddress >> 12) & 0x1FFu;

    Table = MmpPhysToVirtTable(AddressSpace->RootTablePhysical);
    for (Level = 0; Level < 3; Level++) {
        uint32_t UserAddress = MmpIsUserAddress(VirtualAddress);
        Entry = Table[Indexes[Level]];
        if ((Entry & MM_PTE_PRESENT) != 0u && (Entry & MM_PTE_LARGE) != 0u) {
            /*
             * Split 2 MiB PDE mappings on demand so we can descend to 4 KiB PTEs.
             * Without this, a large-entry physical base would be misused as a PT pointer.
             */
            if (Level != 2u) {
                return KSTATUS_NOT_IMPLEMENTED;
            }
            if (CreateMissing == 0u) {
                return KSTATUS_NOT_FOUND;
            }
            if (MmAllocPage(&NextTablePa) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            {
                uint64_t *SplitTable = MmpPhysToVirtTable(NextTablePa);
                uint64_t SplitBase = Entry & MM_PTE_ADDR_MASK;
                uint64_t SplitFlags = Entry & (MM_PTE_WRITABLE | MM_PTE_USER | MM_PTE_NX);
                uint32_t SplitIndex;

                for (SplitIndex = 0u; SplitIndex < 512u; SplitIndex++) {
                    SplitTable[SplitIndex] = (SplitBase + ((uint64_t)SplitIndex * MM_PAGE_SIZE)) |
                                             SplitFlags |
                                             MM_PTE_PRESENT;
                }
            }

            Entry = NextTablePa | MM_PTE_PRESENT | MM_PTE_WRITABLE;
            if (VirtualAddress < MM_USER_TOP) {
                Entry |= MM_PTE_USER;
            }
            Table[Indexes[Level]] = Entry;
        }
        if ((Entry & MM_PTE_PRESENT) != 0u && UserAddress != 0u && CreateMissing != 0u) {
            uint64_t Sanitized = Entry;
            Sanitized |= MM_PTE_USER;
            Sanitized &= ~MM_PTE_NX;
            if (Sanitized != Entry) {
                Entry = Sanitized;
                Table[Indexes[Level]] = Entry;
            }
        }
        if ((Entry & MM_PTE_PRESENT) == 0u) {
            if (CreateMissing == 0u) {
                return KSTATUS_NOT_FOUND;
            }

            if (MmAllocPage(&NextTablePa) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            {
                uint64_t *NextTable = MmpPhysToVirtTable(NextTablePa);
                uint32_t i;
                for (i = 0; i < 512u; i++) {
                    NextTable[i] = 0u;
                }
            }

            Entry = NextTablePa | MM_PTE_PRESENT | MM_PTE_WRITABLE;
            if (UserAddress != 0u) {
                Entry |= MM_PTE_USER;
            }
            Table[Indexes[Level]] = Entry;
        }

        NextTablePa = Entry & MM_PTE_ADDR_MASK;
        Table = MmpPhysToVirtTable(NextTablePa);
    }

    *OutPte = &Table[Indexes[3]];
    return KSTATUS_SUCCESS;
}

KSTATUS MmInitialize(void) {
    HAL_PLATFORM_INFO PlatformInfo;
    const BOOT_INFO *BootInfo;
    uint64_t TopPages;
    uint32_t Index;

    if (gMmReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }
    HalWriteDebugString("[MM] init begin\n");

    BootInfo = NkGetBootInfo();
    if (BootInfo == 0 || BootInfo->Magic != BOOTINFO_MAGIC) {
        return KSTATUS_INVALID_PARAMETER;
    }
    HalWriteDebugString("[MM] init boot info ok\n");

    HalQueryPlatformInfo(&PlatformInfo);
    TopPages = PlatformInfo.PhysicalMemoryTop / MM_PAGE_SIZE;
    if (TopPages > MM_MAX_TRACKED_PAGES) {
        TopPages = MM_MAX_TRACKED_PAGES;
    }

    gMmTrackedPages = (uint32_t)TopPages;
    gMmNextScanPage = 0u;
    gMmNextAddressSpaceId = 1u;
    gMmFaultTraceNext = 0u;
    gMmPhysmapReady = 0u;
    gMmPhysmapBase = MM_PHYSMAP_BASE;
    gMmPhysmapBytes = 0u;
    gMmSectionType = 0;
    HalWriteDebugString("[MM] init topology prepared\n");

    for (Index = 0; Index < MM_BITMAP_WORD_COUNT; Index++) {
        gMmPageBitmap[Index] = 0xFFFFFFFFFFFFFFFFull;
    }

    for (Index = 0; Index < MM_MAX_TRACKED_PAGES; Index++) {
        gMmPageRefCount[Index] = 0u;
    }

    for (Index = 0; Index < MM_MAX_ADDRESS_SPACES; Index++) {
        gMmAddressSpaceInUse[Index] = 0u;
    }

    for (Index = 0; Index < MM_MAX_SECTION_OBJECTS; Index++) {
        gMmSectionInUse[Index] = 0u;
    }

    MmpMarkUsableFromBootInfo(BootInfo);
    HalWriteDebugString("[MM] init usable ranges marked\n");
    MmpReserveBootCriticalRanges(BootInfo);
    HalWriteDebugString("[MM] init critical ranges reserved\n");

    if (MmpCloneKernelRootTable() != KSTATUS_SUCCESS) {
        return KSTATUS_INIT_FAILED;
    }
    HalWriteDebugString("[MM] init cr3 cloned\n");
    if (MmpBuildKernelPhysmap() != KSTATUS_SUCCESS) {
        return KSTATUS_INIT_FAILED;
    }
    HalWriteDebugString("[MM] init physmap built\n");

    if (ObCreateType("Section", MmpDeleteSectionObject, &gMmSectionType) != KSTATUS_SUCCESS) {
        return KSTATUS_INIT_FAILED;
    }
    HalWriteDebugString("[MM] init section type ready\n");

    HalRegisterPageFaultRoutine(MmpOnPageFault);

    gMmReady = 1u;
    HalWriteDebugString("[MM] memory manager online (regions + address spaces + pager)\n");
    return KSTATUS_SUCCESS;
}

KSTATUS MmAllocPage(uint64_t *OutPhysicalAddress) {
    if (gMmReady == 0u || OutPhysicalAddress == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    return MmpAllocPageInternal(OutPhysicalAddress, 1u);
}

KSTATUS MmFreePage(uint64_t PhysicalAddress) {
    uint64_t Page;

    if (gMmReady == 0u || (PhysicalAddress % MM_PAGE_SIZE) != 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Page = PhysicalAddress / MM_PAGE_SIZE;
    if (Page >= (uint64_t)gMmTrackedPages) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (MmpIsPageAllocated((uint32_t)Page) == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    gMmPageRefCount[Page] = 0u;
    MmpSetPageFree((uint32_t)Page);
    return KSTATUS_SUCCESS;
}

KSTATUS MmCreateAddressSpace(uint64_t OwnerProcessId, MM_ADDRESS_SPACE **OutAddressSpace) {
    uint32_t Index;

    if (gMmReady == 0u || OutAddressSpace == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < MM_MAX_ADDRESS_SPACES; Index++) {
        if (gMmAddressSpaceInUse[Index] == 0u) {
            MM_ADDRESS_SPACE *AddressSpace = &gMmAddressSpacePool[Index];
            uint64_t RootPa;
            uint64_t *NewRoot;
            uint64_t *KernelRoot;
            uint32_t Entry;

            if (MmAllocPage(&RootPa) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            NK_ASSERT(gMmKernelRootTablePhysical != 0u);
            NewRoot = MmpPhysToVirtTable(RootPa);
            KernelRoot = MmpPhysToVirtTable(gMmKernelRootTablePhysical);
            /*
             * Bring-up note:
             * The current kernel is linked in low canonical VA (0x0020_0000 range),
             * so process address spaces must preserve low PML4 entries too.
             */
            for (Entry = 0u; Entry < 512u; Entry++) {
                NewRoot[Entry] = KernelRoot[Entry];
            }

            gMmAddressSpaceInUse[Index] = 1u;
            AddressSpace->AddressSpaceId = gMmNextAddressSpaceId++;
            AddressSpace->OwnerProcessId = OwnerProcessId;
            AddressSpace->RootTablePhysical = RootPa;
            AddressSpace->NextUserBase = MM_USER_BASE;
            AddressSpace->RegionCount = 0u;
            *OutAddressSpace = AddressSpace;
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}

MM_ADDRESS_SPACE *MmGetAddressSpaceById(uint64_t AddressSpaceId) {
    uint32_t Index;

    if (gMmReady == 0u || AddressSpaceId == 0u) {
        return 0;
    }

    for (Index = 0; Index < MM_MAX_ADDRESS_SPACES; Index++) {
        if (gMmAddressSpaceInUse[Index] != 0u &&
            gMmAddressSpacePool[Index].AddressSpaceId == AddressSpaceId) {
            return &gMmAddressSpacePool[Index];
        }
    }

    return 0;
}

KSTATUS MmActivateAddressSpace(MM_ADDRESS_SPACE *AddressSpace) {
    uint64_t CurrentCr3;

    if (gMmReady == 0u || AddressSpace == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    CurrentCr3 = MmpReadCr3() & MM_PTE_ADDR_MASK;
    if (CurrentCr3 != AddressSpace->RootTablePhysical) {
        MmpWriteCr3(AddressSpace->RootTablePhysical);
    }

    return KSTATUS_SUCCESS;
}

KSTATUS MmAsMapPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t PhysicalAddress, uint64_t Flags) {
    uint64_t *Pte;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0 || (VirtualAddress % MM_PAGE_SIZE) != 0u || (PhysicalAddress % MM_PAGE_SIZE) != 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = MmpWalkToPte(AddressSpace, VirtualAddress, 1u, &Pte);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    if ((*Pte & MM_PTE_PRESENT) != 0u) {
        MmpReleasePage(*Pte & MM_PTE_ADDR_MASK);
    }

    *Pte = (PhysicalAddress & MM_PTE_ADDR_MASK) | Flags | MM_PTE_PRESENT;
    MmpInvalidatePage(VirtualAddress);
    return KSTATUS_SUCCESS;
}

KSTATUS MmAsUnmapPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress) {
    uint64_t *Pte;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0 || (VirtualAddress % MM_PAGE_SIZE) != 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = MmpWalkToPte(AddressSpace, VirtualAddress, 0u, &Pte);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    if ((*Pte & MM_PTE_PRESENT) == 0u) {
        return KSTATUS_NOT_FOUND;
    }

    MmpReleasePage(*Pte & MM_PTE_ADDR_MASK);
    *Pte = 0u;
    MmpInvalidatePage(VirtualAddress);
    return KSTATUS_SUCCESS;
}

KSTATUS MmAsProtectPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t NewFlags) {
    uint64_t *Pte;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0 || (VirtualAddress % MM_PAGE_SIZE) != 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = MmpWalkToPte(AddressSpace, VirtualAddress, 0u, &Pte);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    if ((*Pte & MM_PTE_PRESENT) == 0u) {
        return KSTATUS_NOT_FOUND;
    }

    *Pte = (*Pte & MM_PTE_ADDR_MASK) | NewFlags | MM_PTE_PRESENT;
    MmpInvalidatePage(VirtualAddress);
    return KSTATUS_SUCCESS;
}

KSTATUS MmAsQueryPte(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t *OutPteValue) {
    uint64_t *Pte;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0 || OutPteValue == 0 || (VirtualAddress % MM_PAGE_SIZE) != 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Status = MmpWalkToPte(AddressSpace, VirtualAddress, 0u, &Pte);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    *OutPteValue = *Pte;
    return KSTATUS_SUCCESS;
}

KSTATUS MmCreateSectionHandle(uint64_t Size, uint32_t DesiredAccess, uint32_t *OutHandle) {
    uint64_t Bytes;
    uint32_t PageCount;
    uint32_t SlotIndex;
    OB_OBJECT_HEADER *Header;
    OB_HANDLE_TABLE *HandleTable;
    KSTATUS Status;
    uint32_t Index;

    if (gMmReady == 0u || OutHandle == 0 || gMmSectionType == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Bytes = MmpAlignSizeToPage(Size);
    PageCount = (uint32_t)(Bytes / MM_PAGE_SIZE);
    if (PageCount == 0u || PageCount > MM_MAX_SECTION_PAGES) {
        return KSTATUS_INVALID_PARAMETER;
    }

    SlotIndex = MM_MAX_SECTION_OBJECTS;
    for (Index = 0u; Index < MM_MAX_SECTION_OBJECTS; Index++) {
        if (gMmSectionInUse[Index] == 0u) {
            SlotIndex = Index;
            break;
        }
    }
    if (SlotIndex >= MM_MAX_SECTION_OBJECTS) {
        return KSTATUS_INIT_FAILED;
    }

    gMmSectionInUse[SlotIndex] = 1u;
    gMmSectionPool[SlotIndex].Header = 0;
    gMmSectionPool[SlotIndex].PageCount = 0u;
    for (Index = 0u; Index < MM_MAX_SECTION_PAGES; Index++) {
        gMmSectionPool[SlotIndex].Pages[Index] = 0u;
    }

    Status = ObCreateObject(gMmSectionType,
                            "mm.section",
                            MM_SECTION_ACCESS_ALL,
                            0u,
                            &Header);
    if (Status != KSTATUS_SUCCESS) {
        gMmSectionInUse[SlotIndex] = 0u;
        return Status;
    }

    gMmSectionPool[SlotIndex].Header = Header;
    gMmSectionPool[SlotIndex].PageCount = PageCount;

    for (Index = 0u; Index < PageCount; Index++) {
        uint64_t Pa;
        uint8_t *PagePtr;
        uint32_t Byte;

        Status = MmAllocPage(&Pa);
        if (Status != KSTATUS_SUCCESS) {
            (void)ObDereferenceObject(Header);
            return Status;
        }

        PagePtr = MmpPhysToVirtPage(Pa);
        for (Byte = 0u; Byte < MM_PAGE_SIZE; Byte++) {
            PagePtr[Byte] = 0u;
        }
        gMmSectionPool[SlotIndex].Pages[Index] = Pa;
    }

    HandleTable = ObGetKernelHandleTable();
    Status = ObInsertHandle(HandleTable, Header, DesiredAccess, OutHandle);
    (void)ObDereferenceObject(Header);
    return Status;
}

KSTATUS MmMapSectionHandle(MM_ADDRESS_SPACE *AddressSpace,
                           uint32_t HandleValue,
                           uint32_t Protection,
                           uint64_t *OutVirtualBase) {
    OB_OBJECT_HEADER *Header;
    MM_SECTION_OBJECT *Section;
    MM_VA_REGION *Region;
    OB_HANDLE_TABLE *HandleTable;
    uint64_t MapSize;
    uint64_t VirtualBase;
    uint32_t DesiredAccess;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0 || OutVirtualBase == 0 || gMmSectionType == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }
    if (AddressSpace->RegionCount >= MM_MAX_VA_REGIONS) {
        return KSTATUS_INIT_FAILED;
    }

    DesiredAccess = MM_SECTION_ACCESS_MAP_READ;
    if ((Protection & MM_PROT_WRITE) != 0u) {
        DesiredAccess |= MM_SECTION_ACCESS_MAP_WRITE;
    }

    HandleTable = ObGetKernelHandleTable();
    Status = ObReferenceObjectByHandle(HandleTable,
                                       HandleValue,
                                       DesiredAccess,
                                       gMmSectionType,
                                       &Header);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Section = MmpFindSectionByHeader(Header);
    if (Section == 0) {
        (void)ObDereferenceObject(Header);
        return KSTATUS_NOT_FOUND;
    }

    MapSize = (uint64_t)Section->PageCount * MM_PAGE_SIZE;
    VirtualBase = MmpAlignUp(AddressSpace->NextUserBase, MM_PAGE_SIZE);

    Region = &AddressSpace->Regions[AddressSpace->RegionCount];
    Region->Base = VirtualBase;
    Region->Size = MapSize;
    Region->Type = MM_REGION_TYPE_SECTION;
    Region->Protection = (Protection == 0u ? MM_PROT_READ : Protection) | MM_PROT_USER;
    Region->Flags = MM_REGION_FLAG_LAZY;
    Region->CommittedPages = 0u;
    Region->BackingPhysicalBase = 0u;
    Region->BackingObject = (uint64_t)(uintptr_t)Header;
    Region->BackingOffset = 0u;

    AddressSpace->RegionCount++;
    AddressSpace->NextUserBase = VirtualBase + MapSize;
    *OutVirtualBase = VirtualBase;
    return KSTATUS_SUCCESS;
}

KSTATUS MmMapAnonymous(MM_ADDRESS_SPACE *AddressSpace,
                       uint64_t Size,
                       uint32_t Protection,
                       uint32_t RegionFlags,
                       uint64_t *OutVirtualBase) {
    MM_VA_REGION *Region;
    uint64_t MapSize;
    uint64_t VirtualBase;
    uint32_t EffectiveFlags;

    if (gMmReady == 0u || AddressSpace == 0 || OutVirtualBase == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if (AddressSpace->RegionCount >= MM_MAX_VA_REGIONS) {
        return KSTATUS_INIT_FAILED;
    }
    NK_ASSERT(AddressSpace->RegionCount < MM_MAX_VA_REGIONS);

    MapSize = MmpAlignSizeToPage(Size);
    VirtualBase = MmpAlignUp(AddressSpace->NextUserBase, MM_PAGE_SIZE);

    Region = &AddressSpace->Regions[AddressSpace->RegionCount];
    Region->Base = VirtualBase;
    Region->Size = MapSize;
    Region->Type = MM_REGION_TYPE_ANON;
    Region->Protection = Protection | MM_PROT_USER;
    EffectiveFlags = RegionFlags;
    if ((EffectiveFlags & (MM_REGION_FLAG_LAZY | MM_REGION_FLAG_WIRED)) == 0u) {
        EffectiveFlags |= MM_REGION_FLAG_LAZY;
    }
    Region->Flags = EffectiveFlags;
    Region->CommittedPages = 0u;
    Region->BackingPhysicalBase = 0u;
    Region->BackingObject = 0u;
    Region->BackingOffset = 0u;

    if ((Region->Flags & MM_REGION_FLAG_WIRED) != 0u) {
        uint64_t Cursor;
        for (Cursor = 0; Cursor < MapSize; Cursor += MM_PAGE_SIZE) {
            uint64_t Pa;
            uint8_t *PagePtr;
            uint32_t i;
            uint64_t Flags;

            if (MmAllocPage(&Pa) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            PagePtr = MmpPhysToVirtPage(Pa);
            for (i = 0; i < MM_PAGE_SIZE; i++) {
                PagePtr[i] = 0u;
            }

            Flags = MmpProtectionToPte(Region->Protection, Region->Flags);
            if (MmAsMapPage(AddressSpace, VirtualBase + Cursor, Pa, Flags) != KSTATUS_SUCCESS) {
                return KSTATUS_INIT_FAILED;
            }

            Region->CommittedPages++;
        }
    }

    AddressSpace->RegionCount++;
    AddressSpace->NextUserBase = VirtualBase + MapSize;
    *OutVirtualBase = VirtualBase;
    return KSTATUS_SUCCESS;
}

static MM_VA_REGION *MmpFindRegion(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress) {
    uint32_t Index;

    for (Index = 0; Index < AddressSpace->RegionCount; Index++) {
        MM_VA_REGION *Region = &AddressSpace->Regions[Index];
        if (VirtualAddress >= Region->Base && VirtualAddress < (Region->Base + Region->Size)) {
            return Region;
        }
    }

    return 0;
}

static MM_SECTION_OBJECT *MmpFindSectionByHeader(OB_OBJECT_HEADER *Header) {
    uint32_t Index;
    if (Header == 0) {
        return 0;
    }

    for (Index = 0; Index < MM_MAX_SECTION_OBJECTS; Index++) {
        if (gMmSectionInUse[Index] != 0u && gMmSectionPool[Index].Header == Header) {
            return &gMmSectionPool[Index];
        }
    }
    return 0;
}

static void MmpDeleteSectionObject(OB_OBJECT_HEADER *Object) {
    MM_SECTION_OBJECT *Section = MmpFindSectionByHeader(Object);
    uint32_t Index;

    if (Section == 0) {
        return;
    }

    for (Index = 0; Index < Section->PageCount; Index++) {
        if (Section->Pages[Index] != 0u) {
            (void)MmFreePage(Section->Pages[Index]);
            Section->Pages[Index] = 0u;
        }
    }

    for (Index = 0; Index < MM_MAX_SECTION_OBJECTS; Index++) {
        if (&gMmSectionPool[Index] == Section) {
            gMmSectionInUse[Index] = 0u;
            Section->Header = 0;
            Section->PageCount = 0u;
            return;
        }
    }
}

static uint64_t MmpProtectionToPte(uint32_t Protection, uint32_t RegionFlags) {
    uint64_t Flags = 0u;

    if ((Protection & MM_PROT_WRITE) != 0u && (RegionFlags & MM_REGION_FLAG_COW) == 0u) {
        Flags |= MM_PTE_WRITABLE;
    }
    if ((Protection & MM_PROT_USER) != 0u) {
        Flags |= MM_PTE_USER;
    }
    if ((Protection & MM_PROT_EXECUTE) == 0u) {
        Flags |= MM_PTE_NX;
    }
    if ((RegionFlags & MM_REGION_FLAG_COW) != 0u) {
        Flags |= MM_PTE_SOFT_COW;
    }

    return Flags;
}

KSTATUS MmHandlePageFault(MM_ADDRESS_SPACE *AddressSpace, uint64_t FaultAddress, uint64_t ErrorCode) {
    MM_VA_REGION *Region;
    uint64_t PageVa;
    uint64_t Pte;
    KSTATUS Status;

    if (gMmReady == 0u || AddressSpace == 0) {
        return KSTATUS_INVALID_PARAMETER;
    }

    Region = MmpFindRegion(AddressSpace, FaultAddress);
    if (Region == 0 || (Region->Flags & MM_REGION_FLAG_GUARD) != 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    if ((ErrorCode & MM_PF_ERROR_WRITE) != 0u && (Region->Protection & MM_PROT_WRITE) == 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    if ((ErrorCode & MM_PF_ERROR_USER) != 0u && (Region->Protection & MM_PROT_USER) == 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    if ((ErrorCode & MM_PF_ERROR_INSTR) != 0u && (Region->Protection & MM_PROT_EXECUTE) == 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    PageVa = MmpAlignDown(FaultAddress, MM_PAGE_SIZE);
    Status = MmAsQueryPte(AddressSpace, PageVa, &Pte);

    if ((ErrorCode & MM_PF_ERROR_PRESENT) != 0u) {
        if (Status == KSTATUS_SUCCESS && (Pte & MM_PTE_SOFT_COW) != 0u && (ErrorCode & MM_PF_ERROR_WRITE) != 0u) {
            uint64_t OldPa = Pte & MM_PTE_ADDR_MASK;
            uint64_t OldPageIndex = OldPa / MM_PAGE_SIZE;

            if (OldPageIndex < MM_MAX_TRACKED_PAGES && gMmPageRefCount[OldPageIndex] == 1u) {
                return MmAsProtectPage(AddressSpace, PageVa, (Pte & ~MM_PTE_SOFT_COW) | MM_PTE_WRITABLE);
            } else {
                uint64_t NewPa;
                uint8_t *OldPtr;
                uint8_t *NewPtr;
                uint32_t i;

                if (MmAllocPage(&NewPa) != KSTATUS_SUCCESS) {
                    return KSTATUS_INIT_FAILED;
                }

                OldPtr = MmpPhysToVirtPage(OldPa);
                NewPtr = MmpPhysToVirtPage(NewPa);
                for (i = 0; i < MM_PAGE_SIZE; i++) {
                    NewPtr[i] = OldPtr[i];
                }

                MmpReleasePage(OldPa);
                return MmAsMapPage(AddressSpace,
                                   PageVa,
                                   NewPa,
                                   ((Pte & ~MM_PTE_SOFT_COW) | MM_PTE_WRITABLE) & ~MM_PTE_ADDR_MASK);
            }
        }

        return KSTATUS_ACCESS_DENIED;
    }

    if (Region->Type == MM_REGION_TYPE_ANON) {
        uint64_t NewPa;
        uint8_t *PagePtr;
        uint32_t i;
        uint64_t Flags;

        if (MmAllocPage(&NewPa) != KSTATUS_SUCCESS) {
            return KSTATUS_INIT_FAILED;
        }

        PagePtr = MmpPhysToVirtPage(NewPa);
        for (i = 0; i < MM_PAGE_SIZE; i++) {
            PagePtr[i] = 0u;
        }

        Flags = MmpProtectionToPte(Region->Protection, Region->Flags);
        Status = MmAsMapPage(AddressSpace, PageVa, NewPa, Flags);
        if (Status != KSTATUS_SUCCESS) {
            return Status;
        }

        Region->CommittedPages++;
        return KSTATUS_SUCCESS;
    }

    if (Region->Type == MM_REGION_TYPE_PHYS) {
        uint64_t Offset = PageVa - Region->Base;
        uint64_t Pa = Region->BackingPhysicalBase + Offset;

        MmpRetainPage(Pa);
        return MmAsMapPage(AddressSpace, PageVa, Pa, MmpProtectionToPte(Region->Protection, Region->Flags));
    }

    if (Region->Type == MM_REGION_TYPE_SECTION) {
        OB_OBJECT_HEADER *Header = (OB_OBJECT_HEADER *)(uintptr_t)Region->BackingObject;
        MM_SECTION_OBJECT *Section = MmpFindSectionByHeader(Header);
        uint64_t Offset = (PageVa - Region->Base) + Region->BackingOffset;
        uint32_t PageIndex;
        uint64_t Pa;

        if (Section == 0) {
            return KSTATUS_NOT_FOUND;
        }

        PageIndex = (uint32_t)(Offset / MM_PAGE_SIZE);
        if (PageIndex >= Section->PageCount) {
            return KSTATUS_ACCESS_DENIED;
        }

        Pa = Section->Pages[PageIndex];
        MmpRetainPage(Pa);
        Status = MmAsMapPage(AddressSpace,
                             PageVa,
                             Pa,
                             MmpProtectionToPte(Region->Protection, Region->Flags));
        if (Status != KSTATUS_SUCCESS) {
            return Status;
        }

        Region->CommittedPages++;
        return KSTATUS_SUCCESS;
    }

    return KSTATUS_NOT_IMPLEMENTED;
}

static void MmpOnPageFault(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer) {
    KPROCESS *Process = PsGetCurrentProcess();
    MM_ADDRESS_SPACE *AddressSpace = 0;
    MM_VA_REGION *Region = 0;
    KSTATUS Status;
    uint64_t FaultCr3 = MmpReadCr3() & MM_PTE_ADDR_MASK;

    if (Process != 0) {
        AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
        if (AddressSpace != 0) {
            Region = MmpFindRegion(AddressSpace, FaultAddress);
        }
    }

    Status = MmHandlePageFault(AddressSpace, FaultAddress, ErrorCode);
    MmpRecordFault(AddressSpace, FaultAddress, ErrorCode, InstructionPointer, Status);
    if (Status != KSTATUS_SUCCESS) {
        MM_FAULT_SNAPSHOT Snapshot;
        MM_ADDRESS_SPACE Cr3View;
        Snapshot.AddressSpace = AddressSpace;
        Snapshot.Region = Region;
        Snapshot.FaultAddress = FaultAddress;
        Snapshot.InstructionPointer = InstructionPointer;
        Snapshot.ErrorCode = ErrorCode;
        MmpWriteHexLine("[MMFLT] CR3=", FaultCr3);
        if (AddressSpace != 0) {
            MmpWriteHexLine("[MMFLT] AS.ROOT=", AddressSpace->RootTablePhysical);
        }
        MmDumpFaultSnapshot(&Snapshot);
        Cr3View.AddressSpaceId = 0u;
        Cr3View.OwnerProcessId = 0u;
        Cr3View.RootTablePhysical = FaultCr3;
        Cr3View.NextUserBase = 0u;
        Cr3View.RegionCount = 0u;
        HalWriteDebugString("[MMDBG] translation via current CR3\n");
        MmDumpAddressTranslation(&Cr3View, FaultAddress);
        HalWriteDebugString("[MM] fatal page fault\n");
        HalHalt();
    }
}

void MmDumpFaultSnapshot(const MM_FAULT_SNAPSHOT *Snapshot) {
    KTHREAD *Thread = PsGetCurrentThread();
    KPROCESS *Process = PsGetCurrentProcess();

    if (Snapshot == 0) {
        HalWriteDebugString("[MMFLT] null snapshot\n");
        return;
    }

    MmpWriteHexLine("[MMFLT] VA=", Snapshot->FaultAddress);
    MmpWriteHexLine("[MMFLT] RIP=", Snapshot->InstructionPointer);
    MmpWriteHexLine("[MMFLT] ERR=", Snapshot->ErrorCode);
    MmpWriteAccessType(Snapshot->ErrorCode);
    if (Process != 0) {
        MmpWriteHexLine("[MMFLT] PID=", Process->ProcessId);
    }
    if (Thread != 0) {
        MmpWriteHexLine("[MMFLT] TID=", Thread->ThreadId);
    }
    if (Snapshot->AddressSpace != 0) {
        MmpWriteHexLine("[MMFLT] AS=", Snapshot->AddressSpace->AddressSpaceId);
        MmDumpAddressTranslation(Snapshot->AddressSpace, Snapshot->FaultAddress);
    }
    MmpDumpRegion(Snapshot->Region);
}

void MmDumpCurrentFaultContext(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer) {
    MM_FAULT_SNAPSHOT Snapshot;
    KPROCESS *Process = PsGetCurrentProcess();
    MM_ADDRESS_SPACE *AddressSpace = 0;
    MM_VA_REGION *Region = 0;

    if (Process != 0) {
        AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
    }
    if (AddressSpace != 0) {
        Region = MmpFindRegion(AddressSpace, FaultAddress);
    }

    Snapshot.AddressSpace = AddressSpace;
    Snapshot.Region = Region;
    Snapshot.FaultAddress = FaultAddress;
    Snapshot.InstructionPointer = InstructionPointer;
    Snapshot.ErrorCode = ErrorCode;
    MmDumpFaultSnapshot(&Snapshot);
}

void MmDumpAddressTranslation(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress) {
    uint64_t L4 = (VirtualAddress >> 39) & 0x1FFu;
    uint64_t L3 = (VirtualAddress >> 30) & 0x1FFu;
    uint64_t L2 = (VirtualAddress >> 21) & 0x1FFu;
    uint64_t L1 = (VirtualAddress >> 12) & 0x1FFu;
    uint64_t *Pml4;
    uint64_t Pml4e;
    uint64_t *Pdpt;
    uint64_t Pdpte;
    uint64_t *Pd;
    uint64_t Pde;
    uint64_t *Pt;
    uint64_t Pte;

    if (AddressSpace == 0) {
        HalWriteDebugString("[MMDBG] no address space\n");
        return;
    }

    MmpWriteHexLine("[MMDBG] VA=", VirtualAddress);

    Pml4 = MmpPhysToVirtTable(AddressSpace->RootTablePhysical);
    Pml4e = Pml4[L4];
    MmpWriteHexLine("[MMDBG] PML4E=", Pml4e);
    MmpWriteHexLine("[MMDBG] PML4E.P=", (uint64_t)((Pml4e & MM_PTE_PRESENT) != 0u));
    MmpWriteHexLine("[MMDBG] PML4E.U=", (uint64_t)((Pml4e & MM_PTE_USER) != 0u));
    MmpWriteHexLine("[MMDBG] PML4E.RW=", (uint64_t)((Pml4e & MM_PTE_WRITABLE) != 0u));
    MmpWriteHexLine("[MMDBG] PML4E.NX=", (uint64_t)((Pml4e & MM_PTE_NX) != 0u));
    if ((Pml4e & MM_PTE_PRESENT) == 0u) {
        return;
    }

    Pdpt = MmpPhysToVirtTable(Pml4e & MM_PTE_ADDR_MASK);
    Pdpte = Pdpt[L3];
    MmpWriteHexLine("[MMDBG] PDPTE=", Pdpte);
    MmpWriteHexLine("[MMDBG] PDPTE.P=", (uint64_t)((Pdpte & MM_PTE_PRESENT) != 0u));
    MmpWriteHexLine("[MMDBG] PDPTE.U=", (uint64_t)((Pdpte & MM_PTE_USER) != 0u));
    MmpWriteHexLine("[MMDBG] PDPTE.RW=", (uint64_t)((Pdpte & MM_PTE_WRITABLE) != 0u));
    MmpWriteHexLine("[MMDBG] PDPTE.NX=", (uint64_t)((Pdpte & MM_PTE_NX) != 0u));
    if ((Pdpte & MM_PTE_PRESENT) == 0u) {
        return;
    }

    Pd = MmpPhysToVirtTable(Pdpte & MM_PTE_ADDR_MASK);
    Pde = Pd[L2];
    MmpWriteHexLine("[MMDBG] PDE=", Pde);
    MmpWriteHexLine("[MMDBG] PDE.P=", (uint64_t)((Pde & MM_PTE_PRESENT) != 0u));
    MmpWriteHexLine("[MMDBG] PDE.U=", (uint64_t)((Pde & MM_PTE_USER) != 0u));
    MmpWriteHexLine("[MMDBG] PDE.RW=", (uint64_t)((Pde & MM_PTE_WRITABLE) != 0u));
    MmpWriteHexLine("[MMDBG] PDE.NX=", (uint64_t)((Pde & MM_PTE_NX) != 0u));
    if ((Pde & MM_PTE_PRESENT) == 0u) {
        return;
    }

    Pt = MmpPhysToVirtTable(Pde & MM_PTE_ADDR_MASK);
    Pte = Pt[L1];
    MmpWriteHexLine("[MMDBG] PTE=", Pte);
    MmpWriteHexLine("[MMDBG] PTE.P=", (uint64_t)((Pte & MM_PTE_PRESENT) != 0u));
    MmpWriteHexLine("[MMDBG] PTE.U=", (uint64_t)((Pte & MM_PTE_USER) != 0u));
    MmpWriteHexLine("[MMDBG] PTE.RW=", (uint64_t)((Pte & MM_PTE_WRITABLE) != 0u));
    MmpWriteHexLine("[MMDBG] PTE.NX=", (uint64_t)((Pte & MM_PTE_NX) != 0u));
}

void MmDumpAddressSpaceRegions(MM_ADDRESS_SPACE *AddressSpace) {
    uint32_t Index;

    if (AddressSpace == 0) {
        HalWriteDebugString("[MMDBG] regions: <null address space>\n");
        return;
    }

    MmpWriteHexLine("[MMDBG] as.id=", AddressSpace->AddressSpaceId);
    MmpWriteHexLine("[MMDBG] as.owner=", AddressSpace->OwnerProcessId);
    MmpWriteHexLine("[MMDBG] as.region_count=", AddressSpace->RegionCount);
    for (Index = 0u; Index < AddressSpace->RegionCount; Index++) {
        MM_VA_REGION *Region = &AddressSpace->Regions[Index];
        MmpWriteHexLine("[MMDBG] region.base=", Region->Base);
        MmpWriteHexLine("[MMDBG] region.size=", Region->Size);
        MmpWriteHexLine("[MMDBG] region.type=", Region->Type);
        MmpWriteHexLine("[MMDBG] region.prot=", Region->Protection);
        MmpWriteHexLine("[MMDBG] region.flags=", Region->Flags);
        MmpWriteHexLine("[MMDBG] region.commit=", Region->CommittedPages);
        MmpWriteHexLine("[MMDBG] region.backing=", Region->BackingPhysicalBase);
    }
}

void MmDumpRecentFaults(void) {
    uint32_t Count;
    uint32_t i;

    Count = (gMmFaultTraceNext < MM_FAULT_TRACE_DEPTH) ? gMmFaultTraceNext : MM_FAULT_TRACE_DEPTH;
    for (i = 0; i < Count; i++) {
        uint32_t Index = (gMmFaultTraceNext - Count + i) % MM_FAULT_TRACE_DEPTH;
        MM_FAULT_TRACE *Entry = &gMmFaultTrace[Index];
        MmpWriteHexLine("[MMFLT] VA=", Entry->FaultAddress);
        MmpWriteHexLine("[MMFLT] RIP=", Entry->InstructionPointer);
        MmpWriteHexLine("[MMFLT] ERR=", Entry->ErrorCode);
        MmpWriteHexLine("[MMFLT] PID=", Entry->ProcessId);
        MmpWriteHexLine("[MMFLT] AS=", Entry->AddressSpaceId);
        MmpWriteHexLine("[MMFLT] OUT=", (uint64_t)Entry->Outcome);
    }
}
