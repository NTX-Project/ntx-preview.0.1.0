#include "../../hal/inc/hal.h"
#include "../include/kstatus.h"
#include "../include/lpc.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/ob.h"
#include "../include/ps.h"
#include "../include/syscall.h"

#define KI_SYSCALL_DEBUGPRINT_MAX 512u
#define KI_USER_ADDRESS_TOP 0x0000800000000000ull
#define KI_KERNEL_ADDRESS_BASE 0xFFFF800000000000ull

static uint32_t KipStartsWith(const char *Text, const char *Prefix) {
    uint32_t Index = 0u;
    while (Prefix[Index] != 0) {
        if (Text[Index] != Prefix[Index]) {
            return 0u;
        }
        Index++;
    }
    return 1u;
}

static uint32_t KipParseHex64(const char *Text, uint64_t *OutValue) {
    uint64_t Value = 0u;
    uint32_t Digits = 0u;
    uint32_t Index = 0u;

    if (Text == 0 || OutValue == 0) {
        return 0u;
    }

    if (Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X')) {
        Index = 2u;
    }

    while (Text[Index] != 0 && Text[Index] != ' ' && Text[Index] != '\n' && Text[Index] != '\r') {
        char Ch = Text[Index];
        uint64_t Nibble;
        if (Ch >= '0' && Ch <= '9') {
            Nibble = (uint64_t)(Ch - '0');
        } else if (Ch >= 'a' && Ch <= 'f') {
            Nibble = (uint64_t)(10 + Ch - 'a');
        } else if (Ch >= 'A' && Ch <= 'F') {
            Nibble = (uint64_t)(10 + Ch - 'A');
        } else {
            return 0u;
        }

        Value = (Value << 4) | Nibble;
        Digits++;
        Index++;
    }

    if (Digits == 0u) {
        return 0u;
    }

    *OutValue = Value;
    return 1u;
}

static uint32_t KipIsRangeReadable(uint64_t Address, uint64_t Length) {
    uint64_t End;

    if (Address == 0u || Length == 0u) {
        return 0u;
    }

    End = Address + Length;
    if (End < Address) {
        return 0u;
    }

    if (Address < KI_USER_ADDRESS_TOP && End <= KI_USER_ADDRESS_TOP) {
        return 1u;
    }

    if (Address >= KI_KERNEL_ADDRESS_BASE && End >= Address) {
        return 1u;
    }

    return 0u;
}

static uint32_t KipIsRangeWritable(uint64_t Address, uint64_t Length) {
    return KipIsRangeReadable(Address, Length);
}

