#include <stddef.h>
#include <stdint.h>

void *memcpy(void *Destination, const void *Source, size_t Size) {
    uint8_t *dst = (uint8_t *)Destination;
    const uint8_t *src = (const uint8_t *)Source;
    size_t i;

    for (i = 0; i < Size; i++) {
        dst[i] = src[i];
    }

    return Destination;
}

void *memmove(void *Destination, const void *Source, size_t Size) {
    uint8_t *dst = (uint8_t *)Destination;
    const uint8_t *src = (const uint8_t *)Source;

    if (dst == src || Size == 0u) {
        return Destination;
    }

    if (dst < src) {
        size_t i;
        for (i = 0; i < Size; i++) {
            dst[i] = src[i];
        }
    } else {
        size_t i;
        for (i = Size; i != 0u; i--) {
            dst[i - 1u] = src[i - 1u];
        }
    }

    return Destination;
}

void *memset(void *Destination, int Value, size_t Size) {
    uint8_t *dst = (uint8_t *)Destination;
    uint8_t v = (uint8_t)Value;
    size_t i;

    for (i = 0; i < Size; i++) {
        dst[i] = v;
    }

    return Destination;
}

int memcmp(const void *Left, const void *Right, size_t Size) {
    const uint8_t *lhs = (const uint8_t *)Left;
    const uint8_t *rhs = (const uint8_t *)Right;
    size_t i;

    for (i = 0; i < Size; i++) {
        if (lhs[i] != rhs[i]) {
            return (int)lhs[i] - (int)rhs[i];
        }
    }

    return 0;
}
