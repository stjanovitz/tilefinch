#include "tilefinch/psp_update_session.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tilefinch/build_version.h"
#include "tilefinch/platform.h"
#include "tilefinch/swdec_component_store.h"

#define PSP_UPDATE_INSTALL_PUMP_BUDGET_US UINT64_C(2000)
#define PSP_UPDATE_INSTALL_MAXIMUM_UNITS 4u
#define PSP_UPDATE_INSTALL_UNIT_BYTES (16u * 1024u)

#ifndef TILEFINCH_UPDATE_REPOSITORY_OWNER
#define TILEFINCH_UPDATE_REPOSITORY_OWNER "stjanovitz"
#endif
#ifndef TILEFINCH_UPDATE_REPOSITORY_NAME
#define TILEFINCH_UPDATE_REPOSITORY_NAME "tilefinch"
#endif

static void psp_update_session_log(const char *format, ...)
{
    char message[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    tilefinch_platform_log_message(message);
}

TilefinchUpdateSlot psp_update_session_current_slot(
    const TilefinchInstallPaths *paths)
{
    if (paths != NULL && strcmp(paths->slot_name, "slot-a") == 0)
        return TILEFINCH_UPDATE_SLOT_A;
    if (paths != NULL && strcmp(paths->slot_name, "slot-b") == 0)
        return TILEFINCH_UPDATE_SLOT_B;
    return TILEFINCH_UPDATE_SLOT_NONE;
}

static bool psp_update_hash_is_nonzero(const uint8_t digest[32])
{
    uint8_t aggregate = 0;
    for (size_t index = 0; index < 32; index++)
        aggregate |= digest[index];
    return aggregate != 0;
}

static bool psp_update_release_tag_is_valid(const char *tag)
{
    if (tag == NULL || tag[0] != 'v') return false;
    size_t length = strlen(tag);
    if (length < 2u || length >= sizeof(((PspUpdateSession *) 0)->release_tag))
        return false;
    for (size_t index = 1; index < length; index++) {
        unsigned character = (unsigned char) tag[index];
        if (!((character >= '0' && character <= '9') || character == '.'))
            return false;
    }
    return true;
}

bool psp_update_session_metadata_url(
    const PspUpdateSessionOptions *options, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    BrowserUpdateChannel channel = options == NULL
        ? BROWSER_UPDATE_CHANNEL_STABLE : options->channel;
    int written = 0;
    if (channel == BROWSER_UPDATE_CHANNEL_STABLE
        && options != NULL
        && options->signed_metadata_url_override != NULL
        && options->signed_metadata_url_override[0] != '\0') {
        return tilefinch_update_prepare_download_url(
            options->signed_metadata_url_override, output, capacity);
    } else if (channel == BROWSER_UPDATE_CHANNEL_STABLE
               && options != NULL && options->release_tag != NULL) {
        if (!psp_update_release_tag_is_valid(options->release_tag))
            return false;
        written = snprintf(
            output, capacity,
            "https://github.com/%s/%s/releases/download/%s/"
            "tilefinch-update-v1.tfum",
            TILEFINCH_UPDATE_REPOSITORY_OWNER,
            TILEFINCH_UPDATE_REPOSITORY_NAME, options->release_tag);
    } else if (channel == BROWSER_UPDATE_CHANNEL_DEVELOPER) {
        const char *url = options == NULL
            ? NULL : options->developer_metadata_url;
        return tilefinch_update_prepare_download_url(
            url, output, capacity);
    } else if (channel == BROWSER_UPDATE_CHANNEL_BETA) {
        written = snprintf(
            output, capacity,
            "https://github.com/%s/%s/releases/download/beta/"
            "tilefinch-update-v1.tfum",
            TILEFINCH_UPDATE_REPOSITORY_OWNER,
            TILEFINCH_UPDATE_REPOSITORY_NAME);
    } else if (channel == BROWSER_UPDATE_CHANNEL_STABLE) {
        written = snprintf(
            output, capacity,
            "https://github.com/%s/%s/releases/latest/download/"
            "tilefinch-update-v1.tfum",
            TILEFINCH_UPDATE_REPOSITORY_OWNER,
            TILEFINCH_UPDATE_REPOSITORY_NAME);
    } else {
        return false;
    }
    return written > 0 && (size_t) written < capacity;
}

bool psp_update_session_selected_metadata_url(
    const PspUpdateSession *session, char *output, size_t capacity)
{
    if (session == NULL) return false;
    return psp_update_session_metadata_url(
        &(PspUpdateSessionOptions) {
            .channel = session->channel,
            .release_tag = session->release_tag[0] == '\0'
                ? NULL : session->release_tag
        }, output, capacity);
}

void psp_update_session_refresh_ui(
    PspUpdateSession *update, PspUiState *ui)
{
    if (update == NULL || ui == NULL) return;
    if (!update->available) {
        psp_ui_set_update(
            ui, TILEFINCH_VERSION_STRING,
            update->unavailable_message[0] == '\0'
                ? "SIGNED UPDATES ARE NOT CONFIGURED"
                : update->unavailable_message,
            "", -1, "", false, false);
        return;
    }
    if (update->installer != NULL) {
        (void) tilefinch_update_install_snapshot(
            update->installer, &update->install_snapshot);
        TilefinchUpdateInstallSnapshot *snapshot =
            &update->install_snapshot;
        int progress = -1;
        if (snapshot->phase == TILEFINCH_UPDATE_INSTALL_EXTRACTING
            && snapshot->bytes_total != 0) {
            progress = (int) (snapshot->bytes_processed * 1000u
                              / snapshot->bytes_total);
        }
        bool complete =
            snapshot->phase == TILEFINCH_UPDATE_INSTALL_COMPLETE;
        bool terminal = complete
            || snapshot->phase == TILEFINCH_UPDATE_INSTALL_ERROR
            || snapshot->phase == TILEFINCH_UPDATE_INSTALL_CANCELLED;
        psp_ui_set_update(
            ui, TILEFINCH_VERSION_STRING, snapshot->message, "", progress,
            complete ? "RESTART TO UPDATE"
                     : terminal ? "TRY INSTALL AGAIN" : "",
            complete || terminal,
            !terminal
                && snapshot->phase < TILEFINCH_UPDATE_INSTALL_PROMOTING);
        return;
    }
    (void) tilefinch_update_client_snapshot(
        update->client, &update->client_snapshot);
    TilefinchUpdateClientSnapshot *snapshot = &update->client_snapshot;
    int progress = -1;
    if (snapshot->phase == TILEFINCH_UPDATE_CLIENT_DOWNLOADING
        && snapshot->bytes_total != 0) {
        progress = (int) (snapshot->bytes_received * 1000u
                          / snapshot->bytes_total);
    }
    char status[64];
    snprintf(status, sizeof(status), "%s", snapshot->message);
    if (snapshot->phase == TILEFINCH_UPDATE_CLIENT_IDLE) {
        if (update->allow_downgrade && update->release_tag[0] != '\0') {
            snprintf(status, sizeof(status), "OLDER %s - SIGNED",
                     update->release_tag + 1u);
        } else if (update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER) {
            snprintf(status, sizeof(status),
                     "DEVELOPER URL - UNSIGNED TRIAL");
        } else {
            snprintf(status, sizeof(status), "%s CHANNEL - SIGNED",
                     update->channel == BROWSER_UPDATE_CHANNEL_BETA
                         ? "BETA" : "STABLE");
        }
    }
    if (snapshot->phase == TILEFINCH_UPDATE_CLIENT_AVAILABLE) {
        uint64_t tenths_mib =
            (snapshot->manifest.package_size * 10u
             + (UINT64_C(1024) * 1024u / 2u))
            / (UINT64_C(1024) * 1024u);
        snprintf(
            status, sizeof(status),
            update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
                ? "UNSIGNED %.14s  %llu.%llu MB"
                : "%.23s AVAILABLE  %llu.%llu MB",
            snapshot->manifest.version,
            (unsigned long long) (tenths_mib / 10u),
            (unsigned long long) (tenths_mib % 10u));
    }
    const char *label = "";
    bool enabled = false;
    bool cancellable = false;
    switch (snapshot->phase) {
        case TILEFINCH_UPDATE_CLIENT_IDLE:
            label = "CHECK NOW";
            enabled = true;
            break;
        case TILEFINCH_UPDATE_CLIENT_AVAILABLE:
            label = "DOWNLOAD UPDATE";
            enabled = true;
            break;
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADED:
            label = "INSTALL UPDATE";
            enabled = true;
            break;
        case TILEFINCH_UPDATE_CLIENT_UP_TO_DATE:
        case TILEFINCH_UPDATE_CLIENT_ERROR:
            label = "CHECK AGAIN";
            enabled = true;
            break;
        case TILEFINCH_UPDATE_CLIENT_CHECKING:
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADING:
        case TILEFINCH_UPDATE_CLIENT_CANCELLING:
            cancellable =
                snapshot->phase != TILEFINCH_UPDATE_CLIENT_CANCELLING;
            break;
    }
    const char *notes =
        snapshot->phase == TILEFINCH_UPDATE_CLIENT_AVAILABLE
            || snapshot->phase == TILEFINCH_UPDATE_CLIENT_DOWNLOADED
        ? snapshot->manifest.notes : "";
    if (update->installed_decoder_abi_valid
        && snapshot->manifest.optional_decoder_abi_valid
        && update->installed_decoder_abi
               != snapshot->manifest.optional_decoder_abi)
        notes = "Decoder rebuild needed";
    psp_ui_set_update(
        ui, TILEFINCH_VERSION_STRING, status,
        notes,
        progress,
        label, enabled, cancellable);
}

bool psp_update_session_initialize(
    PspUpdateSession *update, Budget *budget,
    const TilefinchInstallPaths *paths,
    const PspUpdateSessionOptions *session_options, PspUiState *ui)
{
    if (update == NULL || budget == NULL || paths == NULL || ui == NULL)
        return false;
    if (update->initialized) return update->available;
    update->installed_decoder_abi_valid =
        tilefinch_swdec_component_info_read(
            paths, &update->installed_decoder_abi)
        == TILEFINCH_SWDEC_COMPONENT_INFO_VALID;
    update->initialized = true;
    update->budget = budget;
    update->channel = session_options == NULL
        ? BROWSER_UPDATE_CHANNEL_STABLE : session_options->channel;
    if (session_options != NULL && session_options->release_tag != NULL) {
        if (update->channel != BROWSER_UPDATE_CHANNEL_STABLE
            || !session_options->allow_downgrade
            || !psp_update_release_tag_is_valid(
                   session_options->release_tag)) {
            snprintf(update->unavailable_message,
                     sizeof(update->unavailable_message),
                     "OLDER VERSION SELECTION IS INVALID");
            psp_update_session_refresh_ui(update, ui);
            return false;
        }
        snprintf(update->release_tag, sizeof(update->release_tag), "%s",
                 session_options->release_tag);
        update->allow_downgrade = true;
    }
    if (!paths->slotted) {
        psp_update_session_refresh_ui(update, ui);
        return false;
    }
    char update_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_data_path(
            paths, "update", update_dir, sizeof(update_dir))
        || (mkdir(update_dir, 0777) != 0 && errno != EEXIST)
        || !tilefinch_install_data_path(
            paths, "update/package.part",
            update->package_path, sizeof(update->package_path))) {
        psp_update_session_refresh_ui(update, ui);
        return false;
    }
    uint64_t available = 0;
    bool free_space_ok =
        tilefinch_update_query_free_space(update_dir, &available);
    psp_update_session_log(
        "tilefinch-update-storage: statvfs=%s available=%llu "
        "directory=%s\n",
        free_space_ok ? "ok" : "failed",
        (unsigned long long) available, update_dir);
    /* Read the journal before the trust root so that the part-file sweep
       below runs even on a build with no configured release root. */
    if (!tilefinch_update_journal_load(
            paths->data_dir, &update->state, NULL)) {
        memset(&update->state, 0, sizeof(update->state));
        update->state.generation = 1;
        update->state.active_slot =
            psp_update_session_current_slot(paths);
        update->state.installed_sequence = TILEFINCH_RELEASE_SEQUENCE;
    }
    if (update->state.trial == TILEFINCH_UPDATE_TRIAL_NONE) {
        /*
         * No trial is in flight, so any package.part left on the stick is
         * garbage: a cancelled or crashed download, or an install that
         * completed but was cut before it reclaimed its own part file. The
         * client cannot resume a part, so nothing else will ever read it.
         */
        (void) remove(update->package_path);
    }
    char selected_metadata_url[768];
    const char *metadata_url = NULL;
    const char *package_url = NULL;
    bool package_relative = false;
    if (update->channel == BROWSER_UPDATE_CHANNEL_STABLE
        && session_options != NULL
        && session_options->signed_metadata_url_override != NULL
        && session_options->signed_metadata_url_override[0] != '\0') {
        if (!tilefinch_update_prepare_download_url(
                session_options->signed_metadata_url_override,
                selected_metadata_url, sizeof(selected_metadata_url))) {
            snprintf(update->unavailable_message,
                     sizeof(update->unavailable_message),
                     "SIGNED TEST UPDATE URL IS INVALID");
            psp_update_session_refresh_ui(update, ui);
            return false;
        }
        metadata_url = selected_metadata_url;
        package_relative = true;
    } else if (update->channel != BROWSER_UPDATE_CHANNEL_STABLE) {
        if (!psp_update_session_metadata_url(
                session_options, selected_metadata_url,
                sizeof(selected_metadata_url))) {
            snprintf(update->unavailable_message,
                     sizeof(update->unavailable_message),
                     update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
                         ? "SET DEVELOPER URL IN BOOT CONFIG"
                         : "BETA UPDATE URL IS INVALID");
            psp_update_session_refresh_ui(update, ui);
            return false;
        }
        metadata_url = selected_metadata_url;
        if (update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER) {
            package_url = session_options != NULL
                    && session_options->developer_package_url != NULL
                    && session_options->developer_package_url[0] != '\0'
                ? session_options->developer_package_url : NULL;
            package_relative = package_url == NULL;
        }
    }
    bool developer_unsigned =
        update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER;
    bool have_root = tilefinch_update_embedded_root(&update->root);
    if (!developer_unsigned && !have_root) {
        psp_update_session_refresh_ui(update, ui);
        return false;
    }
    bool pair_valid =
        psp_update_hash_is_nonzero(update->state.installed_sha256);
    TilefinchUpdateClientOptions options = {
        .embedded_root = have_root ? &update->root : NULL,
        .installed_sequence = update->state.installed_sequence,
        .installed_package_sha256 = update->state.installed_sha256,
        .installed_sequence_valid =
            update->state.installed_sequence != 0,
        .installed_pair_valid = pair_valid,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .repository_owner = TILEFINCH_UPDATE_REPOSITORY_OWNER,
        .repository_name = TILEFINCH_UPDATE_REPOSITORY_NAME,
        .release_tag = update->release_tag[0] == '\0'
            ? NULL : update->release_tag,
        .metadata_url_override = metadata_url,
        .package_url_override = package_url,
        .package_relative_to_metadata = package_relative,
        .allow_downgrade = update->allow_downgrade,
        .trust = developer_unsigned
            ? TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            : TILEFINCH_UPDATE_TRUST_SIGNED,
        .package_part_path = update->package_path
    };
    update->client = tilefinch_update_client_create(budget, &options);
    update->available = update->client != NULL;
    psp_update_session_refresh_ui(update, ui);
    return update->available;
}

