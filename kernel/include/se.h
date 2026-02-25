#ifndef SE_H
#define SE_H

#include <stdint.h>

#include "kstatus.h"

typedef struct SE_ACCESS_TOKEN {
    uint64_t SubjectId;
    uint32_t PrivilegeMask;
    uint32_t IntegrityLevel;
} SE_ACCESS_TOKEN;

#define SE_INTEGRITY_LOW 1u
#define SE_INTEGRITY_MEDIUM 2u
#define SE_INTEGRITY_HIGH 3u

KSTATUS SeInitialize(void);
KSTATUS SeCreateKernelToken(SE_ACCESS_TOKEN *OutToken);
KSTATUS SeAccessCheck(const SE_ACCESS_TOKEN *Token, uint32_t RequiredMask);

#endif /* SE_H */