static uint64_t KipSysDebugPrint(uint64_t StringAddress, uint64_t Length) {
    char Buffer[KI_SYSCALL_DEBUGPRINT_MAX + 1u];
    uint32_t Index;
    const char *Input;

    if (Length > KI_SYSCALL_DEBUGPRINT_MAX || KipIsRangeReadable(StringAddress, Length) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Input = (const char *)(uintptr_t)StringAddress;
    for (Index = 0; Index < (uint32_t)Length; Index++) {
        Buffer[Index] = Input[Index];
    }
    Buffer[Length] = 0;

    HalWriteDebugString(Buffer);

    if (KipStartsWith(Buffer, "help") != 0u || KipStartsWith(Buffer, ":help") != 0u) {
        HalWriteDebugString("debug commands: help mem ps handles ipc vm panic files objects\n");
        HalWriteDebugString("debug commands: :mm.faults :mm.dumpva <hex>\n");
    } else if (KipStartsWith(Buffer, "mem") != 0u || KipStartsWith(Buffer, ":mem") != 0u) {
        KPROCESS *Process = PsGetCurrentProcess();
        HalWriteDebugString("[DBG] mem\n");
        if (Process != 0 && Process->AddressSpaceRoot != 0u) {
            MmDumpAddressSpaceRegions((MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot);
        }
        MmDumpRecentFaults();
    } else if (KipStartsWith(Buffer, "ps") != 0u || KipStartsWith(Buffer, ":ps") != 0u) {
        HalWriteDebugString("[DBG] ps\n");
        PsDumpSchedulerState();
    } else if (KipStartsWith(Buffer, "handles") != 0u || KipStartsWith(Buffer, ":handles") != 0u) {
        HalWriteDebugString("[DBG] handles\n");
        ObDumpHandles();
    } else if (KipStartsWith(Buffer, "objects") != 0u || KipStartsWith(Buffer, ":objects") != 0u) {
        HalWriteDebugString("[DBG] objects\n");
        ObDumpObjects();
    } else if (KipStartsWith(Buffer, "ipc") != 0u || KipStartsWith(Buffer, ":ipc") != 0u) {
        HalWriteDebugString("[DBG] ipc\n");
        LpcDumpState();
    } else if (KipStartsWith(Buffer, "files") != 0u || KipStartsWith(Buffer, ":files") != 0u) {
        HalWriteDebugString("[DBG] files\n");
        IoDumpFiles();
    } else if (KipStartsWith(Buffer, "vm") != 0u || KipStartsWith(Buffer, ":vm") != 0u) {
        KPROCESS *Process = PsGetCurrentProcess();
        HalWriteDebugString("[DBG] vm\n");
        if (Process != 0 && Process->AddressSpaceRoot != 0u) {
            MmDumpAddressSpaceRegions((MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot);
        }
    } else if (KipStartsWith(Buffer, "panic") != 0u || KipStartsWith(Buffer, ":panic") != 0u) {
        HalWriteDebugString("[DBG] panic requested\n");
        MmDumpRecentFaults();
        HalHalt();
    } else if (KipStartsWith(Buffer, ":mm.faults") != 0u) {
        MmDumpRecentFaults();
    } else if (KipStartsWith(Buffer, ":mm.dumpva ") != 0u) {
        uint64_t Va = 0u;
        KPROCESS *Process = PsGetCurrentProcess();
        if (Process != 0 && Process->AddressSpaceRoot != 0u &&
            KipParseHex64(Buffer + 11, &Va) != 0u) {
            MmDumpAddressTranslation((MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot, Va);
        }
    }

    return (uint64_t)(int64_t)KSTATUS_SUCCESS;
}

static uint64_t KipSysYield(void) {
    KSTATUS Status = PsYieldThread();
    return (uint64_t)(int64_t)Status;
}

static uint64_t KipSysSend(uint64_t Handle, uint64_t MessageAddress, uint64_t MessageLength, uint64_t Wait) {
    NK_MESSAGE Message;
    uint32_t Index;
    const uint8_t *Input;
    KSTATUS Status;

    if (MessageLength > NK_MESSAGE_PAYLOAD_BYTES || MessageAddress == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    if (KipIsRangeReadable(MessageAddress, MessageLength) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Message.Opcode = 0u;
    Message.Length = (uint32_t)MessageLength;
    for (Index = 0u; Index < NK_MESSAGE_PAYLOAD_BYTES; Index++) {
        Message.Payload[Index] = 0u;
    }

    Input = (const uint8_t *)(uintptr_t)MessageAddress;
    for (Index = 0u; Index < (uint32_t)MessageLength; Index++) {
        Message.Payload[Index] = Input[Index];
    }

    Status = LpcSendByHandle((uint32_t)Handle, &Message, (uint32_t)Wait);
    return (uint64_t)(int64_t)Status;
}

static uint64_t KipSysReceive(uint64_t Handle, uint64_t MessageAddress, uint64_t MessageLength, uint64_t Wait) {
    NK_MESSAGE Message;
    uint32_t Index;
    uint8_t *Output;
    KSTATUS Status;

    if (MessageLength > NK_MESSAGE_PAYLOAD_BYTES || MessageAddress == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    if (KipIsRangeWritable(MessageAddress, MessageLength) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Status = LpcReceiveByHandle((uint32_t)Handle, &Message, (uint32_t)Wait);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    if (Message.Length > (uint32_t)MessageLength) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Output = (uint8_t *)(uintptr_t)MessageAddress;
    for (Index = 0u; Index < Message.Length; Index++) {
        Output[Index] = Message.Payload[Index];
    }

    return (uint64_t)Message.Length;
}

static uint64_t KipSysMapMemory(uint64_t Flags) {
    KPROCESS *Process;
    MM_ADDRESS_SPACE *AddressSpace;
    uint64_t VirtualBase;
    uint64_t RequestedSize;
    KSTATUS Status;

    Process = PsGetCurrentProcess();
    if (Process == 0 || Process->AddressSpaceRoot == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;

    RequestedSize = (Flags == 0u) ? MM_PAGE_SIZE : Flags;
    Status = MmMapAnonymous(AddressSpace,
                            RequestedSize,
                            MM_PROT_READ | MM_PROT_WRITE,
                            MM_REGION_FLAG_LAZY,
                            &VirtualBase);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    return VirtualBase;
}

static uint64_t KipSysExitThread(uint64_t ExitCode) {
    KSTATUS Status = PsExitCurrentThread(ExitCode);
    return (uint64_t)(int64_t)Status;
}

static uint64_t KipSysCreatePort(uint64_t DesiredAccess) {
    uint32_t HandleValue = 0u;
    KSTATUS Status = LpcCreatePortHandle((uint32_t)DesiredAccess, &HandleValue);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }
    return (uint64_t)HandleValue;
}

static uint64_t KipSysCloseHandle(uint64_t HandleValue) {
    KSTATUS Status = LpcCloseHandle((uint32_t)HandleValue);
    return (uint64_t)(int64_t)Status;
}

static uint64_t KipSysCreateSection(uint64_t Size, uint64_t DesiredAccess, uint64_t OutHandleAddress) {
    uint32_t HandleValue = 0u;
    uint32_t *OutHandle;
    uint32_t AccessMask;
    KSTATUS Status;

    if (OutHandleAddress == 0u || KipIsRangeWritable(OutHandleAddress, sizeof(uint32_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    AccessMask = (DesiredAccess == 0u) ? MM_SECTION_ACCESS_ALL : (uint32_t)DesiredAccess;
    Status = MmCreateSectionHandle(Size, AccessMask, &HandleValue);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    OutHandle = (uint32_t *)(uintptr_t)OutHandleAddress;
    *OutHandle = HandleValue;
    return (uint64_t)(int64_t)KSTATUS_SUCCESS;
}

static uint64_t KipSysMapSection(uint64_t HandleValue, uint64_t Protection, uint64_t OutAddress) {
    KPROCESS *Process;
    MM_ADDRESS_SPACE *AddressSpace;
    uint64_t VirtualBase = 0u;
    uint64_t *OutVirtualBase;
    KSTATUS Status;

    if (OutAddress == 0u || KipIsRangeWritable(OutAddress, sizeof(uint64_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Process = PsGetCurrentProcess();
    if (Process == 0 || Process->AddressSpaceRoot == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    AddressSpace = (MM_ADDRESS_SPACE *)(uintptr_t)Process->AddressSpaceRoot;
    Status = MmMapSectionHandle(AddressSpace, (uint32_t)HandleValue, (uint32_t)Protection, &VirtualBase);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    OutVirtualBase = (uint64_t *)(uintptr_t)OutAddress;
    *OutVirtualBase = VirtualBase;
    return (uint64_t)(int64_t)KSTATUS_SUCCESS;
}

static uint64_t KipSysCreateProcessFromBuffer(uint64_t ImageAddress,
                                              uint64_t ImageSize,
                                              uint64_t OutProcessIdAddress,
                                              uint64_t OutEntryAddress) {
    uint64_t ProcessId = 0u;
    uint64_t Entry = 0u;
    KSTATUS Status;

    if (ImageAddress == 0u || ImageSize == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (KipIsRangeReadable(ImageAddress, ImageSize) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (OutProcessIdAddress != 0u && KipIsRangeWritable(OutProcessIdAddress, sizeof(uint64_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (OutEntryAddress != 0u && KipIsRangeWritable(OutEntryAddress, sizeof(uint64_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Status = PsSpawnUserKpeFromBuffer((const void *)(uintptr_t)ImageAddress,
                                      ImageSize,
                                      &ProcessId,
                                      &Entry);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    if (OutProcessIdAddress != 0u) {
        *(uint64_t *)(uintptr_t)OutProcessIdAddress = ProcessId;
    }
    if (OutEntryAddress != 0u) {
        *(uint64_t *)(uintptr_t)OutEntryAddress = Entry;
    }
    return (uint64_t)(int64_t)KSTATUS_SUCCESS;
}

static uint64_t KipSysOpen(uint64_t PathAddress, uint64_t PathLength, uint64_t DesiredAccess) {
    char LocalPath[128];
    uint32_t HandleValue;
    uint32_t Index;
    KSTATUS Status;

    if (PathAddress == 0u || PathLength == 0u || PathLength >= sizeof(LocalPath)) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (KipIsRangeReadable(PathAddress, PathLength) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    for (Index = 0u; Index < (uint32_t)PathLength; Index++) {
        LocalPath[Index] = ((const char *)(uintptr_t)PathAddress)[Index];
    }
    LocalPath[PathLength] = 0;

    Status = IoOpenPath(LocalPath, (uint32_t)DesiredAccess, &HandleValue);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }
    return (uint64_t)HandleValue;
}

static uint64_t KipSysRead(uint64_t HandleValue,
                           uint64_t Offset,
                           uint64_t BufferAddress,
                           uint64_t BufferLength) {
    uint64_t BytesRead = 0u;
    KSTATUS Status;

    if (BufferAddress == 0u || BufferLength == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (KipIsRangeWritable(BufferAddress, BufferLength) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Status = IoReadFileHandle((uint32_t)HandleValue,
                              Offset,
                              (void *)(uintptr_t)BufferAddress,
                              BufferLength,
                              &BytesRead);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }
    return BytesRead;
}

static uint64_t KipSysClose(uint64_t HandleValue) {
    KSTATUS Status = IoCloseFileHandle((uint32_t)HandleValue);
    return (uint64_t)(int64_t)Status;
}

static uint64_t KipSysCreateProcessFromFileHandle(uint64_t HandleValue,
                                                  uint64_t OutProcessIdAddress,
                                                  uint64_t OutEntryAddress) {
    uint64_t ProcessId = 0u;
    uint64_t Entry = 0u;
    KSTATUS Status;

    if (OutProcessIdAddress != 0u && KipIsRangeWritable(OutProcessIdAddress, sizeof(uint64_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (OutEntryAddress != 0u && KipIsRangeWritable(OutEntryAddress, sizeof(uint64_t)) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Status = IoSpawnProcessFromFileHandle((uint32_t)HandleValue, &ProcessId, &Entry);
    if (Status != KSTATUS_SUCCESS) {
        return (uint64_t)(int64_t)Status;
    }

    if (OutProcessIdAddress != 0u) {
        *(uint64_t *)(uintptr_t)OutProcessIdAddress = ProcessId;
    }
    if (OutEntryAddress != 0u) {
        *(uint64_t *)(uintptr_t)OutEntryAddress = Entry;
    }
    return (uint64_t)(int64_t)KSTATUS_SUCCESS;
}

static uint64_t KipSysReadConsole(uint64_t BufferAddress, uint64_t BufferLength) {
    uint64_t ReadCount = 0u;
    char *Buffer;

    if (BufferAddress == 0u || BufferLength == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }
    if (KipIsRangeWritable(BufferAddress, BufferLength) == 0u) {
        return (uint64_t)(int64_t)KSTATUS_INVALID_PARAMETER;
    }

    Buffer = (char *)(uintptr_t)BufferAddress;
    while (ReadCount < BufferLength) {
        char Ch = 0;
        if (HalTryReadConsoleChar(&Ch) == 0u) {
            break;
        }
        Buffer[ReadCount] = Ch;
        ReadCount++;
    }

    return ReadCount;
}

uint64_t KiDispatchSystemCall(uint64_t Number,
                              uint64_t Arg0,
                              uint64_t Arg1,
                              uint64_t Arg2,
                              uint64_t Arg3,
                              uint64_t Arg4,
                              uint64_t Arg5) {
    (void)Arg4;
    (void)Arg5;

    switch (Number) {
        case NkSysDebugPrint:
            return KipSysDebugPrint(Arg0, Arg1);
        case NkSysYield:
            return KipSysYield();
        case NkSysSend:
            return KipSysSend(Arg0, Arg1, Arg2, Arg3);
        case NkSysReceive:
            return KipSysReceive(Arg0, Arg1, Arg2, Arg3);
        case NkSysMapMemory:
            return KipSysMapMemory(Arg0);
        case NkSysExitThread:
            return KipSysExitThread(Arg0);
        case NkSysCreatePort:
            return KipSysCreatePort(Arg0);
        case NkSysCloseHandle:
            return KipSysCloseHandle(Arg0);
        case NkSysCreateSection:
            return KipSysCreateSection(Arg0, Arg1, Arg2);
        case NkSysMapSection:
            return KipSysMapSection(Arg0, Arg1, Arg2);
        case NkSysCreateProcessFromBuffer:
            return KipSysCreateProcessFromBuffer(Arg0, Arg1, Arg2, Arg3);
        case NkSysOpen:
            return KipSysOpen(Arg0, Arg1, Arg2);
        case NkSysRead:
            return KipSysRead(Arg0, Arg1, Arg2, Arg3);
        case NkSysClose:
            return KipSysClose(Arg0);
        case NkSysCreateProcessFromFileHandle:
            return KipSysCreateProcessFromFileHandle(Arg0, Arg1, Arg2);
        case NkSysReadConsole:
            return KipSysReadConsole(Arg0, Arg1);
        default:
            return (uint64_t)(int64_t)KSTATUS_NOT_IMPLEMENTED;
    }
}