void psp_update_session_destroy(PspUpdateSession *update)
{
    if (update == NULL) return;
    tilefinch_update_history_destroy(update->history);
    tilefinch_update_install_destroy(update->installer);
    tilefinch_update_client_destroy(update->client);
    memset(update, 0, sizeof(*update));
}

void psp_update_session_pump(
    PspUpdateSession *update, PspUiState *ui)
{
    if (update == NULL || !update->available) return;
    if (update->history != NULL) {
        (void) tilefinch_update_history_pump(update->history, 2000u);
        TilefinchUpdateHistorySnapshot history_snapshot;
        if (tilefinch_update_history_snapshot(
                update->history, &history_snapshot)) {
            psp_ui_set_update_history(ui, &history_snapshot);
            if (history_snapshot.phase != TILEFINCH_UPDATE_HISTORY_LOADING) {
                tilefinch_update_history_destroy(update->history);
                update->history = NULL;
            }
        }
        return;
    }
    if (update->installer != NULL) {
        uint64_t batch_started_us =
            tilefinch_platform_monotonic_time_us();
        for (unsigned unit = 0;
             unit < PSP_UPDATE_INSTALL_MAXIMUM_UNITS; unit++) {
            uint64_t unit_started_us =
                tilefinch_platform_monotonic_time_us();
            if (!tilefinch_update_install_pump(
                    update->installer,
                    PSP_UPDATE_INSTALL_UNIT_BYTES)) {
                break;
            }
            uint64_t unit_us =
                tilefinch_platform_monotonic_time_us()
                - unit_started_us;
            update->install_units++;
            if (unit_us > update->install_maximum_unit_us)
                update->install_maximum_unit_us = unit_us;
            if (!tilefinch_update_install_snapshot(
                    update->installer, &update->install_snapshot)
                || update->install_snapshot.phase
                       >= TILEFINCH_UPDATE_INSTALL_COMPLETE
                || tilefinch_platform_monotonic_time_ns()
                       / UINT64_C(1000)
                       - batch_started_us
                       >= PSP_UPDATE_INSTALL_PUMP_BUDGET_US) {
                break;
            }
        }
        if (!update->install_terminal_reported
            && tilefinch_update_install_snapshot(
                   update->installer, &update->install_snapshot)
            && update->install_snapshot.phase
                   >= TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            update->install_terminal_reported = true;
            psp_update_session_log(
                "tilefinch-update-install: units=%llu "
                "maximum_unit_us=%llu terminal_phase=%u\n",
                (unsigned long long) update->install_units,
                (unsigned long long) update->install_maximum_unit_us,
                (unsigned) update->install_snapshot.phase);
        }
    } else {
        (void) tilefinch_update_client_pump(update->client, 2000u);
    }
    psp_update_session_refresh_ui(update, ui);
}

