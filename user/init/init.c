#include <stdint.h>

enum {
    NkSysDebugPrint = 0,
    NkSysYield = 1,
    NkSysExitThread = 5,
    NkSysReadConsole = 15
};

static uint64_t nk_syscall(uint64_t Number,
                           uint64_t Arg0,
                           uint64_t Arg1,
                           uint64_t Arg2,
                           uint64_t Arg3,
                           uint64_t Arg4,
                           uint64_t Arg5) {
    register uint64_t R10 __asm__("r10") = Arg3;
    register uint64_t R8 __asm__("r8") = Arg4;
    register uint64_t R9 __asm__("r9") = Arg5;
    __asm__ __volatile__(
        "int $0x80"
        : "+a"(Number)
        : "D"(Arg0), "S"(Arg1), "d"(Arg2), "r"(R10), "r"(R8), "r"(R9)
        : "rcx", "r11", "memory");
    return Number;
}

static uint64_t nk_strlen(const char *Text) {
    uint64_t Length = 0u;
    while (Text[Length] != 0) {
        Length++;
    }
    return Length;
}

void _start(void) {
    static const char Banner[] = "init.kpe online\n";
    static const char Prompt[] = "NK> ";
    static const char Hint[] = "type 'help' for debug commands\n";
    static const char Newline[] = "\n";
    static const char CursorOn[] = "_";
    static const char CursorOff[] = "\b \b";
    char Line[128];
    uint64_t LineLen = 0u;
    uint32_t PromptShown = 0u;
    uint32_t CursorShown = 0u;

    (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Banner, nk_strlen(Banner), 0u, 0u, 0u, 0u);
    (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Hint, nk_strlen(Hint), 0u, 0u, 0u, 0u);
    for (;;) {
        char Ch = 0;
        uint64_t Read;

        if (LineLen == 0u && PromptShown == 0u) {
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Prompt, nk_strlen(Prompt), 0u, 0u, 0u, 0u);
            PromptShown = 1u;
        }
        if (PromptShown != 0u && CursorShown == 0u) {
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)CursorOn, 1u, 0u, 0u, 0u, 0u);
            CursorShown = 1u;
        }

        Read = nk_syscall(NkSysReadConsole, (uint64_t)(uintptr_t)&Ch, 1u, 0u, 0u, 0u, 0u);
        if (Read == 0u) {
            (void)nk_syscall(NkSysYield, 0u, 0u, 0u, 0u, 0u, 0u);
            continue;
        }
        if (CursorShown != 0u) {
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)CursorOff, 3u, 0u, 0u, 0u, 0u);
            CursorShown = 0u;
        }

        if (Ch == '\r' || Ch == '\n') {
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Newline, 1u, 0u, 0u, 0u, 0u);
            if (LineLen != 0u) {
                Line[LineLen] = 0;
                (void)nk_syscall(NkSysDebugPrint,
                                 (uint64_t)(uintptr_t)Line,
                                 LineLen,
                                 0u,
                                 0u,
                                 0u,
                                 0u);
                (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Newline, 1u, 0u, 0u, 0u, 0u);
            }
            LineLen = 0u;
            PromptShown = 0u;
            continue;
        }

        if ((Ch == '\b' || Ch == 0x7Fu) && LineLen > 0u) {
            static const char Bs[] = "\b \b";
            LineLen--;
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)Bs, 3u, 0u, 0u, 0u, 0u);
            continue;
        }

        if (LineLen + 1u < sizeof(Line) && Ch >= 0x20 && Ch <= 0x7E) {
            Line[LineLen++] = Ch;
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)&Ch, 1u, 0u, 0u, 0u, 0u);
        }

        if (PromptShown != 0u && CursorShown == 0u) {
            (void)nk_syscall(NkSysDebugPrint, (uint64_t)(uintptr_t)CursorOn, 1u, 0u, 0u, 0u, 0u);
            CursorShown = 1u;
        }
    }
}
