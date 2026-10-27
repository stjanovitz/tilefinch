#ifndef TILEFINCH_FNV1A_H
#define TILEFINCH_FNV1A_H

/* 32-bit FNV-1a hashing shared by the style rule index and the interface
   catalog. A neutral header: including it pulls in no subsystem's budget
   category or internals. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ASCII lowercase. HTML type selectors match without ASCII case, so tag
   keys and tokens hash folded: a foreign element's exact name only shares
   a bucket or token with its case variants, which the matcher tells apart. */
static inline unsigned char style_ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char) (value + 32u)
                                        : value;
}

/* 32-bit FNV-1a of a byte span, continued from `hash` (STYLE_FNV1A_SEED,
   or that seed mixed with a key type). `fold` lowers ASCII letters first,
   for tag names and other case-insensitive keys; a key's every hash must
   agree on it. Tables that keep 0 for an empty slot apply their own
   sentinel to the result. */
#define STYLE_FNV1A_SEED UINT32_C(2166136261)
static inline uint32_t style_fnv1a(uint32_t hash, const void *data,
                                   size_t length, bool fold)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = bytes[i];
        if (fold) value = style_ascii_lower(value);
        hash = (hash ^ value) * UINT32_C(16777619);
    }
    return hash;
}

#endif
