/* Private contract shared by the PSP browser EBOOT translation units.
 *
 * Every psp_app TU (and src/psp_script_main.c itself) must include this
 * header FIRST and must not include system or project headers ahead of it.
 * The reason is the `#define printf psp_log_printf` below: device logging
 * only reaches the bounded validation sink through that redirect, and the
 * redirect must be established after all included declarations are parsed so
 * those declarations never see the macro. A TU that omits this header still
 * compiles but silently bypasses device logging.
 *
 * The common include set deliberately gives every PSP app TU the same SDK and
 * feature-macro environment. Keep TU-specific headers after this one.
 */
#ifndef TILEFINCH_PSP_APP_INTERNAL_H
#define TILEFINCH_PSP_APP_INTERNAL_H

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <psploadexec.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspsysmem.h>

#include <errno.h>
#include <dirent.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef TILEFINCH_PSP_LIVE_NETWORK
#include <time.h>
#endif

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/browser_tabs.h"
#include "tilefinch/build_version.h"
#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/navigation.h"
#include "tilefinch/omnibox.h"
#include "tilefinch/platform.h"
#ifdef TILEFINCH_PSP_LIVE_NETWORK
#include "tilefinch/psp_network.h"
#include "tilefinch/psp_network_supervisor.h"
#endif
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_boot_config.h"
#include "tilefinch/psp_boot_order.h"
#include "tilefinch/psp_clock_worker.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_lifecycle.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_media_scale.h"
#include "tilefinch/psp_media_session.h"
#include "tilefinch/psp_offline_store.h"
#include "tilefinch/psp_power_policy.h"
#include "tilefinch/psp_profile_store.h"
#include "tilefinch/psp_text_input.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/psp_update_session.h"
#include "tilefinch/psp_voice_component_session.h"
#include "tilefinch/psp_glyph_component_session.h"
#include "tilefinch/render.h"
#include "tilefinch/screenshot_png.h"
#include "tilefinch/session_persistence.h"
#include "tilefinch/sha256.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/update.h"
#include "tilefinch/viewport.h"
#include "tilefinch/youtube_resolver.h"
#include "media_backend_psp_policy.h"
#include "psp_media_pixels.h"

/* The device logging redirect. See the file comment: this must come after
   every other header and before any code that logs. */
#define printf psp_log_printf

#define PSP_SCREEN_WIDTH 480
#define PSP_SCREEN_HEIGHT 272
#define PSP_VRAM_STRIDE 512
#define MIB (1024u * 1024u)
#define KIB 1024u
#define PSP_STARTUP_BACKGROUND 0x0843u

#define PSP_MEDIA_PREVIEW_WIDTH 128
#define PSP_MEDIA_PREVIEW_HEIGHT 72
#define PSP_RENDER_JOB_BUDGET_US 2000u
#define PSP_RENDER_JOB_MAXIMUM_TILES 1u
#define PSP_RENDER_JOB_TIMEOUT_US UINT64_C(2000000)
#define PSP_NAVIGATION_JOB_TIMEOUT_US UINT64_C(35000000)
#define PSP_NETWORK_CONNECT_TIMEOUT_US UINT64_C(45000000)
/* A HOME tile must hold focus this long (think-time) before its host earns a
   speculative background TCP+TLS connect (docs/engineering/
   PSP_TRANSPORT.md). */
#define PSP_HOME_PRECONNECT_DWELL_MS 300u
#define PSP_NETWORK_PRESENT_INTERVAL_US UINT64_C(100000)
#define PSP_NAVIGATION_PRESENT_INTERVAL_US UINT64_C(100000)
#define PSP_HOME_EXIT_GRACE_MS 30000u
#define PSP_MEDIA_STABILITY_URL \
    "https://www.youtube.com/watch?v=TFTEST00001"

#ifndef TILEFINCH_UPDATE_REPOSITORY_OWNER
#define TILEFINCH_UPDATE_REPOSITORY_OWNER "stjanovitz"
#endif
#ifndef TILEFINCH_UPDATE_REPOSITORY_NAME
#define TILEFINCH_UPDATE_REPOSITORY_NAME "tilefinch"
#endif

/* ---- private executable-wide declarations ---------------------------- */

/* src/psp_script_main.c */
typedef enum {
    PSP_PROFILE_PAGE_NONE = 0,
    PSP_PROFILE_PAGE_BOOKMARKS,
    PSP_PROFILE_PAGE_HISTORY,
    PSP_PROFILE_PAGE_HOMEPAGE
} PspProfilePageKind;

bool psp_write_failure_report(
    const char *stage, const char *detail, const char *url,
    long http_status, int native_result);
