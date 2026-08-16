#include "tilefinch/voice_component.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__PSP__)
#include <pspiofilemgr.h>
#endif

#include "tilefinch/sha256.h"

#define VOICE_COMPONENT_IO_CHUNK (16u * 1024u)
#define VOICE_COMPONENT_TREE_LIMIT 128u

struct TilefinchVoiceComponentInstall {
    Budget *budget;
    char package_path[TILEFINCH_INSTALL_PATH_LIMIT];
    char root[TILEFINCH_INSTALL_PATH_LIMIT];
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char removed_marker[TILEFINCH_INSTALL_PATH_LIMIT];
    FILE *package_file;
    FILE *output_file;
    uint8_t *buffer;
    uint8_t *table;
    uint8_t *envelope;
    size_t envelope_length;
    size_t table_bytes;
    TilefinchUpdateManifest manifest;
    uint8_t manifest_digest[32];
    TilefinchUpdatePackage package;
    TilefinchSha256 package_sha;
    TilefinchSha256 file_sha;
    uint64_t bytes_processed;
    uint64_t current_file_bytes;
    size_t file_index;
    TilefinchUpdateInstallPhase phase;
    TilefinchUpdateStatus status;
    bool cancel_requested;
    char message[96];
};

static bool join_path(
    const char *directory, const char *relative,
    char *output, size_t output_size)
{
    if (directory == NULL || relative == NULL || output == NULL
        || output_size == 0) return false;
    int written = snprintf(
        output, output_size, "%s/%s", directory, relative);
    return written > 0 && (size_t) written < output_size;
}

static bool model_root_complete(const char *root)
{
    static const char *const required[] = {
        "en-us/feat.params", "en-us/mdef", "en-us/means",
        "en-us/noisedict", "en-us/sendump", "en-us/transition_matrices",
        "en-us/variances", "search/search.dict", "search/search.lm.bin",
        "search/search.dict.tilefinch", "extra-wide/search.dict",
        "extra-wide/search.lm.bin", "extra-wide/search.dict.tilefinch"
    };
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    for (size_t index = 0;
         index < sizeof(required) / sizeof(required[0]); index++) {
        if (!join_path(root, required[index], path, sizeof(path)))
            return false;
        FILE *file = fopen(path, "rb");
        if (file == NULL) return false;
        bool present = fgetc(file) != EOF;
        fclose(file);
        if (!present) return false;
    }
    return true;
}

static bool path_exists(const char *path)
{
    struct stat information;
    return path != NULL && stat(path, &information) == 0;
}

static bool model_info_valid(const char *directory)
{
    static const uint8_t expected[12] = {
        'T','F','V','I','v','1',0,0, 0,1, 0,TILEFINCH_VOICE_COMPONENT_ABI
    };
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!join_path(directory, "model-info.tfv", path, sizeof(path)))
        return false;
    uint8_t found[sizeof(expected)];
    FILE *file = fopen(path, "rb");
    bool ok = file != NULL
        && fread(found, 1, sizeof(found), file) == sizeof(found)
        && fgetc(file) == EOF
        && memcmp(found, expected, sizeof(expected)) == 0;
    if (file != NULL) fclose(file);
    return ok;
}

static bool component_payload_complete(const char *directory)
{
    char model[TILEFINCH_INSTALL_PATH_LIMIT];
    static const char *const notices[] = {
        "LICENSES/ALPHA_CEPHEI_LICENSE.txt",
        "LICENSES/CMUDICT_LICENSE.txt",
        "LICENSES/CMUDICT_NOTICE.md"
    };
    bool notices_present = true;
    for (size_t at = 0;
         notices_present && at < sizeof(notices) / sizeof(notices[0]); at++) {
        char notice[TILEFINCH_INSTALL_PATH_LIMIT];
        notices_present = join_path(
            directory, notices[at], notice, sizeof(notice))
            && path_exists(notice);
    }
    return join_path(directory, "model", model, sizeof(model))
        && model_info_valid(directory)
        && notices_present && model_root_complete(model);
}

static bool component_directory_complete(const char *directory)
{
    char ready[TILEFINCH_INSTALL_PATH_LIMIT];
    return join_path(directory, "READY", ready, sizeof(ready))
        && path_exists(ready) && component_payload_complete(directory);
}

