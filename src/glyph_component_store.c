#include "tilefinch/glyph_component_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/component_store.h"
#include "tilefinch/glyph_component.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/document.h"
#include "tilefinch/sha256.h"

#define GLYPH_INSTALL_IO_CHUNK (16u * 1024u)
#define GLYPH_COMPONENT_TREE_DEPTH 4u
#define GLYPH_COMPONENT_TREE_LIMIT 24u
#define GLYPH_COMPONENT_REMOVED_MARKER "UNINSTALLED"
/* Written before candidate promotion and removed only after the active
   generation is durable.  It distinguishes a newly promoted generation from
   a stale pre-uninstall active directory if power fails before the uninstall
   tombstone can be retired. */
#define GLYPH_COMPONENT_ACTIVATED_MARKER "ACTIVATED"
#define GLYPH_COMPONENT_DOWNLOAD_NAME "glyph-component.part"
#define GLYPH_INSTALL_RECONCILE_ATTEMPTS 2u

/* Published packs are about 1 MB each and color emoji about 5 MB
   (docs/STORAGE.md). Bare Han cannot tell Simplified from Traditional
   Chinese, so both Chinese packs list it; kana and Hangul are unambiguous. */
static const TilefinchGlyphPackSpec glyph_specs[TILEFINCH_GLYPH_PACK_COUNT] = {
    {"glyph-ja", "Japanese", "tilefinch-glyph-ja-v1.tfgm",
     "tilefinch-glyph-ja-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_JAPANESE, 1024u},
    {"glyph-zh-hans", "Simplified Chinese",
     "tilefinch-glyph-zh-hans-v1.tfgm",
     "tilefinch-glyph-zh-hans-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_HAN, 1024u},
    {"glyph-zh-hant", "Traditional Chinese",
     "tilefinch-glyph-zh-hant-v1.tfgm",
     "tilefinch-glyph-zh-hant-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_HAN, 1024u},
    {"glyph-ko", "Korean", "tilefinch-glyph-ko-v1.tfgm",
     "tilefinch-glyph-ko-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_KOREAN, 1024u},
    {"glyph-emoji-color", "Color Emoji",
     "tilefinch-glyph-emoji-color-v1.tfgm",
     "tilefinch-glyph-emoji-color-v1.tfgf", 0u, 5120u},
    {"glyph-cyrillic", "Cyrillic",
     "tilefinch-glyph-cyrillic-v1.tfgm",
     "tilefinch-glyph-cyrillic-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_CYRILLIC,
     1024u},
    {"glyph-latin-extended", "Extended Latin",
     "tilefinch-glyph-latin-extended-v1.tfgm",
     "tilefinch-glyph-latin-extended-v1.tfgf",
     DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED, 1024u},
    {"glyph-arabic", "Arabic", "tilefinch-glyph-arabic-v1.tfgm",
     "tilefinch-glyph-arabic-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_ARABIC, 1024u},
    {"glyph-hebrew", "Hebrew", "tilefinch-glyph-hebrew-v1.tfgm",
     "tilefinch-glyph-hebrew-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_HEBREW, 1024u},
    {"glyph-devanagari", "Devanagari (Hindi)",
     "tilefinch-glyph-devanagari-v1.tfgm",
     "tilefinch-glyph-devanagari-v1.tfgf", DOCUMENT_GLYPH_SCRIPT_DEVANAGARI,
     284u},
    {"glyph-bengali-assamese", "Bengali-Assamese",
     "tilefinch-glyph-bengali-assamese-v1.tfgm",
     "tilefinch-glyph-bengali-assamese-v1.tfgf",
     DOCUMENT_GLYPH_SCRIPT_BENGALI_ASSAMESE, 284u}
};

const TilefinchGlyphPackSpec *tilefinch_glyph_pack_spec(
    TilefinchGlyphPack pack)
{
    return pack < TILEFINCH_GLYPH_PACK_COUNT ? &glyph_specs[pack] : NULL;
}

bool tilefinch_glyph_pack_for_language(
    unsigned language, TilefinchGlyphPack *pack)
{
    if (pack == NULL) return false;
    switch ((BrowserGlyphLanguage) language) {
        case BROWSER_GLYPH_LANGUAGE_JAPANESE:
            *pack = TILEFINCH_GLYPH_PACK_JAPANESE;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED:
            *pack = TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL:
            *pack = TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL;
            return true;
        case BROWSER_GLYPH_LANGUAGE_KOREAN:
            *pack = TILEFINCH_GLYPH_PACK_KOREAN;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CYRILLIC:
            *pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
            return true;
        case BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED:
            *pack = TILEFINCH_GLYPH_PACK_LATIN_EXTENDED;
            return true;
        case BROWSER_GLYPH_LANGUAGE_ARABIC:
            *pack = TILEFINCH_GLYPH_PACK_ARABIC;
            return true;
        case BROWSER_GLYPH_LANGUAGE_HEBREW:
            *pack = TILEFINCH_GLYPH_PACK_HEBREW;
            return true;
        case BROWSER_GLYPH_LANGUAGE_DEVANAGARI:
            *pack = TILEFINCH_GLYPH_PACK_DEVANAGARI;
            return true;
        case BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE:
            *pack = TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE;
            return true;
        case BROWSER_GLYPH_LANGUAGE_COUNT:
        case BROWSER_GLYPH_LANGUAGE_EMBEDDED:
        default:
            return false;
    }
}

struct TilefinchGlyphComponentInstall {
    Budget *budget;
    FILE *package_file;
    uint8_t *buffer;
    uint8_t *envelope;
    size_t envelope_length;
    TilefinchUpdateManifest manifest;
    uint8_t manifest_digest[32];
    TilefinchSha256 package_sha;
    uint64_t bytes_processed;
    TilefinchGlyphPack pack;
    TilefinchUpdateInstallPhase phase;
    TilefinchUpdateStatus status;
    bool cancel_requested;
    bool activated;
    bool tombstone_cleared;
    bool activation_marker_cleared;
    uint8_t reconcile_attempts;
    TilefinchGlyphInstallFaultHook fault;
    void *fault_opaque;
    char package_path[TILEFINCH_INSTALL_PATH_LIMIT];
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate_pack[TILEFINCH_INSTALL_PATH_LIMIT];
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char removed_marker[TILEFINCH_INSTALL_PATH_LIMIT];
    char message[96];
};

static bool restore_pre_activation_package(
    TilefinchGlyphComponentInstall *job);

static bool component_paths(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    char *component, char *active, char *previous, char *marker)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    if (paths == NULL || !paths->slotted || spec == NULL) return false;
    char relative[96];
    int written = snprintf(relative, sizeof(relative), "components/%s", spec->id);
    return written > 0 && (size_t) written < sizeof(relative)
        && tilefinch_component_store_join(
               paths->install_root, relative, component,
               TILEFINCH_INSTALL_PATH_LIMIT)
        && tilefinch_component_store_join(
               component, "active", active, TILEFINCH_INSTALL_PATH_LIMIT)
        && tilefinch_component_store_join(
               component, "previous", previous, TILEFINCH_INSTALL_PATH_LIMIT)
        && tilefinch_component_store_join(
               component, GLYPH_COMPONENT_REMOVED_MARKER, marker,
               TILEFINCH_INSTALL_PATH_LIMIT);
}

static bool directory_pack_path(const char *directory, char *output,
                                size_t output_size)
{
    return tilefinch_component_store_join(
        directory, "pack.tfgf", output, output_size);
}

static bool directory_activation_path(
    const char *directory, char *output, size_t output_size)
{
    return tilefinch_component_store_join(
        directory, GLYPH_COMPONENT_ACTIVATED_MARKER, output, output_size);
}

static bool component_directory_complete(
    Budget *budget, const char *directory, const char *expected_id)
{
    char ready[TILEFINCH_INSTALL_PATH_LIMIT];
    char pack[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_component_store_join(
            directory, "READY", ready, sizeof(ready))
        || !tilefinch_component_store_exists(ready)
        || !directory_pack_path(directory, pack, sizeof(pack))) return false;
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(budget);
    bool valid = provider != NULL
        && tilefinch_glyph_provider_attach(provider, pack, expected_id);
    tilefinch_glyph_provider_destroy(provider);
    return valid;
}

bool tilefinch_glyph_component_resolve(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return false;
    output[0] = '\0';
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char marker[TILEFINCH_INSTALL_PATH_LIMIT];
    if (spec == NULL || !component_paths(
            paths, pack, component, active, previous, marker)) return false;
    bool removed = tilefinch_component_store_exists(marker);
    char activated[TILEFINCH_INSTALL_PATH_LIMIT];
    bool active_supersedes_removal = removed
        && directory_activation_path(active, activated, sizeof(activated))
        && tilefinch_component_store_exists(activated);
    if (removed && !active_supersedes_removal) return false;
    char pack_path[TILEFINCH_INSTALL_PATH_LIMIT];
    const char *directories[2] = {active, previous};
    size_t directory_count = active_supersedes_removal ? 1u : 2u;
    for (size_t at = 0; at < directory_count; at++) {
        char ready[TILEFINCH_INSTALL_PATH_LIMIT];
        if (!directory_pack_path(
                directories[at], pack_path, sizeof(pack_path))
            || !tilefinch_component_store_join(
                   directories[at], "READY", ready, sizeof(ready))
            || !tilefinch_component_store_exists(pack_path)
            || !tilefinch_component_store_exists(ready)) continue;
        int written = snprintf(output, output_size, "%s", pack_path);
        return written > 0 && (size_t) written < output_size;
    }
    return false;
}

static bool identity_from_directory(
    Budget *budget, const char *directory, const char *expected_id,
    const TilefinchUpdateRoot *root, uint64_t *sequence,
    uint8_t package_sha256[32])
{
    if (!component_directory_complete(budget, directory, expected_id))
        return false;
    char metadata[TILEFINCH_INSTALL_PATH_LIMIT];
    char ready[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_component_store_join(
            directory, "component.tfgm", metadata, sizeof(metadata))
        || !tilefinch_component_store_join(
               directory, "READY", ready, sizeof(ready))) return false;
    size_t length = 0;
    uint8_t *envelope = tilefinch_component_store_read_file(
        budget, metadata, TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES, &length);
    TilefinchUpdateVerifiedEnvelope verified = {0};
    TilefinchUpdateStatus status = envelope != NULL
        ? tilefinch_update_verify_glyph_envelope(
              envelope, length,
              &(TilefinchUpdateVerifyOptions) {
                  .embedded_root = root,
                  .crypto = tilefinch_update_default_crypto(),
                  .clock_valid = true,
                  .launcher_protocol = TILEFINCH_GLYPH_COMPONENT_ABI
              }, &verified)
        : TILEFINCH_UPDATE_IO;
    budget_free(budget, envelope);
    if (status != TILEFINCH_UPDATE_OK
        || !tilefinch_component_store_file_matches(
               ready, verified.manifest_digest, 32)) return false;
    *sequence = verified.manifest.release_sequence;
    memcpy(package_sha256, verified.manifest.package_sha256, 32);
    return true;
}

bool tilefinch_glyph_component_installed_identity(
    Budget *budget, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack, const TilefinchUpdateRoot *root,
    uint64_t *sequence, uint8_t package_sha256[32])
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char marker[TILEFINCH_INSTALL_PATH_LIMIT];
    if (budget == NULL || root == NULL || sequence == NULL
        || package_sha256 == NULL || spec == NULL
        || !component_paths(paths, pack, component, active, previous, marker))
        return false;
    bool removed = tilefinch_component_store_exists(marker);
    char activated[TILEFINCH_INSTALL_PATH_LIMIT];
    bool active_supersedes_removal = removed
        && directory_activation_path(active, activated, sizeof(activated))
        && tilefinch_component_store_exists(activated);
    if (removed && !active_supersedes_removal) return false;
    return identity_from_directory(
               budget, active, spec->id, root, sequence, package_sha256)
        || (!active_supersedes_removal
            && identity_from_directory(
                budget, previous, spec->id, root, sequence, package_sha256));
}

static void install_fail(TilefinchGlyphComponentInstall *job,
                         TilefinchUpdateStatus status, const char *message)
{
    job->status = status;
    job->phase = TILEFINCH_UPDATE_INSTALL_ERROR;
    snprintf(job->message, sizeof(job->message), "%s", message);
}

static bool install_fault(
    TilefinchGlyphComponentInstall *job,
    TilefinchGlyphInstallFaultPoint point)
{
    return job->fault != NULL && job->fault(job->fault_opaque, point);
}

static bool restore_pre_activation_package(
    TilefinchGlyphComponentInstall *job)
{
    if (job->activated || tilefinch_component_store_exists(job->package_path))
        return true;
    if (!tilefinch_component_store_exists(job->candidate_pack)
        || !tilefinch_component_store_rename(
               job->candidate_pack, job->package_path)) return false;
    /* A retry stages from the same directory, so no record of this attempt
       may stay beside the package. */
    static const char *const records[] = {
        "READY", "component.tfgm", GLYPH_COMPONENT_ACTIVATED_MARKER
    };
    for (size_t at = 0; at < sizeof(records) / sizeof(records[0]); at++) {
        char record[TILEFINCH_INSTALL_PATH_LIMIT];
        if (tilefinch_component_store_join(
                job->candidate, records[at], record, sizeof(record)))
            (void) unlink(record);
    }
    return true;
}

static void install_fail_before_activation(
    TilefinchGlyphComponentInstall *job, TilefinchUpdateStatus status,
    const char *message)
{
    if (!restore_pre_activation_package(job)) {
        install_fail(job, TILEFINCH_UPDATE_IO,
                     "FONT PACK RECOVERY FAILED");
        return;
    }
    install_fail(job, status, message);
}

static bool candidate_paths(
    const char *install_root, const TilefinchGlyphPackSpec *spec,
    char *component, char *candidate)
{
    char relative[96];
    snprintf(relative, sizeof(relative), "components/%s", spec->id);
    return tilefinch_component_store_join(
               install_root, relative, component, TILEFINCH_INSTALL_PATH_LIMIT)
        && tilefinch_component_store_join(
               component, "candidate.tmp", candidate,
               TILEFINCH_INSTALL_PATH_LIMIT);
}

bool tilefinch_glyph_component_download_path(
    const char *install_root, TilefinchGlyphPack pack,
    char *output, size_t output_size)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    return install_root != NULL && spec != NULL
        && candidate_paths(install_root, spec, component, candidate)
        && tilefinch_component_store_join(
               candidate, GLYPH_COMPONENT_DOWNLOAD_NAME, output, output_size);
}

bool tilefinch_glyph_component_prepare_download(
    const char *install_root, TilefinchGlyphPack pack,
    char *output, size_t output_size)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    if (install_root == NULL || spec == NULL
        || !candidate_paths(install_root, spec, component, candidate)
        || !tilefinch_component_store_join(
               candidate, GLYPH_COMPONENT_DOWNLOAD_NAME, output, output_size))
        return false;
    return tilefinch_component_store_make_directory(component)
        && tilefinch_component_store_remove_tree(
               candidate, GLYPH_COMPONENT_TREE_DEPTH,
               GLYPH_COMPONENT_TREE_LIMIT)
        && mkdir(candidate, 0777) == 0;
}

TilefinchGlyphComponentInstall *tilefinch_glyph_component_install_create(
    Budget *budget, const TilefinchGlyphComponentInstallOptions *options)
{
    const TilefinchGlyphPackSpec *spec = options == NULL ? NULL
        : tilefinch_glyph_pack_spec(options->pack);
    if (budget == NULL || options == NULL || spec == NULL
        || options->envelope == NULL
        || options->envelope_length == 0 || options->manifest == NULL
        || options->manifest_digest == NULL || options->install_root == NULL
        || options->manifest->package_format
               != TILEFINCH_UPDATE_PACKAGE_GLYPH
        || strcmp(options->manifest->asset, spec->pack_asset) != 0
        || options->envelope_length > TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES)
        return NULL;
    TilefinchGlyphComponentInstall *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->buffer = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, GLYPH_INSTALL_IO_CHUNK);
    job->envelope = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, options->envelope_length);
    if (job->buffer == NULL || job->envelope == NULL) {
        tilefinch_glyph_component_install_destroy(job);
        return NULL;
    }
    memcpy(job->envelope, options->envelope, options->envelope_length);
    job->envelope_length = options->envelope_length;
    job->manifest = *options->manifest;
    memcpy(job->manifest_digest, options->manifest_digest, 32);
    job->pack = options->pack;
    job->fault = options->fault;
    job->fault_opaque = options->fault_opaque;
    if (!candidate_paths(options->install_root, spec, job->component,
                         job->candidate)
        || !tilefinch_component_store_join(
               job->candidate, GLYPH_COMPONENT_DOWNLOAD_NAME,
               job->package_path, sizeof(job->package_path))
        || !tilefinch_component_store_join(
               job->candidate, "pack.tfgf", job->candidate_pack,
               sizeof(job->candidate_pack))
        || !tilefinch_component_store_join(
               job->component, "active", job->active, sizeof(job->active))
        || !tilefinch_component_store_join(
               job->component, "previous", job->previous,
               sizeof(job->previous))
        || !tilefinch_component_store_join(
               job->component, GLYPH_COMPONENT_REMOVED_MARKER,
               job->removed_marker, sizeof(job->removed_marker))) {
        tilefinch_glyph_component_install_destroy(job);
        return NULL;
    }
    job->package_file = fopen(job->package_path, "rb");
    if (job->package_file == NULL) {
        tilefinch_glyph_component_install_destroy(job);
        return NULL;
    }
    tilefinch_sha256_init(&job->package_sha);
    job->phase = TILEFINCH_UPDATE_INSTALL_VERIFYING;
    snprintf(job->message, sizeof(job->message), "VERIFYING FONT PACK...");
    return job;
}

