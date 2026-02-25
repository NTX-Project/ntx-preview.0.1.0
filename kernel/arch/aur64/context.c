#include "../../include/kiarch.h"
#include "../../../hal/inc/hal.h"
#include "../../include/kstatus.h"
#include "../../include/mm.h"
#include "../../include/ps.h"
#include "../../include/syscall.h"

#define AUR64_USER_DATA_SELECTOR 0x1B
#define AUR64_USER_CODE_SELECTOR 0x23

static void KiUserSmokeBuildCode(uint8_t *Code, uint32_t *OutCodeBytes, uint32_t MessageLen) {
    uint32_t Index = 0u;
#define PUT8(v) Code[Index++] = (uint8_t)(v)
#define PUT32(v) do { \
    uint32_t _v = (uint32_t)(v); \
    PUT8(_v & 0xFFu); PUT8((_v >> 8) & 0xFFu); PUT8((_v >> 16) & 0xFFu); PUT8((_v >> 24) & 0xFFu); \
} while (0)

    PUT8(0x48); PUT8(0x89); PUT8(0xFB); /* mov rbx, rdi */
    PUT8(0xC6); PUT8(0x03); PUT8(0x5A); /* mov byte ptr [rbx], 0x5A */

    PUT8(0x48); PUT8(0xC7); PUT8(0xC0); PUT32(0u); /* mov rax, NkSysDebugPrint */
    PUT8(0x48); PUT8(0x8D); PUT8(0xBB); PUT32(0x100u); /* lea rdi, [rbx+0x100] */
    PUT8(0x48); PUT8(0xC7); PUT8(0xC6); PUT32(MessageLen); /* mov rsi, msg_len */
    PUT8(0x48); PUT8(0x31); PUT8(0xD2); /* xor rdx, rdx */
    PUT8(0x4D); PUT8(0x31); PUT8(0xD2); /* xor r10, r10 */
    PUT8(0x4D); PUT8(0x31); PUT8(0xC0); /* xor r8, r8 */
    PUT8(0x4D); PUT8(0x31); PUT8(0xC9); /* xor r9, r9 */
    PUT8(0xCD); PUT8(0x80); /* int 0x80 */

    PUT8(0x48); PUT8(0xC7); PUT8(0xC0); PUT32(1u); /* mov rax, NkSysYield */
    PUT8(0x48); PUT8(0x31); PUT8(0xFF); /* xor rdi, rdi */
    PUT8(0x48); PUT8(0x31); PUT8(0xF6); /* xor rsi, rsi */
    PUT8(0x48); PUT8(0x31); PUT8(0xD2); /* xor rdx, rdx */
    PUT8(0x4D); PUT8(0x31); PUT8(0xD2); /* xor r10, r10 */
    PUT8(0x4D); PUT8(0x31); PUT8(0xC0); /* xor r8, r8 */
    PUT8(0x4D); PUT8(0x31); PUT8(0xC9); /* xor r9, r9 */
    PUT8(0xCD); PUT8(0x80); /* int 0x80 */

    PUT8(0x48); PUT8(0xC7); PUT8(0xC0); PUT32(1u); /* mov rax, NkSysYield */
    PUT8(0xCD); PUT8(0x80); /* int 0x80 */

    PUT8(0x48); PUT8(0xC7); PUT8(0xC0); PUT32(5u); /* mov rax, NkSysExitThread */
    PUT8(0x48); PUT8(0x31); PUT8(0xFF); /* xor rdi, rdi */
    PUT8(0xCD); PUT8(0x80); /* int 0x80 */

    PUT8(0xF4); /* hlt */
    PUT8(0xEB); PUT8(0xFD); /* jmp $-1 */

    *OutCodeBytes = Index;
#undef PUT32
#undef PUT8
}

void KiArchSaveContext(AUR64_CONTEXT *OutContext) {
    if (OutContext == 0) {
        return;
    }

    __asm__ __volatile__(
        "movq %%rbx, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        "movq %%rsp, 48(%0)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, 56(%0)\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, 64(%0)\n\t"
        "1:\n\t"
        :
        : "r"(OutContext)
        : "rax", "memory");
}

void KiArchLoadContext(const AUR64_CONTEXT *Context) {
    if (Context == 0) {
        return;
    }

    __asm__ __volatile__(
        "movq 0(%0), %%rbx\n\t"
        "movq 8(%0), %%rbp\n\t"
        "movq 16(%0), %%r12\n\t"
        "movq 24(%0), %%r13\n\t"
        "movq 32(%0), %%r14\n\t"
        "movq 40(%0), %%r15\n\t"
        "movq 48(%0), %%rsp\n\t"
        "movq 64(%0), %%rax\n\t"
        "pushq %%rax\n\t"
        "popfq\n\t"
        "movq 56(%0), %%rax\n\t"
        "jmp *%%rax\n\t"
        :
        : "r"(Context)
        : "rax", "memory");
}

