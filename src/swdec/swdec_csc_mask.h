#ifndef TILEFINCH_SWDEC_CSC_MASK_H
#define TILEFINCH_SWDEC_CSC_MASK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CPU and Media Engine share this completion map on a 32-bit machine.  Keep
 * every published unit inside one naturally aligned word: a shared uint64_t
 * would permit torn reads/writes, while one uint32_t cannot name the 34
 * eight-row units in a 272-pixel frame.
 */
#define SWDEC_CSC_MASK_WORDS 2u
#define SWDEC_CSC_MASK_UNITS (SWDEC_CSC_MASK_WORDS * 32u)

typedef struct {
    volatile uint32_t words[SWDEC_CSC_MASK_WORDS];
} SwdecCscMask;

static inline bool swdec_csc_mask_test(
    const SwdecCscMask *mask, unsigned unit)
{
    return mask != NULL && unit < SWDEC_CSC_MASK_UNITS
        && (mask->words[unit >> 5u]
            & (UINT32_C(1) << (unit & 31u))) != 0;
}

static inline void swdec_csc_mask_set(SwdecCscMask *mask, unsigned unit)
{
    if (mask == NULL || unit >= SWDEC_CSC_MASK_UNITS) return;
    unsigned word = unit >> 5u;
    mask->words[word] = mask->words[word]
        | (UINT32_C(1) << (unit & 31u));
}

static inline void swdec_csc_mask_clear(SwdecCscMask *mask)
{
    if (mask == NULL) return;
    for (unsigned word = 0; word < SWDEC_CSC_MASK_WORDS; word++)
        mask->words[word] = 0;
}

#endif
