#include "tilefinch/update.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__PSP__)
#include <pspiofilemgr.h>
#endif

#include "tilefinch/sha256.h"

#define INSTALL_IO_CHUNK (16u * 1024u)
#define INSTALL_REMOVE_ENTRY_LIMIT 256u
#define INSTALL_REMOVE_DEPTH_LIMIT 8u
#define INSTALL_FREE_SPACE_MARGIN (4u * 1024u * 1024u)

struct TilefinchUpdateInstallJob {
    Budget *budget;
    char package_path[768];
    char install_root[768];
    char data_dir[768];
    char target_path[768];
    char temporary_path[768];
    char old_path[768];
    uint8_t *envelope;
    size_t envelope_length;
    TilefinchUpdateManifest manifest;
    uint8_t manifest_digest[32];
    TilefinchUpdateSlot inactive_slot;
    TilefinchUpdateTrust trust;
    bool allow_downgrade;
    TilefinchUpdateState state;
    TilefinchUpdateFaultHook fault;
    void *fault_opaque;
    FILE *package_file;
    FILE *output_file;
    TilefinchSha256 package_sha;
    TilefinchSha256 file_sha;
    uint8_t *buffer;
    uint8_t *table;
    size_t table_bytes;
    TilefinchUpdatePackage package;
    TilefinchUpdateInstallPhase phase;
    TilefinchUpdateStatus status;
    uint64_t bytes_processed;
    uint64_t current_file_bytes;
    size_t file_index;
    bool cancel_requested;
    char message[96];
};

static bool install_path(
    char *output, size_t size, const char *directory, const char *name)
{
    int length = snprintf(output, size, "%s/%s", directory, name);
    return length > 0 && (size_t) length < size;
}

static void install_error(
    TilefinchUpdateInstallJob *job, TilefinchUpdateStatus status,
    const char *message)
{
    if (job->output_file != NULL) {
        fclose(job->output_file);
        job->output_file = NULL;
    }
    job->phase = TILEFINCH_UPDATE_INSTALL_ERROR;
    job->status = status;
    snprintf(job->message, sizeof(job->message), "%s", message);
}

static bool install_fault(
    TilefinchUpdateInstallJob *job, const char *operation)
{
    return job->fault != NULL
        && job->fault(job->fault_opaque, operation);
}

static bool install_sync_file(FILE *file)
{
    if (file == NULL || fflush(file) != 0) return false;
#if defined(__PSP__)
    return true;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool install_sync_device(void)
{
#if defined(__PSP__)
    return sceIoSync("ms0:", 0) >= 0;
#else
    return true;
#endif
}

static bool remove_tree_bounded(
    const char *path, unsigned depth, size_t *entries)
{
    if (depth > INSTALL_REMOVE_DEPTH_LIMIT
        || *entries >= INSTALL_REMOVE_ENTRY_LIMIT) return false;
    DIR *directory = opendir(path);
    if (directory == NULL) return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        if (++*entries > INSTALL_REMOVE_ENTRY_LIMIT) {
            ok = false;
            break;
        }
        char child[768];
        if (!install_path(child, sizeof(child), path, entry->d_name)) {
            ok = false;
            break;
        }
        struct stat info;
        if (lstat(child, &info) != 0) {
            ok = false;
        } else if (S_ISDIR(info.st_mode)) {
            ok = remove_tree_bounded(child, depth + 1u, entries);
        } else if (S_ISREG(info.st_mode)) {
            ok = remove(child) == 0;
        } else {
            /* Refuse symlinks and special files rather than following them. */
            ok = false;
        }
    }
    if (closedir(directory) != 0) ok = false;
    return ok && rmdir(path) == 0;
}

static bool install_make_parents(char *path)
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

static bool install_has_free_space(
    const char *path, uint64_t required)
{
    uint64_t available = 0;
    return tilefinch_update_query_free_space(path, &available)
        && available >= required;
}

