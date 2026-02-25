#include "../../hal/inc/hal.h"
#include "../include/cc.h"
#include "../include/ex.h"
#include "../include/io.h"
#include "../include/lpc.h"
#include "../include/se.h"

KSTATUS MmInitialize(void);
KSTATUS ObInitialize(void);
KSTATUS PsInitialize(void);
KSTATUS ExInitializeHybridServices(void);

static const EX_MODULE_DESCRIPTOR gExBootModules[] = {
    { "OB", ObInitialize, 0u },
    { "SE", SeInitialize, 0u },
    { "MM", MmInitialize, 0u },
    { "CC", CcInitialize, 0u },
    { "IO", IoInitialize, 0u },
    { "PS", PsInitialize, 0u },
    { "LPC", LpcInitialize, 0u }
};

static void ExWriteHexLine(const char *Prefix, uint64_t Value) {
    static const char Hex[] = "0123456789ABCDEF";
    char Buffer[96];
    char Digits[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Digits[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Digits[16] = 0;
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Digits[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

KSTATUS ExInitializeExecutive(void) {
    uint32_t Index;

    HalWriteDebugString("[EX] CORE -> EXEC phase begin\n");

    for (Index = 0; Index < (sizeof(gExBootModules) / sizeof(gExBootModules[0])); Index++) {
        HalWriteDebugString("[EX] module init begin: ");
        HalWriteDebugString(gExBootModules[Index].Name);
        HalWriteDebugString("\n");
        ExWriteHexLine("[EX] module init fn=", (uint64_t)(uintptr_t)gExBootModules[Index].Initialize);
        KSTATUS Status = gExBootModules[Index].Initialize();
        if (Status != KSTATUS_SUCCESS && Status != KSTATUS_ALREADY_INITIALIZED) {
            HalWriteDebugString("[EX] module init failed: ");
            HalWriteDebugString(gExBootModules[Index].Name);
            HalWriteDebugString("\n");
            return Status;
        }
    }

    {
        KSTATUS Status = ExInitializeHybridServices();

        if (Status != KSTATUS_SUCCESS) {
            return Status;
        }
    }

    HalWriteDebugString("[EX] EXEC modules complete\n");
    HalWriteDebugString("[EX] EXEC phase complete\n");
    return KSTATUS_SUCCESS;
}
