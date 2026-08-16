#include "tilefinch/psp_glyph_component_session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define GLYPH_COMPONENT_IO_UNIT (16u * 1024u)

#ifndef TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER
#define TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER "stjanovitz"
#endif
#ifndef TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME
#define TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME "tilefinch-models"
#endif

static uint8_t pack_bit(TilefinchGlyphPack pack)
{
    return pack < TILEFINCH_GLYPH_PACK_COUNT
        ? (uint8_t) (1u << (unsigned) pack) : 0;
}

static bool language_pack(
    BrowserGlyphLanguage language, TilefinchGlyphPack *pack)
{
    if (pack == NULL) return false;
    switch (language) {
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
        case BROWSER_GLYPH_LANGUAGE_EMBEDDED:
        default:
            return false;
    }
}

static bool ensure_root(PspGlyphComponentSession *session)
{
    if (session->root_ready) return true;
    session->root_ready = tilefinch_update_embedded_root(&session->root);
    return session->root_ready;
}

static bool pack_signed_and_resolved(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack, char path[TILEFINCH_INSTALL_PATH_LIMIT])
{
    uint64_t sequence = 0;
    uint8_t digest[32];
    return session != NULL && session->budget != NULL && ensure_root(session)
        && tilefinch_glyph_component_installed_identity(
               session->budget, paths, pack, &session->root,
               &sequence, digest)
        && tilefinch_glyph_component_resolve(
               paths, pack, path, TILEFINCH_INSTALL_PATH_LIMIT);
}

static bool attach_pack(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (spec == NULL
        || !pack_signed_and_resolved(session, paths, pack, path)) return false;
    session->installed_mask |= pack_bit(pack);
    if (!tilefinch_glyph_provider_attach(
            session->provider, path, spec->id)) return false;
    session->attached_mask |= pack_bit(pack);
    return true;
}

bool psp_glyph_component_session_attach_selected(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, BrowserGlyphLanguage language,
    bool color_emoji)
{
    if (session == NULL || budget == NULL || paths == NULL
        || !paths->slotted || session->provider != NULL) return false;
    session->budget = budget;
    TilefinchGlyphPack regional = TILEFINCH_GLYPH_PACK_JAPANESE;
    bool have_regional = language_pack(language, &regional);
    if (!have_regional && !color_emoji) return true;
    session->provider = tilefinch_glyph_provider_create(budget);
    if (session->provider == NULL) return false;
    if (have_regional) (void) attach_pack(session, paths, regional);
    if (color_emoji)
        (void) attach_pack(session, paths, TILEFINCH_GLYPH_PACK_COLOR_EMOJI);
    if (tilefinch_glyph_provider_pack_count(session->provider) == 0) {
        tilefinch_glyph_provider_destroy(session->provider);
        session->provider = NULL;
        return true;
    }
    if (!font_optional_glyph_provider_install(session->provider)) {
        tilefinch_glyph_provider_destroy(session->provider);
        session->provider = NULL;
        session->attached_mask = 0;
        return false;
    }
    return true;
}

static void deactivate_runtime(
    PspGlyphComponentSession *session, BrowserEngine *engine)
{
    if (session == NULL || session->provider == NULL) return;
    (void) font_optional_glyph_provider_uninstall(session->provider);
    tilefinch_glyph_provider_destroy(session->provider);
    session->provider = NULL;
    session->attached_mask = 0;
    session->runtime_changed = true;
    if (engine != NULL)
        (void) browser_engine_optional_glyphs_updated(engine);
}

void psp_glyph_component_session_destroy(PspGlyphComponentSession *session)
{
    if (session == NULL) return;
    if (session->provider != NULL) {
        (void) font_optional_glyph_provider_uninstall(session->provider);
        tilefinch_glyph_provider_destroy(session->provider);
    }
    tilefinch_glyph_component_install_destroy(session->installer);
    tilefinch_update_client_destroy(session->client);
    memset(session, 0, sizeof(*session));
}

bool psp_glyph_component_session_pump_runtime(
    PspGlyphComponentSession *session, BrowserEngine *engine)
{
    if (session == NULL || session->provider == NULL) return false;
    bool changed = false;
    size_t bytes = 0;
    if (!tilefinch_glyph_provider_pump(
            session->provider, GLYPH_COMPONENT_IO_UNIT,
            &changed, &bytes)) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (bytes != 0 || changed)
        printf("tilefinch-glyph-component: runtime-read=%u changed=%u\n",
               (unsigned) bytes, changed ? 1u : 0u);
#else
    (void) bytes;
#endif
    if (changed && engine != NULL)
        (void) browser_engine_optional_glyph_payloads_ready(engine);
    return changed;
}

