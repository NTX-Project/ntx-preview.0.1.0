#include "../../hal/inc/hal.h"
#include "../include/se.h"

static uint32_t gSeReady;
static uint32_t gSeDefaultPrivilegeMask;

KSTATUS SeInitialize(void) {
    if (gSeReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }

    gSeDefaultPrivilegeMask = 0x0000000Fu;
    gSeReady = 1u;

    HalWriteDebugString("[SE] security manager online\n");
    return KSTATUS_SUCCESS;
}

KSTATUS SeCreateKernelToken(SE_ACCESS_TOKEN *OutToken) {
    if (OutToken == 0 || gSeReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    OutToken->SubjectId = 0u;
    OutToken->PrivilegeMask = 0xFFFFFFFFu;
    OutToken->IntegrityLevel = SE_INTEGRITY_HIGH;
    return KSTATUS_SUCCESS;
}

KSTATUS SeAccessCheck(const SE_ACCESS_TOKEN *Token, uint32_t RequiredMask) {
    if (Token == 0 || gSeReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    if ((Token->PrivilegeMask & RequiredMask) != RequiredMask) {
        return KSTATUS_ACCESS_DENIED;
    }

    if (Token->IntegrityLevel < SE_INTEGRITY_MEDIUM && RequiredMask != 0u) {
        return KSTATUS_ACCESS_DENIED;
    }

    (void)gSeDefaultPrivilegeMask;
    return KSTATUS_SUCCESS;
}
