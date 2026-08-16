#include "psp_app/psp_app_internal.h"

#include "psp_update_e2e.h"

#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)

void psp_update_e2e_init(
    PspUpdateE2E *driver, bool requested, bool trial_health_pending)
{
    if (driver == NULL) return;
    memset(driver, 0, sizeof(*driver));
    driver->requested = requested;
    driver->candidate_boot = requested && trial_health_pending;
}

bool psp_update_e2e_confirmed(
    PspUpdateE2E *driver, const TilefinchUpdateState *healthy,
    TilefinchUpdateSlot running_slot)
{
    if (driver == NULL || healthy == NULL || !driver->candidate_boot
        || driver->terminal || healthy->active_slot != running_slot
        || healthy->installed_sequence <= TILEFINCH_RELEASE_SEQUENCE) {
        return false;
    }
    driver->terminal = true;
    printf(
        "tilefinch-update-e2e: outcome=complete slot=%u sequence=%llu\n",
        (unsigned) running_slot,
        (unsigned long long) healthy->installed_sequence);
    psp_log_checkpoint("update-e2e-complete");
    return true;
}

static void psp_update_e2e_fail(
    PspUpdateE2E *driver, PspExitPlan *exit_plan,
    const char *stage, const PspUpdateSession *session)
{
    driver->terminal = true;
    printf(
        "tilefinch-update-e2e: outcome=failed stage=%s "
        "client-phase=%u status=%u install-phase=%u message=%.48s\n",
        stage,
        session == NULL ? 0u
            : (unsigned) session->client_snapshot.phase,
        session == NULL ? 0u
            : (unsigned) session->client_snapshot.status,
        session == NULL ? 0u
            : (unsigned) session->install_snapshot.phase,
        session == NULL ? "unavailable" : session->client_snapshot.message);
    psp_log_checkpoint("update-e2e-failed");
    psp_exit_plan_request(exit_plan, PSP_EXIT_VALIDATION_COMPLETE);
}

void psp_update_e2e_pump(
    PspUpdateE2E *driver, const char *trace,
    const char *metadata_url, const PspNetworkLifecycle *network_lifecycle,
    const PspNetwork *network, PspUpdateSession *update_session,
    Budget *budget, const TilefinchInstallPaths *install_paths,
    PspUiState *ui, PspExitPlan *exit_plan)
{
    if (driver == NULL || !driver->requested || driver->candidate_boot
        || driver->terminal || trace == NULL || metadata_url == NULL
        || network_lifecycle == NULL || network == NULL
        || update_session == NULL || budget == NULL || install_paths == NULL
        || ui == NULL || exit_plan == NULL) {
        return;
    }
    if (!driver->started && strcmp(trace, "none") == 0
        && psp_network_lifecycle_ready(network_lifecycle)
        && network->status == PSP_NETWORK_READY) {
        /* HOME may have spent the shared transport's one PSP operation slot
           on a speculative homepage connection in this same frame.  The
           foreground update owns the slot now; retiring speculation first is
           the same precedence rule ordinary navigation uses. */
        fetch_preconnect_cancel("update-e2e");
        bool initialized = psp_update_session_initialized(update_session)
            || psp_update_session_initialize(
                   update_session, budget, install_paths,
                   &(PspUpdateSessionOptions) {
                       .channel = BROWSER_UPDATE_CHANNEL_STABLE,
                       .signed_metadata_url_override = metadata_url
                   },
                   ui);
        time_t update_now = time(NULL);
        if (!initialized || update_now <= 0
            || !psp_update_session_begin_check(
                   update_session, (uint64_t) update_now, true)) {
            psp_update_session_refresh_ui(update_session, ui);
            psp_update_e2e_fail(
                driver, exit_plan, "begin", update_session);
            return;
        }
        driver->started = true;
        printf(
            "tilefinch-update-e2e: outcome=started url=%s\n",
            metadata_url);
    }
    if (!driver->started || driver->terminal) return;

    TilefinchUpdateClientPhase phase =
        update_session->client_snapshot.phase;
    bool install_failed = update_session->installer != NULL
        && update_session->install_snapshot.phase
               >= TILEFINCH_UPDATE_INSTALL_ERROR;
    if (phase == TILEFINCH_UPDATE_CLIENT_ERROR
        || phase == TILEFINCH_UPDATE_CLIENT_UP_TO_DATE || install_failed) {
        psp_update_e2e_fail(
            driver, exit_plan,
            install_failed ? "install" : "client", update_session);
        return;
    }
    PspUpdatePrimaryResult primary = psp_update_session_primary(
        update_session, install_paths);
    if (primary == PSP_UPDATE_PRIMARY_RESTART_REQUIRED) {
        printf(
            "tilefinch-update-e2e: outcome=installed action=cold-reboot\n");
        psp_log_checkpoint("update-e2e-restart");
        /* PPSSPP preserves argv[0] across a browser -> launcher -> candidate
           nested LoadExec chain. That makes a correctly launched slot B
           identify itself as slot A and withhold trial health. Leave the
           journal PENDING and let the isolated host harness perform the same
           cold launcher entry a real reboot provides. Production updates do
           not enable this validation driver and keep their in-app restart. */
        psp_exit_plan_request(
            exit_plan, PSP_EXIT_VALIDATION_COMPLETE);
    }
}

#endif
