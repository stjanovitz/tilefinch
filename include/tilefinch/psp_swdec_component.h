#ifndef TILEFINCH_PSP_SWDEC_COMPONENT_H
#define TILEFINCH_PSP_SWDEC_COMPONENT_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/swdec_component.h"

typedef enum {
    PSP_SWDEC_COMPONENT_COLD = 0,
    PSP_SWDEC_COMPONENT_LOADED,
    PSP_SWDEC_COMPONENT_ATTACHED,
    PSP_SWDEC_COMPONENT_RESTORED,
    PSP_SWDEC_COMPONENT_FAILED
} PspSwdecComponentState;

typedef enum {
    PSP_SWDEC_COMPONENT_FAILURE_NONE = 0,
    PSP_SWDEC_COMPONENT_FAILURE_MISSING,
    PSP_SWDEC_COMPONENT_FAILURE_REBUILD,
    PSP_SWDEC_COMPONENT_FAILURE_RUNTIME
} PspSwdecComponentFailure;

/*
 * Process-lifetime owner for the optional software decoder.
 *
 * The user PRX and its small kernel helper remain resident once loaded. The
 * state below owns only lifecycle facts; media-session policy decides when a
 * route needs the component. `took_me` is deliberately monotonic between
 * suspend cycles: after the first successful attach, ordinary firmware AVC
 * must not be used until restore has completed.
 */
typedef struct {
    Budget *budget;
    const TilefinchInstallPaths *install_paths;
    BudgetReservation resident_reservation;
    TilefinchSwdecComponentApi api;
    int module_id;
    int last_native_error;
    PspSwdecComponentState state;
    PspSwdecComponentFailure failure;
    bool took_me;
    bool shared_install;
} PspSwdecComponent;

void psp_swdec_component_init(
    PspSwdecComponent *component, Budget *budget,
    const TilefinchInstallPaths *install_paths);
bool psp_swdec_component_prepare(
    PspSwdecComponent *component, char *error, size_t error_size);
bool psp_swdec_component_suspend(
    PspSwdecComponent *component, char *error, size_t error_size);
bool psp_swdec_component_resume(
    PspSwdecComponent *component, char *error, size_t error_size);
void psp_swdec_component_shutdown(PspSwdecComponent *component);
bool psp_swdec_component_owns_me(const PspSwdecComponent *component);
bool psp_swdec_component_attached(const PspSwdecComponent *component);
const TilefinchSwdecComponentApi *psp_swdec_component_api(
    const PspSwdecComponent *component);

#endif