void tilefinch_glyph_component_install_destroy(
    TilefinchGlyphComponentInstall *job)
{
    if (job == NULL) return;
    if (job->package_file != NULL) fclose(job->package_file);
    if (!job->activated) (void) restore_pre_activation_package(job);
    Budget *budget = job->budget;
    budget_free(budget, job->envelope);
    budget_free(budget, job->buffer);
    memset(job, 0, sizeof(*job));
    budget_free(budget, job);
}

bool tilefinch_glyph_component_install_cancel(
    TilefinchGlyphComponentInstall *job)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_PROMOTING)
        return false;
    job->cancel_requested = true;
    return true;
}

static bool finalize_candidate(TilefinchGlyphComponentInstall *job)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(job->pack);
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(
        job->budget);
    bool valid = provider != NULL && spec != NULL
        && tilefinch_glyph_provider_attach(
            provider, job->candidate_pack, spec->id);
    tilefinch_glyph_provider_destroy(provider);
    char metadata[TILEFINCH_INSTALL_PATH_LIMIT];
    char ready[TILEFINCH_INSTALL_PATH_LIMIT];
    char activated[TILEFINCH_INSTALL_PATH_LIMIT];
    static const uint8_t activation_record[] = {'T','F','G','A','v','1','\n'};
    return valid
        && tilefinch_component_store_join(
               job->candidate, "component.tfgm", metadata, sizeof(metadata))
        && tilefinch_component_store_write_file(
               metadata, job->envelope, job->envelope_length)
        && tilefinch_component_store_join(
               job->candidate, "READY", ready, sizeof(ready))
        && tilefinch_component_store_write_file(
               ready, job->manifest_digest, 32)
        && directory_activation_path(
            job->candidate, activated, sizeof(activated))
        && tilefinch_component_store_write_file(
            activated, activation_record, sizeof(activation_record));
}

