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

/* Implements the SRI strongest-metadata rule for sha256, sha384, and sha512.
   Unknown algorithms are ignored. Recognized metadata is bounded and matched
   without allocating; malformed or over-limit recognized metadata fails
   closed instead of silently weakening a requested integrity check. */
TilefinchIntegrityResult tilefinch_resource_integrity_verify(
    const char *metadata, size_t metadata_length,
    const uint8_t *bytes, size_t byte_length);

#endif
