#ifndef TILEFINCH_PSP_MULTIPLAYER_H
#define TILEFINCH_PSP_MULTIPLAYER_H

#include "psp_app/psp_app_internal.h"

#include <stdbool.h>

bool psp_multiplayer_bind(
    PspProcessResources *process, PspBrowserResources *browser,
    PspNetwork *network, PspNetworkLifecycle *network_lifecycle);
void psp_multiplayer_pump(PspApp *app);
bool psp_multiplayer_shutdown(PspApp *app);
/* Atomic request/observation only; safe at the power-callback boundary.
   The browser pump retires the physical worker before reporting quiescence;
   a live realm may still own its queued Close notification. */
void psp_multiplayer_network_request_quiesce(void);
bool psp_multiplayer_network_quiesced(void);

#endif
