#include "../inc/hal.h"
#include "halp_aur64.h"
#include "../../kernel/include/syscall.h"

typedef struct AUR64_IDT_ENTRY {
    uint16_t OffsetLow;
    uint16_t Selector;
    uint8_t Ist;
    uint8_t Attributes;
    uint16_t OffsetMiddle;
    uint32_t OffsetHigh;
    uint32_t Reserved;
} __attribute__((packed)) AUR64_IDT_ENTRY;

typedef struct AUR64_IDTR {
    uint16_t Limit;
    uint64_t Base;
} __attribute__((packed)) AUR64_IDTR;

typedef struct AUR64_GDT_ENTRY {
    uint16_t LimitLow;
    uint16_t BaseLow;
    uint8_t BaseMiddle;
    uint8_t Access;
    uint8_t Granularity;
    uint8_t BaseHigh;
} __attribute__((packed)) AUR64_GDT_ENTRY;

typedef struct AUR64_TSS_DESC {
    uint16_t LimitLow;
    uint16_t BaseLow;
    uint8_t BaseMid0;
    uint8_t Access;
    uint8_t Granularity;
    uint8_t BaseMid1;
    uint32_t BaseHigh;
    uint32_t Reserved;
} __attribute__((packed)) AUR64_TSS_DESC;

typedef struct AUR64_GDTR {
    uint16_t Limit;
    uint64_t Base;
} __attribute__((packed)) AUR64_GDTR;

typedef struct AUR64_TSS64 {
    uint32_t Reserved0;
    uint64_t Rsp0;
    uint64_t Rsp1;
    uint64_t Rsp2;
    uint64_t Reserved1;
    uint64_t Ist1;
    uint64_t Ist2;
    uint64_t Ist3;
    uint64_t Ist4;
    uint64_t Ist5;
    uint64_t Ist6;
    uint64_t Ist7;
    uint64_t Reserved2;
    uint16_t Reserved3;
    uint16_t IoMapBase;
} __attribute__((packed)) AUR64_TSS64;

static AUR64_IDT_ENTRY gAur64Idt[256];
static uint64_t gAur64Gdt[8];
static AUR64_TSS64 gAur64Tss;

typedef struct AUR64_INTERRUPT_FRAME {
    uint64_t Rip;
    uint64_t Cs;
    uint64_t Rflags;
    uint64_t Rsp;
    uint64_t Ss;
} AUR64_INTERRUPT_FRAME;

enum {
    HALP_PIC1_CMD = 0x20,
    HALP_PIC1_DATA = 0x21,
    HALP_PIC2_CMD = 0xA0,
    HALP_PIC2_DATA = 0xA1,
    HALP_PIC_EOI = 0x20,
    HALP_PIT_CMD = 0x43,
    HALP_PIT_CHANNEL0 = 0x40,
    HALP_PIT_BASE_HZ = 1193182,
    HALP_PIT_TICK_HZ = 100,
    HALP_IRQ_BASE_VECTOR = 32,
    HALP_SYSCALL_VECTOR = 0x80,
    HALP_PAGE_FAULT_VECTOR = 14,
    HALP_SEL_KERNEL_CODE = 0x08,
    HALP_SEL_KERNEL_DATA = 0x10,
    HALP_SEL_USER_CODE = 0x1B,
    HALP_SEL_USER_DATA = 0x23,
    HALP_SEL_TSS = 0x28
};

static void HalpSetIdtEntry(unsigned long long Vector, void *Handler) {
    uint64_t Address = (uint64_t)(unsigned long long)Handler;

    gAur64Idt[Vector].OffsetLow = (uint16_t)(Address & 0xFFFFu);
    gAur64Idt[Vector].Selector = HALP_SEL_KERNEL_CODE;
    gAur64Idt[Vector].Ist = 0;
    gAur64Idt[Vector].Attributes = 0x8Eu;
    gAur64Idt[Vector].OffsetMiddle = (uint16_t)((Address >> 16) & 0xFFFFu);
    gAur64Idt[Vector].OffsetHigh = (uint32_t)((Address >> 32) & 0xFFFFFFFFu);
    gAur64Idt[Vector].Reserved = 0;
}

