#include "tilefinch/sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t load_big_endian_u32(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << 24)
        | ((uint32_t) bytes[1] << 16)
        | ((uint32_t) bytes[2] << 8)
        | (uint32_t) bytes[3];
}

static void store_big_endian_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t words[64];
    for (size_t index = 0; index < 16; index++) {
        words[index] = load_big_endian_u32(block + index * 4u);
    }
    for (size_t index = 16; index < 64; index++) {
        uint32_t left = words[index - 15];
        uint32_t right = words[index - 2];
        uint32_t sigma_zero = rotate_right(left, 7)
            ^ rotate_right(left, 18) ^ (left >> 3);
        uint32_t sigma_one = rotate_right(right, 17)
            ^ rotate_right(right, 19) ^ (right >> 10);
        words[index] = words[index - 16] + sigma_zero
            + words[index - 7] + sigma_one;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (size_t index = 0; index < 64; index++) {
        uint32_t sum_one = rotate_right(e, 6) ^ rotate_right(e, 11)
            ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temporary_one = h + sum_one + choice
            + round_constants[index] + words[index];
        uint32_t sum_zero = rotate_right(a, 2) ^ rotate_right(a, 13)
            ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary_two = sum_zero + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary_one;
        d = c;
        c = b;
        b = a;
        a = temporary_one + temporary_two;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

bool tilefinch_sha256_digest(
    const uint8_t *data, size_t length,
    uint8_t output[TILEFINCH_SHA256_DIGEST_BYTES])
{
    TilefinchSha256 context;
    tilefinch_sha256_init(&context);
    return tilefinch_sha256_update(&context, data, length)
        && tilefinch_sha256_final(&context, output);
}

void tilefinch_sha256_init(TilefinchSha256 *context)
{
    if (context == NULL) return;
    *context = (TilefinchSha256) {
        .state = {
            UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
            UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
            UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
            UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
        }
    };
}

bool tilefinch_sha256_update(
    TilefinchSha256 *context, const uint8_t *data, size_t length)
{
    if (context == NULL || context->finalized
        || (data == NULL && length != 0)
        || (uint64_t) length > UINT64_MAX - context->total_bytes) {
        return false;
    }
    context->total_bytes += (uint64_t) length;
    while (length != 0) {
        size_t room = sizeof(context->block) - context->block_bytes;
        size_t take = length < room ? length : room;
        memcpy(context->block + context->block_bytes, data, take);
        context->block_bytes += take;
        data += take;
        length -= take;
        if (context->block_bytes == sizeof(context->block)) {
            sha256_compress(context->state, context->block);
            context->block_bytes = 0;
        }
    }
    return true;
}

bool tilefinch_sha256_final(
    TilefinchSha256 *context,
    uint8_t output[TILEFINCH_SHA256_DIGEST_BYTES])
{
    if (context == NULL || output == NULL || context->finalized
        || context->total_bytes > UINT64_MAX / UINT64_C(8)) return false;
    uint64_t bit_length = context->total_bytes * UINT64_C(8);
    context->block[context->block_bytes++] = UINT8_C(0x80);
    if (context->block_bytes > 56u) {
        memset(context->block + context->block_bytes, 0,
               sizeof(context->block) - context->block_bytes);
        sha256_compress(context->state, context->block);
        context->block_bytes = 0;
    }
    memset(context->block + context->block_bytes, 0,
           56u - context->block_bytes);
    for (size_t index = 0; index < 8; index++)
        context->block[63u - index] =
            (uint8_t) (bit_length >> (index * 8u));
    sha256_compress(context->state, context->block);
    for (size_t index = 0; index < 8; index++)
        store_big_endian_u32(output + index * 4u, context->state[index]);
    context->finalized = true;
    memset(context->block, 0, sizeof(context->block));
    return true;
}
