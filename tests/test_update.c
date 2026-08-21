#include "tilefinch/update.h"
#include "tilefinch/update_history.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/sha256.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[4096];
    size_t length;
} Bytes;

static void append(Bytes *out, const void *bytes, size_t length)
{
    memcpy(out->bytes + out->length, bytes, length);
    out->length += length;
}

static void append_u8(Bytes *out, uint8_t value)
{
    append(out, &value, 1);
}

static void append_u16(Bytes *out, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t) (value >> 8), (uint8_t) value};
    append(out, bytes, sizeof(bytes));
}

static void append_u32(Bytes *out, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t) (value >> 24), (uint8_t) (value >> 16),
        (uint8_t) (value >> 8), (uint8_t) value
    };
    append(out, bytes, sizeof(bytes));
}

static void append_u64(Bytes *out, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t index = 0; index < 8; index++) {
        bytes[7u - index] = (uint8_t) value;
        value >>= 8;
    }
    append(out, bytes, sizeof(bytes));
}

static bool domain_digest(
    const char *domain, const uint8_t *bytes, size_t length,
    uint8_t digest[32])
{
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    return tilefinch_sha256_update(
               &sha, (const uint8_t *) domain, strlen(domain) + 1u)
        && tilefinch_sha256_update(&sha, bytes, length)
        && tilefinch_sha256_final(&sha, digest);
}

static EC_KEY *test_key(unsigned scalar, uint8_t point[65])
{
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *private_value = BN_new();
    EC_POINT *public_value = key == NULL ? NULL
        : EC_POINT_new(EC_KEY_get0_group(key));
    if (key == NULL || private_value == NULL || public_value == NULL
        || BN_set_word(private_value, scalar) != 1
        || EC_POINT_mul(
               EC_KEY_get0_group(key), public_value, private_value,
               NULL, NULL, NULL) != 1
        || EC_KEY_set_private_key(key, private_value) != 1
        || EC_KEY_set_public_key(key, public_value) != 1
        || EC_POINT_point2oct(
               EC_KEY_get0_group(key), public_value,
               POINT_CONVERSION_UNCOMPRESSED, point, 65, NULL) != 65) {
        EC_KEY_free(key);
        key = NULL;
    }
    EC_POINT_free(public_value);
    BN_free(private_value);
    return key;
}

static bool sign_low_s(
    EC_KEY *key, const uint8_t digest[32],
    uint8_t low[64], uint8_t high[64])
{
    ECDSA_SIG *signature = ECDSA_do_sign(digest, 32, key);
    BIGNUM *order = BN_new(), *half = BN_new(), *normalized = BN_new();
    const BIGNUM *r = NULL, *s = NULL;
    bool ok = signature != NULL && order != NULL && half != NULL
        && normalized != NULL
        && EC_GROUP_get_order(EC_KEY_get0_group(key), order, NULL) == 1;
    if (ok) {
        ECDSA_SIG_get0(signature, &r, &s);
        ok = BN_rshift1(half, order) == 1
            && BN_copy(normalized, s) != NULL;
    }
    if (ok && BN_cmp(normalized, half) > 0)
        ok = BN_sub(normalized, order, normalized) == 1;
    if (ok) {
        BIGNUM *opposite = BN_new();
        ok = opposite != NULL
            && BN_sub(opposite, order, normalized) == 1
            && BN_bn2binpad(r, low, 32) == 32
            && BN_bn2binpad(normalized, low + 32, 32) == 32
            && BN_bn2binpad(r, high, 32) == 32
            && BN_bn2binpad(opposite, high + 32, 32) == 32;
        BN_free(opposite);
    }
    BN_free(normalized);
    BN_free(half);
    BN_free(order);
    ECDSA_SIG_free(signature);
    return ok;
}

static void build_manifest_for_package(
    Bytes *manifest, uint64_t release_sequence, uint64_t package_size,
    const uint8_t package_hash[32])
{
    append_u16(manifest, 1);
    append_u32(manifest, 1);
    append_u64(manifest, release_sequence);
    append_u64(manifest, UINT64_C(2000000000));
    append_u16(manifest, TILEFINCH_UPDATE_LAUNCHER_PROTOCOL);
    append_u16(manifest, TILEFINCH_UPDATE_PLATFORM_PSP);
    append_u16(manifest, TILEFINCH_UPDATE_PACKAGE_TFUP);
    append_u64(manifest, package_size);
    append(manifest, package_hash, 32);
    static const char version[] = "0.4.1";
    static const char tag[] = "v0.4.1";
    static const char asset[] = "tilefinch-psp.tfup";
    static const char notes[] =
        "Decoder ABI 3; rebuild if different. Safer, faster updates.";
    append_u8(manifest, sizeof(version) - 1u);
    append(manifest, version, sizeof(version) - 1u);
    append_u8(manifest, sizeof(tag) - 1u);
    append(manifest, tag, sizeof(tag) - 1u);
    append_u8(manifest, sizeof(asset) - 1u);
    append(manifest, asset, sizeof(asset) - 1u);
    append_u16(manifest, sizeof(notes) - 1u);
    append(manifest, notes, sizeof(notes) - 1u);
}

static void build_manifest(Bytes *manifest)
{
    static const uint8_t package_hash[32] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
    };
    build_manifest_for_package(manifest, 43, 12345, package_hash);
}

static void build_envelope(
    const Bytes *manifest, const uint8_t key_id[32],
    const uint8_t signature[64], Bytes *envelope)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'M', 'v', '1', 0, 0
    };
    append(envelope, magic, sizeof(magic));
    append_u16(envelope, 1);
    append_u8(envelope, 0);
    append_u16(envelope, (uint16_t) manifest->length);
    append(envelope, manifest->bytes, manifest->length);
    append_u8(envelope, 1);
    append(envelope, key_id, 32);
    append(envelope, signature, 64);
}

static void build_voice_envelope(
    const Bytes *manifest, const uint8_t key_id[32],
    const uint8_t signature[64], Bytes *envelope)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'V', 'M', 'v', '1', 0, 0
    };
    append(envelope, magic, sizeof(magic));
    append_u16(envelope, 1);
    append_u8(envelope, 0);
    append_u16(envelope, (uint16_t) manifest->length);
    append(envelope, manifest->bytes, manifest->length);
    append_u8(envelope, 1);
    append(envelope, key_id, 32);
    append(envelope, signature, 64);
}