void psp_glyph_component_session_probe(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths)
{
    if (session == NULL || paths == NULL) return;
    session->installed_mask = 0;
    if (session->budget == NULL || !ensure_root(session)) return;
    for (TilefinchGlyphPack pack = 0;
         pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        uint64_t sequence = 0;
        uint8_t digest[32];
        if (tilefinch_glyph_component_installed_identity(
                session->budget, paths, pack, &session->root,
                &sequence, digest))
            session->installed_mask |= pack_bit(pack);
    }
}

bool psp_glyph_component_session_installed(
    const PspGlyphComponentSession *session, TilefinchGlyphPack pack)
{
    return session != NULL
        && (session->installed_mask & pack_bit(pack)) != 0;
}

bool psp_glyph_component_session_metadata_url(
    TilefinchGlyphPack pack, char *output, size_t capacity)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    if (spec == NULL || output == NULL || capacity == 0) return false;
    int written = snprintf(
        output, capacity,
        "https://github.com/%s/%s/releases/latest/download/%s",
        TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER,
        TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME,
        spec->metadata_asset);
    return written > 0 && (size_t) written < capacity;
}

static void clear_operation(PspGlyphComponentSession *session)
{
    tilefinch_glyph_component_install_destroy(session->installer);
    tilefinch_update_client_destroy(session->client);
    session->installer = NULL;
    session->client = NULL;
    session->operation_initialized = false;
    session->auto_install = false;
    memset(&session->client_snapshot, 0, sizeof(session->client_snapshot));
    memset(&session->install_snapshot, 0, sizeof(session->install_snapshot));
}

bool psp_glyph_component_session_select_operation(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    if (session == NULL || budget == NULL || paths == NULL
        || !paths->slotted || spec == NULL
        || psp_glyph_component_session_active(session)) return false;
    clear_operation(session);
    session->budget = budget;
    if (!ensure_root(session)) return false;
    char update_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_data_path(
            paths, "update", update_dir, sizeof(update_dir))
        || (mkdir(update_dir, 0777) != 0 && errno != EEXIST)
        || !tilefinch_install_data_path(
            paths, "update/glyph-component.part", session->package_path,
            sizeof(session->package_path))) return false;
    uint64_t sequence = 0;
    uint8_t digest[32] = {0};
    bool installed = tilefinch_glyph_component_installed_identity(
        budget, paths, pack, &session->root, &sequence, digest);
    session->client = tilefinch_update_client_create(
        budget, &(TilefinchUpdateClientOptions) {
            .embedded_root = &session->root,
            .installed_sequence = sequence,
            .installed_package_sha256 = digest,
            .installed_sequence_valid = installed,
            .installed_pair_valid = installed,
            .launcher_protocol = TILEFINCH_GLYPH_COMPONENT_ABI,
            .repository_owner = TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER,
            .repository_name = TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME,
            .trust = TILEFINCH_UPDATE_TRUST_SIGNED,
            .package_part_path = session->package_path,
            .metadata_asset = spec->metadata_asset,
            .artifact = TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT
        });
    session->operation_pack = pack;
    session->operation_initialized = session->client != NULL;
    return session->operation_initialized;
}

