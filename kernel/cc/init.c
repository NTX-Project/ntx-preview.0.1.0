#include "../../hal/inc/hal.h"
#include "../include/cc.h"

static CC_POLICY gCcPolicy;
static uint32_t gCcReady;

KSTATUS CcInitialize(void) {
    HAL_PLATFORM_INFO PlatformInfo;
    uint64_t EstimatedPages;

    if (gCcReady != 0u) {
        return KSTATUS_ALREADY_INITIALIZED;
    }

    HalQueryPlatformInfo(&PlatformInfo);

    EstimatedPages = PlatformInfo.PhysicalMemoryTop >> 12;
    if (EstimatedPages == 0u) {
        EstimatedPages = 4096u;
    }

    gCcPolicy.MaxCachedPages = (uint32_t)(EstimatedPages / 8u);
    gCcPolicy.DirtyWritebackThresholdPages = gCcPolicy.MaxCachedPages / 4u;
    gCcPolicy.AggressiveTrim = 0u;

    gCcReady = 1u;
    HalWriteDebugString("[CC] cache manager online\n");
    return KSTATUS_SUCCESS;
}

KSTATUS CcQueryPolicy(CC_POLICY *OutPolicy) {
    if (OutPolicy == 0 || gCcReady == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    *OutPolicy = gCcPolicy;
    return KSTATUS_SUCCESS;
}
