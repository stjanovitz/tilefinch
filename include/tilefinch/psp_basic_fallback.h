#ifndef TILEFINCH_PSP_BASIC_FALLBACK_H
#define TILEFINCH_PSP_BASIC_FALLBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/reader_mode.h"

/*
 * The PSP frontend's Basic view fallback policy (docs/BASIC_VIEW.md), kept
 * free of firmware and engine state so every setting and trigger is a host
 * test. The engine decides whether a page qualifies
 * (browser_engine_prepare_basic_view_recovery: scripts stopped or failed,
 * author work settled, memory-limited or blank/without a usable action); these
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
   failed (the turn failed, memory admission failed, the realm retired, or a
   live realm was refused memory and the page shows little:
   browser_engine_page_memory_recovery_due) should be re-examined for the
   fallback. Pages are otherwise examined only at
   commit; this happens at most once per navigation (failure_examined). */
bool psp_basic_fallback_recheck(BrowserBasicFallbackMode mode,
                                bool extracted_view_active,
                                bool recovery_pending, bool runtime_ok,
                                bool memory_limited_or_retired,
                                bool failure_examined);

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

/* Whether the page the browser opened at boot gets the post-navigation
   examination every later navigation gets at commit (declared media card,
   blank-page recovery, automatic Reader): a loaded web page, not native
   Home or another internal page. The boot load commits outside the
   interactive loop, so without this it is asked only by a later recheck. */
bool psp_basic_fallback_boot_page_examined(bool loaded, bool native_home,
                                           bool internal_url);

/*
 * Basic view by default (docs/BASIC_VIEW.md, "Opening in Basic view by
 * default"). Under Automatic, a site whose page's scripts ran out of memory
 * on an earlier visit (its SMEM note, browser_profile_site_script_memory_
 * stopped) opens in Basic view: the load runs with page JavaScript off and
 * its commit is presented in Basic view with a notice that offers the full
 * page. "Try full page" loads that address once with scripts; when that full
 * page commits the site's note is forgotten, and the next memory stop notes
 * it again. Ask and Off keep the "Load without scripts?" offer instead.
 *
 * Basic view is used only where it keeps what the page offers: when the
 * script-free page has form controls Basic view would leave out (a currency
 * converter's amount field, a POST form), almost no text (an app shell that
 * needs its scripts), or a redirect took it to another site, the page loads
 * again with its scripts and the requested site keeps the offer instead.
 * The site is remembered for the session, and in the profile (SMEMFULL,
 * browser_profile_site_loads_in_full) while URL history is on. Only GET
 * navigations open in Basic view, and a per-site Reader choice wins.
 *
 * The notice takes X (Try full page) and O only the first time a site is
 * presented this session; later pages of the site get it as a plain status,
 * and Page tools keeps Try full page.
 *
 * One state per browser: a navigation start, its commit or failure, and the
 * reader's request move it. Kept free of firmware state.
 */
typedef enum {
    PSP_BASIC_DEFAULT_NONE = 0,
    /* The site's page ran out of script memory on an earlier visit. */
    PSP_BASIC_DEFAULT_SCRIPT_MEMORY
} PspBasicDefaultReason;

typedef enum {
    PSP_BASIC_DEFAULT_IDLE = 0,
    /* "Try full page" was chosen; the load for `url` has not started. */
    PSP_BASIC_DEFAULT_FULL_REQUESTED,
    /* A load opened in Basic view by default is in flight. */
    PSP_BASIC_DEFAULT_BASIC_LOADING,
    /* The full page's load ("Try full page") is in flight. */
    PSP_BASIC_DEFAULT_FULL_LOADING
} PspBasicDefaultPhase;

/* Sites whose pages load in full, and sites whose notice was answerable,
   this session. */
#define PSP_BASIC_DEFAULT_FULL_SITE_LIMIT 8u

typedef struct {
    PspBasicDefaultPhase phase;
    PspBasicDefaultReason reason;
    /* The address the phase is for; after a Basic commit, the requested
       address until the commit is presented or kept in full. */
    char url[NAVIGATION_URL_LIMIT];
    /* The entry presented in Basic view by default (Page tools' Try full
       page belongs to it), cleared by the next commit. */
    char presented_url[NAVIGATION_URL_LIMIT];
    uint8_t full_site_count;
    uint8_t noticed_site_count;
    char full_sites[PSP_BASIC_DEFAULT_FULL_SITE_LIMIT]
                   [CONTENT_BLOCKER_HOST_LIMIT];
    char noticed_sites[PSP_BASIC_DEFAULT_FULL_SITE_LIMIT]
                      [CONTENT_BLOCKER_HOST_LIMIT];
} PspBasicDefault;

