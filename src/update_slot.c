#include "tilefinch/update.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tilefinch/sha256.h"

#define SLOT_PATH_LIMIT 768u
#define SLOT_IO_CHUNK (16u * 1024u)

static bool slot_path(
    char *output, size_t output_size, const char *directory,
    const char *relative)
{
    size_t directory_length = strlen(directory);
    size_t relative_length = strlen(relative);
    if (directory_length + 1u + relative_length >= output_size)
        return false;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(
        output + directory_length + 1u,
        relative, relative_length + 1u);
    return true;
}

static bool slot_read_bounded(
    const char *path, uint8_t *bytes, size_t capacity, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t count = fread(bytes, 1, capacity, file);
    bool ok = !ferror(file) && count != capacity && fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (ok && length != NULL) *length = count;
    return ok;
}

static bool slot_hash_file(
    const char *path, uint64_t expected_size, const uint8_t expected_sha[32],
    TilefinchSha256 *package_sha, uint8_t buffer[SLOT_IO_CHUNK])
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_size < 0 || (uint64_t) info.st_size != expected_size)
        return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    TilefinchSha256 file_sha;
    tilefinch_sha256_init(&file_sha);
    uint64_t total = 0;
    bool ok = true;
    while (total < expected_size) {
        size_t wanted = expected_size - total < SLOT_IO_CHUNK
            ? (size_t) (expected_size - total) : SLOT_IO_CHUNK;
        size_t count = fread(buffer, 1, wanted, file);
        if (count != wanted
            || !tilefinch_sha256_update(&file_sha, buffer, count)
            || !tilefinch_sha256_update(package_sha, buffer, count)) {
            ok = false;
            break;
        }
        total += count;
    }
    if (ok && fgetc(file) != EOF) ok = false;
    if (fclose(file) != 0) ok = false;
    uint8_t digest[32];
    return ok && tilefinch_sha256_final(&file_sha, digest)
        && memcmp(digest, expected_sha, 32) == 0;
}

TilefinchUpdateStatus tilefinch_update_verify_slot(
    const char *slot_dir, const TilefinchUpdateSlotVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified)
{
    if (slot_dir == NULL || options == NULL
        || options->trust > TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
        || (options->trust == TILEFINCH_UPDATE_TRUST_SIGNED
            && options->embedded_root == NULL))
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;

    char path[SLOT_PATH_LIMIT];
    uint8_t envelope[TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES + 1u];
    size_t envelope_length = 0;
    if (!slot_path(path, sizeof(path), slot_dir, "slot.tfum")
        || !slot_read_bounded(
               path, envelope, sizeof(envelope), &envelope_length))
        return TILEFINCH_UPDATE_IO;

    TilefinchUpdateVerifiedEnvelope candidate;
    TilefinchUpdateStatus status;
    if (options->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED) {
        status = tilefinch_update_parse_developer_envelope(
            envelope, envelope_length, options->launcher_protocol,
            &candidate);
    } else {
        TilefinchUpdateVerifyOptions verify_options = {
            .embedded_root = options->embedded_root,
            .crypto = tilefinch_update_default_crypto(),
            .now_unix = options->now_unix,
            .clock_valid = options->clock_valid,
            .launcher_protocol = options->launcher_protocol,
            .installed_sequence = options->installed_sequence,
            .installed_sequence_valid = options->installed_sequence_valid,
            .installed_pair_valid = options->installed_pair_valid,
            .allow_downgrade = options->allow_downgrade
        };
        if (options->installed_package_sha256 != NULL) {
            memcpy(
                verify_options.installed_package_sha256,
                options->installed_package_sha256, 32);
        }
        status = tilefinch_update_verify_envelope(
            envelope, envelope_length, &verify_options, &candidate);
    }
    if (status != TILEFINCH_UPDATE_OK) return status;

    uint8_t developer[33];
    size_t developer_length = 0;
    bool developer_marker = slot_path(
            path, sizeof(path), slot_dir, "DEVELOPER")
        && slot_read_bounded(
            path, developer, sizeof(developer), &developer_length);
    if (options->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED) {
        if (!developer_marker || developer_length != 32
            || memcmp(developer, candidate.manifest_digest, 32) != 0) {
            return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
        }
    } else if (developer_marker) {
        return TILEFINCH_UPDATE_BAD_SIGNATURE;
    }

    uint8_t ready[33];
    size_t ready_length = 0;
    if (!slot_path(path, sizeof(path), slot_dir, "READY")
        || !slot_read_bounded(
               path, ready, sizeof(ready), &ready_length)
        || ready_length != 32
        || memcmp(ready, candidate.manifest_digest, 32) != 0)
        return TILEFINCH_UPDATE_PACKAGE_MISMATCH;

    uint8_t table[TILEFINCH_UPDATE_MAX_PACKAGE_TABLE_BYTES + 1u];
    size_t table_length = 0;
    if (!slot_path(path, sizeof(path), slot_dir, "slot.tfut")
        || !slot_read_bounded(
               path, table, sizeof(table), &table_length))
        return TILEFINCH_UPDATE_IO;
    TilefinchUpdatePackage package;
    status = tilefinch_update_parse_package_table(
        table, table_length, candidate.manifest.package_size, &package);
    if (status != TILEFINCH_UPDATE_OK) return status;

    TilefinchSha256 package_sha;
    tilefinch_sha256_init(&package_sha);
    if (!tilefinch_sha256_update(&package_sha, table, table_length))
        return TILEFINCH_UPDATE_IO;
    uint8_t buffer[SLOT_IO_CHUNK];
    uint64_t total = table_length;
    for (size_t index = 0; index < package.file_count; index++) {
        const TilefinchUpdatePackageEntry *entry = &package.entries[index];
        if (!slot_path(path, sizeof(path), slot_dir, entry->path)
            || !slot_hash_file(
                   path, entry->size, entry->sha256, &package_sha, buffer))
            return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
        total += entry->size;
    }
    uint8_t digest[32];
    if (total != candidate.manifest.package_size
        || !tilefinch_sha256_final(&package_sha, digest)
        || memcmp(digest, candidate.manifest.package_sha256, 32) != 0)
        return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
    if (verified != NULL) *verified = candidate;
    return TILEFINCH_UPDATE_OK;
}

bool tilefinch_update_slot_is_developer(const char *slot_dir)
{
    if (slot_dir == NULL) return false;
    char path[SLOT_PATH_LIMIT];
    uint8_t marker[33];
    size_t length = 0;
    return slot_path(path, sizeof(path), slot_dir, "DEVELOPER")
        && slot_read_bounded(path, marker, sizeof(marker), &length)
        && length == 32;
}