static void build_glyph_envelope(
    const Bytes *manifest, const uint8_t key_id[32],
    const uint8_t signature[64], Bytes *envelope)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'G', 'M', 'v', '1', 0, 0
    };
    append(envelope, magic, sizeof(magic));
    append_u16(envelope, 1);
    append_u8(envelope, 0);
    append_u16(envelope, (uint16_t) manifest->length);
    append(envelope, manifest->bytes, manifest->length);
    append_u8(envelope, 1);
    append(envelope, key_id, 32);
    append(envelope, signature, 64);
}

static void build_developer_envelope(
    const Bytes *manifest, Bytes *envelope)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'M', 'v', '1', 0, 0
    };
    append(envelope, magic, sizeof(magic));
    append_u16(envelope, 1);
    append_u8(envelope, 0); /* no root rotation */
    append_u16(envelope, (uint16_t) manifest->length);
    append(envelope, manifest->bytes, manifest->length);
    append_u8(envelope, 0); /* deliberately unsigned */
}

static int test_envelope(void)
{
    uint8_t root_point[65], release_point[65];
    EC_KEY *root_key = test_key(1, root_point);
    EC_KEY *release_key = test_key(2, release_point);
    CHECK(root_key != NULL && release_key != NULL);
    TilefinchUpdateRoot root = {
        .version = 1,
        .expires_unix = UINT64_C(2000000000),
        .root_threshold = 1,
        .release_threshold = 1,
        .root_key_count = 1,
        .release_key_count = 1
    };
    memcpy(root.root_keys[0].point, root_point, 65);
    memcpy(root.release_keys[0].point, release_point, 65);
    CHECK(tilefinch_update_key_id(
              root_point, root.root_keys[0].id)
          && tilefinch_update_key_id(
              release_point, root.release_keys[0].id));

    Bytes manifest = {0};
    build_manifest(&manifest);
    uint8_t digest[32], low[64], high[64];
    CHECK(domain_digest(
              "tilefinch:update-manifest:v1",
              manifest.bytes, manifest.length, digest)
          && sign_low_s(release_key, digest, low, high)
          && tilefinch_update_signature_is_low_s(low)
          && !tilefinch_update_signature_is_low_s(high));
    TilefinchUpdateVerifyOptions options = {
        .embedded_root = &root,
        .crypto = tilefinch_update_default_crypto(),
        .now_unix = UINT64_C(1900000000),
        .clock_valid = true,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .installed_sequence = 42,
        .installed_sequence_valid = true,
        .installed_pair_valid = true
    };
    memset(options.installed_package_sha256, 0xaa, 32);
    Bytes envelope = {0};
    build_envelope(
        &manifest, root.release_keys[0].id, low, &envelope);
    TilefinchUpdateVerifiedEnvelope verified = {0};
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_OK
          && verified.manifest.release_sequence == 43
          && strcmp(verified.manifest.version, "0.4.1") == 0
          && verified.manifest.optional_decoder_abi_valid
          && verified.manifest.optional_decoder_abi == 3u
          && strcmp(verified.manifest.notes, "Safer, faster updates.") == 0);
    options.expected_tag = "v0.4.0";
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_BAD_STRING);
    options.expected_tag = "v0.4.1";
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    options.expected_tag = NULL;
    Bytes voice_manifest = manifest;
    voice_manifest.bytes[26] = 0;
    voice_manifest.bytes[27] = TILEFINCH_UPDATE_PACKAGE_VOICE;
    CHECK(domain_digest(
              "tilefinch:voice-component-manifest:v1",
              voice_manifest.bytes, voice_manifest.length, digest)
          && sign_low_s(release_key, digest, low, high));
    Bytes voice_envelope = {0};
    build_voice_envelope(
        &voice_manifest, root.release_keys[0].id, low, &voice_envelope);
    CHECK(tilefinch_update_verify_voice_envelope(
              voice_envelope.bytes, voice_envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_OK
          && verified.manifest.package_format
                 == TILEFINCH_UPDATE_PACKAGE_VOICE
          && tilefinch_update_verify_envelope(
                 voice_envelope.bytes, voice_envelope.length,
                 &options, &verified) == TILEFINCH_UPDATE_BAD_MAGIC);
    Bytes glyph_manifest = manifest;
    glyph_manifest.bytes[26] = 0;
    glyph_manifest.bytes[27] = TILEFINCH_UPDATE_PACKAGE_GLYPH;
    CHECK(domain_digest(
              "tilefinch:glyph-component-manifest:v1",
              glyph_manifest.bytes, glyph_manifest.length, digest)
          && sign_low_s(release_key, digest, low, high));
    Bytes glyph_envelope = {0};
    build_glyph_envelope(
        &glyph_manifest, root.release_keys[0].id, low, &glyph_envelope);
    CHECK(tilefinch_update_verify_glyph_envelope(
              glyph_envelope.bytes, glyph_envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_OK
          && verified.manifest.package_format
                 == TILEFINCH_UPDATE_PACKAGE_GLYPH
          && tilefinch_update_verify_voice_envelope(
                 glyph_envelope.bytes, glyph_envelope.length,
                 &options, &verified) == TILEFINCH_UPDATE_BAD_MAGIC);
    Bytes developer = {0};
    build_developer_envelope(&manifest, &developer);
    CHECK(tilefinch_update_parse_developer_envelope(
              developer.bytes, developer.length,
              TILEFINCH_UPDATE_LAUNCHER_PROTOCOL, &verified)
          == TILEFINCH_UPDATE_OK
          && verified.manifest.package_size == 12345);
    developer.bytes[10] = 1; /* root chains are never accepted unsigned */
    CHECK(tilefinch_update_parse_developer_envelope(
              developer.bytes, developer.length,
              TILEFINCH_UPDATE_LAUNCHER_PROTOCOL, &verified)
          == TILEFINCH_UPDATE_ROOT_CHAIN);
    developer.bytes[10] = 0;
    developer.bytes[developer.length - 1u] = 1;
    CHECK(tilefinch_update_parse_developer_envelope(
              developer.bytes, developer.length,
              TILEFINCH_UPDATE_LAUNCHER_PROTOCOL, &verified)
          == TILEFINCH_UPDATE_BAD_SIGNATURE);
    options.installed_sequence = 43;
    options.installed_pair_valid = false;
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    options.installed_sequence = 44;
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_DOWNGRADE);
    options.allow_downgrade = true;
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    CHECK(tilefinch_update_verify_voice_envelope(
              voice_envelope.bytes, voice_envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_DOWNGRADE);
    options.allow_downgrade = false;
    options.installed_sequence = 42;
    options.installed_pair_valid = true;

    Bytes high_envelope = {0};
    build_envelope(
        &manifest, root.release_keys[0].id, high, &high_envelope);
    CHECK(tilefinch_update_verify_envelope(
              high_envelope.bytes, high_envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_BAD_SIGNATURE);
    Bytes unauthenticated_invalid = envelope;
    /*
     * The first version byte follows the 13-byte envelope prefix and the
     * 69-byte fixed manifest prefix. Authentication must fail before the
     * parser or policy reports anything about this non-printable string.
     */
    unauthenticated_invalid.bytes[13u + 69u] = '\n';
    CHECK(tilefinch_update_verify_envelope(
              unauthenticated_invalid.bytes,
              unauthenticated_invalid.length,
              &options, &verified) == TILEFINCH_UPDATE_BAD_SIGNATURE);
    options.clock_valid = false;
    CHECK(tilefinch_update_verify_envelope(
              envelope.bytes, envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_CLOCK_UNAVAILABLE);
    Bytes terminal_manifest = {0}, terminal_envelope = {0};
    build_manifest_for_package(
        &terminal_manifest, TILEFINCH_UPDATE_DEVELOPER_SEQUENCE,
        12345, verified.manifest.package_sha256);
    CHECK(domain_digest(
              "tilefinch:update-manifest:v1",
              terminal_manifest.bytes, terminal_manifest.length, digest)
          && sign_low_s(release_key, digest, low, high));
    build_envelope(
        &terminal_manifest, root.release_keys[0].id, low,
        &terminal_envelope);
    options.clock_valid = true;
    CHECK(tilefinch_update_verify_envelope(
              terminal_envelope.bytes, terminal_envelope.length,
              &options, &verified) == TILEFINCH_UPDATE_LIMIT);
    EC_KEY_free(release_key);
    EC_KEY_free(root_key);
    return 0;
}

static int test_package(void)
{
    CHECK(tilefinch_update_package_path_allowed("EBOOT.PBP")
          && tilefinch_update_package_path_allowed("fonts/ui.ttf")
          && !tilefinch_update_package_path_allowed(
                 "voice-model/en-us/mdef")
          && tilefinch_update_voice_package_path_allowed(
                 "model/en-us/mdef")
          && tilefinch_update_voice_package_path_allowed(
                 "LICENSES/CMUDICT_LICENSE.txt")
          && !tilefinch_update_voice_package_path_allowed("EBOOT.PBP"));
    Bytes table = {0};
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'P', 'v', '1', 0, 0
    };
    append(&table, magic, sizeof(magic));
    append_u16(&table, 1);
    append_u16(&table, 2);
    size_t table_length_offset = table.length;
    append_u32(&table, 0);
    size_t start = table.length;
    static const char first[] = "EBOOT.PBP";
    static const char second[] = "fonts/ui.ttf";
    append_u8(&table, sizeof(first) - 1u);
    append(&table, first, sizeof(first) - 1u);
    append_u64(&table, 4);
    uint8_t digest[32] = {0};
    append(&table, digest, 32);
    size_t first_offset = table.length;
    append_u64(&table, 0);
    append_u8(&table, sizeof(second) - 1u);
    append(&table, second, sizeof(second) - 1u);
    append_u64(&table, 3);
    append(&table, digest, 32);
    size_t second_offset = table.length;
    append_u64(&table, 0);
    uint32_t table_length = (uint32_t) (table.length - start);
    table.bytes[table_length_offset] = (uint8_t) (table_length >> 24);
    table.bytes[table_length_offset + 1] = (uint8_t) (table_length >> 16);
    table.bytes[table_length_offset + 2] = (uint8_t) (table_length >> 8);
    table.bytes[table_length_offset + 3] = (uint8_t) table_length;
    uint64_t payload_start = table.length;
    for (size_t index = 0; index < 8; index++) {
        table.bytes[first_offset + 7u - index] =
            (uint8_t) (payload_start >> (index * 8u));
        table.bytes[second_offset + 7u - index] =
            (uint8_t) ((payload_start + 4u) >> (index * 8u));
    }
    TilefinchUpdatePackage package = {0};
    CHECK(tilefinch_update_parse_package_table(
              table.bytes, table.length, payload_start + 7u, &package)
          == TILEFINCH_UPDATE_OK
          && package.file_count == 2
          && package.entries[1].payload_offset == payload_start + 4u);
    table.bytes[start + 1] = '.';
    CHECK(tilefinch_update_parse_package_table(
              table.bytes, table.length, payload_start + 7u, &package)
          == TILEFINCH_UPDATE_BAD_PATH);
    return 0;
}

static int test_state_and_streaming_hash(void)
{
    TilefinchUpdateState first = {
        .generation = 7,
        .active_slot = TILEFINCH_UPDATE_SLOT_A
    };
    TilefinchUpdateState second = {
        .generation = 8,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_PENDING,
        .installed_sequence = 42,
        .candidate_sequence = 41,
        .candidate_downgrade = true
    };
    uint8_t encoded[2][TILEFINCH_UPDATE_STATE_BYTES];
    TilefinchUpdateState invalid_downgrade = first;
    invalid_downgrade.candidate_downgrade = true;
    CHECK(tilefinch_update_state_encode(&invalid_downgrade, encoded[0])
          == TILEFINCH_UPDATE_INVALID_ARGUMENT);
    CHECK(tilefinch_update_state_encode(&first, encoded[0])
          == TILEFINCH_UPDATE_OK
          && tilefinch_update_state_encode(&second, encoded[1])
          == TILEFINCH_UPDATE_OK);
    TilefinchUpdateState selected = {0};
    unsigned copy = 99;
    CHECK(tilefinch_update_state_select(
              encoded[0], sizeof(encoded[0]), encoded[1],
              sizeof(encoded[1]), &selected, &copy)
          && copy == 1 && selected.generation == 8);
    CHECK(selected.candidate_downgrade);
    encoded[1][142] ^= 1;
    CHECK(tilefinch_update_state_select(
              encoded[0], sizeof(encoded[0]), encoded[1],
              sizeof(encoded[1]), &selected, &copy)
          && copy == 0 && selected.generation == 7);

    TilefinchUpdateSlot boot_slot = TILEFINCH_UPDATE_SLOT_NONE;
    CHECK(tilefinch_update_boot_decide(
              &first, true, true, &boot_slot)
              == TILEFINCH_UPDATE_BOOT_ACTIVE
          && boot_slot == TILEFINCH_UPDATE_SLOT_A);
    CHECK(tilefinch_update_boot_decide(
              &second, false, false, &boot_slot)
              == TILEFINCH_UPDATE_BOOT_RECOVERY
          && boot_slot == TILEFINCH_UPDATE_SLOT_A);
    CHECK(tilefinch_update_boot_decide(
              &second, false, true, &boot_slot)
              == TILEFINCH_UPDATE_BOOT_START_TRIAL
          && boot_slot == TILEFINCH_UPDATE_SLOT_B
          && tilefinch_update_state_start_trial(&second)
          && second.trial == TILEFINCH_UPDATE_TRIAL_STARTED
          && !second.candidate_downgrade
          && tilefinch_update_state_encode(&second, encoded[1])
                 == TILEFINCH_UPDATE_OK
          /* Schema-1 readers predating downgrade support consume the raw
             trial byte. The started record must remain valid for them. */
          && encoded[1][21]
                 == (uint8_t) TILEFINCH_UPDATE_TRIAL_STARTED);
    CHECK(tilefinch_update_boot_decide(
              &second, false, true, &boot_slot)
              == TILEFINCH_UPDATE_BOOT_RECOVERY
          && tilefinch_update_state_retry_trial(&second)
          && second.candidate_downgrade
          && tilefinch_update_state_start_trial(&second)
          && !second.candidate_downgrade
          && tilefinch_update_state_confirm_healthy(&second)
          && second.active_slot == TILEFINCH_UPDATE_SLOT_B
          && second.previous_slot == TILEFINCH_UPDATE_SLOT_A
          && second.pending_slot == TILEFINCH_UPDATE_SLOT_NONE
          && !second.candidate_downgrade
          && second.installed_sequence == 41
          && second.previous_sequence == 42);
    CHECK(tilefinch_update_boot_decide(
              &second, true, true, &boot_slot)
              == TILEFINCH_UPDATE_BOOT_PREVIOUS
          && boot_slot == TILEFINCH_UPDATE_SLOT_A);
    TilefinchUpdateState developer_trial = {
        .generation = 8,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_STARTED,
        .installed_sequence = 42,
        .installed_sha256 = {0x42},
        .candidate_sequence = TILEFINCH_UPDATE_DEVELOPER_SEQUENCE,
        .candidate_sha256 = {0xdd}
    };
    CHECK(tilefinch_update_state_confirm_healthy(&developer_trial)
          && developer_trial.active_slot == TILEFINCH_UPDATE_SLOT_B
          && developer_trial.previous_slot == TILEFINCH_UPDATE_SLOT_A
          && developer_trial.installed_sequence == 42
          && developer_trial.installed_sha256[0] == 0x42
          && developer_trial.previous_sequence == 42
          && developer_trial.candidate_sequence == 0);
    TilefinchUpdateState stale_floor = {
        .generation = 3,
        .active_slot = TILEFINCH_UPDATE_SLOT_B,
        .installed_sequence = 7,
        .installed_sha256 = {1}
    };
    CHECK(tilefinch_update_state_raise_installed_floor(
              &stale_floor, TILEFINCH_UPDATE_SLOT_B, 11)
          && stale_floor.generation == 4
          && stale_floor.installed_sequence == 11
          && stale_floor.installed_sha256[0] == 0
          && !tilefinch_update_state_raise_installed_floor(
                 &stale_floor, TILEFINCH_UPDATE_SLOT_B, 11));
    stale_floor.pending_slot = TILEFINCH_UPDATE_SLOT_A;
    stale_floor.trial = TILEFINCH_UPDATE_TRIAL_PENDING;
    CHECK(!tilefinch_update_state_raise_installed_floor(
        &stale_floor, TILEFINCH_UPDATE_SLOT_B, 12));

    uint8_t one_shot[32], streaming[32];
    static const uint8_t message[] =
        "streamed package hashing must equal one-shot hashing";
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    CHECK(tilefinch_sha256_digest(
              message, sizeof(message) - 1u, one_shot)
          && tilefinch_sha256_update(&sha, message, 7)
          && tilefinch_sha256_update(
              &sha, message + 7, sizeof(message) - 1u - 7u)
          && tilefinch_sha256_final(&sha, streaming)
          && memcmp(one_shot, streaming, 32) == 0);
    return 0;
}

typedef struct {
    unsigned fail_at;
    unsigned operation;
} JournalFault;

static bool journal_fault(void *opaque, const char *operation)
{
    (void) operation;
    JournalFault *fault = opaque;
    return fault->operation++ == fault->fail_at;
}

static int test_journal_faults(void)
{
    uint64_t available = 0;
    CHECK(tilefinch_update_query_free_space("/tmp", &available)
          && available > 0);
    /* Seed both copies so every fault is exercised while replacing the stale
       target generation. This is the first write shape on which PSP FAT and
       POSIX rename semantics differ. */
    for (unsigned fail_at = 0; fail_at < 7; fail_at++) {
        char directory[] = "/tmp/tilefinch-update-journal.XXXXXX";
        CHECK(mkdtemp(directory) != NULL);
        TilefinchUpdateState healthy = {
            .generation = 1,
            .active_slot = TILEFINCH_UPDATE_SLOT_A,
            .installed_sequence = 42
        };
        CHECK(tilefinch_update_journal_store(
                  directory, &healthy, NULL, NULL));
        TilefinchUpdateState selected = healthy;
        selected.generation = 2;
        CHECK(tilefinch_update_journal_store(
                  directory, &selected, NULL, NULL));
        TilefinchUpdateState pending = selected;
        pending.generation = 3;
        pending.pending_slot = TILEFINCH_UPDATE_SLOT_B;
        pending.trial = TILEFINCH_UPDATE_TRIAL_PENDING;
        pending.candidate_sequence = 43;
        JournalFault fault = {.fail_at = fail_at};
        (void) tilefinch_update_journal_store(
            directory, &pending, journal_fault, &fault);
        TilefinchUpdateState recovered;
        CHECK(tilefinch_update_journal_load(
              directory, &recovered, NULL)
              && (recovered.generation == 2
                  || recovered.generation == 3)
              && recovered.active_slot == TILEFINCH_UPDATE_SLOT_A);
        char path[256];
        for (unsigned copy = 0; copy < 2; copy++) {
            snprintf(path, sizeof(path), "%s/update-state.%u", directory, copy);
            remove(path);
            snprintf(
                path, sizeof(path), "%s/update-state.%u.tmp",
                directory, copy);
            remove(path);
        }
        CHECK(rmdir(directory) == 0);
    }

    /* The ordinary, unfaulted third write must advance on filesystems which
       reject rename-over-existing, not merely recover the previous copy. */
    char directory[] = "/tmp/tilefinch-update-journal-third.XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    TilefinchUpdateState state = {
        .generation = 1,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .installed_sequence = 44
    };
    CHECK(tilefinch_update_journal_store(directory, &state, NULL, NULL));
    state.generation = 2;
    CHECK(tilefinch_update_journal_store(directory, &state, NULL, NULL));
    state.generation = 3;
    CHECK(tilefinch_update_journal_store(directory, &state, NULL, NULL));
    TilefinchUpdateState recovered;
    CHECK(tilefinch_update_journal_load(directory, &recovered, NULL)
          && recovered.generation == 3);
    char path[256];
    for (unsigned copy = 0; copy < 2; copy++) {
        snprintf(path, sizeof(path), "%s/update-state.%u", directory, copy);
        remove(path);
        snprintf(
            path, sizeof(path), "%s/update-state.%u.tmp", directory, copy);
        remove(path);
    }
    CHECK(rmdir(directory) == 0);
    return 0;
}

typedef struct {
    size_t fail_at;
    size_t operation;
    bool fired;
} InstallerFault;

static bool installer_fault(void *opaque, const char *operation)
{
    (void) operation;
    InstallerFault *fault = opaque;
    if (fault->operation++ != fault->fail_at) return false;
    fault->fired = true;
    return true;
}

static void cleanup_test_slot(const char *root, const char *slot)
{
    static const char *const files[] = {
        "fonts/ui.ttf", "EBOOT.PBP", "slot.tfum", "slot.tfut", "READY"
    };
    char path[512];
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++) {
        snprintf(path, sizeof(path), "%s/%s/%s", root, slot, files[index]);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/%s/fonts", root, slot);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/%s", root, slot);
    rmdir(path);
}