PspGlyphComponentPrimaryResult psp_glyph_component_session_primary(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, BrowserEngine *engine)
{
    if (session == NULL || !session->operation_initialized
        || session->client == NULL)
        return PSP_GLYPH_COMPONENT_PRIMARY_NONE;
    if (session->installer != NULL) {
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            tilefinch_glyph_component_install_destroy(session->installer);
            session->installer = NULL;
            psp_glyph_component_session_probe(session, paths);
            return PSP_GLYPH_COMPONENT_PRIMARY_NONE;
        }
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_ERROR
            || session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_CANCELLED) {
            tilefinch_glyph_component_install_destroy(session->installer);
            session->installer = NULL;
        } else {
            return PSP_GLYPH_COMPONENT_PRIMARY_NONE;
        }
    }
    (void) tilefinch_update_client_snapshot(
        session->client, &session->client_snapshot);
    switch (session->client_snapshot.phase) {
        case TILEFINCH_UPDATE_CLIENT_IDLE:
        case TILEFINCH_UPDATE_CLIENT_ERROR:
        case TILEFINCH_UPDATE_CLIENT_UP_TO_DATE:
            session->auto_install = true;
            return PSP_GLYPH_COMPONENT_PRIMARY_CHECK_REQUIRED;
        case TILEFINCH_UPDATE_CLIENT_AVAILABLE:
            session->auto_install = true;
            (void) tilefinch_update_client_begin_download(session->client);
            break;
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADED: {
            deactivate_runtime(session, engine);
            size_t envelope_length = 0;
            const uint8_t *envelope = tilefinch_update_client_envelope(
                session->client, &envelope_length);
            session->installer = tilefinch_glyph_component_install_create(
                session->budget,
                &(TilefinchGlyphComponentInstallOptions) {
                    .package_path = session->package_path,
                    .envelope = envelope,
                    .envelope_length = envelope_length,
                    .manifest = &session->client_snapshot.manifest,
                    .manifest_digest =
                        session->client_snapshot.manifest_digest,
                    .install_root = paths->install_root,
                    .pack = session->operation_pack
                });
            break;
        }
        default:
            break;
    }
    return PSP_GLYPH_COMPONENT_PRIMARY_NONE;
}

bool psp_glyph_component_session_begin_check(
    PspGlyphComponentSession *session, uint64_t now_unix, bool clock_valid)
{
    return session != NULL && session->client != NULL
        && tilefinch_update_client_begin_check(
               session->client, now_unix, clock_valid);
}

bool psp_glyph_component_session_cancel(PspGlyphComponentSession *session)
{
    if (session == NULL) return false;
    return session->installer != NULL
        ? tilefinch_glyph_component_install_cancel(session->installer)
        : tilefinch_update_client_cancel(session->client);
}

bool psp_glyph_component_session_pump_operation(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, BrowserEngine *engine)
{
    if (session == NULL) return false;
    if (session->installer != NULL) {
        (void) tilefinch_glyph_component_install_pump(
            session->installer, GLYPH_COMPONENT_IO_UNIT);
        (void) tilefinch_glyph_component_install_snapshot(
            session->installer, &session->install_snapshot);
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            psp_glyph_component_session_probe(session, paths);
            session->auto_install = false;
            tilefinch_glyph_component_install_destroy(session->installer);
            session->installer = NULL;
        }
        return true;
    }
    if (session->client == NULL) return false;
    (void) tilefinch_update_client_pump(session->client, 2000u);
    (void) tilefinch_update_client_snapshot(
        session->client, &session->client_snapshot);
    if (session->auto_install
        && session->client_snapshot.phase
               == TILEFINCH_UPDATE_CLIENT_AVAILABLE) {
        (void) tilefinch_update_client_begin_download(session->client);
        (void) tilefinch_update_client_snapshot(
            session->client, &session->client_snapshot);
    } else if (session->auto_install
               && session->client_snapshot.phase
                      == TILEFINCH_UPDATE_CLIENT_DOWNLOADED) {
        (void) psp_glyph_component_session_primary(session, paths, engine);
    }
    return true;
}

bool psp_glyph_component_session_active(
    const PspGlyphComponentSession *session)
{
    if (session == NULL) return false;
    if (session->installer != NULL
        && session->install_snapshot.phase
               < TILEFINCH_UPDATE_INSTALL_COMPLETE) return true;
    return session->client != NULL
        && (session->client_snapshot.phase
                == TILEFINCH_UPDATE_CLIENT_CHECKING
            || session->client_snapshot.phase
                == TILEFINCH_UPDATE_CLIENT_DOWNLOADING
            || session->client_snapshot.phase
                == TILEFINCH_UPDATE_CLIENT_CANCELLING);
}

bool psp_glyph_component_session_remove(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    BrowserEngine *engine)
{
    if (session == NULL || !psp_glyph_component_session_installed(
            session, pack) || psp_glyph_component_session_active(session))
        return false;
    if ((session->attached_mask & pack_bit(pack)) != 0)
        deactivate_runtime(session, engine);
    bool removed = tilefinch_glyph_component_remove(paths, pack);
    if (removed) {
        clear_operation(session);
        psp_glyph_component_session_probe(session, paths);
    }
    return removed;
}
