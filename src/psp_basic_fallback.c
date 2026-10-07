#include "tilefinch/psp_basic_fallback.h"

#include "tilefinch/psp_ui.h"

bool psp_basic_fallback_consulted(BrowserBasicFallbackMode mode,
                                  bool explicit_reader_request)
{
    return mode != BROWSER_BASIC_FALLBACK_OFF && !explicit_reader_request;
}

PspBasicFallbackDecision psp_basic_fallback_decide(
    BrowserBasicFallbackMode mode, BrowserBasicViewRecovery recovery)
{
    if (mode == BROWSER_BASIC_FALLBACK_OFF)
        return PSP_BASIC_FALLBACK_KEEP_PAGE;
    switch (recovery) {
    case BROWSER_BASIC_VIEW_RECOVERY_DEFERRED:
        return PSP_BASIC_FALLBACK_DEFER;
    case BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE:
        return mode == BROWSER_BASIC_FALLBACK_ASK
            ? PSP_BASIC_FALLBACK_ASK : PSP_BASIC_FALLBACK_SWITCH;
    case BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE:
        return PSP_BASIC_FALLBACK_UNAVAILABLE;
    case BROWSER_BASIC_VIEW_RECOVERY_NONE:
    default:
        return PSP_BASIC_FALLBACK_KEEP_PAGE;
    }
}

bool psp_basic_fallback_recheck(BrowserBasicFallbackMode mode,
                                bool extracted_view_active,
                                bool recovery_pending, bool runtime_ok,
                                bool realm_retired, bool failure_examined)
{
    if (mode == BROWSER_BASIC_FALLBACK_OFF || extracted_view_active
        || recovery_pending) return false;
    return (!runtime_ok || realm_retired) && !failure_examined;
}

uint8_t psp_basic_fallback_failure_actions(BrowserBasicFallbackMode mode,
                                           bool basic_available,
                                           bool basic_active,
                                           bool document_failed_after_arrival)
{
    if (mode == BROWSER_BASIC_FALLBACK_OFF || basic_active) return 0u;
    if (basic_available) return (uint8_t) PSP_UI_FAILURE_BASIC_VIEW;
    return document_failed_after_arrival
        ? (uint8_t) PSP_UI_FAILURE_RELOAD_BASIC : 0u;
}

bool psp_basic_fallback_navigation_reloadable(long http_status,
                                              bool internal_url)
{
    return !internal_url
        && http_status >= 200 && http_status <= 299;
}
