/*
 * Signing-ceremony rehearsal against the real embedded trust root.
 *
 * The synthetic-key suites (test_update.c) prove the verification machinery;
 * this test proves a concrete ceremony output: a build embedding the real
 * public root record accepts the envelope signed by the real offline release
 * key, binds it to the exact packed TFUP bytes, and rejects the standard
 * failure cases (wrong key, tampered bytes, downgrade, expiry).
 *
 * It runs only when a ceremony has been rehearsed: the build must embed a
 * root (updater-enabled configuration) and the artifact paths must be
 * supplied via TILEFINCH_PROOF_ENVELOPE, TILEFINCH_PROOF_WRONG_ENVELOPE,
 * and TILEFINCH_PROOF_PACKAGE. Otherwise it skips (exit 77) so ordinary
 * updater-disabled test runs stay green. docs/RELEASE_PROCESS.md describes
 * how to produce the artifacts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/sha256.h"
#include "tilefinch/update.h"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            failures++; \
            fprintf(stderr, "update-root-proof failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition); \
        } \
    } while (0)

static bool read_file(const char *path, uint8_t **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    uint8_t *data = malloc(size == 0 ? 1u : (size_t) size);
    if (data == NULL) {
        fclose(file);
        return false;
    }
    bool ok = fread(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!ok) {
        free(data);
        return false;
    }
    *bytes = data;
    *length = (size_t) size;
    return true;
}

static bool sha256_bytes(const uint8_t *bytes, size_t length,
                         uint8_t digest[32])
{
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    return tilefinch_sha256_update(&sha, bytes, length)
        && tilefinch_sha256_final(&sha, digest);
}

int main(void)
{
    if (!tilefinch_update_root_is_configured()) {
        puts("update-root-proof-tests: skipped "
             "(build has no embedded update root)");
        return 77;
    }
    const char *envelope_path = getenv("TILEFINCH_PROOF_ENVELOPE");
    const char *wrong_path = getenv("TILEFINCH_PROOF_WRONG_ENVELOPE");
    const char *package_path = getenv("TILEFINCH_PROOF_PACKAGE");
    if (envelope_path == NULL || wrong_path == NULL
        || package_path == NULL) {
        puts("update-root-proof-tests: skipped "
             "(TILEFINCH_PROOF_* artifact paths not set)");
        return 77;
    }

    TilefinchUpdateRoot root = {0};
    CHECK(tilefinch_update_embedded_root(&root));
    CHECK(root.version == 1
          && root.root_threshold == 1 && root.release_threshold == 1
          && root.root_key_count == 1 && root.release_key_count == 1);
    uint8_t key_id[32];
    CHECK(tilefinch_update_key_id(root.release_keys[0].point, key_id)
          && memcmp(key_id, root.release_keys[0].id, 32) == 0);
    CHECK(tilefinch_update_key_id(root.root_keys[0].point, key_id)
          && memcmp(key_id, root.root_keys[0].id, 32) == 0);

    uint8_t *envelope = NULL, *wrong = NULL, *package_bytes = NULL;
    size_t envelope_length = 0, wrong_length = 0, package_length = 0;
    CHECK(read_file(envelope_path, &envelope, &envelope_length));
    CHECK(read_file(wrong_path, &wrong, &wrong_length));
    CHECK(read_file(package_path, &package_bytes, &package_length));
    if (failures != 0) return 1;

    /* The manifest expiry in the rehearsal artifacts is 2000000000
       (2033-05); the embedded root expires 1893456000 (2030-01). This
       instant is safely inside both. */
    const uint64_t proof_now = UINT64_C(1785000000);

    TilefinchUpdateVerifyOptions options = {
        .embedded_root = &root,
        .crypto = tilefinch_update_default_crypto(),
        .now_unix = proof_now,
        .clock_valid = true,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .installed_sequence = 1,
        .installed_sequence_valid = true
    };
    TilefinchUpdateVerifiedEnvelope verified = {0};
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    CHECK(verified.manifest.root_version == 1
          && verified.manifest.release_sequence == 2
          && verified.manifest.package_size == package_length);

    /* The envelope binds the exact packed bytes. */
    uint8_t real_package_sha[32];
    memcpy(real_package_sha, verified.manifest.package_sha256, 32);
    uint8_t package_digest[32];
    CHECK(sha256_bytes(package_bytes, package_length, package_digest));
    CHECK(memcmp(package_digest, real_package_sha, 32) == 0);

    /* The package table parses, stays inside the path policy, and its
       first entry's payload hashes to the recorded per-file digest. */
    TilefinchUpdatePackage package = {0};
    CHECK(tilefinch_update_parse_package_table(
              package_bytes, package_length, package_length, &package)
          == TILEFINCH_UPDATE_OK);
    CHECK(package.file_count > 0);
    bool saw_eboot = false;
    for (size_t index = 0; index < package.file_count; index++) {
        const TilefinchUpdatePackageEntry *entry = &package.entries[index];
        CHECK(tilefinch_update_package_path_allowed(entry->path));
        if (strcmp(entry->path, "EBOOT.PBP") == 0) saw_eboot = true;
        CHECK(entry->payload_offset <= package_length
              && entry->size <= package_length - entry->payload_offset);
    }
    CHECK(saw_eboot);
    uint8_t entry_digest[32];
    CHECK(sha256_bytes(
              package_bytes + package.entries[0].payload_offset,
              (size_t) package.entries[0].size, entry_digest));
    CHECK(memcmp(entry_digest, package.entries[0].sha256, 32) == 0);

    /* A single flipped payload byte breaks the manifest binding. */
    package_bytes[package.payload_start + 100] ^= 1u;
    CHECK(sha256_bytes(package_bytes, package_length, package_digest));
    CHECK(memcmp(package_digest, real_package_sha, 32) != 0);
    package_bytes[package.payload_start + 100] ^= 1u;

    /* A flipped envelope byte fails signature verification. */
    envelope[envelope_length - 1] ^= 1u;
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          != TILEFINCH_UPDATE_OK);
    envelope[envelope_length - 1] ^= 1u;

    /* An envelope signed by a key outside the root never verifies. */
    TilefinchUpdateStatus wrong_status = tilefinch_update_verify_envelope(
        wrong, wrong_length, &options, &verified);
    CHECK(wrong_status == TILEFINCH_UPDATE_BAD_SIGNATURE
          || wrong_status == TILEFINCH_UPDATE_THRESHOLD
          || wrong_status == TILEFINCH_UPDATE_BAD_KEY);

    /* Sequence 2 does not clear an installed floor of 3; re-offering the
       installed sequence itself is allowed only for the identical package
       (retry), and a different package at the same sequence is
       equivocation. */
    options.installed_sequence = 3;
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_DOWNGRADE);
    options.installed_sequence = 2;
    options.installed_pair_valid = true;
    memcpy(options.installed_package_sha256, real_package_sha, 32);
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    options.installed_package_sha256[0] ^= 1u;
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_EQUIVOCATION);
    options.installed_pair_valid = false;
    memset(options.installed_package_sha256, 0, 32);
    options.installed_sequence = 1;

    /* Past the manifest expiry the envelope is dead. */
    options.now_unix = UINT64_C(2100000000);
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_EXPIRED);
    options.now_unix = proof_now;

    /* Re-verify cleanly, then walk the A/B transaction with the real
       sequence pair: installed 1 in slot A, this package as the slot-B
       candidate. */
    CHECK(tilefinch_update_verify_envelope(
              envelope, envelope_length, &options, &verified)
          == TILEFINCH_UPDATE_OK);
    TilefinchUpdateState state = {
        .generation = 1,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_PENDING,
        .installed_sequence = 1,
        .candidate_sequence = verified.manifest.release_sequence
    };
    memcpy(state.candidate_sha256, verified.manifest.package_sha256, 32);
    TilefinchUpdateSlot slot = TILEFINCH_UPDATE_SLOT_NONE;
    CHECK(tilefinch_update_boot_decide(&state, false, true, &slot)
              == TILEFINCH_UPDATE_BOOT_START_TRIAL
          && slot == TILEFINCH_UPDATE_SLOT_B);
    /* An unverified pending slot never boots. */
    CHECK(tilefinch_update_boot_decide(&state, false, false, &slot)
              == TILEFINCH_UPDATE_BOOT_RECOVERY
          && slot == TILEFINCH_UPDATE_SLOT_A);
    CHECK(tilefinch_update_state_start_trial(&state)
          && state.trial == TILEFINCH_UPDATE_TRIAL_STARTED);
    CHECK(tilefinch_update_state_confirm_healthy(&state)
          && state.active_slot == TILEFINCH_UPDATE_SLOT_B
          && state.previous_slot == TILEFINCH_UPDATE_SLOT_A
          && state.installed_sequence == 2
          && state.previous_sequence == 1
          && memcmp(state.installed_sha256,
                    verified.manifest.package_sha256, 32) == 0);
    /* The recovery button still reaches the previous healthy slot. */
    CHECK(tilefinch_update_boot_decide(&state, true, true, &slot)
              == TILEFINCH_UPDATE_BOOT_PREVIOUS
          && slot == TILEFINCH_UPDATE_SLOT_A);

    free(envelope);
    free(wrong);
    free(package_bytes);
    if (failures != 0) {
        fprintf(stderr, "update-root-proof-tests: %d failures\n", failures);
        return 1;
    }
    puts("update-root-proof-tests: ok");
    return 0;
}
