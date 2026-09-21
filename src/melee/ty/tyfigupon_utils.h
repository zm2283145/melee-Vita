#ifndef MELEE_TY_TYFIGUPON_UTILS_H
#define MELEE_TY_TYFIGUPON_UTILS_H

#include <stddef.h>

#define TYFIGUPON_MAX_COIN_TOTAL 0x270FU

static inline unsigned int
tyFigupon_ValidatedCoinTotal(unsigned int value)
{
    return value <= TYFIGUPON_MAX_COIN_TOTAL ? value : 0;
}

static inline size_t tyFigupon_StoreDigits(int* digits, size_t capacity,
                                           unsigned int value)
{
    size_t count = 0;

    do {
        if (count >= capacity) {
            break;
        }
        digits[count++] = (int) (value % 10U);
        value /= 10U;
    } while (value > 0);

    return count;
}

#endif
