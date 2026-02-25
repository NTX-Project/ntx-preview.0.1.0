#ifndef HAL_H
#define HAL_H

#include <stdint.h>

#include "../../boot/include/bootproto.h"

typedef struct HAL_PLATFORM_INFO {
    uint32_t ProcessorCount;
    uint32_t TimerFrequencyHz;
    uint64_t PhysicalMemoryTop;
} HAL_PLATFORM_INFO;

typedef void (*HAL_TIMER_TICK_ROUTINE)(void);
typedef void (*HAL_PAGE_FAULT_ROUTINE)(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer);

void HalInitializePhase0(const BOOT_INFO *BootInfo);
void HalInitializePhase1(void);
void HalQueryPlatformInfo(HAL_PLATFORM_INFO *OutInfo);
void HalRegisterTimerTickRoutine(HAL_TIMER_TICK_ROUTINE Routine);
void HalRegisterPageFaultRoutine(HAL_PAGE_FAULT_ROUTINE Routine);
void HalHandlePageFaultTrap(uint64_t FaultAddress, uint64_t ErrorCode, uint64_t InstructionPointer);
void HalTriggerSyntheticTimerTick(void);
void HalWriteDebugString(const char *Message);
void HalClearDisplay(void);
uint32_t HalTryReadConsoleChar(char *OutChar);
void HalStallExecution(uint32_t Microseconds);
void HalDisableInterrupts(void);
void HalEnableInterrupts(void);
void HalSetKernelStackPointer(uint64_t StackTop);
uint64_t HalReadTimestampCounter(void);
void HalHalt(void);

#endif /* HAL_H */