static bool component_removed(const TilefinchInstallPaths *paths)
{
    char marker[TILEFINCH_INSTALL_PATH_LIMIT];
    return paths != NULL && paths->slotted
        && join_path(paths->install_root,
                     "components/voice-en-us/"
                     TILEFINCH_VOICE_COMPONENT_REMOVED_MARKER,
                     marker, sizeof(marker))
        && path_exists(marker);
}

bool tilefinch_voice_component_path(
    const TilefinchInstallPaths *paths, char *output, size_t output_size)
{
    if (paths == NULL || !paths->slotted) return false;
    return join_path(
        paths->install_root,
        "components/voice-en-us/active/model", output, output_size);
}

TilefinchVoiceComponentSource tilefinch_voice_component_resolve(
    const TilefinchInstallPaths *paths, char *output, size_t output_size)
{
    if (paths == NULL || output == NULL || output_size == 0)
        return TILEFINCH_VOICE_COMPONENT_NONE;
    if (component_removed(paths)) {
        output[0] = '\0';
        return TILEFINCH_VOICE_COMPONENT_NONE;
    }
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    if (tilefinch_voice_component_path(
            paths, candidate, sizeof(candidate))) {
        char shared_dir[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(shared_dir, sizeof(shared_dir), "%s", candidate);
        char *model = strrchr(shared_dir, '/');
        if (model != NULL) *model = '\0';
        if (component_directory_complete(shared_dir)) {
            int written = snprintf(output, output_size, "%s", candidate);
            if (written <= 0 || (size_t) written >= output_size) {
                output[0] = '\0';
                return TILEFINCH_VOICE_COMPONENT_NONE;
            }
            return TILEFINCH_VOICE_COMPONENT_SHARED;
        }
    }
    if (paths->slotted
        && join_path(paths->install_root,
                     "components/voice-en-us/previous", candidate,
                     sizeof(candidate))) {
        char model[TILEFINCH_INSTALL_PATH_LIMIT];
        if (component_directory_complete(candidate)
            && join_path(candidate, "model", model, sizeof(model))) {
            int written = snprintf(output, output_size, "%s", model);
            if (written <= 0 || (size_t) written >= output_size) {
                output[0] = '\0';
                return TILEFINCH_VOICE_COMPONENT_NONE;
            }
            return TILEFINCH_VOICE_COMPONENT_SHARED;
        }
    }
    if (tilefinch_install_program_path(
            paths, "voice-model", candidate, sizeof(candidate))
        && model_root_complete(candidate)) {
        int written = snprintf(output, output_size, "%s", candidate);
        if (written <= 0 || (size_t) written >= output_size) {
            output[0] = '\0';
            return TILEFINCH_VOICE_COMPONENT_NONE;
        }
        return TILEFINCH_VOICE_COMPONENT_LEGACY;
    }
    output[0] = '\0';
    return TILEFINCH_VOICE_COMPONENT_NONE;
}

static bool component_identity_from_directory(
    Budget *budget, const char *directory, const TilefinchUpdateRoot *root,
    uint64_t *sequence, uint8_t package_sha256[32])
{
    if (!component_directory_complete(directory)) return false;
    char metadata[TILEFINCH_INSTALL_PATH_LIMIT];
    char ready_path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!join_path(directory, "component.tfvm", metadata, sizeof(metadata))
        || !join_path(directory, "READY", ready_path, sizeof(ready_path)))
        return false;
    FILE *file = fopen(metadata, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long signed_length = ftell(file);
    if (signed_length <= 0
        || signed_length > (long) TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t) signed_length;
    uint8_t *envelope = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, length);
    bool read_ok = envelope != NULL
        && fread(envelope, 1, length, file) == length
        && fgetc(file) == EOF;
    fclose(file);
    TilefinchUpdateVerifiedEnvelope verified = {0};
    TilefinchUpdateStatus status = read_ok
        ? tilefinch_update_verify_voice_envelope(
              envelope, length,
              &(TilefinchUpdateVerifyOptions) {
                  .embedded_root = root,
                  .crypto = tilefinch_update_default_crypto(),
                  /* Installed metadata is an anti-rollback record, not a
                     fresh-release offer. Its signature remains meaningful
                     after the release's download expiry. */
                  .now_unix = 0,
                  .clock_valid = true,
                  .launcher_protocol = TILEFINCH_VOICE_COMPONENT_ABI
              },
              &verified)
        : TILEFINCH_UPDATE_IO;
    budget_free(budget, envelope);
    if (status != TILEFINCH_UPDATE_OK) return false;
    uint8_t ready_digest[32];
    file = fopen(ready_path, "rb");
    bool ready_ok = file != NULL
        && fread(ready_digest, 1, sizeof(ready_digest), file)
               == sizeof(ready_digest)
        && fgetc(file) == EOF
        && memcmp(ready_digest, verified.manifest_digest,
                  sizeof(ready_digest)) == 0;
    if (file != NULL) fclose(file);
    if (!ready_ok) return false;
    *sequence = verified.manifest.release_sequence;
    memcpy(package_sha256, verified.manifest.package_sha256, 32);
    return true;
}