TilefinchUpdateInstallJob *tilefinch_update_install_create(
    Budget *budget, const TilefinchUpdateInstallOptions *options)
{
    if (budget == NULL || options == NULL || options->package_path == NULL
        || options->envelope == NULL || options->envelope_length == 0
        || options->envelope_length > TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES
        || options->manifest == NULL || options->manifest_digest == NULL
        || options->install_root == NULL || options->data_dir == NULL
        || (options->inactive_slot != TILEFINCH_UPDATE_SLOT_A
            && options->inactive_slot != TILEFINCH_UPDATE_SLOT_B)
        || options->inactive_slot == options->current_state.active_slot
        || options->trust > TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
        || (options->allow_downgrade
            && (options->trust != TILEFINCH_UPDATE_TRUST_SIGNED
                || options->manifest->release_sequence
                       >= options->current_state.installed_sequence))
        || strlen(options->package_path) >= 768
        || strlen(options->install_root) >= 768
        || strlen(options->data_dir) >= 768) return NULL;
    TilefinchUpdateInstallJob *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->buffer = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, INSTALL_IO_CHUNK);
    job->envelope = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, options->envelope_length);
    if (job->buffer == NULL || job->envelope == NULL) {
        tilefinch_update_install_destroy(job);
        return NULL;
    }
    memcpy(job->envelope, options->envelope, options->envelope_length);
    job->envelope_length = options->envelope_length;
    job->manifest = *options->manifest;
    memcpy(job->manifest_digest, options->manifest_digest, 32);
    job->inactive_slot = options->inactive_slot;
    job->trust = options->trust;
    job->allow_downgrade = options->allow_downgrade;
    job->state = options->current_state;
    job->fault = options->fault;
    job->fault_opaque = options->fault_opaque;
    snprintf(job->package_path, sizeof(job->package_path), "%s",
             options->package_path);
    snprintf(job->install_root, sizeof(job->install_root), "%s",
             options->install_root);
    snprintf(job->data_dir, sizeof(job->data_dir), "%s", options->data_dir);
    const char *slot = options->inactive_slot == TILEFINCH_UPDATE_SLOT_A
        ? "slot-a" : "slot-b";
    if (!install_path(
            job->target_path, sizeof(job->target_path),
            job->install_root, slot)) {
        tilefinch_update_install_destroy(job);
        return NULL;
    }
    int length = snprintf(
        job->temporary_path, sizeof(job->temporary_path),
        "%s.tmp", job->target_path);
    int old_length = snprintf(
        job->old_path, sizeof(job->old_path), "%s.old", job->target_path);
    if (length <= 0 || (size_t) length >= sizeof(job->temporary_path)
        || old_length <= 0
        || (size_t) old_length >= sizeof(job->old_path)) {
        tilefinch_update_install_destroy(job);
        return NULL;
    }
    job->package_file = fopen(job->package_path, "rb");
    if (job->package_file == NULL) {
        tilefinch_update_install_destroy(job);
        return NULL;
    }
    tilefinch_sha256_init(&job->package_sha);
    job->phase = TILEFINCH_UPDATE_INSTALL_VERIFYING;
    snprintf(job->message, sizeof(job->message), "VERIFYING DOWNLOAD...");
    return job;
}

void tilefinch_update_install_destroy(TilefinchUpdateInstallJob *job)
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

bool tilefinch_update_install_cancel(TilefinchUpdateInstallJob *job)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_PROMOTING)
        return false;
    job->cancel_requested = true;
    snprintf(job->message, sizeof(job->message), "STOPPING...");
    return true;
}