bool psp_write_navigation_failure_report(
    const char *stage, const char *detail, const char *url,
    const NavigationSession *navigation);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
bool psp_write_network_failure_report(const PspNetwork *network);
#endif
bool psp_offline_url(const char *url);
bool psp_internal_action_url(const char *url, const char *name);
bool psp_request_omnibox(
    PspTextInputService *text_input, const uint16_t *engine_frame,
    PspUiState *ui, const BrowserProfile *profile,
    const char *current_url, bool use_voice, bool start_empty,
    char *destination, size_t destination_capacity);

/* src/psp_app/psp_app_page.c */
PspProfilePageKind psp_profile_page_kind(const char *url);
bool psp_set_presentation_css(
    BrowserEngine *engine, const PspUiState *ui,
    const BrowserProfile *profile, bool reader_mode,
    const char *url, unsigned font_percent, bool relayout);
void psp_leave_reader_for_navigation(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    const char *url);
typedef struct {
    bool pending;
    unsigned incumbent_percent;
    unsigned destination_percent;
} PspReaderNavigation;
bool psp_reader_navigation_prepare(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    PspReaderNavigation *navigation, const char *url);
void psp_reader_navigation_finish(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    PspReaderNavigation *navigation, const char *current_url,
    bool succeeded);
bool psp_retry_navigation_action_after_reclaim(
    BrowserEngine *engine, const ControllerAction *action,
    size_t maximum_bytes, long timeout_ms);
void psp_report_job_failure(
    const char *kind, const char *checkpoint, int status, long http_status,
    const char *error);
bool psp_begin_page_load(BrowserEngine *engine, PspUiState *ui,
                         const BrowserProfile *profile,
                         const uint16_t *frame,
                         PspTextInputService *text_input,
                         const char *url, bool record_history,
                         size_t maximum_bytes, long timeout_ms);
bool psp_replace_focused_text(
    BrowserEngine *engine, const uint16_t *frame, PspUiState *ui,
    PspTextInputService *text_input, bool voice_requested,
    bool *submit_requested);
BrowserNavigationJobQuota psp_navigation_quota(void);
bool psp_run_initial_page_load(
    BrowserEngine *engine, PspUiState *ui, const uint16_t *frame,
    const char *url, size_t maximum_bytes, long timeout_ms,
    const char *argv0, bool dump_provisional, bool *stopped);
BrowserSessionPersistenceLimits psp_site_data_limits(
    unsigned cache_megabytes);
bool psp_site_data_load(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits);
bool psp_site_data_save(
    const BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits);
void psp_report_blocking_script_samples(
    const NavigationPerformance *performance);
void psp_report_background_transport_metrics(void);
void psp_report_budget_counters(const Budget *budget, const char *phase);
void psp_profile_record_current(
    BrowserProfile *profile, PspProfileStore *store,
    const NavigationSession *navigation, uint64_t now_us);
bool psp_recovery_record_current(
    const BrowserProfile *profile, const char *path,
    const NavigationSession *navigation);
bool psp_profile_open_page(
    BrowserEngine *engine, PspUiState *ui, BrowserProfile *profile,
    PspProfilePageKind kind);
bool psp_profile_open_page_history(
    BrowserEngine *engine, PspUiState *ui, BrowserProfile *profile,
    PspProfilePageKind kind, bool record_history);

/* src/psp_app/psp_app_runtime.c
 *
 * These four have process lifetime and are written by this file's exit,
 * power, and background-UI callbacks, so the runtime TU owns their storage.
 */
extern PspMediaSession *psp_active_media;
extern atomic_uint psp_background_ui_available;
extern atomic_bool psp_home_exit_requested;
extern PspLifecycle psp_lifecycle;
extern PspDisplay psp_display;
/* Forget which decoded picture each scanout buffer holds. Anything that
   writes a buffer other than the video presenter must call this. */
void psp_media_present_forget_buffers(void);

typedef struct {
    uint64_t maximum_us;
    uint64_t steady_since_us;
    uint64_t steady_maximum_us;
    uint64_t steady_peak_elapsed_us;
    uint64_t steady_peak_audio_us;
    uint64_t steady_peak_video_us;
    unsigned steady_peak_no_frame_ms;
    unsigned steady_samples;
} PspMediaStabilitySkew;

/* The cooperative-work supervisor's state. The interactive loop and callback
   presenter share the provisional-present request, button history, and
   checkpoint counters, so this layout is part of their private contract. */
