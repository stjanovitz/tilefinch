#include "tilefinch/resource_integrity.h"

#include <string.h>

#include "tilefinch/sha256.h"

#if defined(__PSP__)
#include <mbedtls/sha512.h>
#else
#include <openssl/evp.h>
#endif

typedef struct {
    const char *encoded;
    size_t encoded_length;
    size_t digest_length;
} IntegrityToken;

static bool integrity_ascii_whitespace(unsigned char character)
{
    return character == '\t' || character == '\n' || character == '\f'
        || character == '\r' || character == ' ';
}

static bool integrity_ascii_equal_lower(
    const char *value, const char *lower, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) value[i];
        if (character >= 'A' && character <= 'Z')
            character = (unsigned char) (character - 'A' + 'a');
        if (character != (unsigned char) lower[i]) return false;
    }
    return true;
}

/* The digest size of a token's algorithm prefix; 0 when unknown. */
static size_t integrity_digest_length(const char *token, size_t length)
{
    if (length < 7u) return 0;
    if (integrity_ascii_equal_lower(token, "sha256-", 7u)) return 32u;
    if (integrity_ascii_equal_lower(token, "sha384-", 7u)) return 48u;
    if (integrity_ascii_equal_lower(token, "sha512-", 7u)) return 64u;
    return 0;
}

bool tilefinch_integrity_next_token(const char *metadata, size_t length,
                                    size_t *at,
                                    TilefinchIntegrityToken *token)
{
    if (metadata == NULL || at == NULL || token == NULL) return false;
    while (*at < length) {
        while (*at < length
               && integrity_ascii_whitespace(
                      (unsigned char) metadata[*at])) (*at)++;
        size_t start = *at;
        while (*at < length
               && !integrity_ascii_whitespace(
                      (unsigned char) metadata[*at])) (*at)++;
        if (start == *at) continue;
        size_t end = *at;
        const char *question = memchr(metadata + start, '?', end - start);
        if (question != NULL) end = (size_t) (question - metadata);
        size_t digest_length = integrity_digest_length(
            metadata + start, end - start);
        if (digest_length == 0) continue;
        *token = (TilefinchIntegrityToken) {
            .algorithm = metadata + start,
            .digest_length = digest_length,
            .value = metadata + start + 7u,
            .value_length = end - start - 7u
        };
        return true;
    }
    return false;
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

#if defined(__PSP__)
_Static_assert(sizeof(mbedtls_sha512_context)
                   <= TILEFINCH_SHA512_STATE_BYTES,
               "SHA-512 state must fit its opaque storage");
#endif

bool tilefinch_sha512_begin(TilefinchSha512 *context, bool sha384)
{
    if (context == NULL) return false;
    memset(context, 0, sizeof(*context));
    context->sha384 = sha384;
#if defined(__PSP__)
    mbedtls_sha512_context *state =
        (mbedtls_sha512_context *) context->state.bytes;
    mbedtls_sha512_init(state);
    context->active = true;
    context->failed = mbedtls_sha512_starts(state, sha384 ? 1 : 0) != 0;
#else
    EVP_MD_CTX *state = EVP_MD_CTX_new();
    context->state.pointer = state;
    context->active = state != NULL;
    context->failed = state == NULL || EVP_DigestInit_ex(
        state, sha384 ? EVP_sha384() : EVP_sha512(), NULL) != 1;
#endif
    return context->active && !context->failed;
}

void tilefinch_sha512_update(TilefinchSha512 *context,
                             const uint8_t *bytes, size_t length)
{
    if (context == NULL || !context->active || context->failed
        || length == 0) return;
    if (bytes == NULL) {
        context->failed = true;
        return;
    }
#if defined(__PSP__)
    context->failed = mbedtls_sha512_update(
        (mbedtls_sha512_context *) context->state.bytes, bytes, length) != 0;
#else
    context->failed = EVP_DigestUpdate(
        context->state.pointer, bytes, length) != 1;
#endif
}

bool tilefinch_sha512_finish(TilefinchSha512 *context, uint8_t output[64])
{
    if (context == NULL || !context->active) return false;
    bool ok = !context->failed && output != NULL;
#if defined(__PSP__)
    mbedtls_sha512_context *state =
        (mbedtls_sha512_context *) context->state.bytes;
    if (ok) ok = mbedtls_sha512_finish(state, output) == 0;
    mbedtls_sha512_free(state);
#else
    unsigned length = 0;
    if (ok) ok = EVP_DigestFinal_ex(context->state.pointer, output,
                                    &length) == 1
        && length == (context->sha384 ? 48u : 64u);
    EVP_MD_CTX_free(context->state.pointer);
#endif
    context->active = false;
    return ok;
}

static bool digest_sha384_or_512(const uint8_t *bytes, size_t length,
                                 bool sha384, uint8_t output[64])
{
    TilefinchSha512 context;
    (void) tilefinch_sha512_begin(&context, sha384);
    tilefinch_sha512_update(&context, bytes, length);
    return tilefinch_sha512_finish(&context, output);
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
    size_t strongest = 0;
    bool recognized = false;
    size_t at = 0;
    TilefinchIntegrityToken token;
    while (tilefinch_integrity_next_token(metadata, metadata_length, &at,
                                          &token)) {
        recognized = true;
        uint8_t decoded[64];
        if (!decode_digest(token.value, token.value_length, decoded,
                           token.digest_length)) continue;
        if (token_count == TILEFINCH_INTEGRITY_TOKEN_LIMIT) {
            return TILEFINCH_INTEGRITY_INVALID;
        }
        tokens[token_count++] = (IntegrityToken) {
            .encoded = token.value,
            .encoded_length = token.value_length,
            .digest_length = token.digest_length
        };
        if (token.digest_length > strongest) strongest = token.digest_length;
    }
    if (strongest == 0) {
        return recognized ? TILEFINCH_INTEGRITY_INVALID
                          : TILEFINCH_INTEGRITY_NOT_ENFORCED;
    }
    uint8_t digest[64];
    bool digested = strongest == 32u
        ? tilefinch_sha256_digest(bytes, byte_length, digest)
        : digest_sha384_or_512(bytes, byte_length, strongest == 48u, digest);
    if (!digested) return TILEFINCH_INTEGRITY_INVALID;
    for (size_t i = 0; i < token_count; i++) {
        if (tokens[i].digest_length != strongest) continue;
        uint8_t expected[64];
        if (decode_digest(tokens[i].encoded, tokens[i].encoded_length,
                          expected, strongest)
            && constant_time_equal(digest, expected, strongest)) {
            return TILEFINCH_INTEGRITY_MATCH;
        }
    }
    return TILEFINCH_INTEGRITY_MISMATCH;
}