static bool install_read_table(TilefinchUpdateInstallJob *job)
{
    uint8_t header[16];
    if (fseek(job->package_file, 0, SEEK_SET) != 0
        || fread(header, 1, sizeof(header), job->package_file)
               != sizeof(header)) return false;
    uint32_t table_length = (uint32_t) header[12] << 24
        | (uint32_t) header[13] << 16
        | (uint32_t) header[14] << 8 | header[15];
    if (table_length
        > TILEFINCH_UPDATE_MAX_PACKAGE_TABLE_BYTES - sizeof(header))
        return false;
    job->table_bytes = sizeof(header) + table_length;
    job->table = budget_malloc_category(
        job->budget, BUDGET_CATEGORY_SESSION, job->table_bytes);
    if (job->table == NULL) return false;
    memcpy(job->table, header, sizeof(header));
    if (fread(
            job->table + sizeof(header), 1, table_length,
            job->package_file) != table_length) return false;
    return tilefinch_update_parse_package_table(
               job->table, job->table_bytes, job->manifest.package_size,
               &job->package) == TILEFINCH_UPDATE_OK;
}

static bool install_prepare(TilefinchUpdateInstallJob *job)
{
    uint64_t required =
        job->manifest.package_size
            > UINT64_MAX - INSTALL_FREE_SPACE_MARGIN
        ? UINT64_MAX
        : job->manifest.package_size + INSTALL_FREE_SPACE_MARGIN;
    if (!install_has_free_space(job->install_root, required)) {
        job->status = TILEFINCH_UPDATE_NO_SPACE;
        return false;
    }
    size_t entries = 0;
    if (install_fault(job, "before-remove-temporary")) return false;
    if (!remove_tree_bounded(job->temporary_path, 0, &entries)
        && errno != ENOENT) return false;
    entries = 0;
    if (install_fault(job, "before-remove-old")) return false;
    if (!remove_tree_bounded(job->old_path, 0, &entries)
        && errno != ENOENT) return false;
    if (install_fault(job, "before-create-temporary")) return false;
    return mkdir(job->temporary_path, 0777) == 0;
}

static bool install_open_entry(TilefinchUpdateInstallJob *job)
{
    const TilefinchUpdatePackageEntry *entry =
        &job->package.entries[job->file_index];
    char output_path[768];
    if (!install_path(
            output_path, sizeof(output_path),
            job->temporary_path, entry->path)
        || !install_make_parents(output_path)
        || fseek(
               job->package_file, (long) entry->payload_offset,
               SEEK_SET) != 0
        || install_fault(job, "before-open-output")) return false;
    job->output_file = fopen(output_path, "wb");
    if (job->output_file == NULL) return false;
    tilefinch_sha256_init(&job->file_sha);
    job->current_file_bytes = 0;
    return true;
}

static bool install_write_metadata(TilefinchUpdateInstallJob *job)
{
    char path[768];
    if (!install_path(
            path, sizeof(path), job->temporary_path, "slot.tfum"))
        return false;
    if (install_fault(job, "before-write-slot-envelope")) return false;
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL
        && fwrite(job->envelope, 1, job->envelope_length, file)
               == job->envelope_length;
    if (ok && install_fault(job, "after-write-slot-envelope")) ok = false;
    if (ok && install_fault(job, "before-sync-slot-envelope")) ok = false;
    ok = ok
        && install_sync_file(file);
    if (ok && install_fault(job, "after-sync-slot-envelope")) ok = false;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (!ok) return false;
    if (!install_path(
            path, sizeof(path), job->temporary_path, "slot.tfut"))
        return false;
    if (install_fault(job, "before-write-slot-table")) return false;
    file = fopen(path, "wb");
    ok = file != NULL
        && fwrite(job->table, 1, job->table_bytes, file) == job->table_bytes;
    if (ok && install_fault(job, "after-write-slot-table")) ok = false;
    if (ok && install_fault(job, "before-sync-slot-table")) ok = false;
    ok = ok
        && install_sync_file(file);
    if (ok && install_fault(job, "after-sync-slot-table")) ok = false;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (!ok) return false;
    if (job->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED) {
        if (!install_path(
                path, sizeof(path), job->temporary_path, "DEVELOPER")
            || install_fault(job, "before-write-developer-marker")) {
            return false;
        }
        file = fopen(path, "wb");
        ok = file != NULL
            && fwrite(job->manifest_digest, 1, 32, file) == 32;
        if (ok && install_fault(job, "after-write-developer-marker"))
            ok = false;
        if (ok && install_fault(job, "before-sync-developer-marker"))
            ok = false;
        ok = ok && install_sync_file(file);
        if (ok && install_fault(job, "after-sync-developer-marker"))
            ok = false;
        if (file != NULL && fclose(file) != 0) ok = false;
        if (!ok) return false;
    }
    if (!install_path(path, sizeof(path), job->temporary_path, "READY"))
        return false;
    if (install_fault(job, "before-write-ready")) return false;
    file = fopen(path, "wb");
    ok = file != NULL
        && fwrite(job->manifest_digest, 1, 32, file) == 32;
    if (ok && install_fault(job, "after-write-ready")) ok = false;
    if (ok && install_fault(job, "before-sync-ready")) ok = false;
    ok = ok
        && install_sync_file(file);
    if (ok && install_fault(job, "after-sync-ready")) ok = false;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (!ok || install_fault(job, "before-sync-staged-slot")
        || !install_sync_device()) return false;
    return !install_fault(job, "after-sync-staged-slot");
}