typedef enum {
    GLYPH_PROMOTION_COMPLETE = 0,
    GLYPH_PROMOTION_RETRY,
    GLYPH_PROMOTION_PRE_ACTIVATION_FAILURE,
    GLYPH_PROMOTION_ACTIVATED_FAILURE
} GlyphPromotionOutcome;

static GlyphPromotionOutcome promotion_reconcile_failure(
    TilefinchGlyphComponentInstall *job)
{
    if (job->reconcile_attempts != UINT8_MAX) job->reconcile_attempts++;
    return job->reconcile_attempts < GLYPH_INSTALL_RECONCILE_ATTEMPTS
        ? GLYPH_PROMOTION_RETRY : GLYPH_PROMOTION_ACTIVATED_FAILURE;
}

static GlyphPromotionOutcome promote_candidate(
    TilefinchGlyphComponentInstall *job)
{
    if (!job->activated) {
        if (install_fault(
                job, TILEFINCH_GLYPH_INSTALL_FAULT_REMOVE_PREVIOUS)
            || !tilefinch_component_store_remove_tree(
                   job->previous, GLYPH_COMPONENT_TREE_DEPTH,
                   GLYPH_COMPONENT_TREE_LIMIT)) {
            return GLYPH_PROMOTION_PRE_ACTIVATION_FAILURE;
        }
        bool had_active = tilefinch_component_store_rename(
            job->active, job->previous);
        if (!had_active && errno != ENOENT)
            return GLYPH_PROMOTION_PRE_ACTIVATION_FAILURE;
        if (install_fault(
                job, TILEFINCH_GLYPH_INSTALL_FAULT_ACTIVATE_CANDIDATE)
            || !tilefinch_component_store_rename(
                   job->candidate, job->active)) {
            if (had_active)
                (void) tilefinch_component_store_rename(
                    job->previous, job->active);
            return GLYPH_PROMOTION_PRE_ACTIVATION_FAILURE;
        }
        job->activated = true;
        job->reconcile_attempts = 0;
        /* Each sync after a completed step is a durability hint. */
        (void) tilefinch_memory_stick_sync();
    }
    if (!job->tombstone_cleared) {
        if (install_fault(
                job, TILEFINCH_GLYPH_INSTALL_FAULT_TOMBSTONE_UNLINK)
            || (unlink(job->removed_marker) != 0 && errno != ENOENT)) {
            return promotion_reconcile_failure(job);
        }
        job->tombstone_cleared = true;
        job->reconcile_attempts = 0;
        (void) tilefinch_memory_stick_sync();
    }
    if (!job->activation_marker_cleared) {
        char activated[TILEFINCH_INSTALL_PATH_LIMIT];
        if (!directory_activation_path(
                job->active, activated, sizeof(activated))
            || (unlink(activated) != 0 && errno != ENOENT)) {
            return promotion_reconcile_failure(job);
        }
        job->activation_marker_cleared = true;
        job->reconcile_attempts = 0;
    }
    (void) tilefinch_memory_stick_sync();
    return GLYPH_PROMOTION_COMPLETE;
}