typedef struct {
    PspUiState *ui;
    PspUiState supervisor_ui;
    PspUiMediaState supervisor_media_ui;
    BrowserEngine *engine;
    const uint16_t *frame;
    uint64_t last_present_us;
    uint64_t last_checkpoint_us;
    uint64_t maximum_checkpoint_gap_us;
    const char *maximum_checkpoint_gap_phase;
    const char *last_phase;
    size_t last_completed_work_units;
    size_t checkpoint_calls;
    size_t presentations;
    size_t input_acknowledgements;
    uint64_t maximum_input_ack_us;
    uint64_t cancellation_requested_us;
    uint64_t started_us;
    uint32_t supervisor_previous_buttons;
    uint32_t fallback_previous_buttons;
    const char *cancel_status;
    bool periodic_present;
    bool acknowledge_non_cancel_busy;
    bool log_session;
    bool media_surface;
    /* Set once a media scope's cancellation has been latched and the player
       overlay has been dropped from the supervisor's snapshot. media_surface
       is cleared at the same moment so the periodic tick repaints the page;
       this flag is what still remembers that the stopping work is a video, so
       later presses keep saying so. */
    bool media_detached;
    bool validation_cancel_injected;
    bool validation_preview_scroll_injected;
    volatile unsigned active;
    unsigned supervised;
    volatile unsigned presenting;
    volatile unsigned provisional_present_requested;
    volatile int provisional_scroll_requests;
    /* Latest media command accepted by the callback-thread supervisor while
       the browser thread is inside one bounded decoder/range service. The
       supervisor owns this tuple until cooperate_end fences presentation;
       the browser thread then takes it as an ordinary input event. */
    PspUiMediaIntent pending_media_intent;
    TilefinchCancellation cancellation;
} PspNavigationCooperate;

extern PspNavigationCooperate psp_navigation_cooperate;
extern volatile unsigned psp_validation_cancel_after_ms;
extern volatile unsigned psp_validation_preview_scroll;

int psp_setup_callbacks(void);
bool psp_home_exit_pending(void);
uint64_t psp_time_ns(void *context);
uint64_t psp_time_us(void *context);
uint64_t psp_wall_time_ns(void *context);
const char *psp_preferred_language(void *context);
TilefinchDateFormat psp_preferred_date_format(void *context);
void psp_log_message(void *context, const char *message);
bool psp_present_internal(
    const uint16_t *frame, const PspUiState *ui, bool include_media);
void psp_present(const uint16_t *frame, const PspUiState *ui);
void psp_present_supervisor_ui(const uint16_t *frame, const PspUiState *ui);
void psp_present_boot_surface(
    PspUiStartupView view, const char *status, int progress_per_mille);
void psp_present_boot_entrance(
    unsigned frame, const char *stage, bool wave, const PspUiState *home);
void psp_refresh_page_color_mode(PspUiState *ui);
bool psp_request_policy_clock(
    void *context, unsigned cpu_mhz, unsigned bus_mhz,
    uint64_t *request_us);
bool psp_platform_present(
    void *context, const uint16_t *pixels, size_t width,
    size_t height, size_t stride_pixels);
bool psp_platform_cooperate(
    void *context, const char *phase, size_t completed_work_units);
void psp_work_cooperate_begin(
    PspUiState *ui, const uint16_t *frame,
    bool periodic_present, bool acknowledge_non_cancel_busy,
    bool log_session, const char *cancel_status, const char *phase,
    BrowserEngine *engine, const PspUiMediaState *media_ui);
void psp_navigation_cooperate_begin(
    PspUiState *ui, const uint16_t *frame, BrowserEngine *engine);
void psp_work_cooperate_refresh_media(const PspUiMediaState *media_ui);
void psp_work_cooperate_begin_media_open(
    PspUiState *ui, const uint16_t *engine_frame,
    const PspUiMediaState *media_ui);
void psp_navigation_cooperate_end(const char *scope);
bool psp_navigation_cooperate_take_media_intent(
    PspUiMediaIntent *intent);
bool psp_navigation_cooperate_active(void);
bool psp_navigation_cooperate_supervised(void);
bool psp_navigation_cancel_requested(void);
const TilefinchCancellation *psp_navigation_cancellation(void);
uint32_t psp_navigation_observed_buttons(void);
bool psp_request_provisional_scroll(
    BrowserEngine *engine, PspUiState *ui, int direction);
void psp_background_ui_tick(void);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
void psp_sample_wifi_strength(const PspNetwork *network, uint64_t now_us);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
typedef enum {
    PSP_POWER_TEST_OFF = 0,
    PSP_POWER_TEST_ADAPTIVE,
    PSP_POWER_TEST_FIXED_HIGH
} PspPowerTestPhase;

