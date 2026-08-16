#include "tilefinch/voice_component.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    uint8_t data[8192];
    size_t length;
} Buffer;

static void put(Buffer *buffer, const void *data, size_t length)
{
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static void put_u8(Buffer *buffer, uint8_t value)
{
    put(buffer, &value, 1);
}

static void put_u16(Buffer *buffer, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t) (value >> 8), (uint8_t) value};
    put(buffer, bytes, sizeof(bytes));
}

static void put_u32(Buffer *buffer, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t) (value >> 24), (uint8_t) (value >> 16),
        (uint8_t) (value >> 8), (uint8_t) value
    };
    put(buffer, bytes, sizeof(bytes));
}

static void put_u64(Buffer *buffer, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t at = 0; at < 8; at++) {
        bytes[7u - at] = (uint8_t) value;
        value >>= 8;
    }
    put(buffer, bytes, sizeof(bytes));
}

static bool digest_bytes(
    const uint8_t *bytes, size_t length, uint8_t digest[32])
{
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    return tilefinch_sha256_update(&sha, bytes, length)
        && tilefinch_sha256_final(&sha, digest);
}

static bool read_file_bounded(const char *path, size_t maximum,
                              uint8_t **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end <= 0 || (size_t) end > maximum
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    uint8_t *data = malloc((size_t) end);
    bool ok = data != NULL
        && fread(data, 1, (size_t) end, file) == (size_t) end
        && fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        free(data);
        return false;
    }
    *bytes = data;
    *length = (size_t) end;
    return true;
}

static int verify_release_artifacts(void)
{
    const char *directory = getenv("TILEFINCH_VOICE_PROOF_DIR");
    if (directory == NULL || directory[0] == '\0') return 0;
    CHECK(tilefinch_update_root_is_configured());
    char envelope_path[512], package_path[512];
    CHECK(snprintf(envelope_path, sizeof(envelope_path),
                   "%s/tilefinch-voice-en-us-v1.tfvm", directory) > 0);
    CHECK(snprintf(package_path, sizeof(package_path),
                   "%s/tilefinch-voice-en-us-v1.tfvp", directory) > 0);
    uint8_t *envelope = NULL, *package = NULL;
    size_t envelope_length = 0, package_length = 0;
    CHECK(read_file_bounded(
        envelope_path, TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES,
        &envelope, &envelope_length));
    CHECK(read_file_bounded(
        package_path, TILEFINCH_UPDATE_MAX_PACKAGE_BYTES,
        &package, &package_length));
    TilefinchUpdateRoot root = {0};
    CHECK(tilefinch_update_embedded_root(&root));
    TilefinchUpdateVerifiedEnvelope verified = {0};
    CHECK(tilefinch_update_verify_voice_envelope(
              envelope, envelope_length,
              &(TilefinchUpdateVerifyOptions) {
                  .embedded_root = &root,
                  .crypto = tilefinch_update_default_crypto(),
                  .now_unix = UINT64_C(1786600000),
                  .clock_valid = true,
                  .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL
              }, &verified)
          == TILEFINCH_UPDATE_OK);
    CHECK(verified.manifest.package_format
          == TILEFINCH_UPDATE_PACKAGE_VOICE);
    CHECK(verified.manifest.release_sequence == 1u);
    CHECK(strcmp(verified.manifest.tag, "components-v1") == 0);
    CHECK(strcmp(verified.manifest.asset,
                 "tilefinch-voice-en-us-v1.tfvp") == 0);
    uint8_t digest[32];
    CHECK(package_length == verified.manifest.package_size
          && digest_bytes(package, package_length, digest)
          && memcmp(digest, verified.manifest.package_sha256, 32) == 0);
    TilefinchUpdatePackage parsed = {0};
    CHECK(tilefinch_update_parse_voice_package_table(
              package, package_length, package_length, &parsed)
          == TILEFINCH_UPDATE_OK);
    CHECK(parsed.file_count == 18u);
    free(envelope);
    free(package);
    puts("voice-component release-proof=PASS");
    return 0;
}

typedef struct {
    const char *path;
    const uint8_t *bytes;
    size_t length;
} FixtureFile;

