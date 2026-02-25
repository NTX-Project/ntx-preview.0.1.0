#ifndef BOOTPROTO_H
#define BOOTPROTO_H

#include <stdint.h>

#define BOOTINFO_MAGIC 0x49425441u /* "ATBI" */
#define BOOTINFO_VERSION 1u
#define BOOTINFO_FLAG_VALIDATE 0x1u
#define BOOTINFO_FLAG_INIT_IMAGE_PRESENT 0x2u
#define BOOTINFO_FLAG_VERBOSE_BOOT 0x4u

typedef struct PHYSICAL_MEMORY_RANGE {
    uint64_t Base;
    uint64_t Length;
    uint32_t Type;
    uint32_t Attributes;
} PHYSICAL_MEMORY_RANGE;

typedef struct BOOT_VIDEO_INFO {
    uint64_t FrameBufferBase;
    uint32_t FrameBufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;
} BOOT_VIDEO_INFO;

typedef struct BOOT_INFO {
    uint32_t Magic;
    uint16_t Version;
    uint16_t Size;
    uint32_t Flags;
    uint64_t FirmwareType; /* 1 = UEFI */
    uint64_t LoaderBase;
    uint64_t KernelBase;
    uint64_t KernelSize;
    uint64_t KernelEntry;
    uint64_t InitImageBase;
    uint64_t InitImageSize;
    uint64_t MemoryMap;
    uint32_t MemoryMapEntryCount;
    uint32_t MemoryMapEntrySize;
    BOOT_VIDEO_INFO Video;
} BOOT_INFO;

#endif /* BOOTPROTO_H */