static bool install_promote(TilefinchUpdateInstallJob *job)
{
    if (install_fault(job, "before-retire-inactive-slot")) return false;
    bool had_target = rename(job->target_path, job->old_path) == 0;
    if (!had_target && errno != ENOENT) return false;
    if (had_target
        && install_fault(job, "after-retire-inactive-slot")) return false;
    if (install_fault(job, "before-promote-staged-slot")) return false;
    if (rename(job->temporary_path, job->target_path) != 0) {
        if (had_target) (void) rename(job->old_path, job->target_path);
        return false;
    }
    if (install_fault(job, "after-promote-staged-slot")) return false;
    if (install_fault(job, "before-sync-promoted-slot")
        || !install_sync_device()) return false;
    if (install_fault(job, "after-sync-promoted-slot")) return false;
    TilefinchUpdateState pending = job->state;
    if (pending.generation == UINT64_MAX) return false;
    pending.generation++;
    pending.pending_slot = job->inactive_slot;
    pending.trial = TILEFINCH_UPDATE_TRIAL_PENDING;
    pending.candidate_sequence =
        job->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
        ? TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
        : job->manifest.release_sequence;
    pending.candidate_downgrade = job->allow_downgrade;
    memcpy(
        pending.candidate_sha256, job->manifest.package_sha256, 32);
    if (install_fault(job, "before-write-pending-journal")
        || !tilefinch_update_journal_store(
            job->data_dir, &pending, job->fault, job->fault_opaque))
        return false;
    job->state = pending;
    if (install_fault(job, "after-write-pending-journal")) return false;
    size_t entries = 0;
    (void) remove_tree_bounded(job->old_path, 0, &entries);
    return true;
}

