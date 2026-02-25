#ifndef EX_H
#define EX_H

#include <stdint.h>

#include "kstatus.h"

typedef KSTATUS (*EX_MODULE_INITIALIZE_ROUTINE)(void);

typedef struct EX_MODULE_DESCRIPTOR {
    const char *Name;
    EX_MODULE_INITIALIZE_ROUTINE Initialize;
    uint32_t Flags;
} EX_MODULE_DESCRIPTOR;

KSTATUS ExInitializeExecutive(void);
KSTATUS ExInitializeHybridServices(void);

#endif /* EX_H */