typedef struct {
    PspPowerTestPhase phase;
    uint64_t started_us;
    uint64_t next_sample_us;
    uint64_t high_ms;
    uint64_t idle_ms;
    uint64_t transition_ms;
    unsigned starting_completions;
    unsigned starting_failures;
    int starting_capacity;
    int starting_percent;
} PspPowerTest;

#define PSP_POWER_AUTO_SEGMENTS 4u
#define PSP_POWER_AUTO_SEGMENT_US UINT64_C(30000000)

typedef struct {
    bool active;
    unsigned segment;
    uint64_t started_us;
    uint64_t adaptive_elapsed_ms;
    uint64_t fixed_elapsed_ms;
    uint64_t adaptive_high_ms;
    uint64_t adaptive_idle_ms;
    uint64_t adaptive_transition_ms;
    uint64_t fixed_high_ms;
    uint64_t fixed_idle_ms;
    uint64_t fixed_transition_ms;
    int adaptive_capacity_delta;
    int fixed_capacity_delta;
    int adaptive_percent_delta;
    int fixed_percent_delta;
    bool adaptive_capacity_valid;
    bool fixed_capacity_valid;
    bool adaptive_percent_valid;
    bool fixed_percent_valid;
} PspPowerAutoTest;

typedef struct {
    PspPowerTestPhase phase;
    uint64_t elapsed_ms;
    uint64_t high_ms;
    uint64_t idle_ms;
    uint64_t transition_ms;
    int capacity_delta;
    int percent_delta;
} PspPowerTestResult;

void psp_report_presentation_cadence(const char *phase);
void psp_video_scanout_note_discontinuity(void);
void psp_cursor_latency_sample(uint64_t sampled_us);
void psp_clock_validation_probe(void);
const char *psp_power_test_phase_name(PspPowerTestPhase phase);
void psp_power_log_battery(
    const char *event, PspPowerTestPhase phase, uint64_t elapsed_ms);
void psp_power_test_tick(
    PspPowerTest *test, uint64_t now_us, unsigned elapsed_ms,
    const PspClockWorkerSnapshot *worker);
PspPowerTestResult psp_power_test_finish(
    PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, const char *reason);
void psp_power_auto_begin_segment(
    PspPowerAutoTest *automatic, PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, PspUiState *ui);
void psp_power_auto_accumulate(
    PspPowerAutoTest *automatic, const PspPowerTestResult *result);
void psp_power_auto_log_summary(
    const PspPowerAutoTest *automatic, uint64_t now_us, const char *reason);
bool psp_power_auto_start(
    PspPowerAutoTest *automatic, PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, PspUiState *ui,
    bool allow_unmeasured, const char *trigger);
#else
/* Shipping builds compile the cadence report out at the call site rather
   than guarding each of them. */
#define psp_report_presentation_cadence(phase) ((void) 0)
#define psp_video_scanout_note_discontinuity() ((void) 0)
#define psp_cursor_latency_sample(sampled_us) ((void) (sampled_us))
#endif

/* src/psp_app/psp_app_input.c */
#ifdef TILEFINCH_PSP_LIVE_NETWORK
typedef struct PspNetworkLifecycle PspNetworkLifecycle;
#endif
typedef struct {
    BrowserEngine *engine;
    PspMediaSession *media;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *network;
    PspNetworkLifecycle *network_lifecycle;
#endif
} PspVoicePrepareContext;

typedef enum {
    PSP_SCREENSHOT_DESTINATION_OK = 0,
    PSP_SCREENSHOT_DESTINATION_UNAVAILABLE,
    PSP_SCREENSHOT_DESTINATION_FULL
} PspScreenshotDestination;

void psp_text_input_present(
    void *user, const uint16_t *frame, const PspUiState *ui);
void psp_find_view_update(PspUiFindView *view,
                          const BrowserFindSnapshot *snapshot);
void psp_find_sync(BrowserEngine *engine, PspUiState *ui,
                   PspUiFindView *view);
size_t psp_text_input_prepare_voice(void *user);
uint32_t psp_ui_buttons(uint32_t buttons);
PspUiInput psp_ui_input(const SceCtrlData *pad, uint32_t previous_buttons,
                        unsigned elapsed_ms);
const char *psp_ui_action_name(PspUiAction action);
const char *psp_ui_action_acknowledgement(PspUiAction action);
bool psp_screenshot_space_short(const char *directory);
PspScreenshotDestination psp_screenshot_destination(
    const char *data_directory,
    char output[SCREENSHOT_PNG_PATH_CAPACITY]);
bool psp_open_screenshot_list(
    BrowserEngine *engine, const char *data_directory, bool push_history);
