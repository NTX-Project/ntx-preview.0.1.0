#ifndef CC_H
#define CC_H

#include <stdint.h>

#include "kstatus.h"

typedef struct CC_POLICY {
    uint32_t MaxCachedPages;
    uint32_t DirtyWritebackThresholdPages;
    uint32_t AggressiveTrim;
} CC_POLICY;

KSTATUS CcInitialize(void);
KSTATUS CcQueryPolicy(CC_POLICY *OutPolicy);

#endif /* CC_H */
