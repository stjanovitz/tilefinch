#ifndef TILEFINCH_SWDEC_CSC_MASK_H
#define TILEFINCH_SWDEC_CSC_MASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * CPU and Media Engine share this completion map on a 32-bit machine.  Keep
 * every published unit inside one naturally aligned word: a shared uint64_t
 * would permit torn reads/writes, while one uint32_t cannot name the 34
 * eight-row units in a 272-pixel frame.
 */
#define SWDEC_CSC_MASK_WORDS 2u
#define SWDEC_CSC_MASK_UNITS (SWDEC_CSC_MASK_WORDS * 32u)

static inline unsigned swdec_csc_unit_count(int height)
{
    return height > 0 && height <= (int) SWDEC_CSC_MASK_UNITS * 8
        ? ((unsigned) height + 7u) / 8u : 0u;
}

/* A known, complete picture can be split without delaying publication by
 * another frame. Reserve one third of its eight-row units for the ME; the
 * CPU owns the suffix until it withdraws the offer and joins the ME.
 * The boundary must separate whole destination cache lines on both cores.
 * Return all units when only serial ownership is safe or worthwhile. */
static inline unsigned swdec_csc_cpu_first(
    unsigned units, const void *destination, unsigned stride_pixels,
    bool me_available)
{
    return me_available && destination != NULL && stride_pixels != 0u
        && units >= 6u && units <= SWDEC_CSC_MASK_UNITS
        && ((uintptr_t) destination & 63u) == 0u
        && (stride_pixels & 3u) == 0u ? units / 3u : units;
}

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