static void HalpSetIdtEntryDpl(unsigned long long Vector, void *Handler, uint8_t Dpl) {
    uint64_t Address = (uint64_t)(unsigned long long)Handler;
    uint8_t Attributes = (uint8_t)(0x8Eu | ((Dpl & 0x3u) << 5));

    gAur64Idt[Vector].OffsetLow = (uint16_t)(Address & 0xFFFFu);
    gAur64Idt[Vector].Selector = HALP_SEL_KERNEL_CODE;
    gAur64Idt[Vector].Ist = 0;
    gAur64Idt[Vector].Attributes = Attributes;
    gAur64Idt[Vector].OffsetMiddle = (uint16_t)((Address >> 16) & 0xFFFFu);
    gAur64Idt[Vector].OffsetHigh = (uint32_t)((Address >> 32) & 0xFFFFFFFFu);
    gAur64Idt[Vector].Reserved = 0;
}

static void HalpHex64(uint64_t Value, char *Out) {
    static const char Hex[] = "0123456789ABCDEF";
    uint32_t i;
    for (i = 0u; i < 16u; i++) {
        uint32_t Shift = (15u - i) * 4u;
        Out[i] = Hex[(Value >> Shift) & 0xFu];
    }
    Out[16] = 0;
}

static void HalpWriteHexLine(const char *Prefix, uint64_t Value) {
    char Buffer[96];
    char Hex[17];
    uint32_t Pos = 0u;
    uint32_t i = 0u;

    while (Prefix[i] != 0 && Pos + 1u < sizeof(Buffer)) {
        Buffer[Pos++] = Prefix[i++];
    }
    Buffer[Pos++] = '0';
    Buffer[Pos++] = 'x';
    HalpHex64(Value, Hex);
    for (i = 0u; i < 16u && Pos + 1u < sizeof(Buffer); i++) {
        Buffer[Pos++] = Hex[i];
    }
    Buffer[Pos++] = '\n';
    Buffer[Pos] = 0;
    HalWriteDebugString(Buffer);
}

static uint64_t HalpMakeSegmentDesc(uint8_t Access, uint8_t GranularityFlags) {
    uint64_t Descriptor = 0;
    Descriptor |= 0xFFFFull;
    Descriptor |= ((uint64_t)Access) << 40;
    Descriptor |= ((uint64_t)(0xFu | (GranularityFlags << 4))) << 48;
    return Descriptor;
}

