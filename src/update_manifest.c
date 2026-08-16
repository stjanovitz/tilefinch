#include "tilefinch/update.h"

#include <string.h>

#include "tilefinch/sha256.h"

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t at;
} UpdateCursor;

static const uint8_t p256_half_order[32] = {
    0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00,
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xde, 0x73, 0x7d, 0x56, 0xd3, 0x8b, 0xcf, 0x42,
    0x79, 0xdc, 0xe5, 0x61, 0x7e, 0x31, 0x92, 0xa8
};

static bool cursor_take(
    UpdateCursor *cursor, size_t length, const uint8_t **bytes)
{
    if (cursor == NULL || length > cursor->length - cursor->at) return false;
    if (bytes != NULL) *bytes = cursor->bytes + cursor->at;
    cursor->at += length;
    return true;
}

static bool cursor_u8(UpdateCursor *cursor, uint8_t *value)
{
    const uint8_t *bytes = NULL;
    if (!cursor_take(cursor, 1, &bytes)) return false;
    *value = bytes[0];
    return true;
}

static bool cursor_u16(UpdateCursor *cursor, uint16_t *value)
{
    const uint8_t *bytes = NULL;
    if (!cursor_take(cursor, 2, &bytes)) return false;
    *value = (uint16_t) ((uint16_t) bytes[0] << 8 | bytes[1]);
    return true;
}

static bool cursor_u32(UpdateCursor *cursor, uint32_t *value)
{
    const uint8_t *bytes = NULL;
    if (!cursor_take(cursor, 4, &bytes)) return false;
    *value = (uint32_t) bytes[0] << 24
        | (uint32_t) bytes[1] << 16
        | (uint32_t) bytes[2] << 8
        | (uint32_t) bytes[3];
    return true;
}

static bool cursor_u64(UpdateCursor *cursor, uint64_t *value)
{
    const uint8_t *bytes = NULL;
    if (!cursor_take(cursor, 8, &bytes)) return false;
    uint64_t parsed = 0;
    for (size_t index = 0; index < 8; index++)
        parsed = parsed << 8 | bytes[index];
    *value = parsed;
    return true;
}

static bool update_digest(
    const char *domain, const uint8_t *bytes, size_t length,
    uint8_t output[32])
{
    TilefinchSha256 context;
    tilefinch_sha256_init(&context);
    return tilefinch_sha256_update(
               &context, (const uint8_t *) domain, strlen(domain) + 1u)
        && tilefinch_sha256_update(&context, bytes, length)
        && tilefinch_sha256_final(&context, output);
}

bool tilefinch_update_key_id(
    const uint8_t public_point[65], uint8_t output[32])
{
    return public_point != NULL && output != NULL
        && public_point[0] == UINT8_C(4)
        && update_digest(
               "tilefinch:p256-key:v1", public_point, 65, output);
}

bool tilefinch_update_signature_is_low_s(const uint8_t signature[64])
{
    if (signature == NULL) return false;
    bool r_nonzero = false, s_nonzero = false;
    for (size_t index = 0; index < 32; index++) {
        r_nonzero |= signature[index] != 0;
        s_nonzero |= signature[32u + index] != 0;
    }
    return r_nonzero && s_nonzero
        && memcmp(signature + 32u, p256_half_order, 32) <= 0;
}

static bool key_id_unique(
    const TilefinchUpdateRoot *root, const uint8_t id[32])
{
    for (size_t index = 0; index < root->root_key_count; index++)
        if (memcmp(root->root_keys[index].id, id, 32) == 0) return false;
    for (size_t index = 0; index < root->release_key_count; index++)
        if (memcmp(root->release_keys[index].id, id, 32) == 0) return false;
    return true;
}