typedef enum {
    /* An ordinary commit. */
    PSP_BASIC_DEFAULT_COMMIT_NONE = 0,
    /* The commit opened in Basic view by default: present it
       (psp_basic_default_presentable, then _presented or
       _keep_full_page). */
    PSP_BASIC_DEFAULT_COMMIT_BASIC,
    /* The full page the reader asked for committed. */
    PSP_BASIC_DEFAULT_COMMIT_FULL
} PspBasicDefaultCommit;

/* Why a page whose JavaScript the profile allows opens in Basic view, if it
   does: only under Automatic and only for a noted site. */
PspBasicDefaultReason psp_basic_default_reason(
    BrowserBasicFallbackMode mode, bool javascript_allowed,
    bool script_memory_noted);

/* A top-level navigation to URL with METHOD (NULL is GET) starts: returns
   the page JavaScript policy for this load (false when it opens in Basic
   view by default). A pending "Try full page" for the same address is used
   up here; any other navigation drops it. */
bool psp_basic_default_navigation(PspBasicDefault *state,
                                  const BrowserProfile *profile,
                                  const char *url, const char *method);

/* The same, applied to ENGINE: the policy is set first, and the state
   moves only when the engine took it (it refuses while a navigation is
   pending). False when it refused. */
bool psp_basic_default_begin(PspBasicDefault *state,
                             const BrowserProfile *profile,
                             BrowserEngine *engine, const char *url,
                             const char *method);

/* The navigation begun did not start, or ended without a commit: a load
   that opened in Basic view by default gives ENGINE the site's JavaScript
   policy back, and the state is left as psp_basic_default_failed leaves
   it. */
void psp_basic_default_abandon(PspBasicDefault *state,
                               const BrowserProfile *profile,
                               BrowserEngine *engine);

/* The reader chose "Try full page" for URL; the caller then reloads it. */
void psp_basic_default_request_full_page(PspBasicDefault *state,
                                         const char *url);

/* A page committed at LOADED_URL. A committed full page forgets the note
   of its site (and of the requested address's site, after a redirect);
   *profile_changed says whether the profile changed (mark it dirty). */
PspBasicDefaultCommit psp_basic_default_commit(PspBasicDefault *state,
                                               BrowserProfile *profile,
                                               const char *loaded_url,
                                               bool *profile_changed);

/* The load failed or was cancelled before a commit (see also
   psp_basic_default_abandon). */
void psp_basic_default_failed(PspBasicDefault *state);

/* A Basic commit is left to Reader instead: the reader's Reader request,
   or the site's "always Reader" choice. */
bool psp_basic_default_reader_wins(const BrowserProfile *profile,
                                   const char *loaded_url,
                                   bool reader_requested);

/* Whether the prepared Basic view of the script-free commit at LOADED_URL
   may be presented by default: it keeps every visible form control, the
   page has text to read (SCRIPT_HEAVY_SHELL_TEXT_BYTES), and it is on the
   requested site or a noted one. */
bool psp_basic_default_presentable(const PspBasicDefault *state,
                                   const BrowserProfile *profile,
                                   const char *loaded_url,
                                   const ReaderDocumentAnalysis *analysis);

/* The script-free commit is not presented in Basic view: the requested
   address's site loads with scripts for the rest of the session, and,
   with REMEMBER (the page itself was the reason), in the profile too; the
   caller reloads the page. True when the profile changed. */
bool psp_basic_default_keep_full_page(PspBasicDefault *state,
                                      BrowserProfile *profile,
                                      const char *loaded_url, bool remember);

/* The commit at LOADED_URL is presented in Basic view by default. True
   when its notice may take X and O: the site's first this session. */
bool psp_basic_default_presented(PspBasicDefault *state,
                                 const char *loaded_url);

/* Whether URL is still the page presented in Basic view by default, so
   Page tools keeps Try full page. */
bool psp_basic_default_page_current(const PspBasicDefault *state,
                                    const char *url);

/* Whether the "Load without scripts?" offer is shown for a noted site at
   URL: Automatic opens the site in Basic view instead, unless the site
   loads in full (psp_basic_default_keep_full_page). */
bool psp_basic_default_scripts_off_offer_allowed(
    BrowserBasicFallbackMode mode, const PspBasicDefault *state,
    const BrowserProfile *profile, const char *url);

/* The status after Page tools' Basic view turned the view on (ENABLE) or
   off: it says when preparing the view stopped the page's scripts
   (ANALYSIS->scripts_stopped), which turning the view off does not undo. */
const char *psp_basic_view_toggle_status(
    bool enable, const ReaderDocumentAnalysis *analysis);

#endif