static void HalpInitializeSegments(void) {
    AUR64_GDTR Gdtr;
    AUR64_TSS_DESC *TssDesc;
    uint64_t TssBase;
    uint64_t TssLimit;
    uint64_t Rsp;

    gAur64Gdt[0] = 0;
    gAur64Gdt[1] = HalpMakeSegmentDesc(0x9Au, 0xAu);
    gAur64Gdt[2] = HalpMakeSegmentDesc(0x92u, 0x8u);
    gAur64Gdt[3] = HalpMakeSegmentDesc(0xF2u, 0x8u);
    gAur64Gdt[4] = HalpMakeSegmentDesc(0xFAu, 0xAu);

    TssDesc = (AUR64_TSS_DESC *)&gAur64Gdt[5];
    TssBase = (uint64_t)(uintptr_t)&gAur64Tss;
    TssLimit = sizeof(gAur64Tss) - 1u;

    gAur64Tss.Reserved0 = 0;
    gAur64Tss.Rsp1 = 0;
    gAur64Tss.Rsp2 = 0;
    gAur64Tss.Reserved1 = 0;
    gAur64Tss.Ist1 = 0;
    gAur64Tss.Ist2 = 0;
    gAur64Tss.Ist3 = 0;
    gAur64Tss.Ist4 = 0;
    gAur64Tss.Ist5 = 0;
    gAur64Tss.Ist6 = 0;
    gAur64Tss.Ist7 = 0;
    gAur64Tss.Reserved2 = 0;
    gAur64Tss.Reserved3 = 0;
    gAur64Tss.IoMapBase = sizeof(gAur64Tss);
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(Rsp));
    gAur64Tss.Rsp0 = Rsp;

    TssDesc->LimitLow = (uint16_t)(TssLimit & 0xFFFFu);
    TssDesc->BaseLow = (uint16_t)(TssBase & 0xFFFFu);
    TssDesc->BaseMid0 = (uint8_t)((TssBase >> 16) & 0xFFu);
    TssDesc->Access = 0x89u;
    TssDesc->Granularity = (uint8_t)((TssLimit >> 16) & 0x0Fu);
    TssDesc->BaseMid1 = (uint8_t)((TssBase >> 24) & 0xFFu);
    TssDesc->BaseHigh = (uint32_t)(TssBase >> 32);
    TssDesc->Reserved = 0u;

    Gdtr.Limit = sizeof(gAur64Gdt) - 1u;
    Gdtr.Base = (uint64_t)(uintptr_t)&gAur64Gdt[0];
    __asm__ __volatile__("lgdt %0" : : "m"(Gdtr));

    __asm__ __volatile__(
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [cs]"i"(HALP_SEL_KERNEL_CODE), [ds]"i"(HALP_SEL_KERNEL_DATA)
        : "rax", "memory");

    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)HALP_SEL_TSS));
}

void HalpSetKernelRsp0(uint64_t StackTop) {
    gAur64Tss.Rsp0 = StackTop;
}

static void HalpRemapPic(void) {
    HalpIoWrite8(HALP_PIC1_CMD, 0x11);
    HalpIoWrite8(HALP_PIC2_CMD, 0x11);
    HalpIoWrite8(HALP_PIC1_DATA, HALP_IRQ_BASE_VECTOR);
    HalpIoWrite8(HALP_PIC2_DATA, HALP_IRQ_BASE_VECTOR + 8);
    HalpIoWrite8(HALP_PIC1_DATA, 0x04);
    HalpIoWrite8(HALP_PIC2_DATA, 0x02);
    HalpIoWrite8(HALP_PIC1_DATA, 0x01);
    HalpIoWrite8(HALP_PIC2_DATA, 0x01);
    HalpIoWrite8(HALP_PIC1_DATA, 0xFE);
    HalpIoWrite8(HALP_PIC2_DATA, 0xFF);
}

__attribute__((interrupt)) static void HalpDefaultInterruptHandler(AUR64_INTERRUPT_FRAME *Frame) {
    (void)Frame;
    HalpIoWrite8(HALP_PIC1_CMD, HALP_PIC_EOI);
}

__attribute__((interrupt)) static void HalpTimerInterruptHandler(AUR64_INTERRUPT_FRAME *Frame) {
    (void)Frame;
    HalTriggerSyntheticTimerTick();
    HalpIoWrite8(HALP_PIC1_CMD, HALP_PIC_EOI);
}

__attribute__((interrupt)) static void HalpPageFaultInterruptHandler(AUR64_INTERRUPT_FRAME *Frame, uint64_t ErrorCode) {
    uint64_t FaultAddress;
    __asm__ __volatile__("movq %%cr2, %0" : "=r"(FaultAddress));
    HalpWriteHexLine("[HAL] PF.CS=", Frame->Cs);
    HalpWriteHexLine("[HAL] PF.SS=", Frame->Ss);
    HalHandlePageFaultTrap(FaultAddress, ErrorCode, Frame->Rip);
}

