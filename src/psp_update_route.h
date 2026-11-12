#ifndef TILEFINCH_PSP_UPDATE_ROUTE_H
#define TILEFINCH_PSP_UPDATE_ROUTE_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/install_paths.h"

/*
 * The recorded update route: a validation-only way to run the shipping
 * update session, client and installer on a PSP whose Wi-Fi does not work.
 *
 * The browser runs from host0: (PSPLink memory mode) with a response-keyed
 * trace that holds the signed metadata and the package
 * (scripts/make-update-replay.py). The trace's update-route.cfg names a
 * separate A/B install on the Memory Stick; the update session alone uses
 * that install's paths, as though the browser ran from its slot, so the
 * download, staging, journal writes and startup-copy preparation land
 * there exactly as they would in an XMB-launched browser. The profile and
 * logs stay on host0. psp_update_e2e.c owns the run; this file is only the
 * pure parser, so the host can test its refusals.
 *
 * update-route.cfg, one key per line ('#' comments and blank lines ignored):
 *   install_root=ms0:/PSP/GAME/<NAME>   a separate test install, never
 *                                       ms0:/PSP/GAME/TILEFINCH
 *   running_slot=a|b                    the slot the session acts as
 *   journal_only=1                      optional: only report the install
 *                                       (journal, file list) and exit,
 *                                       writing nothing on the stick
 * Any other key or value, a repeated key or a missing one refuses the
 * route.
 */

typedef struct {
    TilefinchInstallPaths paths;
    char running_slot;
    bool journal_only;
} PspUpdateRoute;

/* False with a short reason in `why` when the text does not describe an
   acceptable route; `route` is written only on success. */
bool psp_update_route_parse(const char *text, size_t length,
                            PspUpdateRoute *route,
                            char *why, size_t why_size);

#endif
