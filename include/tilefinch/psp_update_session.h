#ifndef TILEFINCH_PSP_UPDATE_SESSION_H
#define TILEFINCH_PSP_UPDATE_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/update.h"

typedef struct {
    Budget *budget;
    TilefinchUpdateRoot root;
    TilefinchUpdateState state;
    TilefinchUpdateClient *client;
    TilefinchUpdateInstallJob *installer;
    TilefinchUpdateHistory *history;
    TilefinchUpdateClientSnapshot client_snapshot;
    TilefinchUpdateInstallSnapshot install_snapshot;
    char package_path[TILEFINCH_INSTALL_PATH_LIMIT];
    char release_tag[16];
    uint64_t install_maximum_unit_us;
    uint64_t install_units;
    BrowserUpdateChannel channel;
    uint16_t installed_decoder_abi;
    char unavailable_message[64];
    bool initialized;
    bool available;
    bool install_terminal_reported;
    bool allow_downgrade;
    bool installed_decoder_abi_valid;
} PspUpdateSession;

typedef struct {
    BrowserUpdateChannel channel;
    /* Used only by DEVELOPER. Empty/NULL leaves that channel unavailable. */
    const char *developer_metadata_url;
    /* Optional direct TFUP URL. Empty retains metadata-relative lookup. */
    const char *developer_package_url;
    /*
     * Signed endpoint substitution for a validation caller. Signature,
     * sequence, package hash, installer, journal, and trial policy remain the
     * Stable path; only the HTTPS origin changes. Shipping callers leave it
     * NULL, and the PSP boot configuration can feed it only from code
     * compiled under TILEFINCH_PSP_VALIDATION_LOG.
     */
    const char *signed_metadata_url_override;
    /* Fixed, signed GitHub release selected from the native history list. */
    const char *release_tag;
    bool allow_downgrade;
} PspUpdateSessionOptions;

typedef enum {
    PSP_UPDATE_PRIMARY_NONE = 0,
    PSP_UPDATE_PRIMARY_CHECK_REQUIRED,
    PSP_UPDATE_PRIMARY_RESTART_REQUIRED
} PspUpdatePrimaryResult;

TilefinchUpdateSlot psp_update_session_current_slot(
    const TilefinchInstallPaths *paths);
bool psp_update_session_metadata_url(
    const PspUpdateSessionOptions *options, char *output, size_t capacity);
bool psp_update_session_selected_metadata_url(
    const PspUpdateSession *session, char *output, size_t capacity);

bool psp_update_session_initialize(
    PspUpdateSession *session, Budget *budget,
    const TilefinchInstallPaths *paths,
    const PspUpdateSessionOptions *options, PspUiState *ui);
void psp_update_session_destroy(PspUpdateSession *session);
void psp_update_session_refresh_ui(
    PspUpdateSession *session, PspUiState *ui);
void psp_update_session_pump(
    PspUpdateSession *session, PspUiState *ui);

bool psp_update_session_cancel(PspUpdateSession *session);
bool psp_update_session_active(const PspUpdateSession *session);
bool psp_update_session_available(const PspUpdateSession *session);
bool psp_update_session_initialized(const PspUpdateSession *session);

PspUpdatePrimaryResult psp_update_session_primary(
    PspUpdateSession *session, const TilefinchInstallPaths *paths);
bool psp_update_session_begin_check(
    PspUpdateSession *session, uint64_t wall_time_seconds,
    bool wall_time_valid);
bool psp_update_session_begin_history(
    PspUpdateSession *session, PspUiState *ui);
void psp_update_session_cancel_history(
    PspUpdateSession *session, PspUiState *ui);

#endif
