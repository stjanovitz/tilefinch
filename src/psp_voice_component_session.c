#include "tilefinch/psp_voice_component_session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define VOICE_INSTALL_UNIT_BYTES (16u * 1024u)

#ifndef TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER
#define TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER "stjanovitz"
#endif
#ifndef TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME
#define TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME "tilefinch-models"
#endif

bool psp_voice_component_session_metadata_url(
    char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    int written = snprintf(
        output, capacity,
        "https://github.com/%s/%s/releases/latest/download/"
        "tilefinch-voice-en-us-v1.tfvm",
        TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER,
        TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME);
    return written > 0 && (size_t) written < capacity;
}
void psp_voice_component_session_probe(
    PspVoiceComponentSession *session, const TilefinchInstallPaths *paths)
{
    if (session == NULL) return;
    session->source = tilefinch_voice_component_resolve(
        paths, session->model_path, sizeof(session->model_path));
    session->installed = session->source != TILEFINCH_VOICE_COMPONENT_NONE;
}

bool psp_voice_component_session_initialize(
    PspVoiceComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths)
{
    if (session == NULL || budget == NULL || paths == NULL
        || !paths->slotted) return false;
    if (session->initialized) return session->available;
    session->budget = budget;
    psp_voice_component_session_probe(session, paths);
    if (!tilefinch_update_embedded_root(&session->root)) return false;
    char update_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_data_path(
            paths, "update", update_dir, sizeof(update_dir))
        || (mkdir(update_dir, 0777) != 0 && errno != EEXIST)
        || !tilefinch_install_data_path(
            paths, "update/voice-model.part", session->package_path,
            sizeof(session->package_path))) return false;
    uint64_t installed_sequence = 0;
    uint8_t installed_digest[32] = {0};
    bool have_installed_identity =
        tilefinch_voice_component_installed_identity(
            budget, paths, &session->root, &installed_sequence,
            installed_digest);
    session->client = tilefinch_update_client_create(
        budget, &(TilefinchUpdateClientOptions) {
            .embedded_root = &session->root,
            .installed_sequence = installed_sequence,
            .installed_package_sha256 = installed_digest,
            .installed_sequence_valid = have_installed_identity,
            .installed_pair_valid = have_installed_identity,
            .launcher_protocol = TILEFINCH_VOICE_COMPONENT_ABI,
            .repository_owner = TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER,
            .repository_name = TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME,
            .trust = TILEFINCH_UPDATE_TRUST_SIGNED,
            .package_part_path = session->package_path,
            .artifact = TILEFINCH_UPDATE_ARTIFACT_VOICE_COMPONENT
        });
    session->available = session->client != NULL;
    session->initialized = session->available;
    return session->available;
}

void psp_voice_component_session_destroy(PspVoiceComponentSession *session)
{
    if (session == NULL) return;
    tilefinch_voice_component_install_destroy(session->installer);
    tilefinch_update_client_destroy(session->client);
    memset(session, 0, sizeof(*session));
}

PspVoiceComponentPrimaryResult psp_voice_component_session_primary(
    PspVoiceComponentSession *session, const TilefinchInstallPaths *paths)
{
    if (session == NULL || !session->available || session->client == NULL)
        return PSP_VOICE_COMPONENT_PRIMARY_NONE;
    if (session->installer != NULL) {
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            tilefinch_voice_component_install_destroy(session->installer);
            session->installer = NULL;
            psp_voice_component_session_probe(session, paths);
            return PSP_VOICE_COMPONENT_PRIMARY_NONE;
        }
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_ERROR
            || session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_CANCELLED) {
            tilefinch_voice_component_install_destroy(session->installer);
            session->installer = NULL;
        } else {
            return PSP_VOICE_COMPONENT_PRIMARY_NONE;
        }
    }
    (void) tilefinch_update_client_snapshot(
        session->client, &session->client_snapshot);
    switch (session->client_snapshot.phase) {
        case TILEFINCH_UPDATE_CLIENT_IDLE:
        case TILEFINCH_UPDATE_CLIENT_ERROR:
        case TILEFINCH_UPDATE_CLIENT_UP_TO_DATE:
            session->auto_install = true;
            return PSP_VOICE_COMPONENT_PRIMARY_CHECK_REQUIRED;
        case TILEFINCH_UPDATE_CLIENT_AVAILABLE:
            session->auto_install = true;
            (void) tilefinch_update_client_begin_download(session->client);
            break;
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADED: {
            size_t envelope_length = 0;
            const uint8_t *envelope = tilefinch_update_client_envelope(
                session->client, &envelope_length);
            session->installer = tilefinch_voice_component_install_create(
                session->budget,
                &(TilefinchVoiceComponentInstallOptions) {
                    .package_path = session->package_path,
                    .envelope = envelope,
                    .envelope_length = envelope_length,
                    .manifest = &session->client_snapshot.manifest,
                    .manifest_digest =
                        session->client_snapshot.manifest_digest,
                    .install_root = paths->install_root
                });
            break;
        }
        default:
            break;
    }
    return PSP_VOICE_COMPONENT_PRIMARY_NONE;
}

