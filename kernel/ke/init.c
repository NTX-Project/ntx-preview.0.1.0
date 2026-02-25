#include "../../boot/include/bootproto.h"
#include "../../hal/inc/hal.h"
#include "../include/io.h"
#include "../include/nk.h"
#include "../include/kstatus.h"
#include "../include/ps.h"

KSTATUS ExInitializeExecutive(void);
KSTATUS KeRunValidationSuite(void);

static void KiWriteHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void KiWriteStatusLine(const char *Prefix, KSTATUS Status) {
    char Buffer[96];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    KiWriteHex64((uint64_t)(uint32_t)Status, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static void KiEarlySerialMarker(void) {
    uint8_t Lsr;
    uint32_t Spins;
    const char *Text = "[KE] kernel entry reached\n";
    const char *Cursor = Text;
    while (*Cursor != 0) {
        char Ch = *Cursor++;
        Spins = 0u;
        do {
            __asm__ __volatile__("inb %1, %0" : "=a"(Lsr) : "Nd"((uint16_t)(0x3F8 + 5)));
            Spins++;
            if (Spins >= 1000000u) {
                return;
            }
        } while ((Lsr & 0x20u) == 0u);
        __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)Ch), "Nd"((uint16_t)0x3F8));
        if (Ch == '\n') {
            Spins = 0u;
            do {
                __asm__ __volatile__("inb %1, %0" : "=a"(Lsr) : "Nd"((uint16_t)(0x3F8 + 5)));
                Spins++;
                if (Spins >= 1000000u) {
                    return;
                }
            } while ((Lsr & 0x20u) == 0u);
            __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3F8));
        }
    }
}

static void KiShowBootSplash(void) {
    HalWriteDebugString("\n");
    HalWriteDebugString("========================================\n");
    HalWriteDebugString("               Nova Kernel              \n");
    HalWriteDebugString("========================================\n");
    HalWriteDebugString("Press ESC for verbose (pending input driver)\n");
}

static void KiLaunchInitProcess(void) {
    uint32_t InitHandle = 0u;
    uint64_t ProcessId = 0u;
    uint64_t Entry = 0u;
    KSTATUS Status;

    HalWriteDebugString("[KE] stage: loading \\System\\init.kpe\n");
    Status = IoOpenPath("\\System\\init.kpe", IO_FILE_ACCESS_READ, &InitHandle);
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] init image not available\n");
        KiWriteStatusLine("[KE] init open status=", Status);
        return;
    }

    HalWriteDebugString("[KE] stage: spawning init\n");
    Status = IoSpawnProcessFromFileHandle(InitHandle, &ProcessId, &Entry);
    (void)IoCloseFileHandle(InitHandle);
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] init spawn failed\n");
        KiWriteStatusLine("[KE] init spawn status=", Status);
        return;
    }

    HalWriteDebugString("[KE] init process launched\n");
    HalWriteDebugString("[KE] CLI bridge online: user init.kpe on serial COM1\n");
    HalClearDisplay();
}

static KSTATUS KiInitializeEarly(const BOOT_INFO *BootInfo) {
    if (BootInfo == 0 || BootInfo->Magic != BOOTINFO_MAGIC) {
        return KSTATUS_INVALID_PARAMETER;
    }

    HalInitializePhase0(BootInfo);
    return KSTATUS_SUCCESS;
}

__attribute__((section(".text.start"), used, noreturn)) void AsterKernelEntry(BOOT_INFO *BootInfo) {
    KSTATUS Status;

    KiEarlySerialMarker();
    Status = KiInitializeEarly(BootInfo);
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] early init failed\n");
        HalHalt();
    }

    HalWriteDebugString("[KE] EARLY complete\n");
    KiShowBootSplash();
    HalWriteDebugString("[KE] stage: nucleus init\n");
    Status = NkInitializeCore(BootInfo);
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] CORE init failed\n");
        HalHalt();
    }

    HalWriteDebugString("[KE] CORE complete\n");
    HalWriteDebugString("[KE] stage: executive init\n");
    Status = ExInitializeExecutive();
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] EXEC init failed\n");
        HalHalt();
    }

    HalInitializePhase1();
    HalWriteDebugString("[KE] kernel bootstrap complete\n");
    KiLaunchInitProcess();

    Status = KeRunValidationSuite();
    if (Status != KSTATUS_SUCCESS) {
        HalWriteDebugString("[KE] validation suite failed\n");
    } else {
        HalWriteDebugString("[KE] validation suite passed\n");
    }

    for (;;) {
        __asm__ __volatile__("hlt");
        PsHandlePreemptionPoint();
    }
}
