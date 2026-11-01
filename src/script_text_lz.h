#ifndef TILEFINCH_SCRIPT_TEXT_LZ_H
#define TILEFINCH_SCRIPT_TEXT_LZ_H

/* LZ4-compatible block codec for script text the runtime keeps out of the
   JavaScript heap: lazy-webpack factory sources (js_lazy_webpack.c) and
   the function-source store (js_source_store.c).  Greedy single-probe
   matching over a 64 KiB window; every block is independent.  The decoder
   checks every length and distance against both buffers, so a corrupt block
   fails instead of reading or writing out of bounds. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_TEXT_LZ_HASH_BITS 14u
#define SCRIPT_TEXT_LZ_HASH_SIZE (1u << SCRIPT_TEXT_LZ_HASH_BITS)
/* The caller-owned match table script_text_lz_compress() works in. */
#define SCRIPT_TEXT_LZ_TABLE_BYTES (SCRIPT_TEXT_LZ_HASH_SIZE * sizeof(uint32_t))

/* Worst-case compressed size of `length` bytes (incompressible input). */
static inline size_t script_text_lz_bound(size_t length)
{
    return length + length / 255u + 64u;
}

/* Compresses `length` bytes into `output`; returns the compressed size, or
   0 if it does not fit in `capacity`.  `table` holds
   SCRIPT_TEXT_LZ_HASH_SIZE entries and is overwritten. */
size_t script_text_lz_compress(const unsigned char *source, size_t length,
                               unsigned char *output, size_t capacity,
                               uint32_t *table);
/* Decodes a whole block; true only if it fills exactly `output_length`. */
bool script_text_lz_decompress(const unsigned char *input,
                               size_t input_length, unsigned char *output,
                               size_t output_length);
/* Decodes on from *input_at and *output_at (0 and 0 to start) until at least
   `want` bytes of output exist or the block ends, and updates both; false
   if the block is corrupt. A later call continues where this one stopped. */
bool script_text_lz_decompress_until(const unsigned char *input,
                                     size_t input_length,
                                     unsigned char *output,
                                     size_t output_length, size_t *input_at,
                                     size_t *output_at, size_t want);
/* True if the block decodes to exactly `original` (no output buffer). */
bool script_text_lz_matches(const unsigned char *input, size_t input_length,
                            const unsigned char *original,
                            size_t original_length);

#endif