__attribute__((interrupt)) static void HalpGeneralProtectionInterruptHandler(AUR64_INTERRUPT_FRAME *Frame, uint64_t ErrorCode) {
    HalWriteDebugString("[HAL] #GP fault\n");
    HalpWriteHexLine("[HAL] GP.RIP=", Frame->Rip);
    HalpWriteHexLine("[HAL] GP.ERR=", ErrorCode);
    HalHalt();
}

__attribute__((interrupt)) static void HalpInvalidOpcodeInterruptHandler(AUR64_INTERRUPT_FRAME *Frame) {
    HalWriteDebugString("[HAL] #UD fault\n");
    HalpWriteHexLine("[HAL] UD.RIP=", Frame->Rip);
    HalHalt();
}

typedef struct HALP_SYSCALL_REG_FRAME {
    uint64_t R11;
    uint64_t R10;
    uint64_t R9;
    uint64_t R8;
    uint64_t Rdx;
    uint64_t Rsi;
    uint64_t Rdi;
    uint64_t Rax;
} HALP_SYSCALL_REG_FRAME;

__attribute__((used)) void HalpHandleSyscallTrap(HALP_SYSCALL_REG_FRAME *Frame) {
    Frame->Rax = KiDispatchSystemCall(
        Frame->Rax,
        Frame->Rdi,
        Frame->Rsi,
        Frame->Rdx,
        Frame->R10,
        Frame->R8,
        Frame->R9);
}

__attribute__((naked)) static void HalpSyscallInterruptHandler(void) {
    __asm__ __volatile__(
        "pushq %rax\n\t"
        "pushq %rdi\n\t"
        "pushq %rsi\n\t"
        "pushq %rdx\n\t"
        "pushq %r8\n\t"
        "pushq %r9\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "movq %rsp, %rdi\n\t"
        "call HalpHandleSyscallTrap\n\t"
        "popq %r11\n\t"
        "popq %r10\n\t"
        "popq %r9\n\t"
        "popq %r8\n\t"
        "popq %rdx\n\t"
        "popq %rsi\n\t"
        "popq %rdi\n\t"
        "popq %rax\n\t"
        "iretq\n\t");
}

void HalpInitializeInterrupts(void) {
    AUR64_IDTR Idtr;
    unsigned long long Index;

    HalpInitializeSegments();

    for (Index = 0; Index < 256; Index++) {
        HalpSetIdtEntry(Index, HalpDefaultInterruptHandler);
    }

    HalpSetIdtEntry(6, HalpInvalidOpcodeInterruptHandler);
    HalpSetIdtEntry(13, HalpGeneralProtectionInterruptHandler);
    HalpSetIdtEntry(HALP_IRQ_BASE_VECTOR, HalpTimerInterruptHandler);
    HalpSetIdtEntry(HALP_PAGE_FAULT_VECTOR, HalpPageFaultInterruptHandler);
    HalpSetIdtEntryDpl(HALP_SYSCALL_VECTOR, HalpSyscallInterruptHandler, 3u);
    HalpRemapPic();

    Idtr.Limit = sizeof(gAur64Idt) - 1;
    Idtr.Base = (uint64_t)(unsigned long long)&gAur64Idt[0];
    __asm__ __volatile__("lidt %0" : : "m"(Idtr));
}

void HalpInitializeTimer(void) {
    uint16_t Divisor = (uint16_t)(HALP_PIT_BASE_HZ / HALP_PIT_TICK_HZ);

    HalpIoWrite8(HALP_PIT_CMD, 0x34);
    HalpIoWrite8(HALP_PIT_CHANNEL0, (uint8_t)(Divisor & 0xFFu));
    HalpIoWrite8(HALP_PIT_CHANNEL0, (uint8_t)((Divisor >> 8) & 0xFFu));
}

void HalpIoWrite8(uint16_t Port, uint8_t Value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

uint8_t HalpIoRead8(uint16_t Port) {
    uint8_t Value;
    __asm__ __volatile__("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}
