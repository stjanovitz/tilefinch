#ifndef TILEFINCH_RESOURCE_INTEGRITY_H
#define TILEFINCH_RESOURCE_INTEGRITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_INTEGRITY_METADATA_LIMIT 2048u
#define TILEFINCH_INTEGRITY_TOKEN_LIMIT 32u

typedef enum {
    TILEFINCH_INTEGRITY_NOT_ENFORCED = 0,
    TILEFINCH_INTEGRITY_MATCH,
    TILEFINCH_INTEGRITY_MISMATCH,
    TILEFINCH_INTEGRITY_INVALID
} TilefinchIntegrityResult;

/* Streaming SHA-384/SHA-512 (CSP inline hash sources over an element's
   text nodes). finish always releases the context, also after a failure. */
#define TILEFINCH_SHA512_STATE_BYTES 256u
typedef struct {
    union {
        void *pointer;
        uint64_t alignment;
        unsigned char bytes[TILEFINCH_SHA512_STATE_BYTES];
    } state;
    bool sha384;
    bool active;
    bool failed;
} TilefinchSha512;

bool tilefinch_sha512_begin(TilefinchSha512 *context, bool sha384);
void tilefinch_sha512_update(TilefinchSha512 *context,
                             const uint8_t *bytes, size_t length);
/* Writes 48 (SHA-384) or 64 bytes. */
bool tilefinch_sha512_finish(TilefinchSha512 *context, uint8_t output[64]);

/* One recognized token of an `integrity` attribute's metadata (SRI
   "parse metadata"): tokens are separated by ASCII whitespace, options
   after '?' are dropped, and tokens of unknown algorithms are skipped. The
   value is the base64 digest as written; it may be empty or malformed. */
typedef struct {
    /* The token's "sha256-" / "sha384-" / "sha512-" prefix (7 bytes, as
       written) and its digest size in bytes. */
    const char *algorithm;
    size_t digest_length;
    const char *value;
    size_t value_length;
} TilefinchIntegrityToken;
/* Finds the next recognized token at or after *at, and moves *at past it;
   false when none is left. */
bool tilefinch_integrity_next_token(const char *metadata, size_t length,
                                    size_t *at,
                                    TilefinchIntegrityToken *token);

/* Implements the SRI strongest-metadata rule for sha256, sha384, and sha512.
   Unknown algorithms are ignored. Recognized metadata is bounded and matched
   without allocating; malformed or over-limit recognized metadata fails
   closed instead of silently weakening a requested integrity check. */
TilefinchIntegrityResult tilefinch_resource_integrity_verify(
    const char *metadata, size_t metadata_length,
    const uint8_t *bytes, size_t byte_length);

#endif