bool tilefinch_glyph_component_install_pump(
    TilefinchGlyphComponentInstall *job, size_t maximum_bytes)
{
    if (job == NULL || job->phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
        return false;
    if (job->cancel_requested) {
        if (!restore_pre_activation_package(job)) {
            install_fail(job, TILEFINCH_UPDATE_IO,
                         "FONT PACK RECOVERY FAILED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_CANCELLED;
        job->status = TILEFINCH_UPDATE_CANCELLED;
        snprintf(job->message, sizeof(job->message), "CANCELLED");
        return true;
    }
    size_t quota = maximum_bytes == 0
        || maximum_bytes > GLYPH_INSTALL_IO_CHUNK
        ? GLYPH_INSTALL_IO_CHUNK : maximum_bytes;
    if (job->phase == TILEFINCH_UPDATE_INSTALL_VERIFYING) {
        size_t count = fread(job->buffer, 1, quota, job->package_file);
        if (count != 0) {
            if (!tilefinch_sha256_update(&job->package_sha,
                                         job->buffer, count))
                install_fail(job, TILEFINCH_UPDATE_IO, "PACK HASH FAILED");
            else
                job->bytes_processed += count;
            return true;
        }
        uint8_t digest[32];
        if (ferror(job->package_file)
            || job->bytes_processed != job->manifest.package_size
            || !tilefinch_sha256_final(&job->package_sha, digest)
            || memcmp(digest, job->manifest.package_sha256, 32) != 0) {
            install_fail(job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                         "FONT PACK DID NOT VERIFY");
            return true;
        }
        fclose(job->package_file);
        job->package_file = NULL;
        job->phase = TILEFINCH_UPDATE_INSTALL_PREPARING;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PREPARING) {
        if (!tilefinch_component_store_rename(
                job->package_path, job->candidate_pack)) {
            install_fail(job, TILEFINCH_UPDATE_IO,
                         "FONT PACK STAGING FAILED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_FINALIZING;
        snprintf(job->message, sizeof(job->message), "INSTALLING FONT PACK...");
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_FINALIZING) {
        if (install_fault(job, TILEFINCH_GLYPH_INSTALL_FAULT_SELF_CHECK)
            || !finalize_candidate(job)) {
            install_fail_before_activation(
                job, TILEFINCH_UPDATE_PACKAGE_MISMATCH,
                "FONT PACK SELF-CHECK FAILED");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_PROMOTING;
    }
    if (job->phase == TILEFINCH_UPDATE_INSTALL_PROMOTING) {
        GlyphPromotionOutcome promoted = promote_candidate(job);
        if (promoted == GLYPH_PROMOTION_RETRY) {
            snprintf(job->message, sizeof(job->message),
                     "FINALIZING FONT PACK...");
            return true;
        }
        if (promoted == GLYPH_PROMOTION_PRE_ACTIVATION_FAILURE) {
            install_fail_before_activation(
                job, TILEFINCH_UPDATE_IO,
                "FONT PACK COULD NOT BE ACTIVATED");
            return true;
        }
        if (promoted == GLYPH_PROMOTION_ACTIVATED_FAILURE) {
            install_fail(job, TILEFINCH_UPDATE_IO,
                         "FONT PACK INSTALLED; RECHECKING STORAGE");
            return true;
        }
        job->phase = TILEFINCH_UPDATE_INSTALL_COMPLETE;
        job->status = TILEFINCH_UPDATE_OK;
        snprintf(job->message, sizeof(job->message),
                 "FONT PACK READY AFTER RESTART");
    }
    return true;
}

bool tilefinch_glyph_component_install_snapshot(
    const TilefinchGlyphComponentInstall *job,
    TilefinchUpdateInstallSnapshot *snapshot)
{
    if (job == NULL || snapshot == NULL) return false;
    *snapshot = (TilefinchUpdateInstallSnapshot) {
        .phase = job->phase,
        .status = job->status,
        .bytes_processed = job->bytes_processed,
        .bytes_total = job->manifest.package_size,
        .files_completed = job->phase >= TILEFINCH_UPDATE_INSTALL_FINALIZING,
        .files_total = 1
    };
    snprintf(snapshot->message, sizeof(snapshot->message), "%s",
             job->message);
    return true;
}

bool tilefinch_glyph_component_install_activated(
    const TilefinchGlyphComponentInstall *job)
{
    return job != NULL && job->activated;
}

bool tilefinch_glyph_component_remove(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack)
{
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    char active[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[TILEFINCH_INSTALL_PATH_LIMIT];
    char marker[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!component_paths(paths, pack, component, active, previous, marker))
        return false;
    bool had_component = tilefinch_component_store_exists(active)
        || tilefinch_component_store_exists(previous)
        || tilefinch_component_store_exists(marker);
    static const uint8_t removed[] = {'T','F','G','R','v','1','\n'};
    char activated[TILEFINCH_INSTALL_PATH_LIMIT];
    bool activation_path_ready = directory_activation_path(
        active, activated, sizeof(activated));
    if (!tilefinch_component_store_make_directory(component)
        || !activation_path_ready
        || (unlink(activated) != 0 && errno != ENOENT)) return false;
    (void) tilefinch_memory_stick_sync();
    if (!tilefinch_component_store_write_file(
            marker, removed, sizeof(removed))) return false;
    (void) tilefinch_component_store_remove_tree(
        active, GLYPH_COMPONENT_TREE_DEPTH, GLYPH_COMPONENT_TREE_LIMIT);
    (void) tilefinch_component_store_remove_tree(
        previous, GLYPH_COMPONENT_TREE_DEPTH, GLYPH_COMPONENT_TREE_LIMIT);
    char candidate[TILEFINCH_INSTALL_PATH_LIMIT];
    if (tilefinch_component_store_join(
            component, "candidate.tmp", candidate, sizeof(candidate))) {
        (void) tilefinch_component_store_remove_tree(
            candidate, GLYPH_COMPONENT_TREE_DEPTH,
            GLYPH_COMPONENT_TREE_LIMIT);
    }
    return had_component;
}