bool psp_voice_component_session_begin_check(
    PspVoiceComponentSession *session, uint64_t now_unix, bool clock_valid)
{
    return session != NULL && session->client != NULL
        && tilefinch_update_client_begin_check(
               session->client, now_unix, clock_valid);
}

bool psp_voice_component_session_cancel(PspVoiceComponentSession *session)
{
    if (session == NULL) return false;
    return session->installer != NULL
        ? tilefinch_voice_component_install_cancel(session->installer)
        : tilefinch_update_client_cancel(session->client);
}

bool psp_voice_component_session_pump(
    PspVoiceComponentSession *session,
    const TilefinchInstallPaths *paths)
{
    if (session == NULL) return false;
    if (session->installer != NULL) {
        (void) tilefinch_voice_component_install_pump(
            session->installer, VOICE_INSTALL_UNIT_BYTES);
        (void) tilefinch_voice_component_install_snapshot(
            session->installer, &session->install_snapshot);
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            psp_voice_component_session_probe(session, paths);
            session->auto_install = false;
            tilefinch_voice_component_install_destroy(session->installer);
            session->installer = NULL;
        }
        return true;
    }
    if (session->client != NULL) {
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
            (void) psp_voice_component_session_primary(session, paths);
        }
        return true;
    }
    return false;
}

bool psp_voice_component_session_active(
    const PspVoiceComponentSession *session)
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

bool psp_voice_component_session_remove(
    PspVoiceComponentSession *session,
    const TilefinchInstallPaths *paths)
{
    if (session == NULL || session->source
            != TILEFINCH_VOICE_COMPONENT_SHARED
        || psp_voice_component_session_active(session)) return false;
    bool removed = tilefinch_voice_component_remove(paths);
    if (removed) {
        /* A removed component must be downloadable again at the same signed
           sequence. Retaining this client's installed-floor snapshot would
           report that release as UP TO DATE until the process restarted. */
        tilefinch_voice_component_install_destroy(session->installer);
        tilefinch_update_client_destroy(session->client);
        session->installer = NULL;
        session->client = NULL;
        session->initialized = false;
        session->available = false;
        session->auto_install = false;
        memset(&session->client_snapshot, 0,
               sizeof(session->client_snapshot));
        memset(&session->install_snapshot, 0,
               sizeof(session->install_snapshot));
        psp_voice_component_session_probe(session, paths);
    }
    return removed;
}

void psp_voice_component_session_refresh_ui(
    const PspVoiceComponentSession *session, PspUiState *ui)
{
    if (session == NULL || ui == NULL) return;
    PspUiVoiceComponentPhase phase = session->installed
        ? (session->source == TILEFINCH_VOICE_COMPONENT_LEGACY
               ? PSP_UI_VOICE_COMPONENT_LEGACY
               : PSP_UI_VOICE_COMPONENT_READY)
        : PSP_UI_VOICE_COMPONENT_NOT_INSTALLED;
    int progress = -1;
    if (session->installer != NULL) {
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_ERROR
            || session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_CANCELLED) {
            phase = PSP_UI_VOICE_COMPONENT_ERROR;
        } else if (session->install_snapshot.phase
                       < TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            phase = PSP_UI_VOICE_COMPONENT_INSTALLING;
            if (session->install_snapshot.bytes_total != 0)
                progress = (int) (session->install_snapshot.bytes_processed
                                  * 1000u
                                  / session->install_snapshot.bytes_total);
        }
    } else if (session->client != NULL) {
        switch (session->client_snapshot.phase) {
            case TILEFINCH_UPDATE_CLIENT_CHECKING:
                phase = PSP_UI_VOICE_COMPONENT_CHECKING;
                break;
            case TILEFINCH_UPDATE_CLIENT_DOWNLOADING:
            case TILEFINCH_UPDATE_CLIENT_CANCELLING:
                phase = PSP_UI_VOICE_COMPONENT_DOWNLOADING;
                if (session->client_snapshot.bytes_total != 0)
                    progress = (int)
                        (session->client_snapshot.bytes_received * 1000u
                         / session->client_snapshot.bytes_total);
                break;
            case TILEFINCH_UPDATE_CLIENT_ERROR:
                phase = PSP_UI_VOICE_COMPONENT_ERROR;
                break;
            default:
                break;
        }
    }
    psp_ui_set_voice_component(ui, phase, progress);
}
