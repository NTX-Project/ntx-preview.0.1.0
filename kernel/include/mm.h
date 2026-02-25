#ifndef MM_H
#define MM_H

#include <stdint.h>

#include "kstatus.h"

#define MM_PAGE_SIZE 4096u
#define MM_MAX_TRACKED_PAGES 262144u
#define MM_MAX_ADDRESS_SPACES 64u
#define MM_MAX_VA_REGIONS 128u
#define MM_FAULT_TRACE_DEPTH 64u
#define MM_MAX_SECTION_OBJECTS 64u
#define MM_MAX_SECTION_PAGES 256u

#define MM_PROT_READ 0x1u
#define MM_PROT_WRITE 0x2u
#define MM_PROT_EXECUTE 0x4u
#define MM_PROT_USER 0x8u

#define MM_REGION_TYPE_ANON 1u
#define MM_REGION_TYPE_PHYS 2u
#define MM_REGION_TYPE_SECTION 3u

#define MM_REGION_FLAG_LAZY 0x1u
#define MM_REGION_FLAG_WIRED 0x2u
#define MM_REGION_FLAG_COW 0x4u
#define MM_REGION_FLAG_GUARD 0x8u

#define MM_SECTION_ACCESS_MAP_READ 0x00000001u
#define MM_SECTION_ACCESS_MAP_WRITE 0x00000002u
#define MM_SECTION_ACCESS_ALL (MM_SECTION_ACCESS_MAP_READ | MM_SECTION_ACCESS_MAP_WRITE)

typedef struct MM_VA_REGION {
    uint64_t Base;
    uint64_t Size;
    uint32_t Type;
    uint32_t Protection;
    uint32_t Flags;
    uint32_t CommittedPages;
    uint64_t BackingPhysicalBase;
    uint64_t BackingObject;
    uint64_t BackingOffset;
} MM_VA_REGION;

typedef struct MM_ADDRESS_SPACE {
    uint64_t AddressSpaceId;
    uint64_t OwnerProcessId;
    uint64_t RootTablePhysical;
    uint64_t NextUserBase;
    uint32_t RegionCount;
    MM_VA_REGION Regions[MM_MAX_VA_REGIONS];
} MM_ADDRESS_SPACE;

typedef struct MM_FAULT_SNAPSHOT {
    MM_ADDRESS_SPACE *AddressSpace;
    MM_VA_REGION *Region;
    uint64_t FaultAddress;
    uint64_t InstructionPointer;
    uint64_t ErrorCode;
} MM_FAULT_SNAPSHOT;

KSTATUS MmInitialize(void);
KSTATUS MmAllocPage(uint64_t *OutPhysicalAddress);
KSTATUS MmFreePage(uint64_t PhysicalAddress);
KSTATUS MmCreateAddressSpace(uint64_t OwnerProcessId, MM_ADDRESS_SPACE **OutAddressSpace);
MM_ADDRESS_SPACE *MmGetAddressSpaceById(uint64_t AddressSpaceId);
KSTATUS MmActivateAddressSpace(MM_ADDRESS_SPACE *AddressSpace);
KSTATUS MmAsMapPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t PhysicalAddress, uint64_t Flags);
KSTATUS MmAsUnmapPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress);
KSTATUS MmAsProtectPage(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t NewFlags);
KSTATUS MmAsQueryPte(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress, uint64_t *OutPteValue);
KSTATUS MmMapAnonymous(MM_ADDRESS_SPACE *AddressSpace,
                       uint64_t Size,
                       uint32_t Protection,
                       uint32_t RegionFlags,
                       uint64_t *OutVirtualBase);
KSTATUS MmHandlePageFault(MM_ADDRESS_SPACE *AddressSpace, uint64_t FaultAddress, uint64_t ErrorCode);
KSTATUS MmCreateSectionHandle(uint64_t Size, uint32_t DesiredAccess, uint32_t *OutHandle);
KSTATUS MmMapSectionHandle(MM_ADDRESS_SPACE *AddressSpace,
                           uint32_t HandleValue,
                           uint32_t Protection,
                           uint64_t *OutVirtualBase);
void MmDumpAddressTranslation(MM_ADDRESS_SPACE *AddressSpace, uint64_t VirtualAddress);
void MmDumpAddressSpaceRegions(MM_ADDRESS_SPACE *AddressSpace);
void MmDumpRecentFaults(void);
void MmDumpFaultSnapshot(const MM_FAULT_SNAPSHOT *Snapshot);
void MmDumpCurrentFaultContext(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer);

#endif /* MM_H */