static bool psp_update_session_begin_install(
    PspUpdateSession *update, const TilefinchInstallPaths *paths)
{
    if (update == NULL || paths == NULL || update->installer != NULL
        || update->client_snapshot.phase
               != TILEFINCH_UPDATE_CLIENT_DOWNLOADED) return false;
    size_t envelope_length = 0;
    const uint8_t *envelope = tilefinch_update_client_envelope(
        update->client, &envelope_length);
    TilefinchUpdateSlot inactive =
        update->state.active_slot == TILEFINCH_UPDATE_SLOT_A
        ? TILEFINCH_UPDATE_SLOT_B : TILEFINCH_UPDATE_SLOT_A;
    TilefinchUpdateInstallOptions options = {
        .package_path = update->package_path,
        .envelope = envelope,
        .envelope_length = envelope_length,
        .manifest = &update->client_snapshot.manifest,
        .manifest_digest = update->client_snapshot.manifest_digest,
        .install_root = paths->install_root,
        .data_dir = paths->data_dir,
        .inactive_slot = inactive,
        .current_state = update->state,
        .trust = update->channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
            ? TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            : TILEFINCH_UPDATE_TRUST_SIGNED,
        .allow_downgrade = update->allow_downgrade
    };
    update->install_maximum_unit_us = 0;
    update->install_units = 0;
    update->install_terminal_reported = false;
    update->installer = tilefinch_update_install_create(
        update->budget, &options);
    return update->installer != NULL;
}

