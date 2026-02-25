#ifndef KPEFORMAT_H
#define KPEFORMAT_H

#include <stdint.h>

#define KPE_DOS_MAGIC 0x5A4Du        /* "MZ" */
#define KPE_SIGNATURE 0x3045504Bu    /* "KPE0" */
#define KPE_OPT_MAGIC_AUR64 0xA641u  /* custom 64-bit optional header */
#define KPE_MACHINE_AUR64 0xA641u

#define KPE_NUMBEROF_DIRECTORY_ENTRIES 16u

typedef enum KPE_CHARACTERISTICS {
    KPE_FILE_EXECUTABLE_IMAGE = 0x0002,
    KPE_FILE_LARGE_ADDRESS_AWARE = 0x0020
} KPE_CHARACTERISTICS;

typedef enum KPE_DLL_CHARACTERISTICS {
    KPE_DLLCHAR_DYNAMIC_BASE = 0x0040,
    KPE_DLLCHAR_NX_COMPAT = 0x0100
} KPE_DLL_CHARACTERISTICS;

typedef enum KPE_SECTION_FLAGS {
    KPE_SCN_CNT_CODE = 0x00000020,
    KPE_SCN_CNT_INITIALIZED_DATA = 0x00000040,
    KPE_SCN_MEM_EXECUTE = 0x20000000,
    KPE_SCN_MEM_READ = 0x40000000,
    KPE_SCN_MEM_WRITE = 0x80000000
} KPE_SECTION_FLAGS;

typedef enum KPE_DIRECTORY_INDEX {
    KPE_DIR_EXPORT = 0,
    KPE_DIR_IMPORT = 1,
    KPE_DIR_BASERELOC = 5,
    KPE_DIR_DEBUG = 6
} KPE_DIRECTORY_INDEX;

typedef enum KPE_BASE_RELOC_TYPE {
    KPE_REL_BASED_ABSOLUTE = 0,
    KPE_REL_BASED_DIR64 = 10
} KPE_BASE_RELOC_TYPE;

typedef enum KPE_SUBSYSTEM {
    KPE_SUBSYSTEM_NATIVE_KERNEL = 1,
    KPE_SUBSYSTEM_USERLAND = 2,
    KPE_SUBSYSTEM_SYSTEM_ADDON = 3
} KPE_SUBSYSTEM;

#pragma pack(push, 1)
typedef struct KPE_DOS_HEADER {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} KPE_DOS_HEADER;

typedef struct KPE_FILE_HEADER {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} KPE_FILE_HEADER;

typedef struct KPE_DATA_DIRECTORY {
    uint32_t VirtualAddress;
    uint32_t Size;
} KPE_DATA_DIRECTORY;

typedef struct KPE_OPTIONAL_HEADER64 {
    uint16_t Magic;
    uint8_t MajorLinkerVersion;
    uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    KPE_DATA_DIRECTORY DataDirectory[KPE_NUMBEROF_DIRECTORY_ENTRIES];
} KPE_OPTIONAL_HEADER64;

typedef struct KPE_NT_HEADERS64 {
    uint32_t Signature;
    KPE_FILE_HEADER FileHeader;
    KPE_OPTIONAL_HEADER64 OptionalHeader;
} KPE_NT_HEADERS64;

typedef struct KPE_SECTION_HEADER {
    uint8_t Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} KPE_SECTION_HEADER;

typedef struct KPE_BASE_RELOCATION_BLOCK {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} KPE_BASE_RELOCATION_BLOCK;
#pragma pack(pop)

#endif /* KPEFORMAT_H */