static bool file_matches(
    const char *path, const uint8_t *expected, size_t expected_length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t buffer[64];
    bool ok = expected_length <= sizeof(buffer)
        && fread(buffer, 1, expected_length, file) == expected_length
        && fgetc(file) == EOF
        && memcmp(buffer, expected, expected_length) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool prepare_inactive_test_slot(const char *root)
{
    char directory[256], path[256];
    int directory_length = snprintf(
        directory, sizeof(directory), "%s/slot-b", root);
    int path_length = snprintf(
        path, sizeof(path), "%s/EBOOT.PBP", directory);
    if (directory_length <= 0
        || (size_t) directory_length >= sizeof(directory)
        || path_length <= 0 || (size_t) path_length >= sizeof(path)
        || mkdir(directory, 0700) != 0) {
        return false;
    }
    FILE *file = fopen(path, "wb");
    static const uint8_t stale[] = "stale-inactive-slot";
    return file != NULL
        && fwrite(stale, 1, sizeof(stale) - 1u, file)
               == sizeof(stale) - 1u
        && fclose(file) == 0;
}

static int test_installer(void)
{
    char root[] = "/tmp/tilefinch-update-install.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char data[256], package_path[256];
    snprintf(data, sizeof(data), "%s/data", root);
    snprintf(package_path, sizeof(package_path), "%s/package.part", root);
    CHECK(mkdir(data, 0700) == 0);
    char active_slot[256], active_eboot[256];
    snprintf(active_slot, sizeof(active_slot), "%s/slot-a", root);
    snprintf(active_eboot, sizeof(active_eboot), "%s/EBOOT.PBP", active_slot);
    CHECK(mkdir(active_slot, 0700) == 0);
    static const uint8_t old_eboot[] = "known-good-eboot";
    FILE *file = fopen(active_eboot, "wb");
    CHECK(file != NULL
          && fwrite(
                 old_eboot, 1, sizeof(old_eboot) - 1u, file)
                 == sizeof(old_eboot) - 1u
          && fclose(file) == 0);

    static const uint8_t eboot[] = "new-eboot";
    static const uint8_t font[] = "new-font";
    uint8_t eboot_hash[32], font_hash[32];
    CHECK(tilefinch_sha256_digest(
              eboot, sizeof(eboot) - 1u, eboot_hash)
          && tilefinch_sha256_digest(
              font, sizeof(font) - 1u, font_hash));
    Bytes package = {0};
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'P', 'v', '1', 0, 0
    };
    append(&package, magic, sizeof(magic));
    append_u16(&package, 1);
    append_u16(&package, 2);
    size_t table_length_at = package.length;
    append_u32(&package, 0);
    size_t table_start = package.length;
    static const char first[] = "EBOOT.PBP";
    static const char second[] = "fonts/ui.ttf";
    append_u8(&package, sizeof(first) - 1u);
    append(&package, first, sizeof(first) - 1u);
    append_u64(&package, sizeof(eboot) - 1u);
    append(&package, eboot_hash, 32);
    size_t first_offset_at = package.length;
    append_u64(&package, 0);
    append_u8(&package, sizeof(second) - 1u);
    append(&package, second, sizeof(second) - 1u);
    append_u64(&package, sizeof(font) - 1u);
    append(&package, font_hash, 32);
    size_t second_offset_at = package.length;
    append_u64(&package, 0);
    uint32_t table_length = (uint32_t) (package.length - table_start);
    package.bytes[table_length_at] = (uint8_t) (table_length >> 24);
    package.bytes[table_length_at + 1] = (uint8_t) (table_length >> 16);
    package.bytes[table_length_at + 2] = (uint8_t) (table_length >> 8);
    package.bytes[table_length_at + 3] = (uint8_t) table_length;
    uint64_t payload_start = package.length;
    for (size_t index = 0; index < 8; index++) {
        package.bytes[first_offset_at + 7u - index] =
            (uint8_t) (payload_start >> (index * 8u));
        package.bytes[second_offset_at + 7u - index] =
            (uint8_t) ((payload_start + sizeof(eboot) - 1u)
                       >> (index * 8u));
    }
    append(&package, eboot, sizeof(eboot) - 1u);
    append(&package, font, sizeof(font) - 1u);
    file = fopen(package_path, "wb");
    CHECK(file != NULL
          && fwrite(package.bytes, 1, package.length, file) == package.length
          && fclose(file) == 0);
    uint8_t package_hash[32];
    CHECK(tilefinch_sha256_digest(
              package.bytes, package.length, package_hash));

    uint8_t root_point[65], release_point[65];
    EC_KEY *root_key = test_key(1, root_point);
    EC_KEY *release_key = test_key(2, release_point);
    CHECK(root_key != NULL && release_key != NULL);
    TilefinchUpdateRoot root_metadata = {
        .version = 1,
        .expires_unix = UINT64_C(2000000000),
        .root_threshold = 1,
        .release_threshold = 1,
        .root_key_count = 1,
        .release_key_count = 1
    };
    memcpy(root_metadata.root_keys[0].point, root_point, 65);
    memcpy(root_metadata.release_keys[0].point, release_point, 65);
    CHECK(tilefinch_update_key_id(
              root_point, root_metadata.root_keys[0].id)
          && tilefinch_update_key_id(
              release_point, root_metadata.release_keys[0].id));
    Bytes manifest_bytes = {0};
    build_manifest_for_package(
        &manifest_bytes, 43, package.length, package_hash);
    uint8_t signed_digest[32], low_signature[64], high_signature[64];
    CHECK(domain_digest(
              "tilefinch:update-manifest:v1",
              manifest_bytes.bytes, manifest_bytes.length, signed_digest)
          && sign_low_s(
              release_key, signed_digest, low_signature, high_signature));
    Bytes envelope_bytes = {0};
    build_envelope(
        &manifest_bytes, root_metadata.release_keys[0].id,
        low_signature, &envelope_bytes);
    TilefinchUpdateVerifyOptions verify_options = {
        .embedded_root = &root_metadata,
        .crypto = tilefinch_update_default_crypto(),
        .now_unix = UINT64_C(1900000000),
        .clock_valid = true,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .installed_sequence = 42,
        .installed_sequence_valid = true,
        .installed_pair_valid = false
    };
    TilefinchUpdateVerifiedEnvelope verified_envelope;
    CHECK(tilefinch_update_verify_envelope(
              envelope_bytes.bytes, envelope_bytes.length,
              &verify_options, &verified_envelope) == TILEFINCH_UPDATE_OK);
    TilefinchUpdateState state = {
        .generation = 1,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .installed_sequence = 44
    };
    TilefinchUpdateInstallOptions options = {
        .package_path = package_path,
        .envelope = envelope_bytes.bytes,
        .envelope_length = envelope_bytes.length,
        .manifest = &verified_envelope.manifest,
        .manifest_digest = verified_envelope.manifest_digest,
        .install_root = root,
        .data_dir = data,
        .inactive_slot = TILEFINCH_UPDATE_SLOT_B,
        .current_state = state,
        .allow_downgrade = true
    };

    size_t exercised_faults = 0;
    for (size_t fail_at = 0; fail_at < 64; fail_at++) {
        cleanup_test_slot(root, "slot-b");
        cleanup_test_slot(root, "slot-b.tmp");
        cleanup_test_slot(root, "slot-b.old");
        CHECK(prepare_inactive_test_slot(root));
        char journal_path[256];
        for (unsigned copy = 0; copy < 2; copy++) {
            snprintf(
                journal_path, sizeof(journal_path),
                "%s/update-state.%u", data, copy);
            remove(journal_path);
            snprintf(
                journal_path, sizeof(journal_path),
                "%s/update-state.%u.tmp", data, copy);
            remove(journal_path);
        }
        CHECK(tilefinch_update_journal_store(data, &state, NULL, NULL));
        InstallerFault fault = {.fail_at = fail_at};
        options.fault = installer_fault;
        options.fault_opaque = &fault;
        Budget fault_budget;
        budget_init(&fault_budget, 1024 * 1024);
        TilefinchUpdateInstallJob *fault_job =
            tilefinch_update_install_create(&fault_budget, &options);
        CHECK(fault_job != NULL);
        TilefinchUpdateInstallSnapshot fault_snapshot = {0};
        for (size_t pump = 0; pump < 100; pump++) {
            CHECK(tilefinch_update_install_pump(fault_job, 4));
            CHECK(tilefinch_update_install_snapshot(
                fault_job, &fault_snapshot));
            if (fault_snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
                break;
        }
        if (fault_snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            exercised_faults = fault.operation;
            tilefinch_update_install_destroy(fault_job);
            CHECK(fault_budget.current == 0);
            break;
        }
        CHECK(fault.fired
              && fault_snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
              && file_matches(
                     active_eboot, old_eboot, sizeof(old_eboot) - 1u));
        TilefinchUpdateState after_fault;
        CHECK(tilefinch_update_journal_load(data, &after_fault, NULL)
              && after_fault.active_slot == TILEFINCH_UPDATE_SLOT_A
              && ((after_fault.pending_slot == TILEFINCH_UPDATE_SLOT_NONE
                   && after_fault.trial == TILEFINCH_UPDATE_TRIAL_NONE)
                  || (after_fault.pending_slot
                          == TILEFINCH_UPDATE_SLOT_B
                      && after_fault.trial
                          == TILEFINCH_UPDATE_TRIAL_PENDING)));
        tilefinch_update_install_destroy(fault_job);
        CHECK(fault_budget.current == 0);
    }
    CHECK(exercised_faults >= 12);
    printf(
        "installer-faults: %zu interrupted operations retained slot A\n",
        exercised_faults);
    options.fault = NULL;
    options.fault_opaque = NULL;

    cleanup_test_slot(root, "slot-b");
    cleanup_test_slot(root, "slot-b.tmp");
    cleanup_test_slot(root, "slot-b.old");
    CHECK(prepare_inactive_test_slot(root));
    char journal_path[256];
    for (unsigned copy = 0; copy < 2; copy++) {
        snprintf(
            journal_path, sizeof(journal_path),
            "%s/update-state.%u", data, copy);
        remove(journal_path);
        snprintf(
            journal_path, sizeof(journal_path),
            "%s/update-state.%u.tmp", data, copy);
        remove(journal_path);
    }
    CHECK(tilefinch_update_journal_store(data, &state, NULL, NULL));
    /* The fault sweep ended on an iteration that reached COMPLETE, and a
       completed install reclaims its own download; restore the part file for
       the clean run below. */
    file = fopen(package_path, "wb");
    CHECK(file != NULL
          && fwrite(package.bytes, 1, package.length, file) == package.length
          && fclose(file) == 0);
    TilefinchUpdateDownloadedPackageProof download_proof = {
        .table = package.bytes,
        .table_length = (size_t) payload_start,
        .package_size = package.length
    };
    memcpy(download_proof.package_sha256, package_hash, 32);
    options.download_proof = &download_proof;

    /* The handoff is accepted only for the package named by the manifest. */
    TilefinchUpdateDownloadedPackageProof wrong_proof = download_proof;
    wrong_proof.package_sha256[0] ^= 0x80u;
    options.download_proof = &wrong_proof;
    Budget rejected_budget;
    budget_init(&rejected_budget, 1024 * 1024);
    CHECK(tilefinch_update_install_create(&rejected_budget, &options) == NULL
          && rejected_budget.current == 0);
    options.download_proof = &download_proof;

    /* The retained table avoids a second whole-package pass, but every
       extracted payload remains authenticated. A same-size mutation made
       after download therefore fails before promotion. */
    package.bytes[payload_start] ^= 0x40u;
    file = fopen(package_path, "wb");
    CHECK(file != NULL
          && fwrite(package.bytes, 1, package.length, file) == package.length
          && fclose(file) == 0);
    Budget corrupt_budget;
    budget_init(&corrupt_budget, 1024 * 1024);
    TilefinchUpdateInstallJob *corrupt_job =
        tilefinch_update_install_create(&corrupt_budget, &options);
    CHECK(corrupt_job != NULL);
    TilefinchUpdateInstallSnapshot corrupt_snapshot = {0};
    CHECK(tilefinch_update_install_snapshot(
              corrupt_job, &corrupt_snapshot)
          && corrupt_snapshot.phase == TILEFINCH_UPDATE_INSTALL_PREPARING);
    for (size_t pump = 0; pump < 100; pump++) {
        CHECK(tilefinch_update_install_pump(corrupt_job, 4));
        CHECK(tilefinch_update_install_snapshot(
            corrupt_job, &corrupt_snapshot));
        if (corrupt_snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
            break;
    }
    CHECK(corrupt_snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
          && corrupt_snapshot.status == TILEFINCH_UPDATE_PACKAGE_MISMATCH
          && file_matches(
                 active_eboot, old_eboot, sizeof(old_eboot) - 1u));
    tilefinch_update_install_destroy(corrupt_job);
    CHECK(corrupt_budget.current == 0);
    package.bytes[payload_start] ^= 0x40u;
    file = fopen(package_path, "wb");
    CHECK(file != NULL
          && fwrite(package.bytes, 1, package.length, file) == package.length
          && fclose(file) == 0);
    Budget budget;
    budget_init(&budget, 1024 * 1024);
    TilefinchUpdateInstallJob *job =
        tilefinch_update_install_create(&budget, &options);
    CHECK(job != NULL);
    TilefinchUpdateInstallSnapshot snapshot;
    CHECK(tilefinch_update_install_snapshot(job, &snapshot)
          && snapshot.phase == TILEFINCH_UPDATE_INSTALL_PREPARING);
    for (size_t pump = 0; pump < 100; pump++) {
        CHECK(tilefinch_update_install_pump(job, 4));
        CHECK(tilefinch_update_install_snapshot(job, &snapshot));
        if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
          && snapshot.files_completed == 2);
    /* A completed install reclaims its own download instead of stranding up
       to 32 MB on the stick until the next one. */
    file = fopen(package_path, "rb");
    CHECK(file == NULL);
    char installed[256];
    snprintf(installed, sizeof(installed), "%s/slot-b/EBOOT.PBP", root);
    file = fopen(installed, "rb");
    uint8_t readback[sizeof(eboot)] = {0};
    CHECK(file != NULL
          && fread(readback, 1, sizeof(eboot) - 1u, file)
                 == sizeof(eboot) - 1u
          && fclose(file) == 0
          && memcmp(readback, eboot, sizeof(eboot) - 1u) == 0);
    TilefinchUpdateState pending;
    CHECK(tilefinch_update_journal_load(data, &pending, NULL)
          && pending.generation == 2
          && pending.active_slot == TILEFINCH_UPDATE_SLOT_A
          && pending.pending_slot == TILEFINCH_UPDATE_SLOT_B
          && pending.trial == TILEFINCH_UPDATE_TRIAL_PENDING
          && pending.candidate_downgrade);
    TilefinchUpdateSlotVerifyOptions slot_options = {
        .embedded_root = &root_metadata,
        .now_unix = UINT64_C(1900000000),
        .clock_valid = true,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .installed_sequence = 44,
        .installed_sequence_valid = true,
        .installed_pair_valid = false
    };
    char slot_dir[256];
    snprintf(slot_dir, sizeof(slot_dir), "%s/slot-b", root);
    TilefinchUpdateVerifiedEnvelope slot_verified;
    CHECK(tilefinch_update_verify_slot(
              slot_dir, &slot_options, NULL)
          == TILEFINCH_UPDATE_DOWNGRADE);
    slot_options.allow_downgrade = true;
    CHECK(tilefinch_update_verify_slot(
              slot_dir, &slot_options, &slot_verified)
          == TILEFINCH_UPDATE_OK
          && slot_verified.manifest.release_sequence == 43);
    slot_options.installed_sequence = 42;
    slot_options.allow_downgrade = false;
    file = fopen(installed, "ab");
    CHECK(file != NULL && fputc(0, file) != EOF && fclose(file) == 0);
    CHECK(tilefinch_update_verify_slot(
              slot_dir, &slot_options, NULL)
          == TILEFINCH_UPDATE_PACKAGE_MISMATCH);
    tilefinch_update_install_destroy(job);
    CHECK(budget.current == 0);

    /* The unsigned Developer path still verifies the package/table/files,
       writes an explicit staged-slot marker and journals an A/B trial. */
    cleanup_test_slot(root, "slot-b");
    cleanup_test_slot(root, "slot-b.tmp");
    cleanup_test_slot(root, "slot-b.old");
    CHECK(prepare_inactive_test_slot(root));
    for (unsigned copy = 0; copy < 2; copy++) {
        snprintf(
            journal_path, sizeof(journal_path),
            "%s/update-state.%u", data, copy);
        remove(journal_path);
        snprintf(
            journal_path, sizeof(journal_path),
            "%s/update-state.%u.tmp", data, copy);
        remove(journal_path);
    }
    CHECK(tilefinch_update_journal_store(data, &state, NULL, NULL));
    file = fopen(package_path, "wb");
    CHECK(file != NULL
          && fwrite(package.bytes, 1, package.length, file) == package.length
          && fclose(file) == 0);
    Bytes developer_envelope = {0};
    build_developer_envelope(&manifest_bytes, &developer_envelope);
    TilefinchUpdateVerifiedEnvelope developer_verified;
    CHECK(tilefinch_update_parse_developer_envelope(
              developer_envelope.bytes, developer_envelope.length,
              TILEFINCH_UPDATE_LAUNCHER_PROTOCOL, &developer_verified)
          == TILEFINCH_UPDATE_OK);
    options.envelope = developer_envelope.bytes;
    options.envelope_length = developer_envelope.length;
    options.manifest = &developer_verified.manifest;
    options.manifest_digest = developer_verified.manifest_digest;
    options.trust = TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED;
    options.allow_downgrade = false;
    budget_init(&budget, 1024 * 1024);
    job = tilefinch_update_install_create(&budget, &options);
    CHECK(job != NULL);
    for (size_t pump = 0; pump < 100; pump++) {
        CHECK(tilefinch_update_install_pump(job, 4));
        CHECK(tilefinch_update_install_snapshot(job, &snapshot));
        if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
          && tilefinch_update_journal_load(data, &pending, NULL)
          && pending.candidate_sequence
                 == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
          && tilefinch_update_slot_is_developer(slot_dir));
    slot_options.embedded_root = NULL;
    slot_options.trust = TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED;
    CHECK(tilefinch_update_verify_slot(
              slot_dir, &slot_options, &slot_verified)
          == TILEFINCH_UPDATE_OK
          && memcmp(
                 slot_verified.manifest.package_sha256,
                 pending.candidate_sha256, 32) == 0);
    slot_options.embedded_root = &root_metadata;
    slot_options.trust = TILEFINCH_UPDATE_TRUST_SIGNED;
    CHECK(tilefinch_update_verify_slot(
              slot_dir, &slot_options, NULL) != TILEFINCH_UPDATE_OK);
    tilefinch_update_install_destroy(job);
    CHECK(budget.current == 0);
    EC_KEY_free(release_key);
    EC_KEY_free(root_key);

    const char *files[] = {
        "slot-b/fonts/ui.ttf", "slot-b/EBOOT.PBP", "slot-b/slot.tfum",
        "slot-b/slot.tfut", "slot-b/DEVELOPER", "slot-b/READY",
        "data/update-state.0",
        "data/update-state.1", "package.part"
    };
    char cleanup[256];
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++) {
        snprintf(cleanup, sizeof(cleanup), "%s/%s", root, files[index]);
        remove(cleanup);
    }
    snprintf(cleanup, sizeof(cleanup), "%s/slot-b/fonts", root);
    rmdir(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/slot-b", root);
    rmdir(cleanup);
    remove(active_eboot);
    rmdir(active_slot);
    rmdir(data);
    CHECK(rmdir(root) == 0);
    return 0;
}

static int test_release_history(void)
{
    static const unsigned char response[] =
        "["
        "{\"tag_name\":\"v0.1.5\",\"draft\":false,"
          "\"prerelease\":false,\"assets\":["
            "{\"name\":\"tilefinch-update-v1.tfum\"}]},"
        "{\"assets\":[{\"name\":\"tilefinch-update-v1.tfum\"}],"
          "\"prerelease\":false,\"tag_name\":\"v0.1.4\","
          "\"draft\":false},"
        "{\"tag_name\":\"v0.1.3\",\"draft\":false,"
          "\"prerelease\":true,\"assets\":["
            "{\"name\":\"tilefinch-update-v1.tfum\"}]},"
        "{\"tag_name\":\"v0.1.2\",\"body\":\"bounded fixture\","
          "\"draft\":false,\"prerelease\":false,\"assets\":["
            "{\"name\":\"notes.txt\"},"
            "{\"name\":\"tilefinch-update-v1.tfum\"}]},"
        "{\"tag_name\":\"v0.1.1\",\"draft\":false,"
          "\"prerelease\":false,\"assets\":["
            "{\"name\":\"tilefinch-update-v1.zip\"}]},"
        "{\"tag_name\":\"unexpected\",\"draft\":false,"
          "\"prerelease\":false,\"assets\":[]}"
        "]";
    TilefinchUpdateHistorySnapshot history;
    CHECK(tilefinch_update_history_parse(
              response, sizeof(response) - 1u, "0.1.5", &history)
          && history.phase == TILEFINCH_UPDATE_HISTORY_READY
          && history.count == 2u
          && strcmp(history.versions[0], "0.1.4") == 0
          && strcmp(history.versions[1], "0.1.2") == 0);
    char tag[16];
    CHECK(tilefinch_update_history_tag(
              &history, 1u, tag, sizeof(tag))
          && strcmp(tag, "v0.1.2") == 0
          && !tilefinch_update_history_tag(
                 &history, history.count, tag, sizeof(tag)));
    static const unsigned char malformed[] =
        "[{\"tag_name\":\"v0.1.4\",\"assets\":[}]";
    CHECK(!tilefinch_update_history_parse(
        malformed, sizeof(malformed) - 1u, "0.1.5", &history));
    return 0;
}

int main(void)
{
    /*
     * This predicate is the developer channel's entire endpoint policy, and
     * that channel ships an unsigned package, so cleartext is refused: over
     * plain http anything on the path can substitute the payload and
     * nothing downstream verifies it. http also skipped the per-hop
     * redirect validator, which only engages for https. Stable and Beta
     * never reach here -- their GitHub endpoints are built as fixed https
     * URLs -- so this tightening cannot affect them.
     */
    CHECK(tilefinch_update_url_is_valid(
              "https://updates.example.test/dev/update.tfum", 256)
          && tilefinch_update_url_is_valid(
              "https://192.0.2.1/update.tfum", 256)
          && !tilefinch_update_url_is_valid(
              "http://updates.example.test/dev/update.tfum", 256)
          && !tilefinch_update_url_is_valid(
              "http://192.0.2.1/update.tfum", 256)
          && !tilefinch_update_url_is_valid(
              "https://user@example.test/update.tfum", 256)
          && !tilefinch_update_url_is_valid(
              "https://example.test/update.tfum#replacement", 256)
          && !tilefinch_update_url_is_valid(
              "file:///tmp/update.tfum", 256));
    char prepared[768];
    CHECK(tilefinch_update_prepare_download_url(
              "https://1drv.ms/u/s!metadata-token", prepared,
              sizeof(prepared))
          && strcmp(
                 prepared,
                 "https://1drv.ms/u/s!metadata-token?download=1") == 0
          && tilefinch_update_prepare_download_url(
                 "https://tenant.sharepoint.com/:u:/g/file?e=abc",
                 prepared, sizeof(prepared))
          && strcmp(
                 prepared,
                 "https://tenant.sharepoint.com/:u:/g/file?e=abc&download=1")
                 == 0
          && tilefinch_update_prepare_download_url(
                 "https://onedrive.live.com/view.aspx?download=0&id=123",
                 prepared, sizeof(prepared))
          && strcmp(
                 prepared,
                 "https://onedrive.live.com/view.aspx?download=1&id=123")
                 == 0
          && tilefinch_update_prepare_download_url(
                 "https://updates.example.test/dev/update.tfum",
                 prepared, sizeof(prepared))
          && strcmp(
                 prepared,
                 "https://updates.example.test/dev/update.tfum") == 0);
    puts("test: canonical signed update envelope and low-S twin");
    CHECK(test_envelope() == 0);
    puts("test: bounded TFUP table and path policy");
    CHECK(test_package() == 0);
    puts("test: redundant journal selection and streaming SHA-256");
    CHECK(test_state_and_streaming_hash() == 0);
    puts("test: journal interruption always retains a healthy choice");
    CHECK(test_journal_faults() == 0);
    puts("test: bounded installer stages and promotes the inactive slot");
    CHECK(test_installer() == 0);
    puts("test: bounded GitHub release history parser");
    CHECK(test_release_history() == 0);
    puts("update-tests: all checks passed");
    return 0;
}