static TilefinchUpdateStatus parse_key(
    UpdateCursor *cursor, TilefinchUpdateRoot *root,
    TilefinchUpdatePublicKey *key)
{
    const uint8_t *id = NULL, *point = NULL;
    if (!cursor_take(cursor, 32, &id)
        || !cursor_take(cursor, 65, &point)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    uint8_t expected[32];
    if (!tilefinch_update_key_id(point, expected)
        || memcmp(id, expected, sizeof(expected)) != 0) {
        return TILEFINCH_UPDATE_BAD_KEY;
    }
    if (!key_id_unique(root, id)) return TILEFINCH_UPDATE_DUPLICATE_KEY;
    memcpy(key->id, id, 32);
    memcpy(key->point, point, 65);
    return TILEFINCH_UPDATE_OK;
}

TilefinchUpdateStatus tilefinch_update_parse_root(
    const uint8_t *bytes, size_t length, TilefinchUpdateRoot *root)
{
    if (bytes == NULL || root == NULL)
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    UpdateCursor cursor = {.bytes = bytes, .length = length};
    TilefinchUpdateRoot parsed = {0};
    uint16_t schema = 0;
    if (!cursor_u16(&cursor, &schema)
        || !cursor_u32(&cursor, &parsed.version)
        || !cursor_u64(&cursor, &parsed.expires_unix)
        || !cursor_u8(&cursor, &parsed.root_threshold)
        || !cursor_u8(&cursor, &parsed.release_threshold)
        || !cursor_u8(&cursor, &parsed.root_key_count)
        || !cursor_u8(&cursor, &parsed.release_key_count)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (schema != 1) return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    if (parsed.version == 0 || parsed.root_threshold == 0
        || parsed.release_threshold == 0
        || parsed.root_key_count > TILEFINCH_UPDATE_MAX_KEYS_PER_ROLE
        || parsed.release_key_count > TILEFINCH_UPDATE_MAX_KEYS_PER_ROLE
        || parsed.root_threshold > parsed.root_key_count
        || parsed.release_threshold > parsed.release_key_count) {
        return TILEFINCH_UPDATE_LIMIT;
    }
    for (size_t index = 0; index < parsed.root_key_count; index++) {
        TilefinchUpdateStatus status = parse_key(
            &cursor, &parsed, &parsed.root_keys[index]);
        if (status != TILEFINCH_UPDATE_OK) return status;
    }
    for (size_t index = 0; index < parsed.release_key_count; index++) {
        TilefinchUpdateStatus status = parse_key(
            &cursor, &parsed, &parsed.release_keys[index]);
        if (status != TILEFINCH_UPDATE_OK) return status;
    }
    if (cursor.at != cursor.length) return TILEFINCH_UPDATE_TRAILING_BYTES;
    *root = parsed;
    return TILEFINCH_UPDATE_OK;
}

static bool safe_ascii(
    const uint8_t *bytes, size_t length, bool filename)
{
    if (length == 0) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned value = bytes[index];
        if (filename) {
            if (!((value >= 'a' && value <= 'z')
                  || (value >= 'A' && value <= 'Z')
                  || (value >= '0' && value <= '9')
                  || value == '.' || value == '_' || value == '-')) {
                return false;
            }
        } else if (value < 0x20u || value > 0x7eu) {
            return false;
        }
    }
    return true;
}

static bool inert_utf8(const uint8_t *bytes, size_t length)
{
    size_t at = 0;
    while (at < length) {
        uint8_t first = bytes[at++];
        if (first < 0x20u || first == 0x7fu) return false;
        if (first < 0x80u) continue;
        size_t continuation = 0;
        uint32_t value = 0, minimum = 0;
        if (first >= 0xc2u && first <= 0xdfu) {
            continuation = 1; value = first & 0x1fu; minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            continuation = 2; value = first & 0x0fu; minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            continuation = 3; value = first & 0x07u; minimum = 0x10000u;
        } else {
            return false;
        }
        if (continuation > length - at) return false;
        for (size_t index = 0; index < continuation; index++) {
            uint8_t byte = bytes[at++];
            if ((byte & 0xc0u) != 0x80u) return false;
            value = value << 6 | (byte & 0x3fu);
        }
        if (value < minimum || value > 0x10ffffu
            || (value >= 0xd800u && value <= 0xdfffu)) return false;
    }
    return true;
}

static TilefinchUpdateStatus parse_manifest(
    const uint8_t *bytes, size_t length, uint16_t expected_package_format,
    TilefinchUpdateManifest *manifest)
{
    UpdateCursor cursor = {.bytes = bytes, .length = length};
    TilefinchUpdateManifest parsed = {0};
    uint16_t schema = 0, platform = 0, package_format = 0;
    if (!cursor_u16(&cursor, &schema)
        || !cursor_u32(&cursor, &parsed.root_version)
        || !cursor_u64(&cursor, &parsed.release_sequence)
        || !cursor_u64(&cursor, &parsed.expires_unix)
        || !cursor_u16(&cursor, &parsed.minimum_launcher_protocol)
        || !cursor_u16(&cursor, &platform)
        || !cursor_u16(&cursor, &package_format)
        || !cursor_u64(&cursor, &parsed.package_size)
        || !cursor_take(&cursor, 32, NULL)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (schema != 1 || package_format != expected_package_format)
        return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    if (platform != TILEFINCH_UPDATE_PLATFORM_PSP)
        return TILEFINCH_UPDATE_WRONG_PLATFORM;
    if (parsed.package_size == 0
        || parsed.package_size > TILEFINCH_UPDATE_MAX_PACKAGE_BYTES)
        return TILEFINCH_UPDATE_LIMIT;
    memcpy(parsed.package_sha256, bytes + cursor.at - 32u, 32);
    parsed.package_format = package_format;

    uint8_t version_length = 0, tag_length = 0, asset_length = 0;
    const uint8_t *value = NULL;
    if (!cursor_u8(&cursor, &version_length)
        || version_length == 0 || version_length >= sizeof(parsed.version)
        || !cursor_take(&cursor, version_length, &value)
        || !safe_ascii(value, version_length, false)) {
        return TILEFINCH_UPDATE_BAD_STRING;
    }
    memcpy(parsed.version, value, version_length);
    if (!cursor_u8(&cursor, &tag_length)
        || tag_length == 0 || tag_length >= sizeof(parsed.tag)
        || !cursor_take(&cursor, tag_length, &value)
        || !safe_ascii(value, tag_length, true)) {
        return TILEFINCH_UPDATE_BAD_STRING;
    }
    memcpy(parsed.tag, value, tag_length);
    if (!cursor_u8(&cursor, &asset_length)
        || asset_length == 0 || asset_length >= sizeof(parsed.asset)
        || !cursor_take(&cursor, asset_length, &value)
        || !safe_ascii(value, asset_length, true)) {
        return TILEFINCH_UPDATE_BAD_STRING;
    }
    memcpy(parsed.asset, value, asset_length);
    uint16_t notes_length = 0;
    if (!cursor_u16(&cursor, &notes_length)
        || notes_length > TILEFINCH_UPDATE_MAX_NOTES_BYTES
        || !cursor_take(&cursor, notes_length, &value)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (!inert_utf8(value, notes_length)) return TILEFINCH_UPDATE_BAD_STRING;
    memcpy(parsed.notes, value, notes_length);
    parsed.notes_length = notes_length;
    if (cursor.at != cursor.length) return TILEFINCH_UPDATE_TRAILING_BYTES;
    *manifest = parsed;
    return TILEFINCH_UPDATE_OK;
}

static const TilefinchUpdatePublicKey *find_key(
    const TilefinchUpdatePublicKey *keys, size_t count, const uint8_t id[32])
{
    for (size_t index = 0; index < count; index++)
        if (memcmp(keys[index].id, id, 32) == 0) return &keys[index];
    return NULL;
}

static TilefinchUpdateStatus verify_signatures(
    UpdateCursor *cursor, uint8_t count,
    const TilefinchUpdatePublicKey *keys, size_t key_count, uint8_t threshold,
    const uint8_t digest[32], const TilefinchUpdateCrypto *crypto)
{
    if (count > TILEFINCH_UPDATE_MAX_SIGNATURES)
        return TILEFINCH_UPDATE_LIMIT;
    uint8_t seen[TILEFINCH_UPDATE_MAX_SIGNATURES][32];
    size_t seen_count = 0, accepted = 0;
    for (size_t index = 0; index < count; index++) {
        const uint8_t *id = NULL, *signature = NULL;
        if (!cursor_take(cursor, 32, &id)
            || !cursor_take(cursor, 64, &signature)) {
            return TILEFINCH_UPDATE_TRUNCATED;
        }
        for (size_t prior = 0; prior < seen_count; prior++)
            if (memcmp(seen[prior], id, 32) == 0)
                return TILEFINCH_UPDATE_DUPLICATE_KEY;
        memcpy(seen[seen_count++], id, 32);
        if (!tilefinch_update_signature_is_low_s(signature))
            return TILEFINCH_UPDATE_BAD_SIGNATURE;
        const TilefinchUpdatePublicKey *key =
            find_key(keys, key_count, id);
        if (key != NULL) {
            if (crypto == NULL || crypto->verify == NULL
                || !crypto->verify(
                       crypto->opaque, key->point, digest, signature)) {
                return TILEFINCH_UPDATE_BAD_SIGNATURE;
            }
            accepted++;
        }
    }
    return accepted >= threshold
        ? TILEFINCH_UPDATE_OK : TILEFINCH_UPDATE_THRESHOLD;
}

static TilefinchUpdateStatus verify_root_update(
    UpdateCursor *cursor, const TilefinchUpdateRoot *old_root,
    const TilefinchUpdateCrypto *crypto, TilefinchUpdateRoot *new_root)
{
    uint16_t root_length = 0;
    const uint8_t *root_bytes = NULL;
    if (!cursor_u16(cursor, &root_length)
        || root_length == 0 || root_length > 2048u
        || !cursor_take(cursor, root_length, &root_bytes)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    TilefinchUpdateStatus status =
        tilefinch_update_parse_root(root_bytes, root_length, new_root);
    if (status != TILEFINCH_UPDATE_OK) return status;
    if (old_root->version == UINT32_MAX
        || new_root->version != old_root->version + 1u)
        return TILEFINCH_UPDATE_ROOT_CHAIN;
    uint8_t digest[32];
    if (!update_digest(
            "tilefinch:root-metadata:v1", root_bytes, root_length, digest))
        return TILEFINCH_UPDATE_BAD_SIGNATURE;
    uint8_t old_count = 0;
    if (!cursor_u8(cursor, &old_count)) return TILEFINCH_UPDATE_TRUNCATED;
    status = verify_signatures(
        cursor, old_count, old_root->root_keys, old_root->root_key_count,
        old_root->root_threshold, digest, crypto);
    if (status != TILEFINCH_UPDATE_OK) return status;
    uint8_t new_count = 0;
    if (!cursor_u8(cursor, &new_count)) return TILEFINCH_UPDATE_TRUNCATED;
    return verify_signatures(
        cursor, new_count, new_root->root_keys, new_root->root_key_count,
        new_root->root_threshold, digest, crypto);
}

static TilefinchUpdateStatus verify_envelope_kind(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified,
    const uint8_t magic[8], const char *domain,
    uint16_t package_format)
{
    if (bytes == NULL || options == NULL || verified == NULL
        || options->embedded_root == NULL
        || options->crypto.verify == NULL) {
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    }
    if (length > TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES)
        return TILEFINCH_UPDATE_LIMIT;
    UpdateCursor cursor = {.bytes = bytes, .length = length};
    const uint8_t *found_magic = NULL;
    uint16_t schema = 0;
    uint8_t root_count = 0;
    if (!cursor_take(&cursor, 8u, &found_magic)
        || !cursor_u16(&cursor, &schema)
        || !cursor_u8(&cursor, &root_count)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (memcmp(found_magic, magic, 8u) != 0)
        return TILEFINCH_UPDATE_BAD_MAGIC;
    if (schema != 1) return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    if (root_count > TILEFINCH_UPDATE_MAX_ROOT_ROTATIONS)
        return TILEFINCH_UPDATE_LIMIT;
    TilefinchUpdateRoot current = *options->embedded_root;
    for (size_t index = 0; index < root_count; index++) {
        TilefinchUpdateRoot next = {0};
        TilefinchUpdateStatus status = verify_root_update(
            &cursor, &current, &options->crypto, &next);
        if (status != TILEFINCH_UPDATE_OK) return status;
        current = next;
    }
    uint16_t manifest_length = 0;
    const uint8_t *manifest_bytes = NULL;
    if (!cursor_u16(&cursor, &manifest_length)
        || manifest_length == 0
        || manifest_length > TILEFINCH_UPDATE_MAX_MANIFEST_BYTES
        || !cursor_take(&cursor, manifest_length, &manifest_bytes)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    uint8_t digest[32];
    if (!update_digest(domain, manifest_bytes, manifest_length, digest))
        return TILEFINCH_UPDATE_BAD_SIGNATURE;
    uint8_t signature_count = 0;
    if (!cursor_u8(&cursor, &signature_count))
        return TILEFINCH_UPDATE_TRUNCATED;
    TilefinchUpdateStatus status = verify_signatures(
        &cursor, signature_count, current.release_keys,
        current.release_key_count, current.release_threshold,
        digest, &options->crypto);
    if (status != TILEFINCH_UPDATE_OK) return status;
    if (cursor.at != cursor.length) return TILEFINCH_UPDATE_TRAILING_BYTES;
    TilefinchUpdateManifest manifest = {0};
    status = parse_manifest(
        manifest_bytes, manifest_length, package_format, &manifest);
    if (status != TILEFINCH_UPDATE_OK) return status;
    /* UINT64_MAX is reserved in the A/B journal to identify an explicitly
       unsigned Developer candidate without changing the on-disk schema. */
    if (manifest.release_sequence == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE)
        return TILEFINCH_UPDATE_LIMIT;
    if (manifest.root_version != current.version)
        return TILEFINCH_UPDATE_ROOT_CHAIN;
    if (manifest.minimum_launcher_protocol > options->launcher_protocol)
        return TILEFINCH_UPDATE_LAUNCHER_TOO_OLD;
    if (!options->clock_valid) return TILEFINCH_UPDATE_CLOCK_UNAVAILABLE;
    if (current.expires_unix < options->now_unix
        || manifest.expires_unix < options->now_unix)
        return TILEFINCH_UPDATE_EXPIRED;
    if (options->installed_sequence_valid
        || options->installed_pair_valid) {
        if (manifest.release_sequence < options->installed_sequence)
            return TILEFINCH_UPDATE_DOWNGRADE;
        if (options->installed_pair_valid
            && manifest.release_sequence == options->installed_sequence
            && memcmp(
                   manifest.package_sha256,
                   options->installed_package_sha256, 32) != 0) {
            return TILEFINCH_UPDATE_EQUIVOCATION;
        }
    }
    *verified = (TilefinchUpdateVerifiedEnvelope) {
        .terminal_root = current,
        .manifest = manifest
    };
    memcpy(verified->manifest_digest, digest, 32);
    return TILEFINCH_UPDATE_OK;
}

TilefinchUpdateStatus tilefinch_update_verify_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'M', 'v', '1', 0, 0
    };
    return verify_envelope_kind(
        bytes, length, options, verified, magic,
        "tilefinch:update-manifest:v1", TILEFINCH_UPDATE_PACKAGE_TFUP);
}

TilefinchUpdateStatus tilefinch_update_verify_voice_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'V', 'M', 'v', '1', 0, 0
    };
    return verify_envelope_kind(
        bytes, length, options, verified, magic,
        "tilefinch:voice-component-manifest:v1",
        TILEFINCH_UPDATE_PACKAGE_VOICE);
}

TilefinchUpdateStatus tilefinch_update_verify_glyph_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'G', 'M', 'v', '1', 0, 0
    };
    return verify_envelope_kind(
        bytes, length, options, verified, magic,
        "tilefinch:glyph-component-manifest:v1",
        TILEFINCH_UPDATE_PACKAGE_GLYPH);
}