const char *psp_media_action_name(PspUiMediaAction action);
void psp_sibling_path(char *output, size_t size, const char *argv0,
                      const char *name);
/* Replaces this process with another Memory Stick EBOOT. Modern CFW's VSH
   handoff is required on hardware; the standard call remains the PPSSPP and
   firmware fallback. A successful call never returns. */
bool psp_load_exec_eboot(const char *eboot_path,
                         int *cfw_result, int *fallback_result);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
bool psp_probe_file(
    const char *path, size_t maximum_bytes, size_t *size_out,
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES]);
/* Loads another EBOOT in this one's place, for the `exit_to=` remote loop.
   Returns only when the handoff failed, so every caller falls through to the
   exit it would have taken anyway. Must be called after psp_log_finish():
   the load replaces the process and nothing after it runs. */
bool psp_exit_handoff(const char *eboot_path);
#endif
void psp_config_warning(
    void *context, const char *path, size_t line_number, const char *key);
uint64_t psp_frame_hash(const uint16_t *frame, size_t pixels);
bool psp_dump_frame_named(
    const char *argv0, const char *name, const uint16_t *frame,
    size_t pixels);
bool psp_dump_frame_strided_named(
    const char *argv0, const char *name, const uint16_t *frame,
    size_t pixels, size_t stride_pixels);
bool psp_content_blocker_apply_allowed_sites(
    BrowserEngine *engine, const BrowserProfile *profile);
void psp_content_blocker_restore_allowed_site_count(
    BrowserProfile *profile, size_t count);
void psp_sync_ui(PspUiState *ui, const BrowserEngine *engine,
                 const BrowserProfile *profile);

/* src/psp_app/psp_app_surfaces.c -- tab strip */
typedef struct {
    bool pending;
    bool added_target;
    bool hibernate_previous_after_success;
    size_t previous_index;
    size_t target_index;
    size_t close_after_success;
} PspTabTransition;

typedef enum {
    PSP_TAB_REQUEST_REFUSED = 0,
    PSP_TAB_REQUEST_HIBERNATION_FAILED,
    PSP_TAB_REQUEST_COMPLETE,
    PSP_TAB_REQUEST_LOAD
} PspTabRequestResult;

void psp_tabs_sync_ui(
    PspUiState *ui, const BrowserTabs *tabs, PspUiTabsView *view);
void psp_tabs_capture_thumbnail(
    PspUiTabsView *view, size_t index, const uint16_t *frame,
    int width, int height, int stride);
void psp_tabs_remove_thumbnail(PspUiTabsView *view, size_t index);
void psp_tabs_invalidate_thumbnail(PspUiTabsView *view, size_t index);
PspTabRequestResult psp_tabs_request(
    BrowserTabs *tabs, const NavigationSession *navigation,
    bool capture_current_document,
    PspUiAction action, size_t requested_index, const char *homepage_url,
    const char *hibernation_path, PspTabTransition *transition,
    char destination[NAVIGATION_URL_LIMIT]);
bool psp_tabs_finish(
    BrowserTabs *tabs, BrowserEngine *engine,
    PspTabTransition *transition, bool succeeded,
    bool hibernation_enabled, const char *hibernation_path);
bool psp_tabs_finish_native_home(
    BrowserTabs *tabs, PspTabTransition *transition,
    bool hibernation_enabled, const char *hibernation_path);

/* src/psp_app/psp_app_surfaces.c -- HOME and COLLECTIONS */
#define PSP_HOME_ROW_LIMIT \
    (PSP_UI_HOME_TILE_LIMIT + PSP_UI_HOME_CONTINUE_LIMIT)

typedef enum {
    PSP_HOME_TARGET_NONE = 0,
    /* Opens the omnibox rather than navigating. */
    PSP_HOME_TARGET_SEARCH,
    PSP_HOME_TARGET_BUILTIN,
    PSP_HOME_TARGET_BOOKMARK,
    PSP_HOME_TARGET_TAB
} PspHomeTargetKind;

typedef struct {
    PspUiHomeView view;
    uint8_t kind[PSP_HOME_ROW_LIMIT];
    uint8_t index[PSP_HOME_ROW_LIMIT];
} PspHomeSurface;

typedef struct {
    PspUiCollectionsView view;
    /* Offline rows carry a library id; bookmark and history rows are read
       back by their position, which is what the accessors take. */
    uint32_t id[PSP_UI_COLLECTIONS_ROW_LIMIT];
} PspCollectionsSurface;

/* Process presentation exists independently of a loaded page. The browser
   engine supplies the optional chrome font faces, but it does not own the UI
   state or native HOME/COLLECTIONS surfaces. */
