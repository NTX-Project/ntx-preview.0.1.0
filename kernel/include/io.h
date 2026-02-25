#ifndef IO_H
#define IO_H

#include <stdint.h>

#include "kstatus.h"

#define IO_FILE_ACCESS_READ 0x00000001u
#define IO_FILE_ACCESS_WRITE 0x00000002u
#define IO_FILE_ACCESS_EXECUTE 0x00000004u
#define IO_FILE_ACCESS_ALL (IO_FILE_ACCESS_READ | IO_FILE_ACCESS_WRITE | IO_FILE_ACCESS_EXECUTE)

KSTATUS IoInitialize(void);
KSTATUS IoRegisterBootFile(const char *Path, const void *Data, uint64_t Size);
KSTATUS IoOpenPath(const char *Path, uint32_t DesiredAccess, uint32_t *OutHandle);
KSTATUS IoReadFileHandle(uint32_t HandleValue,
                         uint64_t Offset,
                         void *OutBuffer,
                         uint64_t BufferLength,
                         uint64_t *OutBytesRead);
KSTATUS IoCloseFileHandle(uint32_t HandleValue);
KSTATUS IoSpawnProcessFromFileHandle(uint32_t HandleValue,
                                     uint64_t *OutProcessId,
                                     uint64_t *OutEntryAddress);
void IoDumpFiles(void);

#endif /* IO_H */
