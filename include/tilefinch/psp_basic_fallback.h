#ifndef TILEFINCH_PSP_BASIC_FALLBACK_H
#define TILEFINCH_PSP_BASIC_FALLBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"

/*
 * The PSP frontend's Basic view fallback policy (docs/BASIC_VIEW.md), kept
 * free of firmware and engine state so every setting and trigger is a host
 * test. The engine decides whether a page qualifies
 * (browser_engine_prepare_basic_view_recovery: scripts stopped or failed,
 * author work settled, the page blank or without a usable action); these
 * functions decide what the browser does with that answer.
 */
typedef enum {
    /* Nothing to do: the raw page stays as it is. */
    PSP_BASIC_FALLBACK_KEEP_PAGE = 0,
    /* Live author work may still reveal the page; ask again later. */
    PSP_BASIC_FALLBACK_DEFER,
    /* Present the prepared Basic view now (Automatic). */
    PSP_BASIC_FALLBACK_SWITCH,
    /* Keep the raw page and offer the prepared Basic view (Ask). */
    PSP_BASIC_FALLBACK_ASK,
    /* The page qualified but Basic could not be prepared. */
    PSP_BASIC_FALLBACK_UNAVAILABLE
} PspBasicFallbackDecision;

/* Whether the automatic seam may consult the engine at all. Off never
   prepares a Basic tree by itself, and an explicit Reader request (manual or
   per-site) owns the page's one extracted-view slot. */
bool psp_basic_fallback_consulted(BrowserBasicFallbackMode mode,
                                  bool explicit_reader_request);

PspBasicFallbackDecision psp_basic_fallback_decide(
    BrowserBasicFallbackMode mode, BrowserBasicViewRecovery recovery);

/* After a page runtime turn: whether a committed page whose scripts have
   failed (the turn failed, or the page's realm is retired) should be
   re-examined for the fallback. Pages are otherwise examined only at
   commit; this happens at most once per navigation (failure_examined). */
bool psp_basic_fallback_recheck(BrowserBasicFallbackMode mode,
                                bool extracted_view_active,
                                bool recovery_pending, bool runtime_ok,
                                bool realm_retired, bool failure_examined);

/* The Basic choices a recovery prompt adds (PSP_UI_FAILURE_* bits):
   "Show Basic view" while the failed page's DOM exists and a Basic tree is
   prepared or preparable for it, otherwise "Reload in Basic view" when a
   navigation failed after its document arrived. Off offers neither. */
uint8_t psp_basic_fallback_failure_actions(BrowserBasicFallbackMode mode,
                                           bool basic_available,
                                           bool basic_active,
                                           bool document_failed_after_arrival);

/* A failed navigation whose document arrived (2xx) may succeed as a
   script-free load; transport and HTTP failures cannot (a cancelled one
   offers no recovery at all). */
bool psp_basic_fallback_navigation_reloadable(long http_status,
                                              bool internal_url);

#endif
