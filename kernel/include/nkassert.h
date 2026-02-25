#ifndef NKASSERT_H
#define NKASSERT_H

#include <stdint.h>

void NkAssertFail(const char *Expression, const char *File, uint32_t Line, const char *Function);

#define NK_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            NkAssertFail(#expr, __FILE__, (uint32_t)__LINE__, __func__); \
        } \
    } while (0)

#endif /* NKASSERT_H */