typedef struct {
    PspUiState ui;
    PspUiTabsView tab_view;
    PspUiFindView find_view;
    PspHomeSurface home_surface;
    PspCollectionsSurface collections_surface;
    bool chrome_fonts_bound;
} PspPresentationResources;

void psp_presentation_init(PspPresentationResources *presentation);
void psp_presentation_bind_chrome_fonts(
    PspPresentationResources *presentation, BrowserEngine *engine);
void psp_presentation_unbind_chrome_fonts(
    PspPresentationResources *presentation);

enum { PSP_STORAGE_PATH_CAPACITY = 768 };

/* Canonical immutable paths derived once from the install layout. Keeping
   them together makes their stack cost explicit and prevents helpers from
   treating a borrowed path pointer as an owned resource. */
typedef struct {
    char profile[PSP_STORAGE_PATH_CAPACITY];
    char tab_hibernation[PSP_STORAGE_PATH_CAPACITY];
    char tab_session[PSP_STORAGE_PATH_CAPACITY];
    char recovery[PSP_STORAGE_PATH_CAPACITY];
    char persistent_cache[PSP_STORAGE_PATH_CAPACITY];
    char local_storage[PSP_STORAGE_PATH_CAPACITY];
    char tls_sessions[PSP_STORAGE_PATH_CAPACITY];
    char content_blocker[PSP_STORAGE_PATH_CAPACITY];
    char content_allowlist[PSP_STORAGE_PATH_CAPACITY];
    char offline_library[PSP_STORAGE_PATH_CAPACITY];
} PspStoragePaths;

/* One operation record, rather than six pointers to main's locals. This is
   persistent loop state, not lifecycle authority. */
typedef struct {
    const NavigationEntry *entry;
    int observed_scroll;
    uint64_t observed_generation;
    uint64_t last_change_us;
    uint64_t last_save_us;
    bool dirty;
} PspRecoveryTracker;

typedef struct {
    ScreenshotPngWriter writer;
    uint16_t *pixels;
    unsigned reported_tenth;
} PspScreenshotJob;

/* Ephemeral copied view of engine-owned state. Refresh the whole record after
   every mutating engine operation; its members are never independently
   sampled or retained across a mutation. */
typedef struct {
    const uint16_t *frame;
    size_t frame_pixels;
    const NavigationSession *navigation;
    const BrowserController *controller;
} PspEngineViews;

bool psp_engine_views_refresh(
    PspEngineViews *views, const BrowserEngine *engine);

/* Process-lifetime storage and physical ownership facts. This record does not
   decide lifecycle transitions: media/network machines remain authoritative,
   while `clock_live` only records whether teardown owes a worker join. */
typedef struct {
    TilefinchInstallPaths install_paths;
    PspBootConfig config;
    PspPresentationResources presentation;
    PspStoragePaths storage;
    PspTextInputService text_input;
    PspClockWorker clock_worker;
    bool clock_live;
    bool persistent_site_data_available;
} PspProcessResources;

/* Resources whose lifetime is bounded by the browser engine. Pointer members
   are owned handles, not expiring views; service objects live here by value.
   Their state machines remain the only control authority. */
typedef struct {
    BrowserEngine *engine;
    BrowserSession *session;
    Budget *budget;
    BrowserProfile *profile;
    BrowserTabs *tabs;
    PspProfileStore profile_store;
    PspOfflineStore offline_store;
    PspMediaSession media;
    PspUpdateSession update_session;
    PspVoiceComponentSession *voice_component_session;
    PspGlyphComponentSession *glyph_component_session;
} PspBrowserResources;

/* Why the interactive loop is ending. The tag owns the handoff policy too:
   restart outcomes relaunch the stable slot launcher, while every other
   outcome follows the configured console handoff. */
typedef enum {
    PSP_EXIT_NONE = 0,
    PSP_EXIT_USER,
    PSP_EXIT_HOME_CALLBACK,
    PSP_EXIT_VALIDATION_COMPLETE,
    PSP_EXIT_UPDATE_RESTART,
    PSP_EXIT_CONFIG_RESTART
} PspExitCause;

typedef struct {
    PspExitCause cause;
} PspExitPlan;

bool psp_exit_plan_requested(const PspExitPlan *plan);
bool psp_exit_plan_restarts_launcher(const PspExitPlan *plan);
void psp_exit_plan_request(PspExitPlan *plan, PspExitCause cause);

/* Independent physical obligations retained after controlled shutdown. The
   bits report what could not safely be freed; callers never choose them. */
