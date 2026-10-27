/* PSP browser boot phases (src/psp_script_main.c main).
 *
 * main is the ordered list of these calls; the order is load-bearing and its
 * reasons stay as comments at the call sites. Each phase is a verbatim move
 * of a stretch of main, kept noinline and cold so main stays a small growth
 * tripwire (cmake/CheckPspHotSymbolSizes.cmake) and boot code stays out of
 * the hot cluster. A phase that can halt boot returns a PspStartupFailure;
 * main reports it on the startup-failure screen exactly as before.
 */
#ifndef TILEFINCH_PSP_APP_BOOT_H
#define TILEFINCH_PSP_APP_BOOT_H

#include "psp_app_internal.h"

/* Why boot halted: the detail line and the screen title. A NULL detail means
   the phase succeeded. */
typedef struct {
    const char *detail;
    const char *screen;
} PspStartupFailure;

static inline PspStartupFailure psp_startup_failure(
    const char *detail, const char *screen)
{
    return (PspStartupFailure) { .detail = detail, .screen = screen };
}

/* Profile and site-data facts the rest of boot reads. */
typedef struct {
    bool tls_session_persistence;
    bool javascript_enabled;
    bool site_data_allowed;
    bool live_cache_limit_set;
    ContentBlockerMode content_blocker_mode;
    size_t startup_requests_blocked;
    unsigned persistent_cache_mb;
    unsigned live_cache_kib;
} PspBootProfileState;

/* What the initial navigation left behind. */
typedef struct {
    bool loaded;
    bool download_offer;
    bool error_page;
    uint64_t load_started_us;
} PspBootNavigation;

/* boot-defaults.cfg, boot-overrides.cfg (or the legacy boot-live.cfg) or a
   sibling boot.cfg, validated and published to the media backend. */
PspStartupFailure psp_boot_load_config(
    PspProcessResources *process, const char *argv0);
/* Trust assets, env knobs and the engine configuration. On success
   *engine_config is the caller's to free. */
PspStartupFailure psp_boot_configure_engine(
    PspProcessResources *process, BrowserConfig **engine_config);
/* Chrome fonts, storage paths, the profile and the components it selects. */
PspStartupFailure psp_boot_open_profile_services(
    PspProcessResources *process, PspBrowserResources *browser,
    PspBootProfileState *state);
/* Site-data, content-blocker and cache policy from the loaded profile. */
void psp_boot_apply_profile_policy(
    PspProcessResources *process, PspBrowserResources *browser,
    PspBootProfileState *state);
/* Native HOME, a profile page, an offline route or the first network page;
   a failed load leaves the retry/home error page instead. */
PspBootNavigation psp_boot_initial_navigation(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *engine_views, const char *startup_url,
    bool native_home_boot, bool restoring_last_page,
    const BrowserRecoveryCheckpoint *startup_recovery, const char *argv0);

#endif