bool tilefinch_voice_component_installed_identity(
    Budget *budget, const TilefinchInstallPaths *paths,
    const TilefinchUpdateRoot *root, uint64_t *sequence,
    uint8_t package_sha256[32])
{
    if (budget == NULL || paths == NULL || !paths->slotted || root == NULL
        || sequence == NULL || package_sha256 == NULL) return false;
    if (component_removed(paths)) return false;
    char directory[TILEFINCH_INSTALL_PATH_LIMIT];
    if (join_path(paths->install_root, "components/voice-en-us/active",
                  directory, sizeof(directory))
        && component_identity_from_directory(
               budget, directory, root, sequence, package_sha256))
        return true;
    return join_path(paths->install_root,
                     "components/voice-en-us/previous", directory,
                     sizeof(directory))
        && component_identity_from_directory(
               budget, directory, root, sequence, package_sha256);
}

static bool make_directories(char *path)
{
    for (char *at = path; *at != '\0'; at++) {
        if (*at != '/') continue;
        if (at == path || (at > path && at[-1] == ':')) continue;
        char saved = *at;
        *at = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            *at = saved;
            return false;
        }
        *at = saved;
    }
    return true;
}

static bool remove_tree(const char *path, size_t depth, size_t *entries)
{
    if (depth > 8u || entries == NULL
        || *entries >= VOICE_COMPONENT_TREE_LIMIT) return false;
    DIR *directory = opendir(path);
    if (directory == NULL) return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        if (++(*entries) > VOICE_COMPONENT_TREE_LIMIT) {
            ok = false;
            break;
        }
        char child[TILEFINCH_INSTALL_PATH_LIMIT];
        if (!join_path(path, entry->d_name, child, sizeof(child))) {
            ok = false;
            break;
        }
        struct stat info;
        if (lstat(child, &info) != 0) {
            ok = false;
        } else if (S_ISDIR(info.st_mode)) {
            ok = remove_tree(child, depth + 1u, entries);
        } else {
            ok = unlink(child) == 0;
        }
    }
    if (closedir(directory) != 0) ok = false;
    return ok && rmdir(path) == 0;
}

static void install_fail(
    TilefinchVoiceComponentInstall *job, TilefinchUpdateStatus status,
    const char *message)
{
    if (job->output_file != NULL) {
        fclose(job->output_file);
        job->output_file = NULL;
    }
    job->status = status;
    job->phase = TILEFINCH_UPDATE_INSTALL_ERROR;
    snprintf(job->message, sizeof(job->message), "%s", message);
}

TilefinchVoiceComponentInstall *tilefinch_voice_component_install_create(
    Budget *budget, const TilefinchVoiceComponentInstallOptions *options)
{
    if (budget == NULL || options == NULL || options->package_path == NULL
        || options->envelope == NULL || options->envelope_length == 0
        || options->manifest == NULL || options->manifest_digest == NULL
        || options->install_root == NULL
        || options->manifest->package_format
               != TILEFINCH_UPDATE_PACKAGE_VOICE) return NULL;
    TilefinchVoiceComponentInstall *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->buffer = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, VOICE_COMPONENT_IO_CHUNK);
    job->envelope = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, options->envelope_length);
    if (job->buffer == NULL || job->envelope == NULL) {
        tilefinch_voice_component_install_destroy(job);
        return NULL;
    }
    memcpy(job->envelope, options->envelope, options->envelope_length);
    job->envelope_length = options->envelope_length;
    job->manifest = *options->manifest;
    memcpy(job->manifest_digest, options->manifest_digest, 32);
    snprintf(job->package_path, sizeof(job->package_path), "%s",
             options->package_path);
    snprintf(job->root, sizeof(job->root), "%s", options->install_root);
    if (!join_path(job->root, "components/voice-en-us", job->component,
                   sizeof(job->component))
        || !join_path(job->component, "candidate.tmp", job->candidate,
                      sizeof(job->candidate))
        || !join_path(job->component, "active", job->active,
                      sizeof(job->active))
        || !join_path(job->component, "previous", job->previous,
                      sizeof(job->previous))
        || !join_path(job->component,
                      TILEFINCH_VOICE_COMPONENT_REMOVED_MARKER,
                      job->removed_marker, sizeof(job->removed_marker))) {
        tilefinch_voice_component_install_destroy(job);
        return NULL;
    }
    job->package_file = fopen(job->package_path, "rb");
    if (job->package_file == NULL) {
        tilefinch_voice_component_install_destroy(job);
        return NULL;
    }
    tilefinch_sha256_init(&job->package_sha);
    job->phase = TILEFINCH_UPDATE_INSTALL_VERIFYING;
    snprintf(job->message, sizeof(job->message), "VERIFYING MODEL...");
    return job;
}

