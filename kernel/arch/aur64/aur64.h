#ifndef AUR64_H
#define AUR64_H

#include <stdint.h>

#define AUR64_ARCH_ID 0x41555236u /* "AUR6" */
#define AUR64_PAGE_SIZE 4096u

typedef struct AUR64_CONTEXT {
    uint64_t Rbx;
    uint64_t Rbp;
    uint64_t R12;
    uint64_t R13;
    uint64_t R14;
    uint64_t R15;
    uint64_t Rsp;
    uint64_t Rip;
    uint64_t Rflags;
} AUR64_CONTEXT;

#endif /* AUR64_H */
