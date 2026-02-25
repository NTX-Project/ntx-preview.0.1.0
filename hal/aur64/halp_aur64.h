#ifndef HALP_AUR64_H
#define HALP_AUR64_H

#include <stdint.h>

void HalpInitializeInterrupts(void);
void HalpInitializeTimer(void);
void HalpSetKernelRsp0(uint64_t StackTop);

void HalpIoWrite8(uint16_t Port, uint8_t Value);
uint8_t HalpIoRead8(uint16_t Port);

#endif /* HALP_AUR64_H */
