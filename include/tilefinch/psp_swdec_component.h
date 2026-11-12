#ifndef TILEFINCH_PSP_SWDEC_COMPONENT_H
#define TILEFINCH_PSP_SWDEC_COMPONENT_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/swdec_component.h"
#include "tilefinch/swdec_component_store.h"

typedef enum {
    PSP_SWDEC_COMPONENT_COLD = 0,
    PSP_SWDEC_COMPONENT_LOADED,
    PSP_SWDEC_COMPONENT_ATTACHED,
    PSP_SWDEC_COMPONENT_RESTORED,
    PSP_SWDEC_COMPONENT_FAILED
} PspSwdecComponentState;

typedef TilefinchSwdecFailure PspSwdecComponentFailure;
#define PSP_SWDEC_COMPONENT_FAILURE_NONE TILEFINCH_SWDEC_FAILURE_NONE
#define PSP_SWDEC_COMPONENT_FAILURE_MISSING TILEFINCH_SWDEC_FAILURE_MISSING
#define PSP_SWDEC_COMPONENT_FAILURE_REBUILD TILEFINCH_SWDEC_FAILURE_REBUILD
#define PSP_SWDEC_COMPONENT_FAILURE_RUNTIME TILEFINCH_SWDEC_FAILURE_RUNTIME

/*
 * Process-lifetime owner for the optional software decoder.
 *
 * The state below owns only lifecycle facts; media-session policy decides when
 * a route needs the component. `took_me` is deliberately monotonic between
 * suspend cycles: after the first successful attach, ordinary firmware AVC
 * must not be used until restore has completed. The user PRX may be unloaded
 * only after a successful ME restore; a partial takeover retains the module
 * rather than freeing executable memory still in use.
 */
typedef struct {
    Budget *budget;
    const TilefinchInstallPaths *install_paths;
    TilefinchSwdecComponentApi api;
    int module_id;
    int last_native_error;
    PspSwdecComponentState state;
    PspSwdecComponentFailure failure;
    /* Failed loads and starts since the last successful load
       (tilefinch_swdec_component_retry_allowed). */
    unsigned runtime_failures;
    bool module_started;
    bool took_me;
    bool shared_install;
} PspSwdecComponent;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation builds only: TILEFINCH_SWDEC_VALIDATION_* flags (boot.cfg
   validation_swdec_reference) for the component's start block, set at boot
   before the component loads. */
void psp_swdec_component_set_validation_flags(uint32_t flags);
/* The steady window (swdec_window.h) opened or closed: advance the mark the
   component's start block points at. */
void psp_swdec_component_window_step(void);
#endif
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
