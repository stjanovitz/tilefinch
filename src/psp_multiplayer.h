#ifndef TILEFINCH_PSP_MULTIPLAYER_H
#define TILEFINCH_PSP_MULTIPLAYER_H

#include "psp_app/psp_app_internal.h"

#include <stdbool.h>

bool psp_multiplayer_bind(
    PspProcessResources *process, PspBrowserResources *browser,
    PspNetwork *network, PspNetworkLifecycle *network_lifecycle);
void psp_multiplayer_pump(PspApp *app);
void psp_multiplayer_shutdown(PspApp *app);

#endif