void tilefinch_voice_component_install_destroy(
    TilefinchVoiceComponentInstall *job)
{
    if (job == NULL) return;
    if (job->output_file != NULL) fclose(job->output_file);
    if (job->package_file != NULL) fclose(job->package_file);
    Budget *budget = job->budget;
    budget_free(budget, job->table);
    budget_free(budget, job->envelope);
    budget_free(budget, job->buffer);
    memset(job, 0, sizeof(*job));
    budget_free(budget, job);
}

bool tilefinch_voice_component_install_cancel(
    TilefinchVoiceComponentInstall *job)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_PROMOTING)
        return false;
    job->cancel_requested = true;
    return true;
}

static bool read_table(TilefinchVoiceComponentInstall *job)
{
    uint8_t header[16];
    if (fseek(job->package_file, 0, SEEK_SET) != 0
        || fread(header, 1, sizeof(header), job->package_file)
               != sizeof(header)) return false;
    uint32_t length = (uint32_t) header[12] << 24
        | (uint32_t) header[13] << 16
        | (uint32_t) header[14] << 8 | header[15];
    if (length > TILEFINCH_UPDATE_MAX_PACKAGE_TABLE_BYTES - 16u)
        return false;
    job->table_bytes = 16u + length;
    job->table = budget_malloc_category(
        job->budget, BUDGET_CATEGORY_SESSION, job->table_bytes);
    if (job->table == NULL) return false;
    memcpy(job->table, header, 16u);
    if (fread(job->table + 16u, 1, length, job->package_file) != length)
        return false;
    return tilefinch_update_parse_voice_package_table(
               job->table, job->table_bytes, job->manifest.package_size,
               &job->package) == TILEFINCH_UPDATE_OK;
}

static bool prepare_install(TilefinchVoiceComponentInstall *job)
{
    char mutable_component[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(mutable_component, sizeof(mutable_component), "%s",
             job->component);
    if (!make_directories(mutable_component)
        || (mkdir(job->component, 0777) != 0 && errno != EEXIST))
        return false;
    size_t entries = 0;
    if (!remove_tree(job->candidate, 0, &entries)) return false;
    return mkdir(job->candidate, 0777) == 0;
}

static bool open_entry(TilefinchVoiceComponentInstall *job)
{
    const TilefinchUpdatePackageEntry *entry =
        &job->package.entries[job->file_index];
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!join_path(job->candidate, entry->path, path, sizeof(path)))
        return false;
    char parent[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL) return false;
    *slash = '\0';
    if (!make_directories(parent)
        || (mkdir(parent, 0777) != 0 && errno != EEXIST)) return false;
    if (fseek(job->package_file, (long) entry->payload_offset, SEEK_SET) != 0)
        return false;
    job->output_file = fopen(path, "wb");
    if (job->output_file == NULL) return false;
    tilefinch_sha256_init(&job->file_sha);
    job->current_file_bytes = 0;
    return true;
}

static bool write_file_bytes(
    const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL && fwrite(bytes, 1, length, file) == length
        && fflush(file) == 0;
#if !defined(__PSP__)
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (file != NULL && fclose(file) != 0) ok = false;
#if defined(__PSP__)
    if (ok) ok = sceIoSync("ms0:", 0) >= 0;
#endif
    return ok;
}

