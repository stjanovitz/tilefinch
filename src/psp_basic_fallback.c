#include "tilefinch/psp_basic_fallback.h"

#include "tilefinch/psp_ui.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/script_admission.h"
#include "tilefinch/url.h"

#include <stdio.h>
#include <string.h>

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
                                bool memory_limited_or_retired,
                                bool failure_examined)
{
    if (mode == BROWSER_BASIC_FALLBACK_OFF || extracted_view_active
        || recovery_pending) return false;
    return (!runtime_ok || memory_limited_or_retired) && !failure_examined;
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

bool psp_basic_fallback_boot_page_examined(bool loaded, bool native_home,
                                           bool internal_url)
{
    return loaded && !native_home && !internal_url;
}

PspBasicDefaultReason psp_basic_default_reason(
    BrowserBasicFallbackMode mode, bool javascript_allowed,
    bool script_memory_noted)
{
    return mode == BROWSER_BASIC_FALLBACK_AUTOMATIC && javascript_allowed
            && script_memory_noted
        ? PSP_BASIC_DEFAULT_SCRIPT_MEMORY : PSP_BASIC_DEFAULT_NONE;
}

/* The same request (normalized, without the fragment). */
static bool psp_basic_default_same_address(const char *a, const char *b)
{
    char a_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char b_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (a == NULL || b == NULL) return false;
    if (tilefinch_url_request_key(a, a_key, sizeof(a_key))
        && tilefinch_url_request_key(b, b_key, sizeof(b_key)))
        return strcmp(a_key, b_key) == 0;
    return strcmp(a, b) == 0;
}

static bool psp_basic_default_site_listed(
    const char (*sites)[CONTENT_BLOCKER_HOST_LIMIT], size_t count,
    const char *url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(sites[i], site) == 0) return true;
    return false;
}

/* Adds URL's site to a session list; a full list forgets its oldest. */
static void psp_basic_default_site_add(
    char (*sites)[CONTENT_BLOCKER_HOST_LIMIT], uint8_t *count,
    const char *url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (psp_basic_default_site_listed(
            (const char (*)[CONTENT_BLOCKER_HOST_LIMIT]) sites, *count, url)
        || !content_blocker_site_from_url(url, site))
        return;
    if (*count >= PSP_BASIC_DEFAULT_FULL_SITE_LIMIT) {
        memmove(sites[0], sites[1],
                sizeof(sites[0]) * (PSP_BASIC_DEFAULT_FULL_SITE_LIMIT - 1u));
        *count = PSP_BASIC_DEFAULT_FULL_SITE_LIMIT - 1u;
    }
    snprintf(sites[(*count)++], CONTENT_BLOCKER_HOST_LIMIT, "%s", site);
}

/* The site loads in full: this session's list, or the profile's mark. */
static bool psp_basic_default_full_site(const PspBasicDefault *state,
                                        const BrowserProfile *profile,
                                        const char *url)
{
    return (state != NULL
            && psp_basic_default_site_listed(
                   (const char (*)[CONTENT_BLOCKER_HOST_LIMIT])
                       state->full_sites,
                   state->full_site_count, url))
        || browser_profile_site_loads_in_full(profile, url);
}

static void psp_basic_default_set(PspBasicDefault *state,
                                  PspBasicDefaultPhase phase,
                                  PspBasicDefaultReason reason,
                                  const char *url)
{
    state->phase = phase;
    state->reason = reason;
    if (url == NULL || snprintf(state->url, sizeof(state->url), "%s", url)
            >= (int) sizeof(state->url))
        state->url[0] = '\0';
}

static bool psp_basic_default_get(const char *method)
{
    if (method == NULL || method[0] == '\0') return true;
    static const char get[] = "GET";
    for (size_t at = 0; at < sizeof(get); at++) {
        char c = method[at];
        if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
        if (c != get[at]) return false;
    }
    return true;
}

/* What a navigation start makes of the state, without changing it. */
static PspBasicDefaultPhase psp_basic_default_plan(
    const PspBasicDefault *state, const BrowserProfile *profile,
    const char *url, const char *method, PspBasicDefaultReason *reason)
{
    bool allowed = browser_profile_javascript_allowed_for_url(profile, url);
    *reason = PSP_BASIC_DEFAULT_NONE;
    bool full_page = state->phase == PSP_BASIC_DEFAULT_FULL_REQUESTED
        && state->url[0] != '\0'
        && psp_basic_default_same_address(state->url, url);
    /* Only a GET opens in Basic view: a form's POST result is the
       server's answer to it, and neither recovery route could repeat it
       (both reload the address). */
    if (!full_page && psp_basic_default_get(method)
        && !psp_basic_default_full_site(state, profile, url))
        *reason = psp_basic_default_reason(
            browser_profile_basic_fallback_mode(profile), allowed,
            browser_profile_site_script_memory_stopped(profile, url));
    if (*reason != PSP_BASIC_DEFAULT_NONE)
        return PSP_BASIC_DEFAULT_BASIC_LOADING;
    return full_page ? PSP_BASIC_DEFAULT_FULL_LOADING
                     : PSP_BASIC_DEFAULT_IDLE;
}

