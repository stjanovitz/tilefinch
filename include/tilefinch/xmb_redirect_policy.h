#ifndef TILEFINCH_XMB_REDIRECT_POLICY_H
#define TILEFINCH_XMB_REDIRECT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define TILEFINCH_XMB_BROWSER_MODULE "htmlviewer_plugin_module"

/*
 * Pure decision seam shared by the host test and the optional VSH plugin.
 * A controller read failure or a module load without a contemporaneous XMB
 * confirmation press is deliberately fail-open: Sony's browser keeps
 * starting rather than turning an incidental startup load into a handoff.
 */
bool tilefinch_xmb_redirect_should_attempt(
    const char *module_name,
    bool controller_sample_valid,
    uint32_t buttons,
    uint32_t activation_buttons,
    uint32_t bypass_button,
    bool launch_already_armed);

/* Loader-callback-safe half of the decision. Controller I/O is deliberately
   left to the worker which runs after the callback chain has returned. */
bool tilefinch_xmb_redirect_module_matches(
    const char *module_name, bool launch_already_armed);

#endif