bool tilefinch_update_install_pump(
    TilefinchUpdateInstallJob *job, size_t maximum_bytes)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
        return false;
    if (job->cancel_requested) {
        if (job->output_file != NULL) {
            fclose(job->output_file);
            job->output_file = NULL;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_CANCELLED;
        job->status = TILEFINCH_UPDATE_CANCELLED;
        snprintf(job->message, sizeof(job->message), "CANCELLED");
        return true;
    }
    size_t quota = maximum_bytes == 0 || maximum_bytes > INSTALL_IO_CHUNK
        ? INSTALL_IO_CHUNK : maximum_bytes;
    if (job->phase == TILEFINCH_UPDATE_INSTALL_VERIFYING) {
        size_t read = fread(job->buffer, 1, quota, job->package_file);
        if (read != 0) {
            if (!tilefinch_sha256_update(
                    &job->package_sha, job->buffer, read)) {
                install_error(job, TILEFINCH_UPDATE_IO, "HASHING FAILED");
                return true;
            }
            job->bytes_processed += read;
            return true;
        }
        uint8_t digest[32];
        if (ferror(job->package_file)
            || job->bytes_processed != job->manifest.package_size
            || !tilefinch_sha256_final(&job->package_sha, digest)
            || memcmp(digest, job->manifest.package_sha256, 32) != 0) {
            install_error(
                job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                "PACKAGE VERIFICATION FAILED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_READING_TABLE;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_READING_TABLE) {
        if (!install_read_table(job)) {
            install_error(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                          "PACKAGE TABLE IS INVALID");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_PREPARING;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PREPARING) {
        if (!install_prepare(job)) {
            install_error(
                job,
                job->status == TILEFINCH_UPDATE_NO_SPACE
                    ? TILEFINCH_UPDATE_NO_SPACE : TILEFINCH_UPDATE_IO,
                job->status == TILEFINCH_UPDATE_NO_SPACE
                    ? "NOT ENOUGH FREE SPACE TO INSTALL"
                    : "STAGING DIRECTORY IS NOT CLEAN");
            return true;
        }
        job->bytes_processed = 0;
        job->phase = TILEFINCH_UPDATE_INSTALL_EXTRACTING;
        snprintf(job->message, sizeof(job->message), "INSTALLING...");
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_EXTRACTING) {
        if (job->file_index >= job->package.file_count) {
            job->phase = TILEFINCH_UPDATE_INSTALL_FINALIZING;
        } else {
            const TilefinchUpdatePackageEntry *entry =
                &job->package.entries[job->file_index];
            if (job->output_file == NULL && !install_open_entry(job)) {
                install_error(job, TILEFINCH_UPDATE_IO,
                              "UPDATE FILE COULD NOT BE CREATED");
                return true;
            }
            uint64_t remaining = entry->size - job->current_file_bytes;
            size_t chunk = remaining < quota ? (size_t) remaining : quota;
            if (chunk != 0) {
                size_t read = fread(
                    job->buffer, 1, chunk, job->package_file);
                if (read != chunk
                    || install_fault(job, "before-write-payload")
                    || fwrite(job->buffer, 1, read, job->output_file) != read
                    || install_fault(job, "after-write-payload")
                    || !tilefinch_sha256_update(
                           &job->file_sha, job->buffer, read)) {
                    install_error(job, TILEFINCH_UPDATE_IO,
                                  "UPDATE EXTRACTION FAILED");
                    return true;
                }
                job->current_file_bytes += read;
                job->bytes_processed += read;
                return true;
            }
            uint8_t digest[32];
            bool ok = tilefinch_sha256_final(&job->file_sha, digest)
                && memcmp(digest, entry->sha256, 32) == 0
                && !install_fault(job, "before-sync-payload")
                && install_sync_file(job->output_file);
            if (ok && install_fault(job, "after-sync-payload")) ok = false;
            if (fclose(job->output_file) != 0) ok = false;
            job->output_file = NULL;
            if (!ok) {
                install_error(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                              "AN UPDATE FILE DID NOT VERIFY");
                return true;
            }
            job->file_index++;
            return true;
        }
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_FINALIZING) {
        if (!install_write_metadata(job)) {
            install_error(job, TILEFINCH_UPDATE_IO,
                          "UPDATE METADATA COULD NOT BE SAVED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_PROMOTING;
        snprintf(job->message, sizeof(job->message), "ACTIVATING UPDATE...");
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PROMOTING) {
        if (!install_promote(job)) {
            install_error(job, TILEFINCH_UPDATE_IO,
                          "UPDATE COULD NOT BE ACTIVATED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_COMPLETE;
        job->status = TILEFINCH_UPDATE_OK;
        /*
         * The package is now staged and journalled; the downloaded part file
         * (up to 32 MB) has no further reader. Close and reclaim it here
         * rather than leaving it on the stick until the next download.
         */
        if (job->package_file != NULL) {
            fclose(job->package_file);
            job->package_file = NULL;
        }
        (void) remove(job->package_path);
        snprintf(job->message, sizeof(job->message), "READY TO RESTART");
    }
    return true;
}

bool tilefinch_update_install_snapshot(
    const TilefinchUpdateInstallJob *job,
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