static bool build_package(Buffer *package, bool corrupt_last_payload)
{
    static const uint8_t info[] = {
        'T','F','V','I','v','1',0,0, 0,1, 0,1
    };
    static const uint8_t value[] = {'x'};
    static const FixtureFile files[] = {
        {"model-info.tfv", info, sizeof(info)},
        {"model/en-us/feat.params", value, sizeof(value)},
        {"model/en-us/mdef", value, sizeof(value)},
        {"model/en-us/means", value, sizeof(value)},
        {"model/en-us/noisedict", value, sizeof(value)},
        {"model/en-us/sendump", value, sizeof(value)},
        {"model/en-us/transition_matrices", value, sizeof(value)},
        {"model/en-us/variances", value, sizeof(value)},
        {"model/search/search.dict", value, sizeof(value)},
        {"model/search/search.lm.bin", value, sizeof(value)},
        {"model/search/search.dict.tilefinch", value, sizeof(value)},
        {"model/extra-wide/search.dict", value, sizeof(value)},
        {"model/extra-wide/search.lm.bin", value, sizeof(value)},
        {"model/extra-wide/search.dict.tilefinch", value, sizeof(value)},
        {"LICENSES/ALPHA_CEPHEI_LICENSE.txt", value, sizeof(value)},
        {"LICENSES/CMUDICT_LICENSE.txt", value, sizeof(value)},
        {"LICENSES/CMUDICT_NOTICE.md", value, sizeof(value)}
    };
    static const uint8_t magic[8] = {
        'T','F','V','P','v','1',0,0
    };
    size_t offsets[sizeof(files) / sizeof(files[0])];
    put(package, magic, sizeof(magic));
    put_u16(package, 1);
    put_u16(package, (uint16_t) (sizeof(files) / sizeof(files[0])));
    size_t table_length_at = package->length;
    put_u32(package, 0);
    size_t table_start = package->length;
    for (size_t at = 0; at < sizeof(files) / sizeof(files[0]); at++) {
        size_t path_length = strlen(files[at].path);
        uint8_t digest[32];
        if (path_length > UINT8_MAX
            || !digest_bytes(files[at].bytes, files[at].length, digest))
            return false;
        put_u8(package, (uint8_t) path_length);
        put(package, files[at].path, path_length);
        put_u64(package, files[at].length);
        put(package, digest, sizeof(digest));
        offsets[at] = package->length;
        put_u64(package, 0);
    }
    uint32_t table_length = (uint32_t) (package->length - table_start);
    package->data[table_length_at] = (uint8_t) (table_length >> 24);
    package->data[table_length_at + 1u] = (uint8_t) (table_length >> 16);
    package->data[table_length_at + 2u] = (uint8_t) (table_length >> 8);
    package->data[table_length_at + 3u] = (uint8_t) table_length;
    uint64_t payload_offset = package->length;
    for (size_t at = 0; at < sizeof(files) / sizeof(files[0]); at++) {
        uint64_t value_offset = payload_offset;
        for (size_t byte = 0; byte < 8; byte++) {
            package->data[offsets[at] + 7u - byte] =
                (uint8_t) value_offset;
            value_offset >>= 8;
        }
        put(package, files[at].bytes, files[at].length);
        payload_offset += files[at].length;
    }
    if (corrupt_last_payload) package->data[package->length - 1u] ^= 1u;
    return true;
}

static bool write_package(const char *path, const Buffer *package)
{
    FILE *file = fopen(path, "wb");
    return file != NULL
        && fwrite(package->data, 1, package->length, file) == package->length
        && fclose(file) == 0;
}