bool psp_update_session_cancel(PspUpdateSession *session)
{
    if (session == NULL) return false;
    return session->installer != NULL
        ? tilefinch_update_install_cancel(session->installer)
        : tilefinch_update_client_cancel(session->client);
}

bool psp_update_session_active(const PspUpdateSession *session)
{
    if (session == NULL || !session->available) return false;
    return session->history != NULL || session->installer != NULL
        || session->client_snapshot.phase
               == TILEFINCH_UPDATE_CLIENT_CHECKING
        || session->client_snapshot.phase
               == TILEFINCH_UPDATE_CLIENT_DOWNLOADING
        || session->client_snapshot.phase
               == TILEFINCH_UPDATE_CLIENT_CANCELLING;
}

bool psp_update_session_available(const PspUpdateSession *session)
{
    return session != NULL && session->available;
}

bool psp_update_session_initialized(const PspUpdateSession *session)
{
    return session != NULL && session->initialized;
}

PspUpdatePrimaryResult psp_update_session_primary(
    PspUpdateSession *session, const TilefinchInstallPaths *paths)
{
    if (session == NULL || !session->available)
        return PSP_UPDATE_PRIMARY_NONE;
    if (session->installer != NULL) {
        (void) tilefinch_update_install_snapshot(
            session->installer, &session->install_snapshot);
        if (session->install_snapshot.phase
                == TILEFINCH_UPDATE_INSTALL_COMPLETE)
            return PSP_UPDATE_PRIMARY_RESTART_REQUIRED;
        if (session->install_snapshot.phase
                >= TILEFINCH_UPDATE_INSTALL_ERROR) {
            tilefinch_update_install_destroy(session->installer);
            session->installer = NULL;
            (void) psp_update_session_begin_install(session, paths);
        }
        return PSP_UPDATE_PRIMARY_NONE;
    }

    (void) tilefinch_update_client_snapshot(
        session->client, &session->client_snapshot);
    switch (session->client_snapshot.phase) {
        case TILEFINCH_UPDATE_CLIENT_IDLE:
        case TILEFINCH_UPDATE_CLIENT_ERROR:
        case TILEFINCH_UPDATE_CLIENT_UP_TO_DATE:
            return PSP_UPDATE_PRIMARY_CHECK_REQUIRED;
        case TILEFINCH_UPDATE_CLIENT_AVAILABLE:
            (void) tilefinch_update_client_begin_download(session->client);
            break;
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADED:
            (void) psp_update_session_begin_install(session, paths);
            break;
        case TILEFINCH_UPDATE_CLIENT_CHECKING:
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADING:
        case TILEFINCH_UPDATE_CLIENT_CANCELLING:
            break;
    }
    return PSP_UPDATE_PRIMARY_NONE;
}