static bool psp_basic_default_apply(PspBasicDefault *state,
                                    const BrowserProfile *profile,
                                    const char *url,
                                    PspBasicDefaultPhase phase,
                                    PspBasicDefaultReason reason)
{
    psp_basic_default_set(state, phase, reason,
                          phase == PSP_BASIC_DEFAULT_IDLE ? NULL : url);
    return phase == PSP_BASIC_DEFAULT_BASIC_LOADING
        ? false : browser_profile_javascript_allowed_for_url(profile, url);
}

bool psp_basic_default_navigation(PspBasicDefault *state,
                                  const BrowserProfile *profile,
                                  const char *url, const char *method)
{
    if (state == NULL)
        return browser_profile_javascript_allowed_for_url(profile, url);
    PspBasicDefaultReason reason;
    PspBasicDefaultPhase phase =
        psp_basic_default_plan(state, profile, url, method, &reason);
    return psp_basic_default_apply(state, profile, url, phase, reason);
}

bool psp_basic_default_begin(PspBasicDefault *state,
                             const BrowserProfile *profile,
                             BrowserEngine *engine, const char *url,
                             const char *method)
{
    if (state == NULL)
        return browser_engine_set_javascript_enabled(
            engine, browser_profile_javascript_allowed_for_url(profile, url));
    PspBasicDefaultReason reason;
    PspBasicDefaultPhase phase =
        psp_basic_default_plan(state, profile, url, method, &reason);
    bool javascript = phase != PSP_BASIC_DEFAULT_BASIC_LOADING
        && browser_profile_javascript_allowed_for_url(profile, url);
    /* The engine refuses while another navigation is pending: that load
       keeps its own state. */
    if (!browser_engine_set_javascript_enabled(engine, javascript))
        return false;
    (void) psp_basic_default_apply(state, profile, url, phase, reason);
    return true;
}

void psp_basic_default_abandon(PspBasicDefault *state,
                               const BrowserProfile *profile,
                               BrowserEngine *engine)
{
    if (state == NULL) return;
    if (state->phase == PSP_BASIC_DEFAULT_BASIC_LOADING) {
        /* The load ran, or was to run, without page JavaScript: the
           site's policy again. */
        (void) browser_engine_set_javascript_enabled(
            engine,
            browser_profile_javascript_allowed_for_url(profile, state->url));
    }
    psp_basic_default_failed(state);
}

void psp_basic_default_request_full_page(PspBasicDefault *state,
                                         const char *url)
{
    if (state == NULL) return;
    if (url == NULL || url[0] == '\0') {
        psp_basic_default_set(state, PSP_BASIC_DEFAULT_IDLE,
                              PSP_BASIC_DEFAULT_NONE, NULL);
        return;
    }
    psp_basic_default_set(state, PSP_BASIC_DEFAULT_FULL_REQUESTED,
                          PSP_BASIC_DEFAULT_NONE, url);
}

PspBasicDefaultCommit psp_basic_default_commit(PspBasicDefault *state,
                                               BrowserProfile *profile,
                                               const char *loaded_url,
                                               bool *profile_changed)
{
    if (profile_changed != NULL) *profile_changed = false;
    if (state == NULL) return PSP_BASIC_DEFAULT_COMMIT_NONE;
    PspBasicDefaultPhase phase = state->phase;
    PspBasicDefaultCommit result = PSP_BASIC_DEFAULT_COMMIT_NONE;
    /* Each commit replaces the page presented in Basic view by default. */
    state->presented_url[0] = '\0';
    if (phase == PSP_BASIC_DEFAULT_BASIC_LOADING) {
        /* The load ran without scripts, wherever it ended up. The
           requested address stays for psp_basic_default_presentable and
           psp_basic_default_keep_full_page. */
        state->phase = PSP_BASIC_DEFAULT_IDLE;
        state->reason = PSP_BASIC_DEFAULT_NONE;
        return PSP_BASIC_DEFAULT_COMMIT_BASIC;
    }
    if (phase == PSP_BASIC_DEFAULT_FULL_LOADING) {
        /* The full page loaded: open the site normally until its scripts
           run out of memory again. */
        bool changed = browser_profile_site_script_memory_stopped(
                           profile, loaded_url)
            && browser_profile_set_site_script_memory_stopped(
                   profile, loaded_url, false);
        if (browser_profile_site_script_memory_stopped(profile, state->url)
            && browser_profile_set_site_script_memory_stopped(
                   profile, state->url, false))
            changed = true;
        if (profile_changed != NULL) *profile_changed = changed;
        result = PSP_BASIC_DEFAULT_COMMIT_FULL;
    }
    psp_basic_default_set(state, PSP_BASIC_DEFAULT_IDLE,
                          PSP_BASIC_DEFAULT_NONE, NULL);
    return result;
}

