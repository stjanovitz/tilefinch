/* LZ4-compatible block codec for retained script text (see
   script_text_lz.h).  Shared by the lazy-webpack factory tier
   (js_lazy_webpack.c) and the function-source store (js_source_store.c). */
#include "script_text_lz.h"

#include <stdint.h>
#include <string.h>

static uint32_t text_lz_word(const unsigned char *source)
{
    uint32_t word;
    memcpy(&word, source, sizeof(word));
    return word;
}

static size_t text_lz_hash(uint32_t word)
{
    return (size_t) ((word * UINT32_C(2654435761))
                     >> (32u - SCRIPT_TEXT_LZ_HASH_BITS));
}

static bool text_lz_emit_length(unsigned char *output, size_t capacity,
                                size_t *at, size_t length)
{
    while (length >= 255) {
        if (*at >= capacity) return false;
        output[(*at)++] = 255;
        length -= 255;
    }
    if (*at >= capacity) return false;
    output[(*at)++] = (unsigned char) length;
    return true;
}

/* A small independent LZ4-compatible block encoder keeps cold factory source
   compact without adding a PSP runtime dependency. Blocks are per factory,
   so first-call compilation and Function#toString never inflate a complete
   multi-megabyte bundle. */
size_t script_text_lz_compress(
    const unsigned char *source, size_t length,
    unsigned char *output, size_t capacity, uint32_t *table)
{
    memset(table, 0, SCRIPT_TEXT_LZ_HASH_SIZE * sizeof(*table));
    size_t anchor = 0, input = 0, out = 0;
    while (input + 4 <= length) {
        size_t hash = text_lz_hash(text_lz_word(source + input));
        uint32_t encoded = table[hash];
        table[hash] = input < UINT32_MAX ? (uint32_t) input + 1u : 0;
        if (encoded == 0) {
            input++;
            continue;
        }
        size_t match = (size_t) encoded - 1u;
        if (match >= input || input - match > UINT16_MAX
            || match + 4 > length
            || memcmp(source + match, source + input, 4) != 0) {
            input++;
            continue;
        }
        size_t matched = 4;
        while (input + matched < length
               && source[match + matched] == source[input + matched]) {
            matched++;
        }
        if (out >= capacity) return 0;
        size_t token_at = out++;
        size_t literals = input - anchor;
        unsigned token = (unsigned) (literals < 15 ? literals : 15) << 4;
        if (literals >= 15
            && !text_lz_emit_length(output, capacity, &out,
                                    literals - 15)) return 0;
        if (literals > capacity - out) return 0;
        memcpy(output + out, source + anchor, literals);
        out += literals;
        if (capacity - out < 2) return 0;
        size_t distance = input - match;
        output[out++] = (unsigned char) distance;
        output[out++] = (unsigned char) (distance >> 8);
        size_t match_code = matched - 4;
        token |= (unsigned) (match_code < 15 ? match_code : 15);
        if (match_code >= 15
            && !text_lz_emit_length(output, capacity, &out,
                                    match_code - 15)) return 0;
        output[token_at] = (unsigned char) token;
        input += matched;
        anchor = input;
    }
    size_t literals = length - anchor;
    if (out >= capacity) return 0;
    size_t token_at = out++;
    output[token_at] = (unsigned char) ((literals < 15 ? literals : 15) << 4);
    if (literals >= 15
        && !text_lz_emit_length(output, capacity, &out,
                                literals - 15)) return 0;
    if (literals > capacity - out) return 0;
    memcpy(output + out, source + anchor, literals);
    return out + literals;
}

static bool text_lz_read_length(const unsigned char *input,
                                size_t input_length, size_t *input_at,
                                size_t *length)
{
    for (;;) {
        if (*input_at >= input_length) return false;
        unsigned value = input[(*input_at)++];
        if (*length > SIZE_MAX - value) return false;
        *length += value;
        if (value != 255) return true;
    }
}

bool script_text_lz_decompress_until(
    const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_length,
    size_t *input_at, size_t *output_at, size_t want)
{
    size_t in = *input_at, out = *output_at;
    if (want > output_length) want = output_length;
    /* A whole block is read to its end: it closes with a literal-only
       token, which may follow the match that filled the output. */
    while (in < input_length && (out < want || want == output_length)) {
        unsigned token = input[in++];
        size_t literals = token >> 4;
        if (literals == 15
            && !text_lz_read_length(
                input, input_length, &in, &literals)) return false;
        if (literals > input_length - in
            || literals > output_length - out) return false;
        memcpy(output + out, input + in, literals);
        in += literals;
        out += literals;
        if (in == input_length) break;
        if (input_length - in < 2) return false;
        size_t distance = (size_t) input[in]
                        | (size_t) input[in + 1] << 8;
        in += 2;
        if (distance == 0 || distance > out) return false;
        size_t matched = token & 15u;
        if (matched == 15
            && !text_lz_read_length(
                input, input_length, &in, &matched)) return false;
        if (matched > SIZE_MAX - 4) return false;
        matched += 4;
        if (matched > output_length - out) return false;
        if (distance >= matched) {
            memcpy(output + out, output + out - distance, matched);
        } else {
            for (size_t i = 0; i < matched; i++) {
                output[out + i] = output[out + i - distance];
            }
        }
        out += matched;
    }
    *input_at = in;
    *output_at = out;
    return true;
}

bool script_text_lz_decompress(
    const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_length)
{
    size_t in = 0, out = 0;
    return script_text_lz_decompress_until(input, input_length, output,
                                           output_length, &in, &out,
                                           output_length)
        && in == input_length && out == output_length;
}

/* Verify a compressed image against its original without materializing a
   second full-size output. Match references point backward into bytes already
   checked in original, which is equivalent to comparing a decoded block. */
bool script_text_lz_matches(
    const unsigned char *input, size_t input_length,
    const unsigned char *original, size_t original_length)
{
    size_t in = 0, out = 0;
    while (in < input_length) {
        unsigned token = input[in++];
        size_t literals = token >> 4;
        if (literals == 15
            && !text_lz_read_length(
                input, input_length, &in, &literals)) return false;
        if (literals > input_length - in
            || literals > original_length - out
            || memcmp(input + in, original + out, literals) != 0) {
            return false;
        }
        in += literals;
        out += literals;
        if (in == input_length) return out == original_length;
        if (input_length - in < 2) return false;
        size_t distance = (size_t) input[in]
                        | (size_t) input[in + 1] << 8;
        in += 2;
        if (distance == 0 || distance > out) return false;
        size_t matched = token & 15u;
        if (matched == 15
            && !text_lz_read_length(
                input, input_length, &in, &matched)) return false;
        if (matched > SIZE_MAX - 4) return false;
        matched += 4;
        if (matched > original_length - out) return false;
        for (size_t i = 0; i < matched; i++) {
            if (original[out + i] != original[out + i - distance]) {
                return false;
            }
        }
        out += matched;
    }
    return out == original_length;
}