bool psp_update_session_begin_check(
    PspUpdateSession *session, uint64_t wall_time_seconds,
    bool wall_time_valid)
{
    return session != NULL && session->client != NULL
        && tilefinch_update_client_begin_check(
            session->client, wall_time_seconds, wall_time_valid);
}

bool psp_update_session_begin_history(
    PspUpdateSession *session, PspUiState *ui)
{
    if (session == NULL || ui == NULL || !session->available
        || session->channel != BROWSER_UPDATE_CHANNEL_STABLE
        || psp_update_session_active(session)) return false;
    session->history = tilefinch_update_history_create(
        session->budget, TILEFINCH_UPDATE_REPOSITORY_OWNER,
        TILEFINCH_UPDATE_REPOSITORY_NAME, TILEFINCH_VERSION_STRING);
    if (session->history == NULL) {
        TilefinchUpdateHistorySnapshot failed = {
            .phase = TILEFINCH_UPDATE_HISTORY_ERROR
        };
        psp_ui_set_update_history(ui, &failed);
        return false;
    }
    bool started = tilefinch_update_history_begin(session->history);
    TilefinchUpdateHistorySnapshot snapshot;
    if (tilefinch_update_history_snapshot(session->history, &snapshot))
        psp_ui_set_update_history(ui, &snapshot);
    if (!started) {
        tilefinch_update_history_destroy(session->history);
        session->history = NULL;
    }
    return started;
}

void psp_update_session_cancel_history(
    PspUpdateSession *session, PspUiState *ui)
{
    if (session == NULL) return;
    if (session->history != NULL) {
        (void) tilefinch_update_history_cancel(session->history);
        tilefinch_update_history_destroy(session->history);
        session->history = NULL;
    }
    if (ui != NULL) psp_update_session_refresh_ui(session, ui);
}
