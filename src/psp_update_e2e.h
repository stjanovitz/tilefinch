#ifndef TILEFINCH_PSP_UPDATE_E2E_H
#define TILEFINCH_PSP_UPDATE_E2E_H

#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)

typedef struct {
    bool requested;
    bool candidate_boot;
    bool started;
    bool terminal;
} PspUpdateE2E;

void psp_update_e2e_init(
    PspUpdateE2E *driver, bool requested, bool trial_health_pending);
bool psp_update_e2e_confirmed(
    PspUpdateE2E *driver, const TilefinchUpdateState *healthy,
    TilefinchUpdateSlot running_slot);
void psp_update_e2e_pump(
    PspUpdateE2E *driver, const char *trace,
    const char *metadata_url, const PspNetworkLifecycle *network_lifecycle,
    const PspNetwork *network, PspUpdateSession *update_session,
    Budget *budget, const TilefinchInstallPaths *install_paths,
    PspUiState *ui, PspExitPlan *exit_plan);

#endif

#endif