void KiArchSwitchContext(AUR64_CONTEXT *CurrentContext, const AUR64_CONTEXT *NextContext) {
    if (NextContext == 0) {
        return;
    }

    __asm__ __volatile__(
        "testq %0, %0\n\t"
        "jz 1f\n\t"
        "movq %%rbx, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        "movq %%rsp, 48(%0)\n\t"
        "leaq 2f(%%rip), %%rax\n\t"
        "movq %%rax, 56(%0)\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, 64(%0)\n\t"
        "1:\n\t"
        "movq 0(%1), %%rbx\n\t"
        "movq 8(%1), %%rbp\n\t"
        "movq 16(%1), %%r12\n\t"
        "movq 24(%1), %%r13\n\t"
        "movq 32(%1), %%r14\n\t"
        "movq 40(%1), %%r15\n\t"
        "movq 48(%1), %%rsp\n\t"
        "movq 64(%1), %%rax\n\t"
        "pushq %%rax\n\t"
        "popfq\n\t"
        "movq 56(%1), %%rax\n\t"
        "jmp *%%rax\n\t"
        "2:\n\t"
        :
        : "r"(CurrentContext), "r"(NextContext)
        : "rax", "memory");
}

void KiArchInitializeContext(AUR64_CONTEXT *Context, uint64_t StackTop, KI_ARCH_ENTRY_ROUTINE EntryRoutine) {
    if (Context == 0 || EntryRoutine == 0 || StackTop < 16u) {
        return;
    }

    Context->Rbx = 0;
    Context->Rbp = 0;
    Context->R12 = 0;
    Context->R13 = 0;
    Context->R14 = 0;
    Context->R15 = 0;
    Context->Rsp = (StackTop & ~((uint64_t)0x0Fu)) - 8u;
    Context->Rip = (uint64_t)(uintptr_t)EntryRoutine;
    Context->Rflags = 0x202u;
}

void KiArchEnterUserMode(uint64_t UserEntry, uint64_t UserStackTop, uint64_t Arg0, uint64_t Arg1) {
    __asm__ __volatile__(
        "movw %[uds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movq %[arg0], %%rdi\n\t"
        "movq %[arg1], %%rsi\n\t"
        "pushq %[uss]\n\t"
        "pushq %[usp]\n\t"
        "pushfq\n\t"
        "orq $0x200, (%%rsp)\n\t"
        "pushq %[ucs]\n\t"
        "pushq %[uip]\n\t"
        "iretq\n\t"
        :
        : [uds] "i"(AUR64_USER_DATA_SELECTOR),
          [uss] "r"((uint64_t)AUR64_USER_DATA_SELECTOR),
          [ucs] "r"((uint64_t)AUR64_USER_CODE_SELECTOR),
          [usp] "r"(UserStackTop),
          [uip] "r"(UserEntry),
          [arg0] "r"(Arg0),
          [arg1] "r"(Arg1)
        : "rax", "rdi", "rsi", "memory");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

KSTATUS KiArchUserModeSmokeTest(void) {
    KPROCESS *Process = PsGetCurrentProcess();
    MM_ADDRESS_SPACE *AddressSpace;
    uint64_t CodeBase;
    uint64_t GuardBase;
    uint64_t StackBase;
    uint8_t *CodePtr;
    uint8_t *DataPtr;
    const char *Message = "[USR] hello from user mode\n";
    uint32_t MessageLen = 0u;
    uint32_t i;
    uint32_t CodeBytes = 0u;
    KTHREAD *UserThread;
    KSTATUS Status;

    if (Process == 0 || Process->AddressSpaceRoot == 0u) {
        return KSTATUS_INVALID_PARAMETER;
    }

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
    Status = MmMapAnonymous(AddressSpace,
                            MM_PAGE_SIZE,
                            MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXECUTE,
                            MM_REGION_FLAG_WIRED,
                            &CodeBase);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = MmMapAnonymous(AddressSpace,
                            MM_PAGE_SIZE,
                            0u,
                            MM_REGION_FLAG_GUARD | MM_REGION_FLAG_LAZY,
                            &GuardBase);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = MmMapAnonymous(AddressSpace,
                            MM_PAGE_SIZE,
                            MM_PROT_READ | MM_PROT_WRITE,
                            MM_REGION_FLAG_WIRED,
                            &StackBase);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    CodePtr = (uint8_t *)(uintptr_t)CodeBase;
    DataPtr = CodePtr;

    for (i = 0u; i < MM_PAGE_SIZE; i++) {
        CodePtr[i] = 0x90u;
    }

    while (Message[MessageLen] != 0) {
        MessageLen++;
    }

    KiUserSmokeBuildCode(CodePtr, &CodeBytes, MessageLen);
    (void)CodeBytes;
    for (i = 0u; i < MessageLen; i++) {
        DataPtr[0x100u + i] = (uint8_t)Message[i];
    }

    (void)GuardBase;
    Status = PsCreateUserThread(Process,
                                CodeBase,
                                StackBase + MM_PAGE_SIZE - 16u,
                                CodeBase,
                                0u,
                                &UserThread);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    Status = PsReadyThread(UserThread);
    if (Status != KSTATUS_SUCCESS) {
        return Status;
    }

    for (i = 0u; i < 200u; i++) {
        HalTriggerSyntheticTimerTick();
        PsHandlePreemptionPoint();
        (void)PsYieldThread();
        if (UserThread->State == PsThreadDead) {
            return KSTATUS_SUCCESS;
        }
    }

    return KSTATUS_INIT_FAILED;
}
