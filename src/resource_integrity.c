#include "tilefinch/resource_integrity.h"

#include <string.h>

#include "tilefinch/sha256.h"

#if defined(__PSP__)
#include <mbedtls/sha512.h>
#else
#include <openssl/evp.h>
#endif

typedef enum {
    INTEGRITY_HASH_NONE = 0,
    INTEGRITY_HASH_SHA256 = 1,
    INTEGRITY_HASH_SHA384 = 2,
    INTEGRITY_HASH_SHA512 = 3
} IntegrityHash;

typedef struct {
    const char *encoded;
    size_t encoded_length;
    IntegrityHash hash;
} IntegrityToken;

static bool integrity_ascii_whitespace(unsigned char character)
{
    return character == '\t' || character == '\n' || character == '\f'
        || character == '\r' || character == ' ';
}

static IntegrityHash integrity_hash_name(const char *token, size_t length,
                                         size_t *prefix_length)
{
    if (length >= 7u && memcmp(token, "sha256-", 7u) == 0) {
        *prefix_length = 7u;
        return INTEGRITY_HASH_SHA256;
    }
    if (length >= 7u && memcmp(token, "sha384-", 7u) == 0) {
        *prefix_length = 7u;
        return INTEGRITY_HASH_SHA384;
    }
    if (length >= 7u && memcmp(token, "sha512-", 7u) == 0) {
        *prefix_length = 7u;
        return INTEGRITY_HASH_SHA512;
    }
    *prefix_length = 0;
    return INTEGRITY_HASH_NONE;
}

static int base64_digit(unsigned char character)
{
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+' || character == '-') return 62;
    if (character == '/' || character == '_') return 63;
    return -1;
}

static bool decode_digest(const char *encoded, size_t length,
                          uint8_t *output, size_t expected)
{
    if (encoded == NULL || output == NULL || length == 0
        || length > ((expected + 2u) / 3u) * 4u) return false;
    size_t at = 0, out = 0;
    while (at < length) {
        uint32_t word = 0;
        unsigned digits = 0;
        unsigned padding = 0;
        for (unsigned slot = 0; slot < 4; slot++) {
            if (at >= length) {
                if (slot < 2) return false;
                word <<= 6;
                padding++;
                continue;
            }
            unsigned char character = (unsigned char) encoded[at++];
            if (character == '=') {
                if (slot < 2) return false;
                word <<= 6;
                padding++;
                continue;
            }
            if (padding != 0) return false;
            int value = base64_digit(character);
            if (value < 0) return false;
            word = (word << 6) | (uint32_t) value;
            digits++;
        }
        if (digits + padding != 4 || padding > 2) return false;
        unsigned produced = 3u - padding;
        if (out + produced > expected) return false;
        output[out++] = (uint8_t) (word >> 16);
        if (produced > 1) output[out++] = (uint8_t) (word >> 8);
        if (produced > 2) output[out++] = (uint8_t) word;
        if (padding != 0 && at != length) return false;
    }
    return out == expected;
}

static bool digest_sha384_or_512(const uint8_t *bytes, size_t length,
                                 bool sha384, uint8_t output[64])
{
    static const uint8_t empty = 0;
    if (bytes == NULL) bytes = &empty;
#if defined(__PSP__)
    return mbedtls_sha512(bytes, length, output, sha384 ? 1 : 0) == 0;
#else
    unsigned output_length = 0;
    const EVP_MD *algorithm = sha384 ? EVP_sha384() : EVP_sha512();
    return EVP_Digest(bytes, length, output, &output_length,
                      algorithm, NULL) == 1
        && output_length == (sha384 ? 48u : 64u);
#endif
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                                size_t length)
{
    unsigned difference = 0;
    for (size_t i = 0; i < length; i++) difference |= left[i] ^ right[i];
    return difference == 0;
}

TilefinchIntegrityResult tilefinch_resource_integrity_verify(
    const char *metadata, size_t metadata_length,
    const uint8_t *bytes, size_t byte_length)
{
    if (metadata == NULL || metadata_length == 0) {
        return TILEFINCH_INTEGRITY_NOT_ENFORCED;
    }
    if (bytes == NULL && byte_length != 0) return TILEFINCH_INTEGRITY_INVALID;
    if (metadata_length > TILEFINCH_INTEGRITY_METADATA_LIMIT) {
        return TILEFINCH_INTEGRITY_INVALID;
    }
    IntegrityToken tokens[TILEFINCH_INTEGRITY_TOKEN_LIMIT];
    size_t token_count = 0;
    IntegrityHash strongest = INTEGRITY_HASH_NONE;
    bool recognized = false;
    size_t at = 0;
    while (at < metadata_length) {
        while (at < metadata_length
               && integrity_ascii_whitespace(
                      (unsigned char) metadata[at])) at++;
        size_t start = at;
        while (at < metadata_length
               && !integrity_ascii_whitespace(
                      (unsigned char) metadata[at])) at++;
        if (start == at) continue;
        size_t end = at;
        const char *question = memchr(metadata + start, '?', end - start);
        if (question != NULL) end = (size_t) (question - metadata);
        size_t prefix = 0;
        IntegrityHash hash = integrity_hash_name(
            metadata + start, end - start, &prefix);
        if (hash == INTEGRITY_HASH_NONE) continue;
        recognized = true;
        size_t encoded_length = end - start - prefix;
        size_t expected = hash == INTEGRITY_HASH_SHA256 ? 32u
            : hash == INTEGRITY_HASH_SHA384 ? 48u : 64u;
        uint8_t decoded[64];
        if (!decode_digest(metadata + start + prefix, encoded_length,
                           decoded, expected)) continue;
        if (token_count == TILEFINCH_INTEGRITY_TOKEN_LIMIT) {
            return TILEFINCH_INTEGRITY_INVALID;
        }
        tokens[token_count++] = (IntegrityToken) {
            .encoded = metadata + start + prefix,
            .encoded_length = encoded_length,
            .hash = hash
        };
        if (hash > strongest) strongest = hash;
    }
    if (strongest == INTEGRITY_HASH_NONE) {
        return recognized ? TILEFINCH_INTEGRITY_INVALID
                          : TILEFINCH_INTEGRITY_NOT_ENFORCED;
    }
    uint8_t digest[64];
    size_t digest_length = strongest == INTEGRITY_HASH_SHA256 ? 32u
        : strongest == INTEGRITY_HASH_SHA384 ? 48u : 64u;
    bool digested = strongest == INTEGRITY_HASH_SHA256
        ? tilefinch_sha256_digest(bytes, byte_length, digest)
        : digest_sha384_or_512(bytes, byte_length,
                              strongest == INTEGRITY_HASH_SHA384, digest);
    if (!digested) return TILEFINCH_INTEGRITY_INVALID;
    for (size_t i = 0; i < token_count; i++) {
        if (tokens[i].hash != strongest) continue;
        uint8_t expected[64];
        if (decode_digest(tokens[i].encoded, tokens[i].encoded_length,
                          expected, digest_length)
            && constant_time_equal(digest, expected, digest_length)) {
            return TILEFINCH_INTEGRITY_MATCH;
        }
    }
    return TILEFINCH_INTEGRITY_MISMATCH;
}