enum {
    PSP_SHUTDOWN_RETAIN_MEDIA_BACKEND = 1u << 0,
    PSP_SHUTDOWN_RETAIN_MEDIA_DMA = 1u << 1,
    PSP_SHUTDOWN_RETAIN_TRANSPORT = 1u << 2,
    PSP_SHUTDOWN_RETAIN_NETWORK_STACK = 1u << 3
};

typedef struct {
    uint32_t retained;
} PspShutdownReport;

/* Persistent state owned by one interactive-loop invocation. These are
   operation records and counters, not parallel media/network control state. */
typedef struct {
    uint32_t previous_buttons;
    uint64_t navigation_job_started_us;
    PspTabTransition tab_transition;
    PspRecoveryTracker recovery;
    PspExitPlan exit;
    PspReaderNavigation reader_navigation;
    bool lifecycle_retry_available;
    char lifecycle_retry_url[NAVIGATION_URL_LIMIT];
    /* Callback-supervisor input waiting for the browser thread to regain a
       usable media pipeline. This is an input mailbox, not lifecycle state. */
    PspUiMediaIntent deferred_media_intent;
    bool deferred_media_intent_pending;
    bool validation_media_play_injected;
    bool validation_media_play_confirmed;
    bool media_stability_active;
    bool media_stability_auto_exit;
    uint64_t media_stability_started_us;
    uint64_t media_stability_next_sample_us;
    bool media_stability_forward_seek;
    bool media_stability_backward_seek;
    unsigned media_stability_loops;
    PspMediaStabilitySkew media_stability_skew;
    unsigned media_stability_min_free;
    unsigned media_stability_min_largest;
    int media_stability_start_capacity;
    int media_stability_start_percent;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    PspPowerTest power_test;
    PspPowerAutoTest power_auto;
    bool power_auto_boot_pending;
#endif
    PspScreenshotJob screenshot;
} PspInteractiveState;

/* src/psp_app/psp_app_input.c. Callback-thread media input crosses one
   generation-bearing supervisor fence, then returns to the ordinary browser-
   thread receiver through these helpers. */
void psp_app_capture_supervisor_media_intent(
    PspInteractiveState *interactive);
bool psp_app_dispatch_deferred_media_intent(
    PspMediaSession *media, PspInteractiveState *interactive);

void psp_home_sync_ui(
    PspUiState *ui, PspHomeSurface *surface, const BrowserProfile *profile,
    const BrowserTabs *tabs, bool engine_ready);
PspHomeTargetKind psp_home_target_kind(
    const PspHomeSurface *surface, size_t row, size_t *target);
const char *psp_home_target_url(
    const PspHomeSurface *surface, size_t row,
    const BrowserProfile *profile);
void psp_collections_sync_ui(
    PspUiState *ui, PspCollectionsSurface *surface,
    const BrowserProfile *profile, const OfflineLibrary *library,
    PspUiCollectionSection section);
const char *psp_collections_row_url(
    const PspCollectionsSurface *surface, size_t row);
PspUiCollectionSection psp_collections_action_section(
    PspUiAction action, PspUiCollectionSection current);

/* ---- UI command receiver --------------------------------------------- *
 *
 * PspApp is a borrowed view over the canonical process,
 * browser, interactive, and engine-view records. It owns none of them and
 * carries no lifecycle authority: boot constructs the owners, the named
 * interactive loop borrows them, and controlled teardown consumes them only
 * after the media/network machines have decided their terminal outcome.
 * Copying an owner into this view would lie about lifetime.
 *
 * PspAppFrameState is different by construction: its three values are
 * sampled afresh for one frame and cannot escape that iteration.
 */
typedef struct {
    PspProcessResources *process;
    PspBrowserResources *browser;

    PspEngineViews *views;

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *network;
    PspNetworkLifecycle *network_lifecycle;
    bool *update_check_pending;
    bool *update_check_running;
#endif
    PspInteractiveState *interactive;
} PspApp;

/* Rebuilt every iteration of the interactive loop. */
typedef struct {
    uint64_t ui_sample_us;
    bool page_dirty;
    bool pointer_activation;
} PspAppFrameState;

/* src/psp_app/psp_app_surfaces.c -- declared after PspApp because they take
   it. Every internal-home navigation goes through these two; see the
   definitions for why the classifier and the routing must stay paired. */
void psp_show_native_home(PspApp *app);
bool psp_route_native_home(PspApp *app, const char *url);

/* src/psp_app/psp_app_actions.c */
void psp_app_dispatch_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent);
/* src/psp_app/psp_app_settings.c */
void psp_app_apply_setting(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent);
void psp_app_refresh_network_profile_label(PspApp *app, int profile);
bool psp_app_edit_developer_update_url(
    PspApp *app, PspAppFrameState *frame);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