static bool finalize_install(TilefinchVoiceComponentInstall *job)
{
    char metadata[TILEFINCH_INSTALL_PATH_LIMIT];
    char ready[TILEFINCH_INSTALL_PATH_LIMIT];
    return model_info_valid(job->candidate)
        && component_payload_complete(job->candidate)
        && join_path(job->candidate, "component.tfvm", metadata,
                     sizeof(metadata))
        && write_file_bytes(
               metadata, job->envelope, job->envelope_length)
        && join_path(job->candidate, "READY", ready, sizeof(ready))
        && write_file_bytes(ready, job->manifest_digest, 32u);
}

static bool promote_install(TilefinchVoiceComponentInstall *job)
{
    size_t entries = 0;
    if (!remove_tree(job->previous, 0, &entries)) return false;
    bool had_active = rename(job->active, job->previous) == 0;
    if (!had_active && errno != ENOENT) return false;
    if (rename(job->candidate, job->active) != 0) {
        if (had_active) (void) rename(job->previous, job->active);
        return false;
    }
#if defined(__PSP__)
    /* UNINSTALLED is the durable authority that suppresses every otherwise
       valid generation.  Commit the newly promoted directory before removing
       that authority: if power fails here, the marker remains and the model
       stays disabled; if it fails after this sync, active is recoverable even
       when the later marker deletion reaches FAT first. */
    if (sceIoSync("ms0:", 0) < 0) return false;
#endif
    if (unlink(job->removed_marker) != 0 && errno != ENOENT) return false;
#if defined(__PSP__)
    /* Publish the now-enabled state as a second, separately durable step. */
    if (sceIoSync("ms0:", 0) < 0) return false;
#endif
    return true;
}