void psp_basic_default_failed(PspBasicDefault *state)
{
    if (state != NULL && state->phase != PSP_BASIC_DEFAULT_FULL_REQUESTED)
        psp_basic_default_set(state, PSP_BASIC_DEFAULT_IDLE,
                              PSP_BASIC_DEFAULT_NONE, NULL);
}

bool psp_basic_default_reader_wins(const BrowserProfile *profile,
                                   const char *loaded_url,
                                   bool reader_requested)
{
    return reader_requested
        || browser_profile_reader_site_always(profile, loaded_url);
}

bool psp_basic_default_presentable(const PspBasicDefault *state,
                                   const BrowserProfile *profile,
                                   const char *loaded_url,
                                   const ReaderDocumentAnalysis *analysis)
{
    if (analysis == NULL || analysis->dropped_controls != 0
        /* A script-free app shell ("enable JavaScript") has nothing to
           read: the page needs its scripts. */
        || analysis->visible_text_bytes < SCRIPT_HEAVY_SHELL_TEXT_BYTES)
        return false;
    /* A redirect to another site lands on a page that was not noted: it
       loads with its scripts, and the requested site loads in full. */
    char requested[CONTENT_BLOCKER_HOST_LIMIT], landed[CONTENT_BLOCKER_HOST_LIMIT];
    if (state != NULL && state->url[0] != '\0'
        && content_blocker_site_from_url(state->url, requested)
        && content_blocker_site_from_url(loaded_url, landed)
        && strcmp(requested, landed) != 0
        && !browser_profile_site_script_memory_stopped(profile, loaded_url))
        return false;
    return true;
}

bool psp_basic_default_keep_full_page(PspBasicDefault *state,
                                      BrowserProfile *profile,
                                      const char *loaded_url, bool remember)
{
    if (state == NULL) return false;
    /* The requested address's site: after a redirect, the landing page's
       would not be asked about on the next visit. */
    const char *url = state->url[0] != '\0' ? state->url : loaded_url;
    psp_basic_default_site_add(state->full_sites, &state->full_site_count,
                               url);
    bool changed = remember
        && browser_profile_set_site_loads_in_full(profile, url, true);
    state->url[0] = '\0';
    return changed;
}

bool psp_basic_default_presented(PspBasicDefault *state,
                                 const char *loaded_url)
{
    if (state == NULL || loaded_url == NULL) return false;
    if (snprintf(state->presented_url, sizeof(state->presented_url), "%s",
                 loaded_url) >= (int) sizeof(state->presented_url))
        state->presented_url[0] = '\0';
    state->url[0] = '\0';
    bool first = !psp_basic_default_site_listed(
        (const char (*)[CONTENT_BLOCKER_HOST_LIMIT]) state->noticed_sites,
        state->noticed_site_count, loaded_url);
    psp_basic_default_site_add(state->noticed_sites,
                               &state->noticed_site_count, loaded_url);
    return first;
}

bool psp_basic_default_page_current(const PspBasicDefault *state,
                                    const char *url)
{
    return state != NULL && state->presented_url[0] != '\0'
        && psp_basic_default_same_address(state->presented_url, url);
}

bool psp_basic_default_scripts_off_offer_allowed(
    BrowserBasicFallbackMode mode, const PspBasicDefault *state,
    const BrowserProfile *profile, const char *url)
{
    return mode != BROWSER_BASIC_FALLBACK_AUTOMATIC
        || psp_basic_default_full_site(state, profile, url);
}

const char *psp_basic_view_toggle_status(
    bool enable, const ReaderDocumentAnalysis *analysis)
{
    if (!enable) return "BASIC VIEW OFF";
    return analysis != NULL && analysis->scripts_stopped
        ? "BASIC VIEW ON\nPAGE SCRIPTS STOPPED" : "BASIC VIEW ON";
}