bool psp_app_set_video_decoder(PspApp *app);
void psp_app_seed_video_decoder_choice(
    PspUiState *ui, const PspBootConfig *config);
#endif

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* src/psp_app/psp_app_input_script.c. Present only in validation builds;
   shipping EBOOTs link neither the glue nor the parser. */
bool psp_input_script_begin(
    const TilefinchInstallPaths *install_paths, const char *argv0,
    const char *name);
bool psp_input_script_running(void);
void psp_input_script_interrupt_by_user(void);
bool psp_input_script_frame(
    PspUiInput *input, bool ready);
bool psp_input_script_busy_frame(PspUiInput *input);
void psp_input_script_observe(
    const PspUiIntent *intent, const PspUiState *ui);
void psp_input_script_observe_media(
    const PspUiMediaIntent *intent, const PspUiMediaState *media);
void psp_input_script_capture_live_mark(
    const uint16_t *frame, size_t pixels, size_t stride_pixels);
void psp_input_script_capture_named(
    const char *mark, const uint16_t *frame, size_t pixels,
    size_t stride_pixels);
void psp_input_script_summary(void);
#endif

#ifdef TILEFINCH_PSP_LIVE_NETWORK
struct PspNetworkLifecycle {
    PspNetworkSupervisor machine;
    PspNetworkRequestTable requests;
    uint64_t next_generation;
    uint64_t request_generation[PSP_NETWORK_REQUEST_COUNT];
    PspNetworkRejoinOperation rejoin;
    PspNetworkShutdownOperation shutdown;
    uint64_t next_health_probe_us;
    uint64_t drain_deadline_us;
    uint32_t health_probes;
    uint32_t regressions;
    bool rejoin_active;
    bool shutdown_active;
    bool quiesce_requested;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint32_t events;
    uint32_t mismatches;
    uint32_t violations;
    uint32_t lease_wedges;
    unsigned trace_head;
    unsigned trace_count;
    PspNetworkSupervisorEventType trace_event[16];
    PspNetworkSupervisorState trace_from[16];
    PspNetworkSupervisorState trace_to[16];
    PspNetworkSupervisorState trace_expected[16];
    uint32_t trace_violations[16];
    const char *trace_checkpoint[16];
#endif
};

/* src/psp_app/psp_app_network.c */
bool psp_network_status_active(PspNetworkStatus status);
void psp_network_lifecycle_init(PspNetworkLifecycle *lifecycle);
void psp_network_lifecycle_bind(PspNetworkLifecycle *lifecycle);
void psp_network_lifecycle_request(
    PspNetworkLifecycle *lifecycle, PspNetworkRequester requester,
    bool active, int profile_index, const PspNetwork *network,
    PspNetworkSupervisorState expected, const char *checkpoint);
void psp_network_lifecycle_ladder_terminal(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    const char *checkpoint);
void psp_network_lifecycle_suspend(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    bool retain_ready_stack, const char *checkpoint);
void psp_network_lifecycle_resume_result(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    bool retained_ready, bool healthy, const char *checkpoint);
void psp_network_lifecycle_report(PspNetworkLifecycle *lifecycle);
bool psp_network_lifecycle_started(const PspNetworkLifecycle *lifecycle);
bool psp_network_lifecycle_ready(const PspNetworkLifecycle *lifecycle);
bool psp_network_lifecycle_warming(const PspNetworkLifecycle *lifecycle);
void psp_network_lifecycle_pump(
    PspNetworkLifecycle *lifecycle, PspNetwork *network,
    uint64_t now_us);
void psp_report_network_result(PspNetwork *network);
bool psp_connect_network(PspNetwork *network, int profile_index,
                         const uint16_t *frame, PspUiState *ui);
bool psp_shutdown_network_logged(PspNetwork *network);
bool psp_ensure_network_for_navigation(
    PspNetwork *network, PspNetworkLifecycle *lifecycle, int profile_index,
    const char *method, const char *url, bool present_destination,
    const uint16_t *frame, PspUiState *ui);
#endif

/* User-initiated optional-model work is deliberately cold. Keeping its
   orchestration outside main preserves the PSP hot-symbol ratchet and makes
   ordinary page/video frames pay only this one inactive call. */
bool psp_voice_component_handle_frame(
    PspApp *app, const PspUiIntent *intent, uint64_t now_us);
bool psp_glyph_component_handle_frame(
    PspApp *app, const PspUiIntent *intent, uint64_t now_us);

#endif /* TILEFINCH_PSP_APP_INTERNAL_H */