TilefinchUpdateStatus tilefinch_update_parse_developer_envelope(
    const uint8_t *bytes, size_t length, uint16_t launcher_protocol,
    TilefinchUpdateVerifiedEnvelope *verified)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'M', 'v', '1', 0, 0
    };
    if (bytes == NULL || verified == NULL)
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    if (length > TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES)
        return TILEFINCH_UPDATE_LIMIT;
    UpdateCursor cursor = {.bytes = bytes, .length = length};
    const uint8_t *found_magic = NULL, *manifest_bytes = NULL;
    uint16_t schema = 0, manifest_length = 0;
    uint8_t root_count = 0, signature_count = 0;
    if (!cursor_take(&cursor, sizeof(magic), &found_magic)
        || !cursor_u16(&cursor, &schema)
        || !cursor_u8(&cursor, &root_count)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (memcmp(found_magic, magic, sizeof(magic)) != 0)
        return TILEFINCH_UPDATE_BAD_MAGIC;
    if (schema != 1) return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    /* Developer metadata is intentionally simple. Root material or a
       signature-like suffix is rejected rather than ambiguously ignored. */
    if (root_count != 0) return TILEFINCH_UPDATE_ROOT_CHAIN;
    if (!cursor_u16(&cursor, &manifest_length)
        || manifest_length == 0
        || manifest_length > TILEFINCH_UPDATE_MAX_MANIFEST_BYTES
        || !cursor_take(&cursor, manifest_length, &manifest_bytes)
        || !cursor_u8(&cursor, &signature_count)) {
        return TILEFINCH_UPDATE_TRUNCATED;
    }
    if (signature_count != 0) return TILEFINCH_UPDATE_BAD_SIGNATURE;
    if (cursor.at != cursor.length) return TILEFINCH_UPDATE_TRAILING_BYTES;
    TilefinchUpdateVerifiedEnvelope parsed = {0};
    TilefinchUpdateStatus status = parse_manifest(
        manifest_bytes, manifest_length, TILEFINCH_UPDATE_PACKAGE_TFUP,
        &parsed.manifest);
    if (status != TILEFINCH_UPDATE_OK) return status;
    if (parsed.manifest.minimum_launcher_protocol > launcher_protocol)
        return TILEFINCH_UPDATE_LAUNCHER_TOO_OLD;
    if (!update_digest(
            "tilefinch:update-manifest:v1", manifest_bytes,
            manifest_length, parsed.manifest_digest)) {
        return TILEFINCH_UPDATE_IO;
    }
    *verified = parsed;
    return TILEFINCH_UPDATE_OK;
}

const char *tilefinch_update_status_name(TilefinchUpdateStatus status)
{
    static const char *const names[] = {
        "ok", "invalid-argument", "truncated", "bad-magic",
        "unsupported-schema", "limit", "trailing-bytes", "bad-string",
        "bad-key", "duplicate-key", "bad-signature", "threshold",
        "root-chain", "expired", "clock-unavailable", "wrong-platform",
        "launcher-too-old", "downgrade", "equivocation", "package-mismatch",
        "bad-path", "duplicate-path", "io", "no-space", "cancelled"
    };
    return (unsigned) status < sizeof(names) / sizeof(names[0])
        ? names[status] : "unknown";
}
