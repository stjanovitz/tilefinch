#ifndef TILEFINCH_INTEGER_MATH_H
#define TILEFINCH_INTEGER_MATH_H

#include <limits.h>
#include <stdint.h>

/* Exact floor(sqrt(value)) with a fixed maximum of sixteen shift/subtract
   rounds. This avoids Allegrex integer division in rounded-shadow pixels. */
static inline uint32_t tilefinch_isqrt_u32(uint32_t value)
{
    uint32_t root = 0;
    uint32_t bit = UINT32_C(1) << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/* The 64-bit quotient, saturated to int; out of line because it expands to
   a soft 64-bit division on the PSP. Requires denominator > 0. */
int tilefinch_mul_div_int_wide(int value, int multiplier, int denominator);

static inline int tilefinch_mul_div_int(int value, int multiplier,
                                        int denominator)
{
    if (denominator <= 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
    int product = 0;
    if (!__builtin_mul_overflow(value, multiplier, &product)) {
        return product / denominator;
    }
#endif
    return tilefinch_mul_div_int_wide(value, multiplier, denominator);
}

static inline int tilefinch_mul_div_floor_int(int value, int multiplier,
                                              int denominator)
{
    if (denominator <= 0) return 0;
    int product = 0;
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_mul_overflow(value, multiplier, &product)) {
        int quotient = product / denominator;
        if (product % denominator < 0) quotient--;
        return quotient;
    }
#endif
    int64_t product64 = (int64_t) value * multiplier;
    int64_t quotient = product64 / denominator;
    if (product64 % denominator < 0) quotient--;
    if (quotient > INT_MAX) return INT_MAX;
    if (quotient < INT_MIN) return INT_MIN;
    return (int) quotient;
}

static inline int tilefinch_mul_div_ceil_int(int value, int multiplier,
                                             int denominator)
{
    if (denominator <= 0) return 0;
    int product = 0;
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_mul_overflow(value, multiplier, &product)) {
        int quotient = product / denominator;
        if (product % denominator > 0) quotient++;
        return quotient;
    }
#endif
    int64_t product64 = (int64_t) value * multiplier;
    int64_t quotient = product64 / denominator;
    if (product64 % denominator > 0) quotient++;
    if (quotient > INT_MAX) return INT_MAX;
    if (quotient < INT_MIN) return INT_MIN;
    return (int) quotient;
}

#endif