static TilefinchUpdateInstallPhase run_install(
    Budget *budget, const char *package_path, const char *root,
    const Buffer *package)
{
    uint8_t package_digest[32], manifest_digest[32] = {0};
    if (!digest_bytes(
            package->data, package->length, package_digest))
        return TILEFINCH_UPDATE_INSTALL_ERROR;
    TilefinchUpdateManifest manifest = {
        .release_sequence = 7,
        .package_format = TILEFINCH_UPDATE_PACKAGE_VOICE,
        .package_size = package->length
    };
    memcpy(manifest.package_sha256, package_digest, 32);
    static const uint8_t envelope[] = {'m'};
    TilefinchVoiceComponentInstall *job =
        tilefinch_voice_component_install_create(
            budget, &(TilefinchVoiceComponentInstallOptions) {
                .package_path = package_path,
                .envelope = envelope,
                .envelope_length = sizeof(envelope),
                .manifest = &manifest,
                .manifest_digest = manifest_digest,
                .install_root = root
            });
    if (job == NULL) return TILEFINCH_UPDATE_INSTALL_ERROR;
    TilefinchUpdateInstallSnapshot snapshot = {0};
    for (size_t pump = 0; pump < 4096; pump++) {
        if (!tilefinch_voice_component_install_pump(job, 7u)
            || !tilefinch_voice_component_install_snapshot(job, &snapshot))
            break;
        if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    TilefinchUpdateInstallPhase phase = snapshot.phase;
    tilefinch_voice_component_install_destroy(job);
    return phase;
}

int main(void)
{
    CHECK(verify_release_artifacts() == 0);
    char root[] = "/tmp/tilefinch-voice-component.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char package_path[512];
    snprintf(package_path, sizeof(package_path), "%s/model.part", root);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    Budget budget;
    budget_init(&budget, 1024u * 1024u);

    Buffer package = {0};
    CHECK(build_package(&package, false)
          && write_package(package_path, &package)
          && run_install(&budget, package_path, root, &package)
                 == TILEFINCH_UPDATE_INSTALL_COMPLETE);
    char model[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(tilefinch_voice_component_resolve(
              &paths, model, sizeof(model))
              == TILEFINCH_VOICE_COMPONENT_SHARED
          && strstr(model, "/components/voice-en-us/active/model") != NULL);

    /* Keep two verified generations, then model the exact durable state left
       by power loss after removal committed but before the older generation
       was deleted. The marker must suppress the otherwise-valid previous
       model instead of silently resurrecting it. */
    CHECK(write_package(package_path, &package)
          && run_install(&budget, package_path, root, &package)
                 == TILEFINCH_UPDATE_INSTALL_COMPLETE);
    char active[512], previous[512], interrupted[512], marker[512];
    snprintf(active, sizeof(active),
             "%s/components/voice-en-us/active", root);
    snprintf(previous, sizeof(previous),
             "%s/components/voice-en-us/previous", root);
    snprintf(interrupted, sizeof(interrupted),
             "%s/components/voice-en-us/interrupted-active", root);
    snprintf(marker, sizeof(marker),
             "%s/components/voice-en-us/%s", root,
             TILEFINCH_VOICE_COMPONENT_REMOVED_MARKER);
    char previous_ready[512];
    snprintf(previous_ready, sizeof(previous_ready), "%s/READY", previous);
    CHECK(access(previous_ready, F_OK) == 0);
    FILE *removed = fopen(marker, "wb");
    CHECK(removed != NULL && fputs("TFVRv1\n", removed) >= 0
          && fclose(removed) == 0);
    /* This is also the reinstall interruption point after a new active
       generation was promoted and synchronized but before UNINSTALLED was
       deleted. The durable marker must still win over both signed trees. */
    CHECK(tilefinch_voice_component_resolve(
              &paths, model, sizeof(model))
              == TILEFINCH_VOICE_COMPONENT_NONE
          && rename(active, interrupted) == 0
          && tilefinch_voice_component_resolve(
                 &paths, model, sizeof(model))
                 == TILEFINCH_VOICE_COMPONENT_NONE
          && rename(interrupted, active) == 0
          && unlink(marker) == 0);

    /* Whole-package verification succeeds, then the per-file digest catches
       the changed payload. The previously active model remains selectable. */
    package = (Buffer) {0};
    CHECK(build_package(&package, true)
          && write_package(package_path, &package)
          && run_install(&budget, package_path, root, &package)
                 == TILEFINCH_UPDATE_INSTALL_ERROR
          && tilefinch_voice_component_resolve(
                 &paths, model, sizeof(model))
                 == TILEFINCH_VOICE_COMPONENT_SHARED);

    CHECK(tilefinch_voice_component_remove(&paths)
          && tilefinch_voice_component_resolve(
                 &paths, model, sizeof(model))
                 == TILEFINCH_VOICE_COMPONENT_NONE);
    CHECK(access(marker, F_OK) == 0);

    /* Explicit reinstallation is the only operation that clears removal. */
    package = (Buffer) {0};
    CHECK(build_package(&package, false)
          && write_package(package_path, &package)
          && run_install(&budget, package_path, root, &package)
                 == TILEFINCH_UPDATE_INSTALL_COMPLETE
          && access(marker, F_OK) != 0
          && tilefinch_voice_component_resolve(
                 &paths, model, sizeof(model))
                 == TILEFINCH_VOICE_COMPONENT_SHARED
          && tilefinch_voice_component_remove(&paths));
    CHECK(budget.current == 0);
    remove(package_path);
    CHECK(unlink(marker) == 0);
    char component[512], components[512];
    snprintf(component, sizeof(component),
             "%s/components/voice-en-us", root);
    snprintf(components, sizeof(components), "%s/components", root);
    CHECK(rmdir(component) == 0 && rmdir(components) == 0);
    CHECK(rmdir(root) == 0);
    puts("voice-component: removal tombstone prevents generation restore");
    return 0;
}