bool tilefinch_voice_component_install_pump(
    TilefinchVoiceComponentInstall *job, size_t maximum_bytes)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
        return false;
    if (job->cancel_requested) {
        job->phase = TILEFINCH_UPDATE_INSTALL_CANCELLED;
        job->status = TILEFINCH_UPDATE_CANCELLED;
        snprintf(job->message, sizeof(job->message), "CANCELLED");
        return true;
    }
    size_t quota = maximum_bytes == 0
        || maximum_bytes > VOICE_COMPONENT_IO_CHUNK
        ? VOICE_COMPONENT_IO_CHUNK : maximum_bytes;
    if (job->phase == TILEFINCH_UPDATE_INSTALL_VERIFYING) {
        size_t count = fread(job->buffer, 1, quota, job->package_file);
        if (count != 0) {
            if (!tilefinch_sha256_update(
                    &job->package_sha, job->buffer, count)) {
                install_fail(job, TILEFINCH_UPDATE_IO, "MODEL HASH FAILED");
            } else {
                job->bytes_processed += count;
            }
            return true;
        }
        uint8_t digest[32];
        if (ferror(job->package_file)
            || job->bytes_processed != job->manifest.package_size
            || !tilefinch_sha256_final(&job->package_sha, digest)
            || memcmp(digest, job->manifest.package_sha256, 32) != 0) {
            install_fail(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                         "MODEL PACKAGE DID NOT VERIFY");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_READING_TABLE;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_READING_TABLE) {
        if (!read_table(job)) {
            install_fail(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                         "MODEL PACKAGE TABLE IS INVALID");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_PREPARING;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PREPARING) {
        if (!prepare_install(job)) {
            install_fail(job, TILEFINCH_UPDATE_IO,
                         "MODEL STAGING DIRECTORY FAILED");
            return true;
        }
        job->bytes_processed = 0;
        job->phase = TILEFINCH_UPDATE_INSTALL_EXTRACTING;
        snprintf(job->message, sizeof(job->message), "INSTALLING MODEL...");
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_EXTRACTING) {
        if (job->file_index >= job->package.file_count) {
            job->phase = TILEFINCH_UPDATE_INSTALL_FINALIZING;
        } else {
            const TilefinchUpdatePackageEntry *entry =
                &job->package.entries[job->file_index];
            if (job->output_file == NULL && !open_entry(job)) {
                install_fail(job, TILEFINCH_UPDATE_IO,
                             "MODEL FILE COULD NOT BE CREATED");
                return true;
            }
            uint64_t remaining = entry->size - job->current_file_bytes;
            size_t chunk = remaining < quota ? (size_t) remaining : quota;
            if (chunk != 0) {
                size_t count = fread(job->buffer, 1, chunk, job->package_file);
                if (count != chunk
                    || fwrite(job->buffer, 1, count, job->output_file) != count
                    || !tilefinch_sha256_update(
                           &job->file_sha, job->buffer, count)) {
                    install_fail(job, TILEFINCH_UPDATE_IO,
                                 "MODEL EXTRACTION FAILED");
                } else {
                    job->current_file_bytes += count;
                    job->bytes_processed += count;
                }
                return true;
            }
            uint8_t digest[32];
            bool digest_ok = tilefinch_sha256_final(&job->file_sha, digest)
                && memcmp(digest, entry->sha256, 32) == 0;
            bool flush_ok = fflush(job->output_file) == 0;
            bool close_ok = fclose(job->output_file) == 0;
            job->output_file = NULL;
            if (!digest_ok || !flush_ok || !close_ok) {
                install_fail(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                             "A MODEL FILE DID NOT VERIFY");
                return true;
            }
            job->file_index++;
            return true;
        }
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_FINALIZING) {
        if (!finalize_install(job)) {
            install_fail(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                         "MODEL SELF-CHECK FAILED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_PROMOTING;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PROMOTING) {
        if (!promote_install(job)) {
            install_fail(job, TILEFINCH_UPDATE_IO,
                         "MODEL COULD NOT BE ACTIVATED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_COMPLETE;
        job->status = TILEFINCH_UPDATE_OK;
        if (job->package_file != NULL) {
            fclose(job->package_file);
            job->package_file = NULL;
        }
        (void) remove(job->package_path);
        snprintf(job->message, sizeof(job->message), "VOICE MODEL READY");
    }
    return true;
}

bool tilefinch_voice_component_install_snapshot(
    const TilefinchVoiceComponentInstall *job,
    TilefinchUpdateInstallSnapshot *snapshot)
{
    if (job == NULL || snapshot == NULL) return false;
    *snapshot = (TilefinchUpdateInstallSnapshot) {
        .phase = job->phase,
        .status = job->status,
        .bytes_processed = job->bytes_processed,
        .bytes_total = job->manifest.package_size,
        .files_completed = job->file_index,
        .files_total = job->package.file_count
    };
    snprintf(snapshot->message, sizeof(snapshot->message), "%s",
             job->message);
    return true;
}

bool tilefinch_voice_component_remove(const TilefinchInstallPaths *paths)
{
    if (paths == NULL || !paths->slotted) return false;
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    char retired[TILEFINCH_INSTALL_PATH_LIMIT];
    char marker[TILEFINCH_INSTALL_PATH_LIMIT];
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!join_path(paths->install_root, "components/voice-en-us/active",
                   active, sizeof(active))
        || !join_path(paths->install_root,
                      "components/voice-en-us/previous", previous,
                      sizeof(previous))
        || !join_path(paths->install_root,
                      "components/voice-en-us/candidate.tmp", candidate,
                      sizeof(candidate))
        || !join_path(paths->install_root, "components/voice-en-us/removed",
                      retired, sizeof(retired))
        || !join_path(paths->install_root,
                      "components/voice-en-us/"
                      TILEFINCH_VOICE_COMPONENT_REMOVED_MARKER,
                      marker, sizeof(marker))
        || !join_path(paths->install_root, "components/voice-en-us",
                      component, sizeof(component))) return false;
    bool had_component = path_exists(active) || path_exists(previous)
        || path_exists(candidate) || path_exists(marker);
    char mutable_component[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(mutable_component, sizeof(mutable_component), "%s", component);
    static const uint8_t removed_record[] = {'T','F','V','R','v','1','\n'};
    if (!make_directories(mutable_component)
        || (mkdir(component, 0777) != 0 && errno != EEXIST)
        || !write_file_bytes(
               marker, removed_record, sizeof(removed_record))) return false;

    /* The durable marker is the logical commit. Cleanup is deliberately
       best-effort: after power loss or a FAT deletion failure the resolver
       must still refuse both generations, and a later verified install will
       rotate/replace them before clearing the marker. */
    size_t entries = 0;
    (void) remove_tree(retired, 0, &entries);
    (void) rename(active, retired);
    entries = 0;
    (void) remove_tree(retired, 0, &entries);
    entries = 0;
    (void) remove_tree(previous, 0, &entries);
    entries = 0;
    (void) remove_tree(candidate, 0, &entries);
    return had_component;
}
