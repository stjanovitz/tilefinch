#ifndef TILEFINCH_SHA256_H
#define TILEFINCH_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_SHA256_DIGEST_BYTES 32u

typedef struct {
    uint32_t state[8];
    uint8_t block[64];
    uint64_t total_bytes;
    size_t block_bytes;
    bool finalized;
} TilefinchSha256;

void tilefinch_sha256_init(TilefinchSha256 *context);
bool tilefinch_sha256_update(
    TilefinchSha256 *context, const uint8_t *data, size_t length);
bool tilefinch_sha256_final(
    TilefinchSha256 *context,
    uint8_t output[TILEFINCH_SHA256_DIGEST_BYTES]);

/* Computes the FIPS 180-4 SHA-256 digest without allocating memory. */
bool tilefinch_sha256_digest(
    const uint8_t *data, size_t length,
    uint8_t output[TILEFINCH_SHA256_DIGEST_BYTES]);

#endif
