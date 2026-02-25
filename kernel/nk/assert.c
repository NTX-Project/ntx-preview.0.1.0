#include "../../hal/inc/hal.h"
#include "../include/mm.h"
#include "../include/nkassert.h"
#include "../include/ps.h"

static void NkHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t Index;
    for (Index = 0; Index < 16u; Index++) {
        uint32_t Shift = (15u - Index) * 4u;
        Out[Index] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void NkWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[128];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }

    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    NkHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

void NkAssertFail(const char *Expression, const char *File, uint32_t Line, const char *Function) {
    KTHREAD *Thread = PsGetCurrentThread();
    KPROCESS *Process = PsGetCurrentProcess();

    HalWriteDebugString("[ASSERT] kernel assertion failed\n");
    HalWriteDebugString("[ASSERT] expr: ");
    HalWriteDebugString((Expression != 0) ? Expression : "<null>");
    HalWriteDebugString("\n");
    HalWriteDebugString("[ASSERT] file: ");
    HalWriteDebugString((File != 0) ? File : "<null>");
    HalWriteDebugString("\n");
    HalWriteDebugString("[ASSERT] func: ");
    HalWriteDebugString((Function != 0) ? Function : "<null>");
    HalWriteDebugString("\n");
    NkWriteHexLine("[ASSERT] line=", (uint64_t)Line);
    if (Process != 0) {
        NkWriteHexLine("[ASSERT] pid=", Process->ProcessId);
    }
    if (Thread != 0) {
        NkWriteHexLine("[ASSERT] tid=", Thread->ThreadId);
    }

    MmDumpRecentFaults();
    HalHalt();
}
