#include "tilefinch/browser_engine.h"
#include "tilefinch/file_download.h"
#include "tilefinch_test_faults.h"
#include "tilefinch_compiler.h"
#include "tilefinch/page_find.h"
#include "tilefinch/linked_video_preview.h"
#include "tilefinch/platform.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/site_identity.h"
#include "tilefinch/youtube_lite.h"
#include "tilefinch/youtube_resolver.h"
#include "tilefinch/ui_language.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#define KIB (1024u)
#define MIB (1024u * 1024u)
#define BROWSER_PAINT_AHEAD_TILE_CAPACITY 24u
#define BROWSER_PROVISIONAL_FINAL_RESERVE (1u * MIB)
/* Tile raster between frontend checkpoints while a preview is painted. */
#define BROWSER_PROVISIONAL_RASTER_SLICE_US UINT64_C(6000)
#define BROWSER_FONT_IDLE_READ_BYTES (16u * KIB)
#define BROWSER_BLANK_READER_SETTLE_TICKS 12u
#define BROWSER_RECOVERY_SETTLE_US UINT64_C(250000)

/* What a history traversal restores in its target document. */
typedef struct {
    int scroll_y;
    int focus_kind;
    size_t focus_index;
    uint64_t focus_identity;
    NavigationHistoryControl controls[NAVIGATION_HISTORY_CONTROL_LIMIT];
    size_t control_count;
} BrowserHistoryView;

typedef struct {
    NavigationLoad *load;
    SiteAdapterLoad *adapter;
    DocumentBacking previous_backing;
    BrowserNavigationJobStatus status;
    TilefinchDiagnosticSubsystem failure_subsystem;
    TilefinchDiagnosticCode failure_code;
    size_t previous_history_index;
    BrowserHistoryView restore;
    BrowserNavigationJobMetrics metrics;
    uint64_t started_us;
    uint64_t generation;
    bool previous_backing_valid;
    bool hooks_installed;
    bool history_move;
    bool history_forward;
    bool record_history;
    char url[NAVIGATION_URL_LIMIT];
    char download_filename[FILE_DOWNLOAD_FILENAME_LIMIT];
} BrowserEngineNavigationWork;

/* The load preview on screen. NONE: nothing. FRAME: a frame is presented
   but nothing can move it (a one-off paint, or the scrollable preview was
   dropped). LIVE: a tile cache over the stream's retained preview composes
   any scroll position. FILLING: live, with visible tiles still to
   rasterize (a checkerboard on screen). */
typedef enum {
    BROWSER_PREVIEW_VIEW_NONE = 0,
    BROWSER_PREVIEW_VIEW_FRAME,
    BROWSER_PREVIEW_VIEW_LIVE,
    BROWSER_PREVIEW_VIEW_FILLING
} BrowserPreviewView;

/* An activation on the preview that waits for the page: IDLE, WAITING for
   the commit, then READY for the frontend to take once. */
typedef enum {
    BROWSER_DEFERRED_ACTIVATION_IDLE = 0,
    BROWSER_DEFERRED_ACTIVATION_WAITING,
    BROWSER_DEFERRED_ACTIVATION_READY
} BrowserDeferredActivation;

static bool browser_preview_view_live(BrowserPreviewView view)
{
    return view == BROWSER_PREVIEW_VIEW_LIVE
        || view == BROWSER_PREVIEW_VIEW_FILLING;
}

/* Largest url-encoded body a script form submission may send. */
#define BROWSER_ENGINE_SCRIPT_FORM_BODY_LIMIT (64u * 1024u)
/* Nodes a whole-document walk (history controls, provider controls, a
   restored focus target) visits at most. */
#define BROWSER_ENGINE_DOM_WALK_LIMIT 8192u

typedef enum {
    YOUTUBE_SIGNIN_IDLE, YOUTUBE_SIGNIN_ACCOUNT, YOUTUBE_SIGNIN_RECOVERING,
    YOUTUBE_SIGNIN_HANDOFF, YOUTUBE_SIGNIN_VERIFYING, YOUTUBE_SIGNIN_VERIFIED,
    YOUTUBE_SIGNIN_FAILED, YOUTUBE_SIGNIN_CANCELLED, YOUTUBE_SIGNIN_MANUAL
} YoutubeSigninState;

/* YouTube provider state, owned by browser_engine/youtube.inc. The facade's
   navigation, commit, finish, cancel and idle paths reach it through that
   file's hooks; it still reads the compact-results preference for site
   adapters and the topics-publishing flag when preparing a candidate shell. */
typedef struct {
    bool compact_results;
    bool topics;
    uint64_t login_verified;
    unsigned login_events;
    bool signin_diagnostics;
    unsigned signin_retries;
    YoutubeSigninState signin_state;
    YoutubeLiteFeedDiagnostics signin_feed;
    long handoff_http_status;
    bool handoff_service_origin;
    unsigned feed_build_outcome, feed_publication_outcome;
    bool header_dirty;
    unsigned header_attempts;
    bool topics_attempted;
    uint64_t topics_generation;
    YoutubeLiteLoadJob *topics_job;
    YoutubeLiteFeedJob *feed_job;
    YoutubeLiteFeedJob *like_job;
    YoutubeLiteFeedJob *account_job;
    lxb_dom_node_t *account_nodes[16];
    size_t account_node_count;
    YoutubeLiteAccountAction account_action;
    BrowserYoutubeFeedState account_state;
    BrowserYoutubeFeedState account_painted_state;
    uint64_t account_generation;
    uint64_t account_shell;
    size_t account_layout_generation;
    BrowserYoutubeFeedState like_state;
    bool like_selected;
#ifndef __PSP__
    YoutubeLiteRatingDiagnostics rating_diagnostics;
    YoutubeLiteAccountDiagnostics account_diagnostics;
    unsigned history_metadata;
#endif
    uint64_t like_generation;
    BrowserYoutubeFeedState like_painted_state;
    size_t like_layout_generation;
    uint64_t like_shell;
    lxb_dom_node_t *like_node;
    lxb_dom_node_t *unlike_node;
    lxb_dom_node_t *signout_node;
    uint64_t like_node_generation;
    YoutubeLiteDocument feed_pending;
    uint64_t feed_pending_identity;
    BrowserYoutubeFeedState feed_state;
    uint64_t browsing_generation;
    bool browsing;
    bool refresh_confirmed;
    BrowserYoutubeFeedState status_painted_state;
    size_t status_layout_generation;
    uint64_t status_shell;
    bool topics_publishing;
} BrowserEngineYoutube;

struct BrowserEngine {
    BrowserConfig config;
    Budget budget;
    /* The last script form submission's body when it outgrew
       ControllerAction.body; the action points here. */
    char *script_form_body;
    BrowserSession session;
    ContentBlocker *content_blocker;
    NavigationSession navigation;
    BrowserController controller;
    BrowserController candidate_controller;
    FontSet fonts;
    /* Completed optional reads are private until the whole bounded batch can
       switch metrics in one transaction, not one page relayout per face. */
    FontSet staged_fonts;
    FontFaceLoad *font_load;
    FontSetFaceMask font_loading_face;
    uint64_t font_publication_retry_us;
    TileCache render;
    TileCache candidate_render;
    /* The load preview: one frame, composed at any scroll position from a
       tile cache over the stream's retained preview layout. The layout is
       borrowed and revalidated (navigation_load_retained_preview) before
       every use. A repaint rasterizes into the other cache while the front
       one keeps serving scrolls, then they swap. */
    BrowserPreviewView provisional_view;
    uint16_t *provisional_frame;
    size_t provisional_frame_pixels;
    TileCache provisional_renders[2];
    /* The cache over provisional_layout (LIVE and FILLING). */
    unsigned provisional_front;
    const LayoutDocument *provisional_layout;
    int provisional_scroll_y;
    int provisional_maximum_scroll;
    /* Of the live preview, for the const viewport accessor (which must not
       read the borrowed layout without revalidating it). */
    int provisional_content_end;
    int provisional_viewport_height;
    /* Focus on the preview (a link or control node of the pending DOM),
       carried into the committed page; an activation that has to wait for
       it (a control, a same-page link) runs once the page commits. */
    lxb_dom_node_t *provisional_focus_node;
    BrowserDeferredActivation provisional_activation;
    /* Direction of the last scroll: where paint-ahead goes (+1 below). */
    int paint_ahead_direction;
    DocumentBacking backing;
    TilefinchDiagnostics diagnostics;
    uint16_t *framebuffer;
    size_t framebuffer_pixels;
    BrowserEngineState state;
    bool session_ready;
    bool navigation_ready;
    /* A script navigation taken for the frontend keeps its referer and
       user activation for the navigation job begun next. */
    bool script_navigation_attributed;
    bool fonts_ready;
    uint64_t startup_deferral_started_us;
    /* Idle turns: the script-bytecode store or persistent-tier step
       skipped one turn for an image retarget that changed a surface. */
    bool idle_cache_waited;
    const void *startup_deferral_runtime;
    uint64_t startup_deferral_generation;
    /* A provisional page's layout completion is running: its cooperative
       checkpoints may page and paint the committed page (see
       browser_engine_completion_scroll_page). */
    bool layout_completion_running;
    /* A completion-time scroll left tiles to fill; one happened at all
       (the frame loop then repaints, whatever the completion's outcome). */
    bool completion_frame_pending;
    bool completion_scrolled;
    bool font_publication_guarded;
    FontSetFaceMask font_requested_faces;
    FontSetFaceMask font_failed_faces;
    bool controller_ready;
    bool render_ready;
    bool candidate_controller_ready;
    bool candidate_render_ready;
    bool candidate_shell_prepared;
    bool replacement_frame_frozen;
    bool forced_dark;
    /* Whether the committed page declares its own dark rendering, cached
       per navigation generation (see browser_engine_page_declares_dark). */
    bool page_dark_valid;
    bool page_dark;
    uint64_t page_dark_generation;
    BrowserEngineYoutube youtube;
    bool autofocus_pending;
    size_t render_relayout_generation;
    uint64_t render_shell_serial;
    uint64_t initial_tile_hold_started_us;
    bool initial_tile_hold;
    size_t render_focus_paint_generation;
    size_t unchanged_runtime_frames_suppressed;
    PageFindIndex find;
    uint64_t find_navigation_generation;
    size_t find_layout_generation;
    TilefinchDiagnosticSubsystem last_diagnostic_subsystem;
    TilefinchDiagnosticCode last_diagnostic_code;
    /* Compile-admission rejections already observed on the current page, so
       each rejection produces one warning event rather than one per frame. */
    size_t observed_compile_rejections;
    uint64_t blank_reader_recovery_generation;
    size_t blank_reader_recovery_first_runtime_tick;
    uint64_t basic_view_recovery_generation;
    size_t basic_view_recovery_first_runtime_tick;
    uint64_t declared_media_recovery_generation;
    size_t declared_media_recovery_first_runtime_tick;
    lxb_dom_node_t *declared_media_card;
    uint64_t declared_media_card_generation;
    uint64_t declared_media_outcome_generation;
    size_t declared_media_outcome_content_generation;
    BrowserDeclaredMediaCard declared_media_outcome;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    BrowserEngineCreationMetrics creation_metrics;
#endif
    BrowserEngineNavigationWork navigation_work;
    char last_error[256];
};

/* Lexbor exposes one process-global allocator table rather than a parser-local
   allocator context. Keep the facade singleton explicit so a later engine
   cannot redirect an earlier engine's DOM allocations or leave a dangling
   allocator owner behind when it is destroyed. */
static BrowserEngine *active_engine;

static bool browser_engine_restore_controls(
    NavigationSession *navigation,
    const NavigationHistoryControl *controls, size_t count);

#if !defined(__PSP__)
static uint64_t browser_engine_test_recovery_time_us;

void browser_engine_test_refuse_next_render_shell_init(void)
{
    tilefinch_test_faults()->refuse_next_render_shell_init = true;
}

void browser_engine_test_set_recovery_time_us(uint64_t now_us)
{
    browser_engine_test_recovery_time_us = now_us;
}

static bool browser_engine_test_consume_render_shell_refusal(void)
{
    if (!tilefinch_test_faults()->refuse_next_render_shell_init) return false;
    tilefinch_test_faults()->refuse_next_render_shell_init = false;
    return true;
}
#else
static bool browser_engine_test_consume_render_shell_refusal(void)
{
    return false;
}
#endif

static uint64_t browser_engine_recovery_now_us(void)
{
#if !defined(__PSP__)
    if (browser_engine_test_recovery_time_us != 0u)
        return browser_engine_test_recovery_time_us;
#endif
    return tilefinch_platform_monotonic_time_us();
}

static bool browser_engine_input_ready(const BrowserEngine *engine);
static bool browser_engine_complete_layout(BrowserEngine *engine);
static bool browser_engine_settle_bounded_layout(BrowserEngine *engine);
static bool browser_engine_finish_input(BrowserEngine *engine,
                                        bool succeeded);

/* YouTube provider hooks the facade calls before browser_engine/youtube.inc,
   which is included ahead of browser_engine_run_idle_work. */
static void browser_engine_youtube_on_navigation_begin(
    BrowserEngine *engine, const char *url, bool signin_recovery);
static const char *browser_engine_youtube_commit_url(const char *url);
static void browser_engine_youtube_on_commit(BrowserEngine *engine,
                                             bool committed);
static void browser_engine_youtube_on_finish(
    BrowserEngine *engine, BrowserNavigationJobStatus outcome,
    const SiteAdapterLoadMetrics *adapter);
static size_t browser_engine_youtube_cancel(BrowserEngine *engine);
static void browser_engine_youtube_browsing(BrowserEngine *engine);
static bool browser_engine_youtube_result_focused(BrowserEngine *engine);
static bool browser_engine_youtube_activate(BrowserEngine *engine,
                                            ControllerAction *action,
                                            bool activated, bool *finished);

static bool browser_engine_find_refresh(BrowserEngine *engine);
static void browser_engine_deferred_reader_images_ready(
    void *opaque, NavigationSession *navigation);
static void browser_engine_image_table_moved(
    void *opaque, NavigationSession *navigation, uintptr_t previous_base,
    size_t previous_count);

static void copy_error(char *output, size_t capacity, const char *message)
{
    if (output == NULL || capacity == 0) return;
    snprintf(output, capacity, "%s", message == NULL ? "" : message);
}

static bool emit_diagnostic(BrowserEngine *engine,
                            TilefinchDiagnosticSeverity severity,
                            TilefinchDiagnosticSubsystem subsystem,
                            TilefinchDiagnosticCode code,
                            const char *name, const char *detail,
                            uint64_t value, uint64_t auxiliary)
{
    if (engine == NULL) return false;
    engine->last_diagnostic_subsystem = subsystem;
    engine->last_diagnostic_code = code;
    return tilefinch_diagnostics_emit(
        &engine->diagnostics, severity, subsystem, code, name, detail,
        value, auxiliary);
}

static bool set_error_code(BrowserEngine *engine,
                           TilefinchDiagnosticSubsystem subsystem,
                           TilefinchDiagnosticCode code,
                           const char *name, const char *message)
{
    if (engine != NULL) {
        snprintf(engine->last_error, sizeof(engine->last_error), "%s",
                 message == NULL ? "browser engine error" : message);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_ERROR, subsystem, code, name,
            engine->last_error, 0, 0);
    }
    return false;
}

static void clear_error(BrowserEngine *engine)
{
    if (engine == NULL) return;
    engine->last_error[0] = '\0';
}

static bool copy_path(char output[BROWSER_ENGINE_PATH_LIMIT],
                      const char *path)
{
    if (path == NULL) path = "";
    size_t length = strlen(path);
    if (length >= BROWSER_ENGINE_PATH_LIMIT) return false;
    memcpy(output, path, length + 1);
    return true;
}

static bool fixed_string_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

void browser_device_profile_psp3000(BrowserDeviceProfile *profile)
{
    if (profile == NULL) return;
    *profile = (BrowserDeviceProfile) {
        .name = "psp-3000-333mhz",
        .target_clock_mhz = 333,
        .framebuffer_width = 480,
        .framebuffer_height = 272,
        .navigation_viewport_width = 480,
        .recommended_memory_limit = BROWSER_PSP_RECOMMENDED_CONTENT_LIMIT,
        .minimum_non_page_reserve =
            BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE,
        /* One screen (up to sixteen 128 px tiles at 480x272) plus two rows
           of paint-ahead. Only the first eight are reserved; the rest are
           admitted with budget headroom and reclaimed first. */
        .maximum_tile_capacity = 24
    };
}

static size_t browser_engine_reclaim_accelerators(void *opaque,
                                                 size_t needed_bytes)
{
    BrowserEngine *engine = opaque;
    size_t reclaimed = browser_session_script_bytecode_reclaim(
        &engine->session, needed_bytes);
    if (reclaimed < needed_bytes)
        reclaimed += browser_session_decoded_image_reclaim(
            &engine->session, needed_bytes - reclaimed);
    /* Then the run measurements. A page that still needs room after the
       session caches (its script heap reaching the page reserve) will need
       it again: they stay off for the page rather than being re-admitted
       and freed at every growth step. */
    if (reclaimed < needed_bytes)
        reclaimed += navigation_yield_layout_text_measures(
            &engine->navigation, engine->navigation_work.load);
    /* And the style-attribute scan memos, under the same rule. */
    if (reclaimed < needed_bytes)
        reclaimed += navigation_yield_style_memos(
            &engine->navigation, engine->navigation_work.load);
    return reclaimed;
}

static void browser_engine_reclaim_for_full_relayout(void *opaque)
{
    BrowserEngine *engine = opaque;
    /* The required rebuild will replace these accelerators anyway. This is
       an owner boundary, not an allocation hook: cache metadata may be
       retired here without invalidating a loader's borrowed response. */
    if (engine->render_ready) (void)tile_cache_reclaim_optional(&engine->render);
    if (engine->session_ready)
        (void)browser_session_cache_reclaim(&engine->session, SIZE_MAX);
}

void browser_config_init(BrowserConfig *config,
                         const BrowserDeviceProfile *profile)
{
    if (config == NULL) return;
    BrowserDeviceProfile selected;
    if (profile == NULL) browser_device_profile_psp3000(&selected);
    else selected = *profile;
    *config = (BrowserConfig) {
        .device = selected,
        .non_page_memory_reserve = selected.minimum_non_page_reserve,
        .memory_limit = selected.recommended_memory_limit,
        .history_capacity = 16,
        .session_cache_limit = 1u * MIB,
        .module_bytecode_cache_limit = BROWSER_MODULE_BYTECODE_CACHE_BYTES,
        .classic_bytecode_cache_limit = BROWSER_CLASSIC_BYTECODE_CACHE_BYTES,
        .maximum_document_bytes = 8u * MIB,
        .navigation_timeout_ms = 20000,
        .navigation_replacement_mode =
            NAVIGATION_REPLACEMENT_TRANSACTIONAL,
        .declared_css_width = 0,
        .declared_css_height = 0,
        .progressive_first_paint = true,
        .relayout_preview_threshold_us = UINT64_C(150000),
        .tile_capacity = selected.maximum_tile_capacity
                             < BROWSER_PAINT_AHEAD_TILE_CAPACITY
                         ? selected.maximum_tile_capacity
                         : BROWSER_PAINT_AHEAD_TILE_CAPACITY,
        .idle_work_budget_us = 500,
        .idle_work_maximum_units = 8,
        .javascript = {
            .enabled = false,
            .document_scripts_enabled = false,
            .game_audio_internal_slots = true,
            .heap_limit = 4u * MIB,
            .runtime_timeout_ms = 10000,
            .maximum_scripts = 48,
            .maximum_total_bytes = 4u * MIB,
            /* The realistic PSP execution policy admits one bounded Game
               Profile script up to 512 KiB. Keep the ordinary aggregate
               quota unchanged; strict mode narrows this unit to 256 KiB. */
            .maximum_file_bytes = 512u * KIB,
            .network_timeout_ms = 10000
        },
        /* About 22 us per layout work unit on the PSP-3000: a page above
           200,000 units would spend more than four seconds reflowing for an
           optional font or image batch. */
        .startup_resource_deferral_us = 3000000u,
        .resources = {
            .maximum_publication_work_units = 200000u,
            .enabled = false,
            .web_fonts_enabled = true,
            .web_font_mode = WEB_FONTS_SMART,
            .maximum_stylesheets = 16,
            .maximum_stylesheet_bytes = 2u * MIB,
            .maximum_stylesheet_file_bytes = 512u * KIB,
            .maximum_font_attempts = 8,
            .maximum_font_bytes = 256u * KIB,
            /* A useful variable subset may exceed 96 KiB. Repartition the
               unchanged 256 KiB total; this does not enlarge page storage. */
            .maximum_font_file_bytes = 128u * KIB,
            .maximum_font_face_backend_bytes = 256u * KIB,
            .maximum_images = 24,
            .maximum_image_bytes = 4u * MIB,
            .maximum_image_file_bytes = 1u * MIB,
            .maximum_decoded_image_bytes = 4u * MIB,
            .timeout_ms = 10000
        },
        .fonts = {
            .maximum_publication_work_units = 200000u,
            .enabled = false,
            .maximum_total_bytes = 1536u * KIB
        }
    };
    (void) script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
        &config->javascript.execution_policy);
}

bool browser_config_apply_psp_memory_profile(
    BrowserConfig *config, BrowserPspMemoryProfile profile)
{
    if (config == NULL) return false;
    if (profile == BROWSER_PSP_MEMORY_REALISTIC)
        return browser_config_apply_psp_app_defaults(config);
    if (profile != BROWSER_PSP_MEMORY_STRICT) return false;
    config->memory_limit = BROWSER_PSP_STRICT_CONTENT_LIMIT;
    config->non_page_memory_reserve =
        BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE;
    config->history_capacity = 8;
    config->session_cache_limit = 512u * KIB;
    config->module_bytecode_cache_limit =
        BROWSER_MODULE_BYTECODE_CACHE_STRICT_BYTES;
    config->classic_bytecode_cache_limit =
        BROWSER_CLASSIC_BYTECODE_CACHE_STRICT_BYTES;
    config->tile_capacity = config->device.maximum_tile_capacity
            < BROWSER_PAINT_AHEAD_TILE_CAPACITY
        ? config->device.maximum_tile_capacity
        : BROWSER_PAINT_AHEAD_TILE_CAPACITY;
    config->javascript.heap_limit = 4u * MIB;
    config->javascript.installed_app_heap_limit =
        BROWSER_PSP_STRICT_INSTALLED_APP_HEAP;
    if (config->javascript.maximum_total_bytes > 1u * MIB)
        config->javascript.maximum_total_bytes = 1u * MIB;
    if (config->javascript.maximum_file_bytes > 256u * KIB)
        config->javascript.maximum_file_bytes = 256u * KIB;
    if (config->resources.maximum_font_file_bytes > 128u * KIB)
        config->resources.maximum_font_file_bytes = 128u * KIB;
    return script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_STRICT,
        &config->javascript.execution_policy);
}

bool browser_config_apply_psp_app_defaults(BrowserConfig *config)
{
    if (config == NULL) return false;
    config->memory_limit = (size_t) BROWSER_PSP_APP_MEMORY_LIMIT_MB * MIB;
    config->history_capacity = BROWSER_PSP_APP_HISTORY_CAPACITY;
    config->session_cache_limit = BROWSER_PSP_APP_SESSION_CACHE_BYTES;
    config->maximum_document_bytes = BROWSER_PSP_APP_DOCUMENT_BYTES;
    config->navigation_timeout_ms = BROWSER_PSP_APP_NAVIGATION_TIMEOUT_MS;
    /* One screen plus two rows of optional paint-ahead (only the first
       eight tiles are reserved up front). */
    config->tile_capacity = config->device.maximum_tile_capacity;
    config->javascript.enabled = true;
    config->javascript.document_scripts_enabled = true;
    config->javascript.heap_limit =
        (size_t) BROWSER_PSP_APP_SCRIPT_HEAP_MB * MIB;
    config->javascript.installed_app_heap_limit =
        (size_t) BROWSER_PSP_APP_INSTALLED_APP_HEAP_MB * MIB;
    config->javascript.runtime_timeout_ms =
        BROWSER_PSP_APP_SCRIPT_TIMEOUT_MS;
    config->javascript.maximum_scripts = BROWSER_PSP_APP_SCRIPT_COUNT;
    config->javascript.maximum_total_bytes =
        (size_t) BROWSER_PSP_APP_SCRIPT_TOTAL_MB * MIB;
    config->javascript.maximum_file_bytes =
        (size_t) BROWSER_PSP_APP_SCRIPT_FILE_KB * KIB;
    config->javascript.network_timeout_ms =
        BROWSER_PSP_APP_NETWORK_TIMEOUT_MS;
    config->resources.enabled = true;
    /* A single useful face may use the shared encoded-font allowance.
       Aggregate downloads and retained decoded-face storage do not grow. */
    config->resources.maximum_font_file_bytes =
        config->resources.maximum_font_bytes;
    /* Modern mobile shells split their critical component rules across many
       small sheets and occasionally one 700 KiB decoded module. Keep the
       limits hard, but retain a small tail allowance for late mobile-header
       and accessibility components. The parse-admission gate still uses
       actual page headroom. */
    config->resources.maximum_stylesheets = BROWSER_PSP_APP_STYLESHEETS;
    config->resources.maximum_stylesheet_bytes =
        BROWSER_PSP_APP_STYLESHEET_BYTES;
    config->resources.maximum_stylesheet_file_bytes =
        BROWSER_PSP_APP_STYLESHEET_FILE_BYTES;
    config->resources.maximum_images = BROWSER_PSP_APP_IMAGES;
    config->resources.maximum_image_bytes = BROWSER_PSP_APP_IMAGE_BYTES;
    /* Keep the aggregate image ceiling, but let one visible mobile hero
       consume up to a third of it: CDNs commonly produce display-sized PNG
       fallbacks just above 384 KiB. */
    config->resources.maximum_image_file_bytes =
        BROWSER_PSP_APP_IMAGE_FILE_BYTES;
    config->resources.maximum_decoded_image_bytes =
        BROWSER_PSP_APP_DECODED_IMAGE_BYTES;
    config->resources.timeout_ms = BROWSER_PSP_APP_NETWORK_TIMEOUT_MS;
    return script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
        &config->javascript.execution_policy);
}

bool browser_config_apply_script_limits(
    BrowserConfig *config, ScriptExecutionProfile profile,
    size_t maximum_scripts, size_t maximum_total_bytes,
    size_t maximum_file_bytes)
{
    if (config == NULL || !script_execution_policy_for_profile(
            profile, &config->javascript.execution_policy)) return false;
    config->javascript.maximum_scripts = maximum_scripts;
    config->javascript.maximum_total_bytes = maximum_total_bytes;
    if (maximum_file_bytes > maximum_total_bytes)
        maximum_file_bytes = maximum_total_bytes;
    size_t maximum_compile_bytes = config->javascript.execution_policy
                                       .maximum_host_compile_source_bytes;
    if (maximum_compile_bytes != 0 && maximum_file_bytes > maximum_compile_bytes)
        maximum_file_bytes = maximum_compile_bytes;
    config->javascript.maximum_file_bytes = maximum_file_bytes;
    return true;
}

bool browser_config_set_font_paths(
    BrowserConfig *config, const char *sans_path, const char *serif_path,
    const char *sans_italic_path, const char *sans_bold_path,
    const char *serif_bold_path, const char *metric_sans_path,
    const char *metric_sans_bold_path, size_t maximum_total_bytes)
{
    if (config == NULL || maximum_total_bytes == 0
        || ((sans_path == NULL || sans_path[0] == '\0')
            && (serif_path == NULL || serif_path[0] == '\0')
            && (sans_italic_path == NULL || sans_italic_path[0] == '\0')
            && (sans_bold_path == NULL || sans_bold_path[0] == '\0')
            && (serif_bold_path == NULL || serif_bold_path[0] == '\0')
            && (metric_sans_path == NULL || metric_sans_path[0] == '\0')
            && (metric_sans_bold_path == NULL
                || metric_sans_bold_path[0] == '\0'))) {
        return false;
    }
    BrowserFontConfig candidate = config->fonts;
    if (!copy_path(candidate.sans_path, sans_path)
        || !copy_path(candidate.serif_path, serif_path)
        || !copy_path(candidate.sans_italic_path, sans_italic_path)
        || !copy_path(candidate.sans_bold_path, sans_bold_path)
        || !copy_path(candidate.serif_bold_path, serif_bold_path)
        || !copy_path(candidate.metric_sans_path, metric_sans_path)
        || !copy_path(candidate.metric_sans_bold_path,
                      metric_sans_bold_path)) {
        return false;
    }
    candidate.maximum_total_bytes = maximum_total_bytes;
    candidate.enabled = true;
    config->fonts = candidate;
    return true;
}

void browser_config_set_staged_font_loading(
    BrowserConfig *config, bool staged)
{
    if (config != NULL) config->fonts.staged_loading = staged;
}

static bool browser_engine_load_font_faces(
    BrowserEngine *engine, FontSetFaceMask faces)
{
    if (engine == NULL || !engine->config.fonts.enabled) return false;
    const BrowserFontConfig *fonts = &engine->config.fonts;
    return font_set_load_selected(
        &engine->fonts, &engine->budget, fonts->sans_path,
        fonts->serif_path, fonts->sans_italic_path,
        fonts->sans_bold_path, fonts->serif_bold_path,
        fonts->metric_sans_path, fonts->metric_sans_bold_path,
        fonts->maximum_total_bytes, faces);
}

static void browser_engine_discard_staged_fonts(BrowserEngine *engine)
{
    font_face_load_destroy(engine->font_load);
    engine->font_load = NULL;
    engine->font_loading_face = 0;
    font_set_destroy(&engine->staged_fonts);
}

#define BROWSER_BASELINE_FONT_FACES ((FontSetFaceMask) (                 \
    FONT_SET_FACE_SANS | FONT_SET_FACE_METRIC_SANS))

/* Built-in provider documents are authored by Tilefinch and have a stable
   typography contract. Load the one page face they use before admitting the
   navigation, so the committed document is never first shown with synthetic
   bold metrics and then laid out again when idle work reaches the font. */
#define BROWSER_PROVIDER_FONT_FACES ((FontSetFaceMask) (                 \
    FONT_SET_FACE_SANS_BOLD | FONT_SET_FACE_METRIC_SANS_BOLD))

bool browser_engine_prepare_native_home_font(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    if (!engine->config.fonts.enabled || engine->fonts.sans.loaded)
        return true;
    if ((engine->font_failed_faces & FONT_SET_FACE_SANS) != 0)
        return false;
    if (!browser_engine_load_font_faces(engine, FONT_SET_FACE_SANS)) {
        /* Keep the fallback stable for this process if the one required file
           is unavailable; the later baseline pump must not make HOME change
           typography after the failure. */
        engine->font_failed_faces |= FONT_SET_FACE_SANS;
        return false;
    }
    return true;
}

const FontFace *browser_engine_native_home_font(const BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
            && engine->fonts.sans.loaded
        ? &engine->fonts.sans : NULL;
}

bool browser_engine_baseline_fonts_ready(const BrowserEngine *engine)
{
    return engine != NULL
        && (!engine->config.fonts.enabled || engine->fonts_ready);
}

bool browser_engine_pump_baseline_fonts(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    if (!engine->config.fonts.enabled) return true;
    if (engine->fonts_ready) return true;
    if ((engine->font_failed_faces & BROWSER_BASELINE_FONT_FACES) != 0)
        return false;
    const FontSetFaceMask order[] = {
        FONT_SET_FACE_SANS, FONT_SET_FACE_METRIC_SANS
    };
    FontSetFaceMask loaded = 0;
    if (engine->fonts.sans.loaded) loaded |= FONT_SET_FACE_SANS;
    if (engine->fonts.metric_sans.loaded)
        loaded |= FONT_SET_FACE_METRIC_SANS;
    for (size_t at = 0; at < sizeof(order) / sizeof(order[0]); at++) {
        if ((loaded & order[at]) != 0) continue;
        if (!browser_engine_load_font_faces(engine, order[at])) {
            engine->font_failed_faces |= order[at];
            return false;
        }
        loaded |= order[at];
        break;
    }
    engine->fonts_ready =
        (loaded & BROWSER_BASELINE_FONT_FACES)
            == BROWSER_BASELINE_FONT_FACES;
    return true;
}

static bool browser_engine_ensure_baseline_fonts(BrowserEngine *engine)
{
    if (engine == NULL || !engine->config.fonts.enabled) return true;
    while (!engine->fonts_ready) {
        if (!browser_engine_pump_baseline_fonts(engine)) return false;
    }
    return true;
}

static FontSetFaceMask browser_engine_loaded_font_faces(
    const BrowserEngine *engine)
{
    if (engine == NULL) return 0;
    FontSetFaceMask loaded = 0;
    if (engine->fonts.sans.loaded) loaded |= FONT_SET_FACE_SANS;
    if (engine->fonts.serif.loaded) loaded |= FONT_SET_FACE_SERIF;
    if (engine->fonts.sans_italic.loaded)
        loaded |= FONT_SET_FACE_SANS_ITALIC;
    if (engine->fonts.sans_bold.loaded) loaded |= FONT_SET_FACE_SANS_BOLD;
    if (engine->fonts.serif_bold.loaded)
        loaded |= FONT_SET_FACE_SERIF_BOLD;
    if (engine->fonts.metric_sans.loaded)
        loaded |= FONT_SET_FACE_METRIC_SANS;
    if (engine->fonts.metric_sans_bold.loaded)
        loaded |= FONT_SET_FACE_METRIC_SANS_BOLD;
    return loaded;
}

bool browser_engine_prepare_navigation_fonts(
    BrowserEngine *engine, const char *method, const char *url)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || method == NULL || url == NULL) return false;
    if (!browser_engine_ensure_baseline_fonts(engine)) return false;
    if (!site_adapter_navigation_requires_stable_typography(method, url)
        || !engine->config.fonts.enabled) return true;

    FontSetFaceMask requested = BROWSER_PROVIDER_FONT_FACES;
    engine->font_requested_faces |= requested;
    FontSetFaceMask missing = requested
        & (FontSetFaceMask) ~(browser_engine_loaded_font_faces(engine)
                             | engine->font_failed_faces);
    if (missing == 0) return true;

    /* Navigation already suppresses optional idle work. Drop any partial
       staging allocation as well, otherwise it can make the trusted provider
       face lose an otherwise-valid bounded-budget admission. */
    browser_engine_discard_staged_fonts(engine);

    FontSetFaceMask loaded_before = browser_engine_loaded_font_faces(engine);
    const FontSetFaceMask order[] = {
        FONT_SET_FACE_SANS_BOLD, FONT_SET_FACE_METRIC_SANS_BOLD
    };
    for (size_t at = 0; at < sizeof(order) / sizeof(order[0]); at++) {
        FontSetFaceMask face = order[at];
        if ((missing & face) == 0) continue;
        if (!browser_engine_load_font_faces(engine, face)) {
            /* Optional faces fail stable: keep the fallback for the whole
               document instead of revealing a late metric transition. */
            engine->font_failed_faces |= face;
        }
    }
    FontSetFaceMask loaded_after = browser_engine_loaded_font_faces(engine);
    FontSetFaceMask newly_loaded = loaded_after
        & (FontSetFaceMask) ~loaded_before;
    if (newly_loaded == 0) return true;

    /* A still-visible incumbent may also use bold text. Keep rollback pixels
       and geometry coherent before the candidate begins. */
    if ((newly_loaded & FONT_SET_FACE_SANS_BOLD) != 0
        && engine->navigation.page.loaded
        && !navigation_relayout(&engine->navigation)) {
        font_face_destroy(&engine->fonts.sans_bold);
        engine->font_failed_faces |= FONT_SET_FACE_SANS_BOLD;
        newly_loaded &= (FontSetFaceMask) ~FONT_SET_FACE_SANS_BOLD;
    }
    if (engine->render_ready && newly_loaded != 0)
        tile_cache_invalidate_glyphs(&engine->render);
    return true;
}

bool browser_config_validate(const BrowserConfig *config,
                             char *error, size_t error_capacity)
{
#define CONFIG_REQUIRE(condition, message) do {                              \
    if (!(condition)) { copy_error(error, error_capacity, (message));        \
                       return false; }                                       \
} while (0)
    CONFIG_REQUIRE(config != NULL, "configuration is required");
    CONFIG_REQUIRE(fixed_string_terminated(
                       config->device.name, sizeof(config->device.name))
                       && config->device.name[0] != '\0',
                   "device profile name must be bounded and nonempty");
    CONFIG_REQUIRE(config->device.framebuffer_width > 0
                       && config->device.framebuffer_height > 0
                       && config->device.navigation_viewport_width > 0,
                   "device dimensions must be positive");
    CONFIG_REQUIRE(config->device.framebuffer_width <= 4096
                       && config->device.framebuffer_height <= 4096,
                   "device dimensions exceed the bounded renderer limit");
    size_t width = (size_t) config->device.framebuffer_width;
    size_t height = (size_t) config->device.framebuffer_height;
    CONFIG_REQUIRE(width <= SIZE_MAX / height
                       && width * height <= SIZE_MAX / sizeof(uint16_t),
                   "framebuffer dimensions overflow size_t");
    CONFIG_REQUIRE(config->memory_limit != 0,
                   "content memory limit must be nonzero");
    CONFIG_REQUIRE(config->non_page_memory_reserve
                       <= SIZE_MAX - config->memory_limit,
                   "content limit and non-page reserve overflow size_t");
    CONFIG_REQUIRE(config->non_page_memory_reserve
                       >= config->device.minimum_non_page_reserve,
                   "non-page reserve is below the device profile minimum");
    CONFIG_REQUIRE(config->history_capacity != 0
                       && config->history_capacity <= 64,
                   "history capacity must be between 1 and 64");
    CONFIG_REQUIRE(config->session_cache_limit != 0
                       && config->session_cache_limit < config->memory_limit,
                   "session cache must fit inside the content budget");
    CONFIG_REQUIRE(config->module_bytecode_cache_limit
                       < config->memory_limit,
                   "module bytecode cache must fit inside the content budget");
    CONFIG_REQUIRE(config->classic_bytecode_cache_limit
                       < config->memory_limit,
                   "classic bytecode cache must fit inside the content budget");
    CONFIG_REQUIRE(config->maximum_document_bytes != 0
                       && config->navigation_timeout_ms > 0,
                   "navigation limits must be nonzero");
    CONFIG_REQUIRE(
        config->navigation_replacement_mode
                == NAVIGATION_REPLACEMENT_TRANSACTIONAL
            || config->navigation_replacement_mode
                == NAVIGATION_REPLACEMENT_LOW_MEMORY,
        "navigation replacement mode is invalid");
    CONFIG_REQUIRE(
        (config->declared_css_width == 0
         && config->declared_css_height == 0)
            || (config->declared_css_width > 0
                && config->declared_css_width <= 4096
                && config->declared_css_height > 0
                && config->declared_css_height <= 4096),
        "declared CSS viewport must be zero or a bounded dimension pair");
    CONFIG_REQUIRE(config->device.maximum_tile_capacity == 0
                       || config->tile_capacity
                              <= config->device.maximum_tile_capacity,
                   "tile capacity exceeds the device profile");
    CONFIG_REQUIRE(config->idle_work_budget_us > 0
                       && config->idle_work_budget_us <= 1000000
                       && config->idle_work_maximum_units > 0
                       && config->idle_work_maximum_units <= 64,
                   "idle work limits must be bounded and nonzero");
    CONFIG_REQUIRE(!config->javascript.enabled
                       || (config->javascript.heap_limit != 0
                           && config->javascript.runtime_timeout_ms != 0),
                   "JavaScript heap and runtime limits must be nonzero");
    CONFIG_REQUIRE(!config->javascript.document_scripts_enabled
                       || (config->javascript.enabled
                           && config->javascript.maximum_scripts != 0
                           && config->javascript.maximum_total_bytes != 0
                           && config->javascript.maximum_file_bytes != 0
                           && config->javascript.network_timeout_ms > 0),
                   "document scripts require complete JavaScript limits");
    CONFIG_REQUIRE(
        !config->javascript.document_scripts_enabled
            || config->javascript.execution_policy
                       .maximum_host_compile_source_bytes == 0
            || config->javascript.maximum_file_bytes
                 <= config->javascript.execution_policy
                        .maximum_host_compile_source_bytes,
        "script response limit exceeds the host compile admission limit");
    CONFIG_REQUIRE(
        (config->javascript.execution_policy
                 .maximum_host_compile_projected_us == 0)
            == (config->javascript.execution_policy
                    .modeled_compile_bytes_per_ms == 0),
        "projected compile admission requires both the time budget and the "
        "modeled throughput");
    CONFIG_REQUIRE(!config->resources.enabled
                       || (config->resources.maximum_stylesheets != 0
                           && config->resources.maximum_stylesheet_bytes != 0
                           && config->resources.maximum_stylesheet_file_bytes
                                  != 0
                           && config->resources.maximum_images != 0
                           && config->resources.maximum_image_bytes != 0
                           && config->resources.maximum_image_file_bytes != 0
                           && config->resources.maximum_decoded_image_bytes
                                  != 0
                           && config->resources.timeout_ms > 0),
                   "external resources require complete bounded limits");
    CONFIG_REQUIRE(config->resources.web_font_mode >= WEB_FONTS_ON
                       && config->resources.web_font_mode < WEB_FONTS_MODE_COUNT,
                   "invalid website font mode");
    CONFIG_REQUIRE(
        !config->resources.enabled
            || !config->resources.web_fonts_enabled
            || (config->resources.maximum_font_attempts != 0
                && config->resources.maximum_font_attempts
                    <= TILEFINCH_WEB_FONT_FAMILY_LIMIT * 2u
                       * STYLE_WEB_FONT_SOURCE_LIMIT
                && config->resources.maximum_font_bytes != 0
                && config->resources.maximum_font_file_bytes != 0
                && config->resources.maximum_font_file_bytes
                    <= config->resources.maximum_font_bytes
                && config->resources.maximum_font_face_backend_bytes != 0),
        "web fonts require complete bounded limits");
    CONFIG_REQUIRE(!config->fonts.enabled
                       || config->fonts.maximum_total_bytes != 0,
                   "enabled fonts require a nonzero byte limit");
    CONFIG_REQUIRE(
        !config->fonts.enabled
            || (fixed_string_terminated(
                    config->fonts.sans_path,
                    sizeof(config->fonts.sans_path))
                && fixed_string_terminated(
                    config->fonts.serif_path,
                    sizeof(config->fonts.serif_path))
                && fixed_string_terminated(
                    config->fonts.sans_italic_path,
                    sizeof(config->fonts.sans_italic_path))
                && fixed_string_terminated(
                    config->fonts.sans_bold_path,
                    sizeof(config->fonts.sans_bold_path))
                && fixed_string_terminated(
                    config->fonts.serif_bold_path,
                    sizeof(config->fonts.serif_bold_path))
                && fixed_string_terminated(
                    config->fonts.metric_sans_path,
                    sizeof(config->fonts.metric_sans_path))
                && fixed_string_terminated(
                    config->fonts.metric_sans_bold_path,
                    sizeof(config->fonts.metric_sans_bold_path))),
        "font paths must be bounded strings");
    CONFIG_REQUIRE(
        !config->fonts.enabled
            || config->fonts.sans_path[0] != '\0'
            || config->fonts.serif_path[0] != '\0'
            || config->fonts.sans_italic_path[0] != '\0'
            || config->fonts.sans_bold_path[0] != '\0'
            || config->fonts.serif_bold_path[0] != '\0'
            || config->fonts.metric_sans_path[0] != '\0'
            || config->fonts.metric_sans_bold_path[0] != '\0',
        "enabled fonts require at least one font path");
    CONFIG_REQUIRE(config->diagnostics.minimum_severity
                       >= TILEFINCH_DIAGNOSTIC_DEBUG
                       && config->diagnostics.minimum_severity
                              <= TILEFINCH_DIAGNOSTIC_ERROR,
                   "diagnostic severity is invalid");
    copy_error(error, error_capacity, "");
    return true;
#undef CONFIG_REQUIRE
}

static void browser_engine_reset_render_shell(BrowserEngine *engine)
{
    if (engine == NULL) return;
    if (engine->render_ready) tile_cache_destroy(&engine->render);
    else memset(&engine->render, 0, sizeof(engine->render));
    engine->render_ready = false;
    engine->render_relayout_generation = 0;
    engine->render_focus_paint_generation = 0;
}

static void browser_engine_reset_shell(BrowserEngine *engine)
{
    if (engine == NULL) return;
    browser_engine_reset_render_shell(engine);
    memset(&engine->controller, 0, sizeof(engine->controller));
    engine->controller_ready = false;
}

static bool browser_engine_report_retired_update(
    BrowserEngine *engine, const char *phase)
{
    browser_engine_reset_shell(engine);
    return set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
        TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, phase,
        engine->navigation.last_error[0] != '\0'
            ? engine->navigation.last_error
            : "page update failed; reload or return to the previous page");
}

static bool browser_engine_init_tile_cache(
    BrowserEngine *engine, TileCache *cache, const LayoutDocument *layout,
    size_t tile_capacity)
{
    if (engine == NULL || cache == NULL || layout == NULL
        || !tile_cache_init(
               cache, &engine->budget, layout, tile_capacity)) {
        return false;
    }
    tile_cache_set_forced_dark(cache, engine->forced_dark);
    return true;
}

static void browser_engine_drop_provisional_render(BrowserEngine *engine)
{
    for (size_t i = 0; i < 2u; i++) {
        TileCache *cache = &engine->provisional_renders[i];
        /* The borrowed layout may already be gone; destroying the cache
           touches only what the cache owns. */
        if (cache->budget != NULL) tile_cache_destroy(cache);
        memset(cache, 0, sizeof(*cache));
    }
    engine->provisional_layout = NULL;
    if (browser_preview_view_live(engine->provisional_view))
        engine->provisional_view = BROWSER_PREVIEW_VIEW_FRAME;
}

/* The front cache only: a repaint may be rasterizing the other one (a
   scroll during its cooperative slices reaches here). */
static void browser_engine_drop_provisional_front(BrowserEngine *engine)
{
    TileCache *front = &engine->provisional_renders[engine->provisional_front];
    if (front->budget != NULL) tile_cache_destroy(front);
    memset(front, 0, sizeof(*front));
    engine->provisional_layout = NULL;
    if (browser_preview_view_live(engine->provisional_view))
        engine->provisional_view = BROWSER_PREVIEW_VIEW_FRAME;
}

static void browser_engine_reset_provisional_viewport(BrowserEngine *engine)
{
    if (engine == NULL) return;
    browser_engine_drop_provisional_render(engine);
    if (engine->provisional_frame != NULL)
        tilefinch_platform_retire_frame(engine->provisional_frame);
    budget_free(&engine->budget, engine->provisional_frame);
    engine->provisional_frame = NULL;
    engine->provisional_frame_pixels = 0;
    engine->provisional_scroll_y = 0;
    engine->provisional_maximum_scroll = 0;
    engine->provisional_content_end = 0;
    engine->provisional_viewport_height = 0;
    engine->provisional_focus_node = NULL;
    if (engine->provisional_activation == BROWSER_DEFERRED_ACTIVATION_WAITING)
        engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_IDLE;
    engine->provisional_view = BROWSER_PREVIEW_VIEW_NONE;
    engine->navigation.provisional_reader_y = 0;
}

static void browser_engine_begin_initial_tile_hold(BrowserEngine *engine)
{
    /* Give the native provider up to one second to finish its cold screen,
       retaining the previous pixels instead of flashing an initial
       checkerboard. This never delays raster work or input. */
    engine->initial_tile_hold = engine->render_ready
        && engine->render.frames_rendered == 0
        && youtube_lite_route(engine->navigation.page.document_url)
               == YOUTUBE_LITE_ROUTE_HOME;
    engine->initial_tile_hold_started_us = engine->initial_tile_hold
        ? tilefinch_platform_monotonic_time_us() : 0;
}

static bool browser_engine_initialize_render_shell(
    BrowserEngine *engine, const char *diagnostic_name,
    const char *failure_message)
{
    if (browser_engine_test_consume_render_shell_refusal()) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, diagnostic_name,
            failure_message);
    }
    if (engine->config.tile_capacity == 0) {
        clear_error(engine);
        return true;
    }
    if (engine->framebuffer == NULL
        || !browser_engine_init_tile_cache(
               engine, &engine->render,
               &engine->navigation.page.layout,
               engine->config.tile_capacity)
        || !tile_cache_set_frame(
               &engine->render, engine->framebuffer,
               engine->framebuffer_pixels)) {
        if (engine->render.budget != NULL)
            tile_cache_destroy(&engine->render);
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, diagnostic_name,
            failure_message);
    }
    engine->render_ready = true;
    engine->render_shell_serial++;
    browser_engine_begin_initial_tile_hold(engine);
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    clear_error(engine);
    return true;
}

static void browser_engine_paint_replacement_failure(BrowserEngine *engine)
{
    if (engine == NULL || engine->framebuffer == NULL
        || engine->framebuffer_pixels == 0) return;
    size_t width = (size_t) engine->config.device.framebuffer_width;
    size_t height = (size_t) engine->config.device.framebuffer_height;
    if (width == 0 || height == 0
        || width > engine->framebuffer_pixels / height) return;
    uint16_t *frame = engine->framebuffer;
    if (!engine->replacement_frame_frozen) {
        for (size_t i = 0; i < width * height; i++) frame[i] = UINT16_C(0xef7d);
    }
    size_t banner_height = height < 40 ? height : 40;
    for (size_t y = 0; y < banner_height; y++) {
        for (size_t x = 0; x < width; x++) {
            frame[y * width + x] = y < 3 ? UINT16_C(0xffff)
                                         : UINT16_C(0xb104);
        }
    }
    /* Allocation-free retry glyph: a white return arrow in the fixed banner.
       The detailed failure text remains in NavigationSession for assistive
       UI and logs rather than requiring a font allocation here. */
    size_t center_x = width < 30 ? width / 2 : width - 22;
    size_t center_y = banner_height / 2;
    for (size_t x = center_x > 8 ? center_x - 8 : 0;
         x < width && x <= center_x + 6; x++) {
        if (center_y < height) frame[center_y * width + x] = UINT16_C(0xffff);
    }
    for (size_t d = 0; d < 7; d++) {
        size_t x = center_x > 8 ? center_x - 8 + d : d;
        if (x >= width) break;
        if (center_y >= d && center_y - d < height)
            frame[(center_y - d) * width + x] = UINT16_C(0xffff);
        if (center_y + d < height)
            frame[(center_y + d) * width + x] = UINT16_C(0xffff);
    }
}

static bool browser_engine_freeze_replacement_frame(
    void *opaque, const NavigationSession *incumbent)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || incumbent != &engine->navigation) return false;
    bool frozen = engine->framebuffer != NULL && engine->render_ready
        && engine->render.frames_rendered != 0;
    engine->replacement_frame_frozen = frozen;
    browser_engine_reset_shell(engine);
    return frozen;
}

static void browser_engine_replacement_outcome(
    void *opaque, const NavigationSession *session, bool success)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || session != &engine->navigation) return;
    if (!success) browser_engine_paint_replacement_failure(engine);
    else engine->replacement_frame_frozen = false;
}

static void browser_engine_abort_candidate_shell(void *opaque)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL) return;
    if (engine->candidate_render.budget != NULL)
        tile_cache_destroy(&engine->candidate_render);
    else
        memset(&engine->candidate_render, 0,
               sizeof(engine->candidate_render));
    memset(&engine->candidate_controller, 0,
           sizeof(engine->candidate_controller));
    engine->candidate_controller_ready = false;
    engine->candidate_render_ready = false;
    engine->candidate_shell_prepared = false;
}

/* Form/focus history state belongs to one document request. Fragment-only
   entries share that document; redirects and even same-origin path changes do
   not. Normalize before removing the fragment so equivalent URL spelling does
   not accidentally disable legitimate same-document restoration. */
static bool browser_engine_history_url_matches_document(
    const char *history_url, const char *document_url)
{
    char history[NAVIGATION_URL_LIMIT];
    char document[NAVIGATION_URL_LIMIT];
    if (history_url == NULL || document_url == NULL
        || !tilefinch_url_normalize(history_url, history, sizeof(history))
        || !tilefinch_url_normalize(
               document_url, document, sizeof(document))) return false;
    char *fragment = strchr(history, '#');
    if (fragment != NULL) *fragment = '\0';
    fragment = strchr(document, '#');
    if (fragment != NULL) *fragment = '\0';
    return strcmp(history, document) == 0;
}

static bool browser_engine_restore_candidate_controls(
    void *opaque, NavigationSession *candidate)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || candidate == NULL || candidate->budget != &engine->budget
        || !candidate->page.loaded) {
        return false;
    }
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (work->history_move) {
        const NavigationEntry *target = navigation_current(
            &engine->navigation);
        if (target != NULL
            && browser_engine_history_url_matches_document(
                   target->url, candidate->page.document_url)) {
            if (!browser_engine_restore_controls(
                candidate, work->restore.controls,
                work->restore.control_count)) return false;
        } else {
            /* The response redirected or otherwise committed a different
               document.  Ordinal control state belongs to the requested
               history document and must never populate an unrelated form. */
            work->restore.control_count = 0u;
        }
    }
    return true;
}

static bool browser_engine_prepare_candidate_shell(
    void *opaque, NavigationSession *candidate)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || candidate == NULL
        || candidate->budget != &engine->budget || !candidate->page.loaded)
        return false;
    bool keep_progressive_render = engine->candidate_render_ready
        && engine->candidate_render.budget == &engine->budget
        && engine->candidate_render.source_layout
               == &candidate->page.layout;
    if (!keep_progressive_render) {
        browser_engine_abort_candidate_shell(engine);
    } else {
        /* The progressive callback already rasterized this exact final
           layout. Preserve its bounded tile and decoded-image caches; only
           the controller side of the transaction remains to be prepared. */
        memset(&engine->candidate_controller, 0,
               sizeof(engine->candidate_controller));
        engine->candidate_controller_ready = false;
        engine->candidate_shell_prepared = false;
    }
    if (!controller_init(&engine->candidate_controller,
                         candidate)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-controller",
            "candidate controller initialization failed");
    }
    engine->candidate_controller_ready = true;
    if (engine->youtube.topics_publishing
        && engine->controller.focus_kind == CONTROLLER_FOCUS_CONTROL
        && engine->controller.focus_index < candidate->page.layout.control_count) {
        (void) controller_restore_focus_node(&engine->candidate_controller,
            candidate->page.layout.controls[engine->controller.focus_index].node);
    } else if (engine->youtube.topics_publishing
        && engine->controller.focus_kind == CONTROLLER_FOCUS_LINK
        && engine->controller.focus_index < candidate->page.layout.link_count) {
        (void) controller_restore_focus_node(&engine->candidate_controller,
            candidate->page.layout.links[engine->controller.focus_index].node);
    }
    if (engine->config.tile_capacity != 0 && !keep_progressive_render) {
        bool injected_refusal =
            browser_engine_test_consume_render_shell_refusal();
        if (injected_refusal)
            budget_inject_failure_after(&engine->budget, 0u);
        if (engine->framebuffer == NULL
            || !browser_engine_init_tile_cache(
                   engine, &engine->candidate_render,
                   &candidate->page.layout, engine->config.tile_capacity)
            || !tile_cache_set_frame(
                   &engine->candidate_render, engine->framebuffer,
                   engine->framebuffer_pixels)) {
            if (injected_refusal)
                budget_clear_failure_injection(&engine->budget);
            return set_error_code(
                engine, TILEFINCH_SUBSYSTEM_RENDER,
                TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                "candidate-render",
                "candidate render shell exceeded its bounded memory");
        }
        engine->candidate_render_ready = true;
    }
    engine->candidate_shell_prepared = true;
    return true;
}

static void browser_engine_commit_candidate_shell(void *opaque)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || !engine->candidate_shell_prepared) return;
    browser_engine_reset_shell(engine);
    engine->controller = engine->candidate_controller;
    /* A streamed URL candidate is built in a temporary NavigationSession
       which is memcpy-promoted immediately before this no-fail callback. */
    engine->controller.navigation = &engine->navigation;
    engine->controller_ready = engine->candidate_controller_ready;
    engine->render = engine->candidate_render;
    engine->render.source_layout = &engine->navigation.page.layout;
    if (engine->render.owns_visual_layout) {
        engine->render.layout = &engine->render.visual_layout;
    } else {
        engine->render.layout = engine->render.source_layout;
    }
    engine->render_ready = engine->candidate_render_ready;
    engine->render_shell_serial++;
    browser_engine_begin_initial_tile_hold(engine);
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    memset(&engine->candidate_controller, 0,
           sizeof(engine->candidate_controller));
    memset(&engine->candidate_render, 0,
           sizeof(engine->candidate_render));
    engine->candidate_controller_ready = false;
    engine->candidate_render_ready = false;
    engine->candidate_shell_prepared = false;
}

#include "browser_engine/provisional.inc"

static bool browser_engine_configure_navigation(BrowserEngine *engine)
{
    NavigationSession *navigation = &engine->navigation;
    navigation->reclaim_optional = browser_engine_reclaim_for_full_relayout;
    navigation->reclaim_optional_opaque = engine;
    navigation->declared_css_width = engine->config.declared_css_width;
    navigation->declared_css_height = engine->config.declared_css_height;
    navigation_attach_browser_session(navigation, &engine->session);
    navigation_set_deferred_image_ready_hook(
        navigation, browser_engine_deferred_reader_images_ready, engine);
    navigation_set_image_table_moved_hook(
        navigation, browser_engine_image_table_moved, engine);
    if (!navigation_set_replacement_mode(
            navigation, engine->config.navigation_replacement_mode)
        || !navigation_set_replacement_hooks(
            navigation, browser_engine_freeze_replacement_frame,
            browser_engine_replacement_outcome, engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "replacement-policy",
            "could not apply navigation replacement policy");
    }
    /* Retain half a viewport below the first frame. This is enough to make
       provisional page-down useful while keeping the speculative layout's
       style and intrinsic-size walks local to pixels it can soon present. */
    if (!navigation_set_progressive_preview_lookahead(navigation, 50u)
        || !navigation_set_progressive_paint_hook(
            navigation,
            engine->config.progressive_first_paint
                ? browser_engine_paint_progressive_preview : NULL,
            engine->config.progressive_first_paint ? engine : NULL)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "progressive-paint-policy",
            "could not apply progressive first-paint policy");
    }
    navigation_set_progressive_paint_preserves_incumbent(
        navigation, engine->config.progressive_first_paint);
    navigation->relayout_preview_threshold_us =
        engine->config.relayout_preview_threshold_us;
    navigation->game_audio_internal_slots =
        engine->config.javascript.game_audio_internal_slots;
    if (!navigation_set_script_execution_policy(
            navigation, &engine->config.javascript.execution_policy)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_SCRIPT,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "script-policy",
            "could not apply JavaScript execution policy");
    }
    if (engine->config.javascript.enabled) {
        navigation_enable_scripts(
            navigation, engine->config.javascript.heap_limit,
            engine->config.javascript.runtime_timeout_ms);
    }
    if (engine->config.javascript.document_scripts_enabled) {
        navigation_enable_document_scripts(
            navigation, engine->config.javascript.maximum_scripts,
            engine->config.javascript.maximum_total_bytes,
            engine->config.javascript.maximum_file_bytes,
            engine->config.javascript.network_timeout_ms);
    }
    if (engine->config.resources.enabled) {
        const BrowserResourceConfig *resources = &engine->config.resources;
        navigation_enable_external_resources(
            navigation, resources->maximum_stylesheets,
            resources->maximum_stylesheet_bytes,
            resources->maximum_stylesheet_file_bytes,
            resources->maximum_images, resources->maximum_image_bytes,
            resources->maximum_image_file_bytes,
            resources->maximum_decoded_image_bytes, resources->timeout_ms);
        navigation->maximum_publication_work_units =
            resources->maximum_publication_work_units;
        if (resources->web_fonts_enabled) {
            navigation_enable_web_fonts(
                navigation, resources->maximum_font_attempts,
                resources->maximum_font_bytes,
                resources->maximum_font_file_bytes,
                resources->maximum_font_face_backend_bytes,
                resources->timeout_ms);
        }
        (void) navigation_set_web_font_mode(navigation, resources->web_font_mode);
    }
    return true;
}

BrowserEngine *browser_engine_create(const BrowserConfig *config,
                                     char *error, size_t error_capacity)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t create_started_us = tilefinch_platform_monotonic_time_us();
    BrowserEngineCreationMetrics create_metrics = {0};
#define BROWSER_ENGINE_CREATE_MARK(member_name) do { \
    uint64_t create_now_us = tilefinch_platform_monotonic_time_us(); \
    create_metrics.member_name = create_now_us - create_started_us; \
} while (0)
#else
#define BROWSER_ENGINE_CREATE_MARK(member_name) do {} while (0)
#endif
    if (!browser_config_validate(config, error, error_capacity)) return NULL;
    if (active_engine != NULL) {
        copy_error(error, error_capacity,
                   "another BrowserEngine is already active");
        return NULL;
    }
    BrowserEngine *engine = calloc(1, sizeof(*engine));
    if (engine == NULL) {
        copy_error(error, error_capacity,
                   "could not allocate the fixed browser control block");
        return NULL;
    }
    engine->config = *config;
    engine->state = BROWSER_ENGINE_ACTIVE;
    engine->youtube.topics = true;
    tilefinch_diagnostics_init(
        &engine->diagnostics, config->diagnostics.callback,
        config->diagnostics.opaque, config->diagnostics.minimum_severity);
    clear_error(engine);
    budget_init(&engine->budget, config->memory_limit);
    if (!budget_install_lexbor(&engine->budget)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED,
                       "lexbor-owner",
                       "another live Lexbor allocation owner is installed");
        goto failed;
    }
    active_engine = engine;
    BROWSER_ENGINE_CREATE_MARK(control_ready_us);

    if (!browser_session_init(&engine->session, &engine->budget,
                              config->session_cache_limit)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_STORAGE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "session-init",
                       "browser session initialization failed");
        goto failed;
    }
    youtube_lite_session_register_credentials(&engine->session);
    browser_session_module_bytecode_set_limit(
        &engine->session, config->module_bytecode_cache_limit);
    browser_session_script_bytecode_set_limit(
        &engine->session, BROWSER_SCRIPT_BYTECODE_CLASSIC,
        config->classic_bytecode_cache_limit);
    (void) browser_session_script_disk_configure(
        &engine->session, config->script_cache_dir,
        config->script_cache_write, config->script_cache_limit);
    /* Reproducible accelerators use otherwise-free room, never the required
       page's last bytes. Pixel reclamation leaves response metadata intact:
       an allocating caller may still be borrowing that metadata. */
    budget_set_reclaim_hook(&engine->budget,
                            browser_engine_reclaim_accelerators, engine);
    engine->session_ready = true;
    BROWSER_ENGINE_CREATE_MARK(session_ready_us);
    engine->content_blocker = content_blocker_create(&engine->budget);
    if (engine->content_blocker == NULL) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_STORAGE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "content-blocker-init",
                       "content blocker control allocation failed");
        goto failed;
    }
    engine->session.content_blocker = engine->content_blocker;
    BROWSER_ENGINE_CREATE_MARK(blocker_ready_us);
    if (!navigation_init(&engine->navigation, &engine->budget,
                         config->history_capacity)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                       "navigation-init", "navigation initialization failed");
        goto failed;
    }
    engine->navigation_ready = true;
    if (!browser_engine_configure_navigation(engine)) goto failed;
    BROWSER_ENGINE_CREATE_MARK(navigation_ready_us);

    if (config->fonts.enabled && !config->fonts.staged_loading) {
        const BrowserFontConfig *fonts = &config->fonts;
        if (!font_set_load(
                &engine->fonts, &engine->budget, fonts->sans_path,
                fonts->serif_path, fonts->sans_italic_path,
                fonts->sans_bold_path, fonts->serif_bold_path,
                fonts->metric_sans_path, fonts->metric_sans_bold_path,
                fonts->maximum_total_bytes)) {
            set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                           TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                           "font-load", "bounded font loading failed");
            goto failed;
        }
        engine->fonts_ready = true;
    }
    BROWSER_ENGINE_CREATE_MARK(fonts_ready_us);

    if (config->tile_capacity != 0) {
        engine->framebuffer_pixels =
            (size_t) config->device.framebuffer_width
            * (size_t) config->device.framebuffer_height;
        engine->framebuffer = budget_calloc_category(
            &engine->budget, BUDGET_CATEGORY_RENDER,
            engine->framebuffer_pixels, sizeof(*engine->framebuffer));
        if (engine->framebuffer == NULL) {
            set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                           TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
                           "framebuffer-allocation",
                           "bounded framebuffer allocation failed");
            goto failed;
        }
    }
    BROWSER_ENGINE_CREATE_MARK(framebuffer_ready_us);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    engine->creation_metrics = create_metrics;
#endif
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "engine-created", config->device.name, config->memory_limit,
        engine->framebuffer_pixels * sizeof(*engine->framebuffer));
    copy_error(error, error_capacity, "");
#undef BROWSER_ENGINE_CREATE_MARK
    return engine;

failed: {
        char retained[sizeof(engine->last_error)];
        snprintf(retained, sizeof(retained), "%s", engine->last_error);
        if (browser_engine_shutdown(engine)) free(engine);
        copy_error(error, error_capacity, retained);
        return NULL;
    }
}

bool browser_engine_creation_metrics(
    const BrowserEngine *engine, BrowserEngineCreationMetrics *metrics)
{
    if (metrics != NULL) *metrics = (BrowserEngineCreationMetrics) {0};
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (engine == NULL || metrics == NULL) return false;
    *metrics = engine->creation_metrics;
    return true;
#else
    (void) engine;
    return false;
#endif
}

void browser_engine_arm_installed_app_heap(BrowserEngine *engine, bool armed)
{
    if (engine == NULL) return;
    size_t heap = engine->config.javascript.installed_app_heap_limit;
    /* A floor is never allowed to claim the whole page Budget. */
    navigation_arm_next_top_level_heap(
        &engine->navigation,
        armed && heap < engine->config.memory_limit / 4u * 3u ? heap : 0u);
}

bool browser_engine_set_javascript_enabled(
    BrowserEngine *engine, bool enabled)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || browser_engine_navigation_pending(engine)) return false;
    engine->config.javascript.enabled = enabled;
    engine->config.javascript.document_scripts_enabled = enabled;
    navigation_enable_scripts(
        &engine->navigation,
        enabled ? engine->config.javascript.heap_limit : 0u,
        enabled ? engine->config.javascript.runtime_timeout_ms : 0u);
    navigation_enable_document_scripts(
        &engine->navigation,
        enabled ? engine->config.javascript.maximum_scripts : 0u,
        enabled ? engine->config.javascript.maximum_total_bytes : 0u,
        enabled ? engine->config.javascript.maximum_file_bytes : 0u,
        enabled ? engine->config.javascript.network_timeout_ms : 0);
    return true;
}

bool browser_engine_shutdown(BrowserEngine *engine)
{
    if (engine == NULL) return false;
    if (engine->state == BROWSER_ENGINE_SHUTDOWN) {
        if (active_engine == engine) {
            if (!budget_uninstall_lexbor(&engine->budget)) return false;
            active_engine = NULL;
        }
        return engine->budget.current == 0
            && budget_active_allocations(&engine->budget, NULL) == 0;
    }
    browser_engine_cancel_navigation(engine, "browser engine shutdown");
    (void) browser_engine_youtube_cancel(engine);
    budget_free(&engine->budget, engine->script_form_body);
    engine->script_form_body = NULL;
    /* A page cancellation only invalidates the JPEG completion token; it
       deliberately never waits on entropy decoding. Engine teardown is the
       ownership boundary that must join the worker before destroying the
       page Budget and its retained encoded body. */
    if (!images_decode_worker_shutdown(&engine->budget)) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED,
                       "image-worker-shutdown",
                       "JPEG worker did not quiesce before teardown");
        return false;
    }
    browser_engine_find_clear(engine);
    document_backing_clear(&engine->backing);
    browser_engine_reset_provisional_viewport(engine);
    browser_engine_abort_candidate_shell(engine);
    browser_engine_reset_shell(engine);
    if (engine->navigation_ready) {
        (void) navigation_set_candidate_commit_hooks(
            &engine->navigation, NULL, NULL, NULL, NULL);
        (void) navigation_set_replacement_hooks(
            &engine->navigation, NULL, NULL, NULL);
        navigation_destroy(&engine->navigation);
    }
    engine->navigation_ready = false;
    browser_engine_discard_staged_fonts(engine);
    if (font_set_loaded_bytes(&engine->fonts) != 0)
        font_set_destroy(&engine->fonts);
    engine->fonts_ready = false;
    if (engine->session_ready) engine->session.content_blocker = NULL;
    content_blocker_destroy(engine->content_blocker);
    engine->content_blocker = NULL;
    budget_set_reclaim_hook(&engine->budget, NULL, NULL);
    if (engine->session_ready) browser_session_destroy(&engine->session);
    engine->session_ready = false;
    budget_free(&engine->budget, engine->framebuffer);
    engine->framebuffer = NULL;
    engine->framebuffer_pixels = 0;
    engine->state = BROWSER_ENGINE_SHUTDOWN;
    bool clean = engine->budget.current == 0
        && budget_active_allocations(&engine->budget, NULL) == 0
        && budget_categories_reconcile(&engine->budget);
    if (!clean) {
        set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                       TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED,
                       "engine-shutdown",
                       "browser engine teardown retained memory");
    } else {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            "engine-shutdown", "clean bounded teardown", 0, 0);
    }
    if (active_engine == engine && clean) {
        clean = budget_uninstall_lexbor(&engine->budget);
        if (clean) active_engine = NULL;
    }
    return clean;
}

void browser_engine_destroy(BrowserEngine *engine)
{
    if (engine == NULL) return;
    if (browser_engine_shutdown(engine)) free(engine);
}

const BrowserConfig *browser_engine_config(const BrowserEngine *engine)
{
    return engine == NULL ? NULL : &engine->config;
}

const char *browser_engine_last_error(const BrowserEngine *engine)
{
    return engine == NULL ? "browser engine is null" : engine->last_error;
}

long browser_engine_last_tls_verify_result(const BrowserEngine *engine)
{
    return engine == NULL || !engine->navigation_ready
        ? 0 : engine->navigation.last_tls_verify_result;
}

bool browser_engine_last_tls_verify_result_available(
    const BrowserEngine *engine)
{
    return engine != NULL && engine->navigation_ready
        && engine->navigation.last_tls_verify_result_available;
}

bool browser_engine_last_tls_verification_failed(
    const BrowserEngine *engine)
{
    return engine != NULL && engine->navigation_ready
        && engine->navigation.last_tls_verification_failed;
}

TilefinchDiagnosticCode browser_engine_last_diagnostic_code(
    const BrowserEngine *engine)
{
    return engine == NULL ? TILEFINCH_DIAGNOSTIC_INVALID_INPUT
                          : engine->last_diagnostic_code;
}

bool browser_engine_metrics(const BrowserEngine *engine,
                            BrowserEngineMetrics *metrics)
{
    if (engine == NULL || metrics == NULL) return false;
    memset(metrics, 0, sizeof(*metrics));
    metrics->state = engine->state;
    metrics->control_bytes = sizeof(*engine);
    metrics->framebuffer_bytes = engine->framebuffer_pixels
                                 * sizeof(*engine->framebuffer);
    metrics->non_page_memory_reserve =
        engine->config.non_page_memory_reserve;
    metrics->budget_limit = engine->budget.limit;
    metrics->budget_current = engine->budget.current;
    metrics->budget_peak = engine->budget.peak;
    metrics->budget_external_reserved = engine->budget.external_reserved;
    metrics->budget_external_reserved_peak =
        engine->budget.external_reserved_peak;
    metrics->session_inline_accounting_bytes =
        engine->session.accounting_bytes;
    metrics->budget_active_allocations = budget_active_allocations(
        &engine->budget, &metrics->budget_largest_allocation);
    metrics->allocation_failures = engine->budget.failure_count;
    metrics->diagnostics_emitted = engine->diagnostics.emitted;
    metrics->diagnostics_filtered = engine->diagnostics.filtered;
    metrics->last_diagnostic_subsystem = engine->last_diagnostic_subsystem;
    metrics->last_diagnostic_code = engine->last_diagnostic_code;
    metrics->fonts_ready = engine->fonts_ready;
    metrics->controller_ready = engine->controller_ready;
    metrics->render_ready = engine->render_ready;
    metrics->backing_kind = document_backing_kind(&engine->backing);
    if (engine->navigation_ready) {
        const NavigationSession *navigation = &engine->navigation;
        metrics->page_loaded = navigation->page.loaded;
        metrics->loads_started = navigation->loads_started;
        metrics->loads_committed = navigation->loads_committed;
        metrics->loads_cancelled = navigation->loads_cancelled;
        metrics->pages_destroyed = navigation->page_destroys;
        metrics->navigation = navigation->performance;
    }
    if (engine->controller_ready) {
        metrics->controller_focus_moves = engine->controller.focus_moves;
        metrics->controller_activations = engine->controller.activations;
        metrics->controller_text_edits = engine->controller.text_edits;
    }
    if (engine->render_ready) {
        metrics->rendered_frames = engine->render.frames_rendered;
        metrics->unchanged_runtime_frames_suppressed =
            engine->unchanged_runtime_frames_suppressed;
        metrics->maximum_render_us = engine->render.max_frame_us;
        metrics->idle_jobs_scheduled = engine->render.idle_jobs_scheduled;
        metrics->idle_jobs_completed = engine->render.idle_jobs_completed;
        metrics->idle_jobs_cancelled = engine->render.idle_jobs_cancelled;
        metrics->idle_slices = engine->render.idle_slices;
        metrics->idle_slice_overruns = engine->render.idle_slice_overruns;
        metrics->idle_image_admission_skips =
            engine->render.idle_image_admission_skips;
        metrics->idle_glyphs_prewarmed =
            engine->render.idle_glyphs_prewarmed;
        metrics->idle_glyph_cache_misses =
            engine->render.idle_glyph_cache_misses;
        metrics->idle_work_us = engine->render.idle_us;
        metrics->maximum_idle_slice_us = engine->render.max_idle_slice_us;
        metrics->maximum_idle_unit_us = engine->render.max_idle_unit_us;
        metrics->startup_visual_slices =
            engine->render.startup_visual_slices;
        metrics->startup_visual_us = engine->render.startup_visual_us;
        metrics->maximum_startup_visual_slice_us =
            engine->render.max_startup_visual_slice_us;
        metrics->maximum_startup_visual_unit_us =
            engine->render.max_startup_visual_unit_us;
    }
    return true;
}

bool browser_engine_content_blocker_configure(
    BrowserEngine *engine, ContentBlockerMode mode,
    const char *custom_path)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_configure(
               engine->content_blocker, mode, custom_path);
}

bool browser_engine_content_blocker_set_allowed_sites(
    BrowserEngine *engine, const char *const *sites, size_t count)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_set_allowed_sites(
               engine->content_blocker, sites, count);
}

bool browser_engine_content_blocker_metrics(
    const BrowserEngine *engine, ContentBlockerMetrics *metrics)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && content_blocker_metrics(engine->content_blocker, metrics);
}

bool browser_engine_refresh_shell(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "shell-refresh",
            "no committed page is available");
    }
    lxb_dom_node_t *focused_node = engine->controller_ready
        ? controller_focused_node(&engine->controller) : NULL;
    browser_engine_reset_shell(engine);
    if (!controller_init(&engine->controller, &engine->navigation))
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, "controller-init",
            "controller initialization failed");
    engine->controller_ready = true;
    if (focused_node != NULL)
        (void) controller_restore_focus_node(
            &engine->controller, focused_node);
    return browser_engine_initialize_render_shell(
        engine, "render-shell-init",
        "render shell initialization failed");
}

bool browser_engine_refresh_render_shell(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded
        || engine->config.tile_capacity == 0) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "render-shell-refresh",
            "no engine-owned render shell can be refreshed");
    }
    browser_engine_cancel_idle_work(engine);
    browser_engine_reset_render_shell(engine);
    return browser_engine_initialize_render_shell(
        engine, "render-shell-refresh", "render shell refresh failed");
}

bool browser_engine_apply_layout_damage(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded
        || !engine->render_ready) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "render-layout-damage",
            "render shell is not ready for layout damage");
    }
    if (engine->render_relayout_generation
        == engine->navigation.incremental_relayouts) {
        clear_error(engine);
        return true;
    }
    browser_engine_cancel_idle_work(engine);
    if (engine->navigation.canvas_paint_damage_pending) {
        const ImageResources *images = &engine->navigation.page.images;
        for (size_t i = 0; i < images->count; i++) {
            const ImageResource *image = &images->items[i];
            if (image->is_canvas) {
                tile_cache_invalidate_image_identity(
                    &engine->render,
                    image_resource_backing_identity(image));
            }
        }
        /* The fixed-image proof only admits script-free pages. Runtime canvas
           updates retain their unrelated overlay caches on every frame. */
        bool (*sync_pixels)(TileCache *, int, int, int, int) =
            engine->navigation.page.runtime == NULL
                ? tile_cache_sync_document_image_paint
                : tile_cache_sync_canvas_paint;
        if (sync_pixels(
                &engine->render,
                engine->navigation.relayout_damage_left,
                engine->navigation.relayout_damage_top,
                engine->navigation.relayout_damage_right,
                engine->navigation.relayout_damage_bottom)) {
            engine->navigation.canvas_paint_damage_pending = false;
            engine->render_relayout_generation =
                engine->navigation.incremental_relayouts;
            clear_error(engine);
            return true;
        }
        /* An embedder may have replaced the source document before refreshing
           its visual shell. Same-geometry sync then declines; stage the
           current paint list through the ordinary transactional fallback. */
        engine->navigation.canvas_paint_damage_pending = false;
    }
    bool preserve_fixed = engine->navigation.incremental_relayouts != 0u
        && engine->render_relayout_generation
            == engine->navigation.incremental_relayouts - 1u
        && engine->navigation.fixed_paint_relayout_generation
            == engine->navigation.incremental_relayouts;
    bool (*replace_damage)(TileCache *, const LayoutDocument *, int, int, int, int) =
        preserve_fixed ? tile_cache_replace_layout_damage_preserving_fixed
                       : tile_cache_replace_layout_damage;
    if (!replace_damage(
            &engine->render, &engine->navigation.page.layout,
            engine->navigation.relayout_damage_left,
            engine->navigation.relayout_damage_top,
            engine->navigation.relayout_damage_right,
            engine->navigation.relayout_damage_bottom)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "render-layout-damage",
            "render shell could not apply layout damage");
    }
    engine->render_relayout_generation =
        engine->navigation.incremental_relayouts;
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    clear_error(engine);
    return true;
}

static bool element_named(const lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *element = node == NULL || node->type
        != LXB_DOM_NODE_TYPE_ELEMENT
        ? NULL : document_element_name((lxb_dom_node_t *) node, &length);
    return element != NULL && length == strlen(name)
        && strncasecmp(element, name, length) == 0;
}

static bool color_scheme_lists_dark(const char *value, size_t length)
{
    for (size_t at = 0; at + 4u <= length; at++) {
        if (strncasecmp(value + at, "dark", 4) == 0
            && (at == 0 || isspace((unsigned char) value[at - 1])
                || value[at - 1] == ',')
            && (at + 4u == length
                || isspace((unsigned char) value[at + 4u])
                || value[at + 4u] == ',')) return true;
    }
    return false;
}

/* A page that declares a dark rendering of its own (<meta name=
   "color-scheme"> or a root color-scheme listing dark) already honours
   night mode through prefers-color-scheme; the forced-dark pixel transform
   would only fight it (chatgpt.com's logo and menu icon, inline SVG, stayed
   black on its dark surface). Cached per committed navigation. */
static bool browser_engine_page_declares_dark(BrowserEngine *engine)
{
    NavigationSession *navigation = &engine->navigation;
    if (!navigation->page.loaded || navigation->page.document.html == NULL)
        return false;
    if (engine->page_dark_valid
        && engine->page_dark_generation == navigation->generation)
        return engine->page_dark;
    bool declares = false;
    lxb_dom_node_t *root = NULL;
    for (lxb_dom_node_t *child = lxb_dom_interface_node(
             navigation->page.document.html)->first_child;
         child != NULL; child = child->next) {
        if (element_named(child, "html")) {
            root = child;
            break;
        }
    }
    if (root == NULL) return false;
    char value[96];
    if (style_retained_property_value(
            &navigation->page.stylesheet, root, "color-scheme",
            sizeof("color-scheme") - 1u, value, sizeof(value))
        && color_scheme_lists_dark(value, strlen(value))) declares = true;
    size_t visited = 0;
    for (lxb_dom_node_t *section = root->first_child;
         !declares && section != NULL && visited < 256u;
         section = section->next, visited++) {
        if (!element_named(section, "head")) continue;
        for (lxb_dom_node_t *child = section->first_child;
             !declares && child != NULL && visited < 256u;
             child = child->next, visited++) {
            if (!element_named(child, "meta")) continue;
            size_t name_length = 0, content_length = 0;
            const char *name = document_attribute(child, "name", &name_length);
            const char *content = document_attribute(
                child, "content", &content_length);
            declares = name != NULL && content != NULL
                && name_length == sizeof("color-scheme") - 1u
                && strncasecmp(name, "color-scheme", name_length) == 0
                && color_scheme_lists_dark(content, content_length);
        }
    }
    engine->page_dark = declares;
    engine->page_dark_generation = navigation->generation;
    engine->page_dark_valid = true;
    return declares;
}

bool browser_engine_forced_dark(const BrowserEngine *engine)
{
    return engine != NULL && engine->forced_dark;
}

bool browser_engine_set_forced_dark(BrowserEngine *engine, bool enabled)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) {
        return false;
    }
    /* Night mode is also the reader's colour-scheme preference: sites with
       their own dark theme select it through prefers-color-scheme. It takes
       effect as stylesheets are next built. */
    stylesheet_set_prefers_dark_color_scheme(enabled);
    if (engine->forced_dark == enabled) return true;
    engine->forced_dark = enabled;
    /*
     * The load preview's frame and tiles are raster products. Keeping them
     * across an appearance change would briefly reintroduce the old palette
     * during a pending navigation, so discard them rather than
     * post-processing pixels whose image provenance has already been lost.
     */
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready) {
        tile_cache_set_forced_dark(&engine->render, enabled);
    }
    if (engine->candidate_render_ready) {
        tile_cache_set_forced_dark(&engine->candidate_render, enabled);
    }
    clear_error(engine);
    return true;
}

bool browser_engine_optional_glyphs_updated(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready)
        tile_cache_invalidate_glyphs(&engine->render);
    if (engine->candidate_render_ready)
        tile_cache_invalidate_glyphs(&engine->candidate_render);
    clear_error(engine);
    return true;
}

bool browser_engine_optional_glyph_payloads_ready(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    browser_engine_reset_provisional_viewport(engine);
    if (engine->render_ready)
        tile_cache_repaint_glyphs(&engine->render);
    if (engine->candidate_render_ready)
        tile_cache_repaint_glyphs(&engine->candidate_render);
    clear_error(engine);
    return true;
}

uint16_t browser_engine_glyph_script_mask(const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? 0 : engine->navigation.page.document.glyph_script_mask;
}

const DocumentGlyphCensus *browser_engine_glyph_census(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : &engine->navigation.page.document.glyph_census;
}


static bool browser_engine_apply_focus_paint(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready
        || engine->render_focus_paint_generation
               == engine->navigation.focus_paint_generation) return true;
    if (!engine->navigation.focus_paint_damage_valid
        || !tile_cache_sync_layout_paint(
            &engine->render,
            engine->navigation.focus_paint_damage_left,
            engine->navigation.focus_paint_damage_top,
            engine->navigation.focus_paint_damage_right,
            engine->navigation.focus_paint_damage_bottom)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "render-focus-paint",
            "render shell could not apply focus paint damage");
    }
    engine->render_focus_paint_generation =
        engine->navigation.focus_paint_generation;
    return true;
}

bool browser_engine_bind_document_backing(BrowserEngine *engine,
                                          const DocumentBacking *backing)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || !document_backing_valid(backing)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "backing-bind",
            "invalid document backing");
    }
    if (backing->kind == DOCUMENT_BACKING_FULL
        && (!engine->navigation.page.loaded
            || backing->full_document != &engine->navigation.page.document)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "backing-bind",
            "full backing must reference the committed document");
    }
    engine->backing = *backing;
    clear_error(engine);
    return true;
}

static bool browser_engine_bind_current_full_document(BrowserEngine *engine)
{
    DocumentBacking full;
    return document_backing_init_full(
               &full, &engine->navigation.page.document)
        && browser_engine_bind_document_backing(engine, &full);
}

#include "browser_engine/history_focus.inc"

static void browser_engine_census_page_fonts(BrowserEngine *engine)
{
    if (engine == NULL || !engine->navigation.page.loaded) return;
    const char *document_url =
        navigation_active_document_url(&engine->navigation);
    if (document_url != NULL
        && site_adapter_handles_navigation("GET", document_url)
        && !site_adapter_navigation_requires_stable_typography(
               "GET", document_url)) {
        /* Provider entry surfaces intentionally stay on the baseline pair:
           the search control becomes usable immediately and no idle-time
           face arrival can visibly move it underneath the user. */
        return;
    }
    const LayoutDocument *layout = &engine->navigation.page.layout;
    FontSetFaceMask requested = 0;
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type != DRAW_TEXT) continue;
        FontFamily family = draw_command_font_family(command);
        /* A web family draws with its built-in fallback until (or unless)
           its own face loads, so census the fallback. */
        if (font_family_is_web(family))
            family = font_family_web_fallback(family);
        bool serif = family == FONT_SERIF;
        if (serif) requested |= FONT_SET_FACE_SERIF;
        if (draw_command_font_italic(command) && !serif)
            requested |= FONT_SET_FACE_SANS_ITALIC;
        /* Request only the bold face font_set_face_variant() selects for
           this family. The two sans bold faces are 77 KB (DejaVu Sans) and
           98 KB (TilefinchSans); a page styled only with Arial, or only
           with sans-serif, never draws the other one. */
        if (draw_uses_bold_face(command)) {
            requested |= serif ? FONT_SET_FACE_SERIF_BOLD
                : (family == FONT_METRIC_SANS || family == FONT_SYSTEM_SANS)
                    ? FONT_SET_FACE_METRIC_SANS_BOLD
                : FONT_SET_FACE_SANS_BOLD;
        }
    }
    engine->font_requested_faces |= requested;
}

static void browser_engine_copy_adapter_metrics(
    BrowserNavigationJobMetrics *metrics, const SiteAdapterLoadMetrics *adapter)
{
    metrics->completion_per_mille =
        adapter->completion_per_mille;
    metrics->load.pump_calls = adapter->network_pumps;
    metrics->load.quota_yields = adapter->quota_yields;
    metrics->load.body_bytes = adapter->body_bytes;
    metrics->load.body_callbacks = adapter->body_callbacks;
    metrics->load.peak_buffered_bytes =
        adapter->peak_buffered_bytes;
    metrics->load.total_pump_us = adapter->network_us;
    metrics->load.maximum_pump_us =
        adapter->maximum_pump_us;
    metrics->adapter_request_wall_us =
        adapter->request_wall_us;
    metrics->adapter_transport_total_us =
        adapter->transport_total_us;
    metrics->adapter_dns_us = adapter->dns_us;
    metrics->adapter_tcp_us = adapter->tcp_us;
    metrics->adapter_tls_us = adapter->tls_us;
    metrics->adapter_server_us = adapter->server_us;
    metrics->adapter_body_transfer_us =
        adapter->body_transfer_us;
    metrics->adapter_admission_collect_us =
        adapter->admission_collect_us;
    metrics->adapter_transport_samples =
        adapter->transport_samples;
    metrics->adapter_reused_connections =
        adapter->reused_connections;
    metrics->adapter_document_cache_hits =
        adapter->document_cache_hits;
    metrics->adapter_document_cache_stores =
        adapter->document_cache_stores;
    metrics->maximum_transform_slice_us =
        adapter->maximum_transform_slice_us;
    metrics->transform_slices =
        adapter->build_slices;
    metrics->transform_quota_overruns =
        adapter->transform_quota_overruns;
    metrics->maximum_transform_slice_bytes =
        adapter->maximum_transform_slice_bytes;
    if (adapter->maximum_irreducible_unit_us
            > metrics->maximum_irreducible_unit_us) {
        metrics->maximum_irreducible_unit_us =
            adapter->maximum_irreducible_unit_us;
    }
}

static BrowserNavigationJobStatus browser_engine_finish_navigation_work(
    BrowserEngine *engine, NavigationLoadStatus terminal)
{
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    (void) navigation_load_metrics(work->load, &work->metrics.load);
    SiteAdapterLoadMetrics adapter = {0};
    bool adapter_metrics = false;
    if (work->adapter != NULL) {
        if (site_adapter_load_metrics(work->adapter, &adapter)) {
            browser_engine_copy_adapter_metrics(&work->metrics, &adapter);
            adapter_metrics = true;
        }
    }
    uint64_t finished_us =
        tilefinch_platform_monotonic_time_us();
    work->metrics.elapsed_us =
        finished_us >= work->started_us
            ? finished_us - work->started_us : 0;
    bool succeeded = terminal == NAVIGATION_LOAD_SUCCEEDED
        && engine->navigation.page.loaded;
    const char *download = navigation_load_download_filename(work->load);
    if (download != NULL) {
        snprintf(work->download_filename, sizeof(work->download_filename), "%s", download);
        terminal = NAVIGATION_LOAD_CANCELLED;
        snprintf(engine->navigation.last_error, sizeof(engine->navigation.last_error), "File download awaiting confirmation");
    }
    if (work->hooks_installed) {
        (void) navigation_set_candidate_commit_hooks(
            &engine->navigation, NULL, NULL, NULL, NULL);
        work->hooks_installed = false;
    }
    if (engine->candidate_shell_prepared
        || engine->candidate_render.budget != NULL) {
        browser_engine_abort_candidate_shell(engine);
    }
    if (succeeded) {
        succeeded = browser_engine_bind_current_full_document(engine);
    } else if (engine->navigation.page.loaded
               && work->previous_backing_valid) {
        engine->backing = work->previous_backing;
        browser_engine_navigation_restore_history(engine, work);
    } else {
        document_backing_clear(&engine->backing);
    }
    bool had_provisional =
        engine->provisional_view != BROWSER_PREVIEW_VIEW_NONE;
    int provisional_scroll_y = had_provisional
        ? engine->provisional_scroll_y : 0;
    /* The pending DOM becomes the page's, so its nodes stay valid. */
    lxb_dom_node_t *provisional_focus =
        had_provisional ? engine->provisional_focus_node : NULL;
    bool provisional_deferred =
        provisional_focus != NULL
        && engine->provisional_activation
               == BROWSER_DEFERRED_ACTIVATION_WAITING;
    navigation_load_destroy(work->load);
    work->load = NULL;
    site_adapter_load_destroy(work->adapter);
    work->adapter = NULL;
    browser_engine_youtube_on_finish(engine,
        succeeded ? BROWSER_NAVIGATION_JOB_SUCCEEDED
        : terminal == NAVIGATION_LOAD_CANCELLED
            ? BROWSER_NAVIGATION_JOB_CANCELLED
            : BROWSER_NAVIGATION_JOB_FAILED,
        adapter_metrics ? &adapter : NULL);
    if (succeeded) {
        browser_engine_census_page_fonts(engine);
        (void) browser_engine_navigation_restore_view(engine, work);
        if (!work->history_move)
            browser_engine_apply_initial_navigation_view(engine, true);
        if (had_provisional && provisional_scroll_y > 0) {
            (void) navigation_set_scroll(
                &engine->navigation, provisional_scroll_y);
            work->metrics.provisional_scroll_y =
                navigation_current(&engine->navigation) == NULL
                    ? 0
                    : navigation_current(
                          &engine->navigation)->scroll_y;
        }
        /* Focus chosen on the preview stays; an activation that waited
           for the page runs now (the frontend takes it). */
        if (provisional_focus != NULL
            && browser_engine_focus_node(engine, provisional_focus))
            engine->provisional_activation = provisional_deferred
                ? BROWSER_DEFERRED_ACTIVATION_READY
                : BROWSER_DEFERRED_ACTIVATION_IDLE;
        work->status = BROWSER_NAVIGATION_JOB_SUCCEEDED;
        work->metrics.status = work->status;
        work->metrics.completion_per_mille = 1000;
        clear_error(engine);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            "navigation-committed", "candidate load committed",
            engine->navigation.loads_committed,
            engine->navigation.page.document.node_count);
    } else if (terminal == NAVIGATION_LOAD_CANCELLED) {
        work->status = BROWSER_NAVIGATION_JOB_CANCELLED;
        work->metrics.status = work->status;
        snprintf(engine->last_error, sizeof(engine->last_error), "%.255s",
                 engine->navigation.last_error[0] == '\0'
                   ? "navigation cancelled"
                   : engine->navigation.last_error);
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_CANCELLED,
            "navigation-cancelled", engine->last_error, 0, 0);
    } else {
        const char *message = engine->navigation.last_error[0] != '\0'
            ? engine->navigation.last_error
            : (engine->last_error[0] != '\0'
               ? engine->last_error : "navigation failed");
        work->status = BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        (void) set_error_code(
            engine, work->failure_subsystem, work->failure_code,
            "navigation-failed", message);
    }
    if (!succeeded && had_provisional && engine->framebuffer != NULL) {
        (void) tilefinch_platform_present_rgb565(
            engine->framebuffer,
            (size_t) engine->config.device.framebuffer_width,
            (size_t) engine->config.device.framebuffer_height,
            (size_t) engine->config.device.framebuffer_width);
    }
    browser_engine_reset_provisional_viewport(engine);
    return work->status;
}

static bool browser_engine_begin_navigation_request(
    BrowserEngine *engine, const char *url, const char *method,
    const char *body, size_t body_length, const char *content_type,
    size_t maximum_bytes, long timeout_ms, bool record_history,
    bool signin_recovery)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || url == NULL || method == NULL
        || url[0] == '\0' || strlen(url) >= NAVIGATION_URL_LIMIT
        || maximum_bytes == 0 || timeout_ms <= 0
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "browser navigation job cannot be started");
    }
    browser_engine_youtube_on_navigation_begin(engine, url, signin_recovery);
    (void) browser_engine_youtube_cancel(engine);
    /* Only the navigation that was refused may fall back. */
    site_identity_clear_google_fallback();
    browser_engine_store_current_focus(engine);
    browser_engine_store_current_controls(engine);
    /* A deferred activation belongs to the page that committed it; one the
       frontend never took must not act on the next page. */
    engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_IDLE;
    /* Native HOME can become interactive before any page font file is read.
       If the user outruns Wi-Fi association, finish the two-face baseline
       here before the candidate is allowed to measure text. */
    if (!browser_engine_prepare_navigation_fonts(engine, method, url)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, "font-baseline",
            "bounded baseline font loading failed");
    }
    /* Release incumbent-page transport lanes before the candidate scheduler
       tries to enqueue its authoritative document request. On PSP the six
       shared descriptors can otherwise all belong to thumbnails, fonts or
       script fetches from the page whose link was just activated. */
    (void) navigation_cancel_network_work(
        &engine->navigation, "superseded by a new navigation");
    navigation_retire_large_incumbent_realm(&engine->navigation);
    /* A candidate navigation keeps the incumbent page alive. End any gesture
       against that page before input is suppressed, so a release after a
       cancelled navigation cannot turn into a stale click. */
    if (engine->controller_ready
        && engine->controller.pointer_down_active) {
        size_t relayouts = engine->navigation.incremental_relayouts;
        bool ignored_activation = false;
        (void) controller_pointer_event(
            &engine->controller, CONTROLLER_POINTER_CANCEL, 0, 0,
            &ignored_activation);
        if (engine->render_ready
            && relayouts != engine->navigation.incremental_relayouts) {
            (void) browser_engine_apply_layout_damage(engine);
        }
    }
    controller_pointer_discard_click(&engine->controller);
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    *work = (BrowserEngineNavigationWork) {
        .status = BROWSER_NAVIGATION_JOB_PENDING,
        .failure_subsystem = TILEFINCH_SUBSYSTEM_NETWORK,
        .failure_code = TILEFINCH_DIAGNOSTIC_NETWORK_FAILED,
        .previous_backing = engine->backing,
        .previous_backing_valid =
            document_backing_valid(&engine->backing),
        .started_us =
            tilefinch_platform_monotonic_time_us(),
        .record_history = record_history
    };
    snprintf(work->url, sizeof(work->url), "%s", url);
    work->metrics.status = BROWSER_NAVIGATION_JOB_PENDING;
    if (engine->navigation.page.loaded)
        work->metrics.incumbent_pages_preserved = 1;
    engine->autofocus_pending = false;
    browser_engine_cancel_idle_work(engine);
    browser_engine_cancel_render_job(engine);
    browser_engine_reset_provisional_viewport(engine);
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-start", "candidate load started",
        engine->navigation.loads_started + 1, 0);
    if (!navigation_set_candidate_commit_hooks(
            &engine->navigation, browser_engine_prepare_candidate_shell,
            browser_engine_abort_candidate_shell,
            browser_engine_commit_candidate_shell, engine)) {
        if (work->previous_backing_valid)
            engine->backing = work->previous_backing;
        work->status = BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-shell-hook",
            "could not install the candidate render-shell transaction");
    }
    engine->navigation.candidate_graph_prepare = browser_engine_restore_candidate_controls;
    work->hooks_installed = true;
    uint64_t generation = navigation_begin(&engine->navigation);
    work->generation = generation;
    uint64_t navigation_started_us =
        tilefinch_platform_monotonic_time_us();
    work->metrics.navigation_session_started_us =
        navigation_started_us >= work->started_us
            ? navigation_started_us - work->started_us : 0;
    if (site_adapter_handles_navigation(method, url)) {
        char error[256] = {0};
        SiteAdapterPreferences preferences = {
            .youtube_compact_results = engine->youtube.compact_results,
            .navigation_initiator = engine->navigation.page.loaded
                ? engine->navigation.page.document_url : NULL,
            .navigation_user_activated = engine->navigation.pending_navigation_user_activated
        };
        work->adapter = site_adapter_load_begin(
            &engine->budget, &engine->session, method, url, &preferences,
            maximum_bytes, timeout_ms, error, sizeof(error));
        if (work->adapter == NULL) {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%.255s",
                     error[0] == '\0'
                         ? "site adapter load allocation failed" : error);
            (void) browser_engine_finish_navigation_work(
                engine, NAVIGATION_LOAD_FAILED);
            return false;
        }
        return true;
    }
    work->load = navigation_load_begin_request(
        &engine->navigation, generation, url, method, body, body_length,
        content_type, maximum_bytes, timeout_ms,
        engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL, record_history);
    if (work->load == NULL) {
        if (engine->navigation.last_error[0] == '\0') {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%s",
                     "navigation load allocation failed");
        }
        (void) browser_engine_finish_navigation_work(
            engine, NAVIGATION_LOAD_FAILED);
        return false;
    }
    return true;
}

void browser_engine_set_defer_script_navigation(BrowserEngine *engine,
                                                bool defer)
{
    if (engine != NULL) engine->navigation.defer_script_navigation = defer;
}

bool browser_engine_take_script_navigation(BrowserEngine *engine,
                                           char *url, size_t capacity,
                                           bool *record_history)
{
    if (engine == NULL
        || !navigation_take_script_navigation(
               &engine->navigation, url, capacity, record_history))
        return false;
    engine->script_navigation_attributed = true;
    return true;
}

NavigationRefreshTake browser_engine_take_refresh_navigation(
    BrowserEngine *engine, char *url, size_t capacity)
{
    if (engine == NULL) return NAVIGATION_REFRESH_TAKE_NONE;
    NavigationRefreshTake taken = navigation_take_refresh_navigation(
        &engine->navigation, url, capacity);
    /* The document is the initiator, as for a script navigation. */
    if (taken == NAVIGATION_REFRESH_TAKE_NAVIGATE)
        engine->script_navigation_attributed = true;
    return taken;
}

void browser_engine_note_user_input(BrowserEngine *engine)
{
    if (engine != NULL) navigation_note_user_input(&engine->navigation);
}

bool browser_engine_capture_script_navigation_attribution(
    const BrowserEngine *engine, BrowserScriptNavigationAttribution *out)
{
    if (engine == NULL || out == NULL
        || !engine->script_navigation_attributed
        || engine->navigation.pending_navigation_referer[0] == '\0')
        return false;
    snprintf(out->initiator_url, sizeof(out->initiator_url), "%s",
             engine->navigation.pending_navigation_referer);
    out->user_activated =
        engine->navigation.pending_navigation_user_activated;
    /* The first attempt's navigation_begin takes the arming with it. */
    out->replace_history = engine->navigation.history_replace_armed;
    out->replace_url_hash = engine->navigation.history_replace_url_hash;
    return true;
}

void browser_engine_restore_script_navigation_attribution(
    BrowserEngine *engine, const BrowserScriptNavigationAttribution *attribution)
{
    if (engine == NULL || attribution == NULL
        || attribution->initiator_url[0] == '\0') return;
    snprintf(engine->navigation.pending_navigation_referer,
             sizeof(engine->navigation.pending_navigation_referer), "%s",
             attribution->initiator_url);
    engine->navigation.pending_navigation_user_activated =
        attribution->user_activated;
    if (attribution->replace_history) {
        engine->navigation.history_replace_url_hash =
            attribution->replace_url_hash;
        engine->navigation.history_replace_armed = true;
    }
    engine->script_navigation_attributed = true;
}

static bool browser_engine_collect_script_form_submission(BrowserEngine *engine,
                                                          ControllerAction *action)
{
    if (engine == NULL || action == NULL
        || !engine->navigation.page.loaded
        || engine->navigation.page.runtime == NULL) return false;
    lxb_dom_node_t *form = NULL, *submitter = NULL;
    char *body = NULL;
    size_t body_length = 0;
    bool script_body = script_runtime_take_form_submission_body(
        engine->navigation.page.runtime, &engine->budget,
        BROWSER_ENGINE_SCRIPT_FORM_BODY_LIMIT, &body, &body_length);
    if (!script_runtime_take_form_submission(
            engine->navigation.page.runtime, &form, &submitter)) {
        budget_free(&engine->budget, body);
        return false;
    }
    if (!controller_build_form_action(
            &engine->controller, form, submitter, false, action)
        || (action->type != CONTROLLER_ACTION_FORM_SUBMIT
            && action->type != CONTROLLER_ACTION_NAVIGATE)) {
        budget_free(&engine->budget, body);
        return false;
    }
    /* Preserve the entry list produced by script, including formdata
       additions/deletions, rather than serializing the DOM a second time.
       Large entry lists use the engine-owned bounded navigation storage. */
    if (script_body && action->type == CONTROLLER_ACTION_FORM_SUBMIT
        && strcasecmp(action->method, "POST") == 0
        && (action->content_type[0] == '\0'
            || strncasecmp(action->content_type,
                           "application/x-www-form-urlencoded", 33) == 0)) {
        controller_action_clear_body(action);
        action->body_length = body_length;
        if (body_length < sizeof(action->body)) {
            memcpy(action->body, body, body_length + 1);
        } else {
            budget_free(&engine->budget, engine->script_form_body);
            engine->script_form_body = body;
            action->external_body = body;
            body = NULL;
        }
    }
    budget_free(&engine->budget, body);
    return true;
}

bool browser_engine_take_script_form_submission(BrowserEngine *engine,
                                               ControllerAction *action)
{
    return engine != NULL && engine->navigation.defer_script_navigation
        && browser_engine_collect_script_form_submission(engine, action);
}

bool browser_engine_begin_navigation_url(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history)
{
    /* An explicit replacement (Reload) requests fresh provider facts.
       History moves use their own entry point and retain bounded reuse. */
    if (engine != NULL && !record_history && engine->navigation.page.loaded)
        browser_session_site_adapter_document_cache_clear(&engine->session);
    if (engine != NULL && !engine->script_navigation_attributed) {
        engine->navigation.pending_navigation_referer[0] = '\0';
        engine->navigation.pending_navigation_user_activated = false;
        /* A browser-initiated address (typed, bookmarked, restored) is the
           user's: the refresh loop guard starts over. */
        navigation_note_user_input(&engine->navigation);
    } else if (engine != NULL
               && engine->navigation.pending_navigation_referer[0] == '\0') {
        /* A script without a document initiator is never a typed address. */
        engine->navigation.pending_navigation_user_activated = false;
    }
    if (engine != NULL) engine->script_navigation_attributed = false;
    return browser_engine_begin_navigation_request(
        engine, url, "GET", NULL, 0, NULL,
        maximum_bytes, timeout_ms, record_history, false);
}

bool browser_engine_begin_navigation_action(
    BrowserEngine *engine, const ControllerAction *action,
    size_t maximum_bytes, long timeout_ms)
{
    if (engine == NULL || action == NULL
        || (action->type != CONTROLLER_ACTION_NAVIGATE
            && action->type != CONTROLLER_ACTION_FORM_SUBMIT)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-action",
            "controller action does not contain a navigation");
    }
    const char *current_url =
        navigation_active_document_url(&engine->navigation);
    if (current_url != NULL) {
        snprintf(engine->navigation.pending_navigation_referer,
                 sizeof(engine->navigation.pending_navigation_referer),
                 "%s", current_url);
    }
    engine->navigation.pending_navigation_user_activated =
        action->navigation_source == CONTROLLER_NAVIGATION_USER;
    if (action->navigation_source == CONTROLLER_NAVIGATION_USER)
        navigation_note_user_input(&engine->navigation);
    const char *method = action->type == CONTROLLER_ACTION_FORM_SUBMIT
        ? action->method : "GET";
    if (action->type == CONTROLLER_ACTION_NAVIGATE
        && strcasecmp(method, "GET") == 0
        && navigation_url_is_same_document(
               &engine->navigation, action->url)) {
        BrowserEngineNavigationWork *work = &engine->navigation_work;
        *work = (BrowserEngineNavigationWork) {
            .status = BROWSER_NAVIGATION_JOB_SUCCEEDED,
            .started_us = tilefinch_platform_monotonic_time_us(),
            .record_history = true
        };
        snprintf(work->url, sizeof(work->url), "%s", action->url);
        bool committed = browser_engine_commit_same_document_action(
            engine, action->url);
        engine->navigation.pending_navigation_user_activated = false;
        work->status = committed ? BROWSER_NAVIGATION_JOB_SUCCEEDED
                                 : BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        work->metrics.completion_per_mille = committed ? 1000u : 0u;
        if (committed) clear_error(engine);
        return committed;
    }
    bool began = browser_engine_begin_navigation_request(
        engine, action->url, method,
        action->body_length == 0 ? NULL : controller_action_body(action),
        action->body_length,
        action->content_type[0] == '\0' ? NULL : action->content_type,
        maximum_bytes, timeout_ms, true, false);
    if (!began)
        engine->navigation.pending_navigation_user_activated = false;
    return began;
}

typedef struct {
    size_t previous_index;
    char previous_url[NAVIGATION_URL_LIMIT];
    char url[NAVIGATION_URL_LIMIT];
    BrowserHistoryView view;
} BrowserHistoryRestore;

/* Snapshot the target before any navigation preparation can mutate the
   history vector. No borrowed entry pointer survives this preparation. */
static bool browser_engine_prepare_history(BrowserEngine *engine, bool forward,
                                           BrowserHistoryRestore *restore)
{
    browser_engine_store_current_focus(engine);
    browser_engine_store_current_controls(engine);
    restore->previous_index = engine->navigation.history_index;
    const char *active_url = navigation_active_document_url(&engine->navigation);
    snprintf(restore->previous_url, sizeof(restore->previous_url), "%s",
             active_url == NULL ? "" : active_url);
    const NavigationEntry *entry = NULL;
    bool moved = forward ? navigation_forward(&engine->navigation, &entry)
                         : navigation_back(&engine->navigation, &entry);
    if (!moved || entry == NULL) return false;
    snprintf(restore->url, sizeof(restore->url), "%s", entry->url);
    browser_engine_history_view_from_entry(&restore->view, entry);
    return true;
}

typedef enum {
    HISTORY_RESTORE_OK,
    HISTORY_RESTORE_PREFLIGHT_FAILED,
    HISTORY_RESTORE_SETTLE_FAILED,
    HISTORY_RESTORE_INPUT_FAILED
} BrowserHistoryRestoreStatus;

static BrowserHistoryRestoreStatus browser_engine_restore_history_document(
    BrowserEngine *engine, bool forward, const BrowserHistoryRestore *restore)
{
    if (!navigation_restore_same_document_url(&engine->navigation,
            restore->url, restore->previous_url)) {
        browser_engine_rollback_same_document_cursor(engine, forward);
        return HISTORY_RESTORE_PREFLIGHT_FAILED;
    }
    if (!browser_engine_settle_same_document_events(engine))
        return HISTORY_RESTORE_SETTLE_FAILED;
    lxb_dom_node_t *focus = browser_engine_history_focus(
        engine, restore->view.focus_kind, restore->view.focus_index,
        restore->view.focus_identity);
    if (focus != NULL && !controller_focus_node(&engine->controller, focus))
        return HISTORY_RESTORE_INPUT_FAILED;
    if (!navigation_set_scroll(&engine->navigation, restore->view.scroll_y)
        || !browser_engine_finish_input(engine, true))
        return HISTORY_RESTORE_INPUT_FAILED;
    return HISTORY_RESTORE_OK;
}

bool browser_engine_begin_navigation_history(
    BrowserEngine *engine, bool forward, size_t maximum_bytes,
    long timeout_ms)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-history",
            "history navigation cannot be started");
    }
    BrowserHistoryRestore restore;
    if (!browser_engine_prepare_history(engine, forward, &restore)) {
        return set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
            "navigation-history", forward ? "no forward history entry is available" : "no back history entry is available");
    }
    size_t previous_index = restore.previous_index;
    const char *previous_url = restore.previous_url;
    const char *url = restore.url;
    if (navigation_url_is_same_document(
            &engine->navigation, previous_url)) {
        BrowserEngineNavigationWork *work = &engine->navigation_work;
        *work = (BrowserEngineNavigationWork) {
            .status = BROWSER_NAVIGATION_JOB_SUCCEEDED,
            .previous_history_index = previous_index,
            .restore = restore.view,
            .started_us = tilefinch_platform_monotonic_time_us(),
            .history_move = true,
            .history_forward = forward
        };
        snprintf(work->url, sizeof(work->url), "%s", url);
        BrowserHistoryRestoreStatus result =
            browser_engine_restore_history_document(engine, forward, &restore);
        bool restored = result == HISTORY_RESTORE_OK;
        if (result == HISTORY_RESTORE_SETTLE_FAILED) {
            (void) set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "history-event-settle",
                "same-document history event layout could not be settled");
        }
        work->status = restored ? BROWSER_NAVIGATION_JOB_SUCCEEDED
                                : BROWSER_NAVIGATION_JOB_FAILED;
        work->metrics.status = work->status;
        work->metrics.completion_per_mille = restored ? 1000u : 0u;
        if (restored) clear_error(engine);
        return restored;
    }
    bool began = browser_engine_begin_navigation_request(
        engine, url, "GET", NULL, 0u, NULL,
        maximum_bytes, timeout_ms, false, false);
    /* begin_navigation_request snapshots the currently indexed entry. During
       a history move the cursor already names the target while the incumbent
       DOM is still displayed, so restore the target metadata captured above
       instead of binding the incumbent controls to the target URL. Do this
       even when request preparation fails so a later retry still has the
       original target state. */
    const BrowserHistoryView *view = &restore.view;
    NavigationEntry *target_entry =
        &engine->navigation.history[engine->navigation.history_index];
    target_entry->focus_kind = view->focus_kind;
    target_entry->focus_index = view->focus_index;
    target_entry->focus_identity = view->focus_identity;
    target_entry->control_count = view->control_count;
    memcpy(target_entry->controls, view->controls,
           view->control_count * sizeof(*view->controls));
    if (!began) {
        browser_engine_rollback_same_document_cursor(engine, forward);
        return false;
    }
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    work->history_move = true;
    work->history_forward = forward;
    work->previous_history_index = previous_index;
    work->restore = *view;
    return true;
}

/* A refused opted-in Google search (site_identity.h) fails without
   committing and clears the opt-in. Continue the same job on the local
   compatibility page for that query, which now carries its refusal note.
   The fallback replaces or adds a history entry as the refused navigation
   would have; a refused history traversal only restores its entry. */
static BrowserNavigationJobStatus browser_engine_google_fallback(
    BrowserEngine *engine, BrowserNavigationJobStatus finished)
{
    char url[NAVIGATION_URL_LIMIT];
    if (finished != BROWSER_NAVIGATION_JOB_FAILED
        || !site_identity_take_google_fallback(url, sizeof(url))
        || engine->navigation_work.history_move
        || !site_adapter_handles_navigation("GET", url)) return finished;
    bool record_history = engine->navigation_work.record_history;
    engine->navigation.pending_navigation_referer[0] = '\0';
    engine->navigation.pending_navigation_user_activated = false;
    engine->script_navigation_attributed = false;
    if (!browser_engine_begin_navigation_request(
            engine, url, "GET", NULL, 0, NULL,
            engine->config.maximum_document_bytes,
            engine->config.navigation_timeout_ms, record_history, false))
        return finished;
    return engine->navigation_work.status;
}

BrowserNavigationJobStatus browser_engine_pump_navigation(
    BrowserEngine *engine, const BrowserNavigationJobQuota *quota)
{
    if (engine == NULL) return BROWSER_NAVIGATION_JOB_FAILED;
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (work->status != BROWSER_NAVIGATION_JOB_PENDING)
        return work->status;
    if (work->adapter != NULL) {
        uint64_t started_us =
            tilefinch_platform_monotonic_time_us();
        work->metrics.pump_calls++;
        const FetchPumpQuota *fetch_quota =
            quota == NULL ? NULL : &quota->load.fetch;
        SiteAdapterLoadStatus adapter_status =
            site_adapter_load_pump(work->adapter, fetch_quota);
        if (adapter_status == SITE_ADAPTER_LOAD_PENDING)
            return BROWSER_NAVIGATION_JOB_PENDING;
        SiteAdapterDocument document = {0};
        bool acquired = adapter_status == SITE_ADAPTER_LOAD_SUCCEEDED
            && site_adapter_load_take_document(work->adapter, &document);
        const char *commit_url =
            acquired ? browser_engine_youtube_commit_url(work->url) : work->url;
        if (commit_url != work->url)
            snprintf(work->url, sizeof(work->url), "%s", commit_url);
        uint64_t parse_before = engine->navigation.performance.parse_us;
        uint64_t style_before = engine->navigation.performance.style_us;
        uint64_t resource_before = engine->navigation.performance.resource_us;
        uint64_t layout_before = engine->navigation.performance.layout_us;
        uint64_t runtime_before = engine->navigation.performance.runtime_us;
        uint64_t commit_started_us = acquired
            ? tilefinch_platform_monotonic_time_us() : 0;
        bool committed = acquired && navigation_commit_static_html(
            &engine->navigation, work->generation, work->url,
            document.html, document.html_length,
            engine->config.device.navigation_viewport_width,
            engine->fonts_ready ? &engine->fonts : NULL, NULL,
            work->record_history);
        browser_engine_youtube_on_commit(engine, committed);
        uint64_t commit_finished_us = acquired
            ? tilefinch_platform_monotonic_time_us() : 0;
        if (acquired) {
            NavigationPerformance *performance =
                &engine->navigation.performance;
#define ADAPTER_DELTA(output, input, before) \
            work->metrics.output = performance->input >= (before) \
                ? performance->input - (before) : 0
            ADAPTER_DELTA(adapter_parse_us, parse_us, parse_before);
            ADAPTER_DELTA(adapter_style_us, style_us, style_before);
            ADAPTER_DELTA(
                adapter_resource_us, resource_us, resource_before);
            ADAPTER_DELTA(adapter_layout_us, layout_us, layout_before);
            ADAPTER_DELTA(adapter_runtime_us, runtime_us, runtime_before);
#undef ADAPTER_DELTA
            work->metrics.adapter_commit_us =
                commit_finished_us >= commit_started_us
                    ? commit_finished_us - commit_started_us : 0;
        }
        if (acquired) {
            engine->navigation.last_http_status = document.status_code;
            snprintf(engine->navigation.last_server,
                     sizeof(engine->navigation.last_server), "%s",
                     document.server);
            snprintf(engine->navigation.last_cf_mitigated,
                     sizeof(engine->navigation.last_cf_mitigated), "%s",
                     document.cf_mitigated);
        }
        site_adapter_document_destroy(&document);
        uint64_t finished_us =
            tilefinch_platform_monotonic_time_us();
        uint64_t unit_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        SiteAdapterLoadMetrics adapter_metrics = {0};
        (void) site_adapter_load_metrics(
            work->adapter, &adapter_metrics);
        work->metrics.maximum_irreducible_unit_us =
            unit_us > adapter_metrics.maximum_irreducible_unit_us
                ? unit_us : adapter_metrics.maximum_irreducible_unit_us;
        work->metrics.maximum_transform_slice_us =
            adapter_metrics.maximum_transform_slice_us;
        work->metrics.transform_slices =
            adapter_metrics.build_slices;
        work->metrics.transform_quota_overruns =
            adapter_metrics.transform_quota_overruns;
        work->metrics.maximum_transform_slice_bytes =
            adapter_metrics.maximum_transform_slice_bytes;
        uint64_t advisory_us = quota == NULL
            ? 0 : quota->load.maximum_parser_time_us;
        if (advisory_us != 0
            && work->metrics.maximum_irreducible_unit_us
                   > advisory_us) {
            work->metrics.irreducible_unit_overruns++;
        }
        if (!acquired) {
            const char *adapter_error =
                site_adapter_load_error(work->adapter);
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%.255s",
                     adapter_status == SITE_ADAPTER_LOAD_CANCELLED
                         ? "site adapter navigation cancelled"
                         : (adapter_error[0] == '\0'
                            ? "site adapter produced no document"
                            : adapter_error));
        }
        return browser_engine_finish_navigation_work(
            engine, committed
                ? NAVIGATION_LOAD_SUCCEEDED
                : (adapter_status == SITE_ADAPTER_LOAD_CANCELLED
                    ? NAVIGATION_LOAD_CANCELLED
                    : NAVIGATION_LOAD_FAILED));
    }
    if (work->load == NULL) return BROWSER_NAVIGATION_JOB_FAILED;
    NavigationLoadStatus status = navigation_load_status(work->load);
    const NavigationLoadQuota *load_quota =
        quota == NULL ? NULL : &quota->load;
    if (status == NAVIGATION_LOAD_PENDING) {
        work->metrics.pump_calls++;
        status = navigation_load_pump(work->load, load_quota);
        const char *redirect = navigation_load_adapter_redirect(work->load);
        if (redirect != NULL) {
            snprintf(work->url, sizeof(work->url), "%s", redirect);
            navigation_load_destroy(work->load);
            work->load = NULL;
            SiteAdapterPreferences preferences = {
                .youtube_compact_results = engine->youtube.compact_results
            };
            work->adapter = site_adapter_load_begin(
                &engine->budget, &engine->session, "GET", work->url,
                &preferences, engine->config.maximum_document_bytes,
                engine->config.navigation_timeout_ms,
                engine->navigation.last_error,
                sizeof(engine->navigation.last_error));
            if (work->adapter != NULL) return BROWSER_NAVIGATION_JOB_PENDING;
            return browser_engine_finish_navigation_work(engine,
                                                         NAVIGATION_LOAD_FAILED);
        }
        if (status == NAVIGATION_LOAD_PENDING
            || status == NAVIGATION_LOAD_READY_TO_FINISH) {
            return BROWSER_NAVIGATION_JOB_PENDING;
        }
    } else if (status == NAVIGATION_LOAD_READY_TO_FINISH
               || status == NAVIGATION_LOAD_FINALIZING) {
        work->metrics.pump_calls++;
        (void) navigation_load_finish(work->load, load_quota);
        status = navigation_load_status(work->load);
        if (status == NAVIGATION_LOAD_READY_TO_FINISH
            || status == NAVIGATION_LOAD_FINALIZING) {
            return BROWSER_NAVIGATION_JOB_PENDING;
        }
    }
    return browser_engine_google_fallback(
        engine, browser_engine_finish_navigation_work(engine, status));
}

void browser_engine_cancel_navigation(BrowserEngine *engine,
                                      const char *reason)
{
    if (engine == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING) return;
    const char *message = reason == NULL
        ? "navigation cancelled by browser UI" : reason;
    if (engine->navigation_work.adapter != NULL) {
        site_adapter_load_cancel(
            engine->navigation_work.adapter, message);
        snprintf(engine->navigation.last_error,
                 sizeof(engine->navigation.last_error), "%s", message);
    } else {
        navigation_load_cancel(engine->navigation_work.load, message);
    }
    (void) browser_engine_finish_navigation_work(
        engine, NAVIGATION_LOAD_CANCELLED);
}

size_t browser_engine_cancel_network_work(
    BrowserEngine *engine, const char *reason)
{
    if (engine == NULL) return 0;
    size_t cancelled = browser_engine_youtube_cancel(engine);
    return cancelled + navigation_cancel_network_work(&engine->navigation, reason);
}

BrowserNavigationJobStatus browser_engine_navigation_status(
    const BrowserEngine *engine)
{
    return engine == NULL ? BROWSER_NAVIGATION_JOB_FAILED
                          : engine->navigation_work.status;
}

void browser_engine_set_heavy_page_policy(BrowserEngine *engine,
                                          ScriptHeavyPolicy policy)
{
    if (engine == NULL) return;
    engine->navigation.heavy_script_policy = policy;
    if (engine->navigation.page.runtime != NULL) {
        script_runtime_set_heavy_policy(engine->navigation.page.runtime,
                                        policy);
    }
}

void browser_engine_set_heavy_page_resolver(
    BrowserEngine *engine, ScriptHeavyPolicyResolver resolver, void *opaque)
{
    if (engine == NULL) return;
    engine->navigation.heavy_policy_resolver = resolver;
    engine->navigation.heavy_policy_opaque = resolver != NULL ? opaque : NULL;
}

bool browser_engine_heavy_page(BrowserEngine *engine,
                               ScriptHeavyState *state)
{
    if (state != NULL) memset(state, 0, sizeof(*state));
    return engine != NULL && engine->navigation.page.loaded
        && engine->navigation.page.runtime != NULL
        && script_runtime_heavy_state(engine->navigation.page.runtime, state);
}

bool browser_engine_answer_heavy_page(BrowserEngine *engine, bool run)
{
    return engine != NULL && engine->navigation.page.runtime != NULL
        && script_runtime_answer_heavy(engine->navigation.page.runtime, run);
}

bool browser_engine_stop_page_scripts(BrowserEngine *engine)
{
    if (engine == NULL || !engine->navigation.page.loaded
        || engine->navigation.page.runtime == NULL) return false;
    navigation_retire_current_page_scripts(&engine->navigation);
    engine->navigation.page.script_degradation_observed = true;
    snprintf(engine->navigation.page.script_result.summary,
             sizeof(engine->navigation.page.script_result.summary),
             "Page scripts stopped");
    return true;
}

bool browser_engine_page_memory_rescued(const BrowserEngine *engine)
{
    return engine != NULL && engine->navigation.page.loaded
        && navigation_page_memory_rescued(&engine->navigation);
}

bool browser_engine_parser_script_time(const BrowserEngine *engine,
                                      uint64_t *used_us, uint64_t *limit_us)
{
    if (used_us != NULL) *used_us = 0;
    if (limit_us != NULL) *limit_us = 0;
    return engine != NULL
        && engine->navigation_work.status == BROWSER_NAVIGATION_JOB_PENDING
        && navigation_load_parser_script_time(
               engine->navigation_work.load, used_us, limit_us);
}

bool browser_engine_navigation_pending(const BrowserEngine *engine)
{
    return browser_engine_navigation_status(engine)
        == BROWSER_NAVIGATION_JOB_PENDING;
}

BrowserNavigationReturnTarget browser_engine_last_navigation_return_target(
    const BrowserEngine *engine)
{
    if (engine == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_SUCCEEDED
        || !engine->navigation.page.loaded
        || engine->navigation.history_count == 0u) {
        return BROWSER_NAVIGATION_RETURN_NONE;
    }
    const BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (work->history_move) {
        if (work->history_forward) {
            return engine->navigation.history_index > 0u
                ? BROWSER_NAVIGATION_RETURN_BACK
                : BROWSER_NAVIGATION_RETURN_NONE;
        }
        return engine->navigation.history_index + 1u
                   < engine->navigation.history_count
            ? BROWSER_NAVIGATION_RETURN_FORWARD
            : BROWSER_NAVIGATION_RETURN_NONE;
    }
    return engine->navigation.history_index > 0u
        ? BROWSER_NAVIGATION_RETURN_BACK
        : BROWSER_NAVIGATION_RETURN_NONE;
}

const char *browser_engine_pending_navigation_url(
    const BrowserEngine *engine)
{
    return browser_engine_navigation_pending(engine)
        ? engine->navigation_work.url : NULL;
}

bool browser_engine_navigation_job_metrics(
    const BrowserEngine *engine, BrowserNavigationJobMetrics *metrics)
{
    if (engine == NULL || metrics == NULL) return false;
    *metrics = engine->navigation_work.metrics;
    metrics->status = engine->navigation_work.status;
    if (engine->navigation_work.load != NULL) {
        (void) navigation_load_metrics(
            engine->navigation_work.load, &metrics->load);
        metrics->completion_per_mille =
            metrics->load.completion_per_mille;
    } else if (engine->navigation_work.adapter != NULL) {
        SiteAdapterLoadMetrics adapter = {0};
        if (site_adapter_load_metrics(
                engine->navigation_work.adapter, &adapter)) {
            browser_engine_copy_adapter_metrics(metrics, &adapter);
        }
    }
    if (engine->navigation_work.status
            == BROWSER_NAVIGATION_JOB_SUCCEEDED) {
        metrics->completion_per_mille = 1000;
    } else if (metrics->completion_per_mille > 999u) {
        metrics->completion_per_mille = 999u;
    }
    if (engine->navigation_work.load != NULL
        || engine->navigation_work.adapter != NULL) {
        uint64_t now_us =
            tilefinch_platform_monotonic_time_us();
        metrics->elapsed_us =
            now_us >= engine->navigation_work.started_us
                ? now_us - engine->navigation_work.started_us : 0;
    }
    return true;
}

#include "browser_engine/provisional_input.inc"

typedef bool (*BrowserEngineLoadOperation)(BrowserEngine *engine,
                                           uint64_t generation,
                                           void *opaque);

static bool browser_engine_run_load(BrowserEngine *engine,
                                    BrowserEngineLoadOperation operation,
                                    void *opaque,
                                    TilefinchDiagnosticSubsystem subsystem,
                                    TilefinchDiagnosticCode failure_code)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || operation == NULL) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "browser engine is not active");
    }
    if (browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "navigation-start",
            "a responsive navigation job is already active");
    }
    /* The incumbent remains the visual/DOM rollback candidate, but its
       optional resource and script fetches must not occupy every shared PSP
       transport descriptor ahead of the authoritative replacement. A
       failed candidate can still restore the incumbent pixels and document;
       only unfinished network embellishment is superseded. */
    (void) browser_engine_youtube_cancel(engine);
    (void) navigation_cancel_network_work(
        &engine->navigation, "superseded by a new navigation");
    navigation_retire_large_incumbent_realm(&engine->navigation);
    engine->autofocus_pending = false;
    browser_engine_cancel_idle_work(engine);
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-start", "candidate load started",
        engine->navigation.loads_started + 1, 0);
    DocumentBacking previous = engine->backing;
    bool previous_valid = document_backing_valid(&previous);
    if (!navigation_set_candidate_commit_hooks(
            &engine->navigation, browser_engine_prepare_candidate_shell,
            browser_engine_abort_candidate_shell,
            browser_engine_commit_candidate_shell, engine)) {
        if (previous_valid) engine->backing = previous;
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "candidate-shell-hook",
            "could not install the candidate render-shell transaction");
    }
    engine->navigation.candidate_graph_prepare = browser_engine_restore_candidate_controls;
    uint64_t generation = navigation_begin(&engine->navigation);
    bool loaded = operation(engine, generation, opaque);
    (void) navigation_set_candidate_commit_hooks(
        &engine->navigation, NULL, NULL, NULL, NULL);
    if (engine->candidate_shell_prepared
        || engine->candidate_render.budget != NULL)
        browser_engine_abort_candidate_shell(engine);
    if (loaded && engine->navigation.page.loaded) {
        if (!browser_engine_bind_current_full_document(engine)) loaded = false;
    } else if (engine->navigation.page.loaded && previous_valid) {
        engine->backing = previous;
    } else {
        document_backing_clear(&engine->backing);
    }
    bool shell_ready = loaded && engine->navigation.page.loaded
        && engine->controller_ready
        && (engine->config.tile_capacity == 0 || engine->render_ready);
    if (!loaded) {
        const char *message = engine->navigation.last_error[0] != '\0'
            ? engine->navigation.last_error
            : (engine->last_error[0] != '\0'
               ? engine->last_error : "navigation failed");
        return set_error_code(engine, subsystem, failure_code,
                              "navigation-failed", message);
    }
    if (!shell_ready) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "shell-commit",
            "committed navigation has no promoted render shell");
    }
    /* Synchronous commits (native/generated pages and host loads) use this
       path rather than finish_navigation_work. They need the same font census
       so a late metric face cannot be skipped merely because no responsive
       fetch job owned the commit. */
    browser_engine_census_page_fonts(engine);
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_INFO,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
        "navigation-committed", "candidate load committed",
        engine->navigation.loads_committed,
        engine->navigation.page.document.node_count);
    return true;
}

typedef struct {
    const char *url;
    const char *html;
    size_t html_length;
    bool record_history;
    bool static_local;
} BrowserEngineHtmlLoad;

static bool browser_engine_do_commit(BrowserEngine *engine,
                                     uint64_t generation, void *opaque)
{
    const BrowserEngineHtmlLoad *load = opaque;
    if (load->static_local)
        return navigation_commit_static_html(
            &engine->navigation, generation, load->url, load->html,
            load->html_length, engine->config.device.navigation_viewport_width,
            engine->fonts_ready ? &engine->fonts : NULL, NULL,
            load->record_history);
    return navigation_commit_html(
        &engine->navigation, generation, load->url, load->html,
        load->html_length, engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL,
        load->record_history);
}

bool browser_engine_commit_html(BrowserEngine *engine, const char *url,
                                const char *html, size_t html_length,
                                bool record_history)
{
    if (url == NULL || html == NULL) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_PARSER, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "html-load", "HTML load is invalid");
    BrowserEngineHtmlLoad load = {
        .url = url,
        .html = html,
        .html_length = html_length,
        .record_history = record_history
    };
    if (record_history && engine != NULL && engine->navigation.page.loaded) {
        browser_engine_store_current_focus(engine);
        browser_engine_store_current_controls(engine);
    }
    return browser_engine_run_load(
        engine, browser_engine_do_commit, &load, TILEFINCH_SUBSYSTEM_PARSER,
        TILEFINCH_DIAGNOSTIC_PARSE_FAILED);
}

bool browser_engine_commit_empty_document(BrowserEngine *engine,
                                          const char *url, bool record_history)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || engine->navigation.page.loaded
        || url == NULL || url[0] == '\0') return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
            "empty-document", "an empty document requires a fresh engine");
    static const char html[] =
        "<!doctype html><meta name=viewport "
        "content=\"width=device-width,initial-scale=1\">"
        "<title>Tilefinch</title>"
        "<style>html,body{margin:0;background:#100e0d}</style>";
    BrowserEngineHtmlLoad load = {
        .url = url, .html = html, .html_length = sizeof(html) - 1u,
        .record_history = record_history, .static_local = true
    };
    NavigationSession *navigation = &engine->navigation;
    bool scripts = navigation->document_scripts_enabled;
    bool resources = navigation->external_resources_enabled;
    bool fonts = navigation->web_fonts_enabled;
    /* The native surface owns all visible content. Its empty backing graph
       needs no curl pool/worker/CA setup. Restore all three policies even on
       refusal, so this optimization never disables the first real page. */
    navigation->document_scripts_enabled = false;
    navigation->external_resources_enabled = false;
    navigation->web_fonts_enabled = false;
    bool loaded = browser_engine_run_load(
        engine, browser_engine_do_commit, &load, TILEFINCH_SUBSYSTEM_PARSER,
        TILEFINCH_DIAGNOSTIC_PARSE_FAILED);
    navigation->document_scripts_enabled = scripts;
    navigation->external_resources_enabled = resources;
    navigation->web_fonts_enabled = fonts;
    return loaded;
}

bool browser_engine_commit_history_html(BrowserEngine *engine, const char *url,
                                        const char *html, size_t html_length)
{
    if (engine == NULL || browser_engine_navigation_pending(engine)
        || engine->navigation_work.history_move) return false;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    if (entry == NULL
        || !browser_engine_history_url_matches_document(entry->url, url)
        || browser_engine_history_url_matches_document(
            entry->url, navigation_active_document_url(&engine->navigation)))
        return browser_engine_commit_html(engine, url, html, html_length, false);
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    work->history_move = true;
    browser_engine_history_view_from_entry(&work->restore, entry);
    bool loaded = browser_engine_commit_html(engine, url, html, html_length, false);
    if (loaded) (void) browser_engine_navigation_restore_view(engine, work);
    work->history_move = false;
    work->restore.control_count = 0;
    return loaded;
}

typedef struct {
    const char *url;
    bool record_history;
    bool used_site_adapter;
    size_t adapter_source_bytes;
    size_t adapter_result_count;
    char adapter_name[32];
} BrowserEngineUrlLoad;

static bool browser_engine_do_load_url(BrowserEngine *engine,
                                       uint64_t generation, void *opaque)
{
    BrowserEngineUrlLoad *load = opaque;
    browser_engine_youtube_on_navigation_begin(engine, load->url, false);
#ifndef __PSP__
    if (site_adapter_handles_navigation("GET", load->url)) {
        SiteAdapterDocument document = {0};
        char error[256] = {0};
        SiteAdapterPreferences preferences = {
            .youtube_compact_results = engine->youtube.compact_results,
            .navigation_initiator = engine->navigation.page.loaded
                ? engine->navigation.page.document_url : NULL,
            .navigation_user_activated = engine->navigation.pending_navigation_user_activated
        };
        uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
        bool acquired = site_adapter_load_sync(
                &engine->budget, &engine->session, "GET", load->url,
                &preferences,
                engine->config.maximum_document_bytes,
                engine->config.navigation_timeout_ms,
                &document, error, sizeof(error));
        uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
        if (finished_ns >= started_ns) {
            engine->navigation.performance.network_us +=
                (finished_ns - started_ns) / UINT64_C(1000);
        }
        if (!acquired) {
            snprintf(engine->navigation.last_error,
                     sizeof(engine->navigation.last_error), "%s",
                     error[0] == '\0'
                         ? "site adapter page failed" : error);
            return false;
        }
        engine->navigation.last_http_status = document.status_code;
        snprintf(engine->navigation.last_server,
                 sizeof(engine->navigation.last_server), "%s",
                 document.server);
        snprintf(engine->navigation.last_cf_mitigated,
                 sizeof(engine->navigation.last_cf_mitigated), "%s",
                 document.cf_mitigated);
        load->used_site_adapter = true;
        load->adapter_source_bytes = document.source_bytes;
        load->adapter_result_count = document.result_count;
        snprintf(load->adapter_name, sizeof(load->adapter_name), "%s",
                 document.adapter);
        bool committed = navigation_commit_static_html(
            &engine->navigation, generation,
            browser_engine_youtube_commit_url(load->url),
            document.html, document.html_length,
            engine->config.device.navigation_viewport_width,
            engine->fonts_ready ? &engine->fonts : NULL, NULL,
            load->record_history);
        browser_engine_youtube_on_commit(engine, committed);
        site_adapter_document_destroy(&document);
        return committed;
    }
#endif
    NavigationLoad *navigation = navigation_load_begin_request(
        &engine->navigation, generation, load->url, "GET", NULL, 0, NULL,
        engine->config.maximum_document_bytes,
        engine->config.navigation_timeout_ms,
        engine->config.device.navigation_viewport_width,
        engine->fonts_ready ? &engine->fonts : NULL, NULL, load->record_history);
    if (navigation == NULL) return false;
    while (navigation_load_status(navigation) == NAVIGATION_LOAD_PENDING)
        (void) navigation_load_pump(navigation, NULL);
    const char *download = navigation_load_download_filename(navigation);
    if (download != NULL) {
        snprintf(engine->navigation_work.download_filename, sizeof(engine->navigation_work.download_filename), "%s", download);
        snprintf(engine->navigation_work.url, sizeof(engine->navigation_work.url), "%s", load->url);
    }
    const char *redirect = navigation_load_adapter_redirect(navigation);
    char destination[NAVIGATION_URL_LIMIT] = {0};
    if (redirect != NULL) snprintf(destination, sizeof(destination), "%s", redirect);
    bool okay = redirect == NULL
        && navigation_load_status(navigation) == NAVIGATION_LOAD_READY_TO_FINISH
        && navigation_load_finish(navigation, NULL);
    navigation_load_destroy(navigation);
#ifndef __PSP__
    if (destination[0] != '\0') {
        const char *previous = load->url;
        load->url = destination;
        okay = browser_engine_do_load_url(engine, generation, load);
        load->url = previous;
    }
#endif
    return okay;
}

bool browser_engine_load_url(BrowserEngine *engine, const char *url,
                             bool record_history)
{
    if (engine == NULL) return false;
    return browser_engine_load_url_with_limits(
        engine, url, engine->config.maximum_document_bytes,
        engine->config.navigation_timeout_ms, record_history);
}

bool browser_engine_load_url_with_limits(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history)
{
    if (url == NULL) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_NETWORK, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "url-load", "URL load is invalid");
    if (engine != NULL) engine->navigation_work.download_filename[0] = '\0';
    if (maximum_bytes == 0 || timeout_ms <= 0) return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_NETWORK, TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
        "url-load", "URL load limits are invalid");
    BrowserEngineUrlLoad load = {
        .url = url,
        .record_history = record_history
    };
    /* A synchronous URL load is the embedder's (typed, reloaded, a lab
       command): the refresh loop guard starts over. */
    if (engine != NULL) navigation_note_user_input(&engine->navigation);
    if (engine != NULL && !record_history && engine->navigation.page.loaded
        && !engine->navigation_work.history_move)
        browser_session_site_adapter_document_cache_clear(&engine->session);
    if (record_history && engine != NULL && engine->navigation.page.loaded) {
        browser_engine_store_current_focus(engine);
        browser_engine_store_current_controls(engine);
    }
    size_t saved_maximum = engine->config.maximum_document_bytes;
    long saved_timeout = engine->config.navigation_timeout_ms;
    engine->config.maximum_document_bytes = maximum_bytes;
    engine->config.navigation_timeout_ms = timeout_ms;
    site_identity_clear_google_fallback();
    bool loaded = browser_engine_run_load(
        engine, browser_engine_do_load_url, &load, TILEFINCH_SUBSYSTEM_NETWORK,
        TILEFINCH_DIAGNOSTIC_NETWORK_FAILED);
    /* The synchronous form of browser_engine_google_fallback(). The opt-in
       is already cleared, so the retry is the bounded local page. */
    char google_fallback[NAVIGATION_URL_LIMIT];
    if (!loaded
        && site_identity_take_google_fallback(
               google_fallback, sizeof(google_fallback))
        && site_adapter_handles_navigation("GET", google_fallback)) {
        engine->config.maximum_document_bytes = saved_maximum;
        engine->config.navigation_timeout_ms = saved_timeout;
        return browser_engine_load_url_with_limits(
            engine, google_fallback, maximum_bytes, timeout_ms,
            record_history);
    }
    if (loaded && load.used_site_adapter) {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_INFO,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_LIFECYCLE,
            load.adapter_name, "bounded site-adapter page committed",
            load.adapter_source_bytes, load.adapter_result_count);
    }
    /* Scripted form default actions (including small edge verification
       forms) are navigations, not telemetry. Follow a bounded chain after
       each committed document while the source DOM is still alive. */
    for (size_t hop = 0; loaded && hop < 4; hop++) {
        ControllerAction action;
        if (!browser_engine_collect_script_form_submission(engine, &action)) {
            break;
        }
        if (!browser_engine_execute_action(
                   engine, &action, maximum_bytes, timeout_ms)) {
            loaded = false;
            break;
        }
    }
    /* Match cooperative navigation: an explicit fragment owns the initial
       viewport; otherwise a fresh history entry honors autofocus. History
       traversal uses its separate saved-focus/scroll restoration path. */
    if (loaded)
        browser_engine_apply_initial_navigation_view(engine, record_history);
    engine->config.maximum_document_bytes = saved_maximum;
    engine->config.navigation_timeout_ms = saved_timeout;
    return loaded;
}

bool browser_engine_history_move(BrowserEngine *engine, bool forward)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready) return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "history-move",
            "browser engine is not active");
    browser_engine_cancel_idle_work(engine);
    BrowserHistoryRestore restore;
    if (!browser_engine_prepare_history(engine, forward, &restore)) {
        return set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
            "history-move", "no history entry is available");
    }
    size_t previous_index = restore.previous_index;
    const char *previous_url = restore.previous_url;
    const char *url = restore.url;
    if (navigation_url_is_same_document(
            &engine->navigation, previous_url)) {
        BrowserHistoryRestoreStatus result =
            browser_engine_restore_history_document(engine, forward, &restore);
        if (result == HISTORY_RESTORE_SETTLE_FAILED) {
            return set_error_code(engine, TILEFINCH_SUBSYSTEM_RENDER,
                TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "history-event-settle",
                "same-document history event layout could not be settled");
        }
        if (result != HISTORY_RESTORE_OK) {
            return set_error_code(engine, TILEFINCH_SUBSYSTEM_ENGINE,
                TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "history-fragment",
                "same-document history restoration failed");
        }
        clear_error(engine);
        return true;
    }
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    work->history_move = true;
    work->restore = restore.view;
    bool loaded = browser_engine_load_url(engine, url, false);
    /* Like the cooperative path, restore while the move is still marked. */
    bool restored = loaded && browser_engine_navigation_restore_view(engine, work);
    work->history_move = false;
    work->restore.control_count = 0u;
    if (!loaded) {
        if (engine->navigation.history_index != previous_index) {
            browser_engine_rollback_same_document_cursor(engine, forward);
        }
        return false;
    }
    return restored;
}

bool browser_engine_history_step(
    BrowserEngine *engine, bool forward, BrowserHistoryEntryText *text,
    int *scroll_y)
{
    if (engine == NULL || !engine->navigation_ready) return false;
    const NavigationSession *navigation = &engine->navigation;
    size_t index = navigation->history_index;
    if (navigation->history_count == 0u || index >= navigation->history_count
        || (forward ? index + 1u >= navigation->history_count : index == 0u))
        return false;
    const NavigationEntry *target =
        &navigation->history[forward ? index + 1u : index - 1u];
    const char *url = target->url == NULL ? "" : target->url;
    const char *title = target->title == NULL ? "" : target->title;
    size_t url_length = strlen(url);
    size_t title_length = strlen(title);
    if (text != NULL && (url_length >= sizeof(text->url)
                         || title_length >= sizeof(text->title)))
        return false;
    /* An idle step must never run against the moved cursor before the
       frontend has shown the page it names. */
    browser_engine_cancel_idle_work(engine);
    const NavigationEntry *moved = NULL;
    if (!(forward ? navigation_forward(&engine->navigation, &moved)
                  : navigation_back(&engine->navigation, &moved)))
        return false;
    if (text != NULL) {
        memcpy(text->url, url, url_length + 1u);
        memcpy(text->title, title, title_length + 1u);
    }
    if (scroll_y != NULL) *scroll_y = moved == NULL ? 0 : moved->scroll_y;
    return true;
}

bool browser_engine_history_step_rollback(BrowserEngine *engine, bool forward)
{
    if (engine == NULL || !engine->navigation_ready) return false;
    browser_engine_cancel_idle_work(engine);
    const NavigationEntry *ignored = NULL;
    return forward ? navigation_back(&engine->navigation, &ignored)
                   : navigation_forward(&engine->navigation, &ignored);
}

bool browser_engine_history_restore_scroll(
    BrowserEngine *engine, int scroll_y)
{
    if (engine == NULL || !engine->navigation_ready) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return navigation_set_scroll(&engine->navigation, scroll_y);
}

bool browser_engine_replace_history(
    BrowserEngine *engine, const NavigationHistoryRecord *records,
    size_t count, size_t current_index)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || browser_engine_navigation_pending(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "history-replace",
            "history cannot be replaced in the current engine state");
    }
    if (!navigation_replace_history(
            &engine->navigation, records, count, current_index)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED, "history-replace",
            "bounded tab history could not be installed");
    }
    clear_error(engine);
    return true;
}

bool browser_engine_restore_view(
    BrowserEngine *engine, int scroll_y, int focus_kind,
    size_t focus_index)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    lxb_dom_node_t *focus = browser_engine_semantic_focus(
        engine, focus_kind, focus_index, NULL);
    if (focus != NULL)
        (void) controller_focus_node(&engine->controller, focus);
    if (!navigation_set_scroll(&engine->navigation, scroll_y)) return false;
    return browser_engine_apply_focus_paint(engine);
}

static bool browser_engine_input_ready(const BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready && engine->navigation.page.loaded
        && engine->controller_ready;
}

/* The frontend half of the rule that nothing answers from past a bounded
   focus layout's region (NavigationPage bounded_focus_region_end; script's
   half is js_rt_bridge_layout). Input that could read the page beyond the
   region (scrolling, pointer hit tests outside a proof, activation, text
   editing, view and anchor restores, find) completes the layout first;
   only focus moves the controller proves confined to the region skip it.
   Author mutations still pending publish first (the completion waits for
   them). A refused completion refuses the input, with an error, instead of
   answering it from the truncated layout. */
static bool browser_engine_settle_bounded_layout(BrowserEngine *engine)
{
    if (navigation_bounded_focus_region_end(&engine->navigation) == 0)
        return true;
    engine->navigation.performance.bounded_focus_input_settles++;
    (void) browser_engine_complete_layout(engine);
    /* The pending mutations' relayout can retire a page that no longer
       fits: the input then has no page to act on. */
    if (!engine->navigation.page.loaded)
        return browser_engine_report_retired_update(engine, "input-settle");
    if (navigation_bounded_focus_region_end(&engine->navigation) == 0)
        return true;
    return set_error_code(
        engine, TILEFINCH_SUBSYSTEM_ENGINE,
        TILEFINCH_DIAGNOSTIC_LAYOUT_FAILED, "bounded-layout-settle",
        "the page's layout could not be completed for this input");
}

static bool browser_engine_settle_for_focus_move(
    BrowserEngine *engine, bool relative, bool forward,
    ControllerFocusDirection direction)
{
    int region_end = navigation_bounded_focus_region_end(&engine->navigation);
    return region_end == 0
        || controller_focus_move_within(&engine->controller, relative,
                                        forward, direction, region_end)
        || browser_engine_settle_bounded_layout(engine);
}

static bool browser_engine_finish_input(BrowserEngine *engine,
                                        bool succeeded)
{
    if (engine != NULL && !engine->navigation.page.loaded) {
        /* Event dispatch may have committed a destructive DOM mutation before
           its replacement layout was refused. The controller can still
           report a delivered/default action from the now-retired realm, so
           page lifetime—not the action outcome—is the authoritative shell
           ownership check. */
        return browser_engine_report_retired_update(engine, "input-publication");
    }
    if (!succeeded) {
        return false;
    }
    /* Explicit focus/scroll/pointer actions retain immediate feedback, even
       during the provider's short automatic first-paint coalescing window. */
    engine->initial_tile_hold = false;
    if (engine->render_ready
        && engine->render_relayout_generation
               != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) return false;
    if (!browser_engine_apply_focus_paint(engine)) return false;
    if (browser_engine_youtube_result_focused(engine)) browser_engine_youtube_browsing(engine);
    browser_engine_store_current_focus(engine);
    clear_error(engine);
    return true;
}

bool browser_engine_focus_move(BrowserEngine *engine, bool forward)
{
    if (!browser_engine_input_ready(engine)) return false;
    if (forward) browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_for_focus_move(
            engine, true, forward, CONTROLLER_FOCUS_DOWN)) return false;
    return browser_engine_finish_input(
        engine, forward ? controller_focus_next(&engine->controller)
                        : controller_focus_previous(&engine->controller));
}

bool browser_engine_focus_direction(BrowserEngine *engine,
                                    ControllerFocusDirection direction)
{
    if (!browser_engine_input_ready(engine)) return false;
    if (direction == CONTROLLER_FOCUS_DOWN) browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_for_focus_move(
            engine, false, false, direction)) return false;
    return browser_engine_finish_input(
        engine, controller_focus_direction(&engine->controller, direction));
}

bool browser_engine_focus_at(BrowserEngine *engine, int x, int y)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_focus_at(&engine->controller, x, y));
}

bool browser_engine_pointer_event(BrowserEngine *engine,
                                  ControllerPointerPhase phase,
                                  int x, int y, bool *activate,
                                  bool *page_changed)
{
    if (activate != NULL) *activate = false;
    if (page_changed != NULL) *page_changed = false;
    if (!browser_engine_input_ready(engine)) return false;
    if (phase == CONTROLLER_POINTER_MOVE && y > engine->controller.pointer_y)
        browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    size_t relayouts = engine->navigation.incremental_relayouts;
    const NavigationEntry *before_entry =
        navigation_current(&engine->navigation);
    int scroll_y = before_entry == NULL ? 0 : before_entry->scroll_y;
    if (!browser_engine_finish_input(
            engine, controller_pointer_event(
                        &engine->controller, phase, x, y, activate))) {
        return false;
    }
    const NavigationEntry *after_entry =
        navigation_current(&engine->navigation);
    if (page_changed != NULL) {
        *page_changed =
            relayouts != engine->navigation.incremental_relayouts
            || (after_entry != NULL && after_entry->scroll_y != scroll_y);
    }
    return true;
}

bool browser_engine_pointer_commit_click(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_commit_pointer_click(&engine->controller));
}

bool browser_engine_focus_node(BrowserEngine *engine, lxb_dom_node_t *node)
{
    if (!browser_engine_input_ready(engine) || node == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_focus_node(&engine->controller, node));
}

bool browser_engine_scroll_by(BrowserEngine *engine, int delta_y)
{
    if (!browser_engine_input_ready(engine)) return false;
    if (delta_y > 0) browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_scroll_by(
                    &engine->controller, delta_y,
                    engine->config.device.framebuffer_height));
}

bool browser_engine_scroll_settle(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_scroll_settle(&engine->controller));
}

LayoutCursor browser_engine_pointer_cursor(const BrowserEngine *engine)
{
    return engine == NULL ? LAYOUT_CURSOR_AUTO
        : controller_pointer_cursor(&engine->controller);
}

LayoutScrollbarWidth browser_engine_root_scrollbar_width(
    const BrowserEngine *engine)
{
    return engine == NULL ? LAYOUT_SCROLLBAR_AUTO
        : controller_root_scrollbar_width(&engine->controller);
}

bool browser_engine_scroll_step(BrowserEngine *engine, int direction,
                                unsigned held_frames)
{
    if (!browser_engine_input_ready(engine)) return false;
    if (direction > 0) browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_scroll_step(
                    &engine->controller, direction, held_frames,
                    engine->config.device.framebuffer_height));
}

bool browser_engine_scroll_page(BrowserEngine *engine, int direction)
{
    if (!browser_engine_input_ready(engine)) return false;
    if (direction > 0) browser_engine_youtube_browsing(engine);
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, controller_scroll_page(
                    &engine->controller, direction,
                    engine->config.device.framebuffer_height));
}

/* A layout completion, or a provisional relayout, is building beside the
   committed page: page presses may scroll and raster it at checkpoints. */
bool browser_engine_layout_completion_running(const BrowserEngine *engine)
{
    return engine != NULL && (engine->layout_completion_running
                              || engine->navigation.committed_layout_servable);
}

bool browser_engine_completion_scroll_page(BrowserEngine *engine,
                                           int direction)
{
    if (!browser_engine_layout_completion_running(engine)
        || !engine->render_ready || !engine->navigation.page.loaded
        || engine->render.preparation_polling || engine->render.frame_composing)
        return false;
    if (!controller_scroll_page(&engine->controller, direction,
                                engine->config.device.framebuffer_height))
        return false;
    /* Publish the move at once; the checkpoints fill the rest. */
    engine->completion_frame_pending = true;
    engine->completion_scrolled = true;
    (void) browser_engine_render_placeholder_frame(engine, NULL);
    return true;
}

bool browser_engine_completion_raster_step(BrowserEngine *engine,
                                           bool show_progress)
{
    if (!browser_engine_layout_completion_running(engine)
        || !engine->completion_frame_pending || !engine->render_ready
        || !engine->navigation.page.loaded || engine->render.preparation_polling
        || engine->render.frame_composing) return false;
    size_t before = engine->render.frame_job_units;
    /* The 8 ms budget bounds the slice; the unit cap only stops a run of
       cheap tiles from outlasting it. */
    BrowserRenderJobStatus status =
        browser_engine_render_frame_bounded(engine, 8000, 16);
    if (status == BROWSER_RENDER_JOB_COMPLETE) {
        engine->completion_frame_pending = false;
        return true;
    }
    if (status != BROWSER_RENDER_JOB_PENDING) {
        engine->completion_frame_pending = false;
        return false;
    }
    return show_progress && engine->render.frame_job_units > before
        && browser_engine_render_placeholder_frame(engine, NULL);
}

bool browser_engine_completion_frame_pending(const BrowserEngine *engine)
{
    return browser_engine_layout_completion_running(engine)
        && engine->completion_frame_pending;
}

bool browser_engine_scroll_to_edge(BrowserEngine *engine, bool bottom)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    return browser_engine_finish_input(
        engine, bottom
            ? controller_scroll_to_bottom(
                  &engine->controller,
                  engine->config.device.framebuffer_height)
            : controller_scroll_to_top(
                  &engine->controller,
                  engine->config.device.framebuffer_height));
}

#include "browser_engine/find.inc"

bool browser_engine_text_value(const BrowserEngine *engine, char *output,
                               size_t capacity, size_t *length)
{
    return browser_engine_input_ready(engine)
        && controller_text_value(
               &engine->controller, output, capacity, length);
}

bool browser_engine_text_input_info(
    const BrowserEngine *engine, ControllerTextInputInfo *info)
{
    return browser_engine_input_ready(engine)
        && controller_text_input_info(&engine->controller, info);
}

bool browser_engine_replace_text(BrowserEngine *engine, const char *utf8,
                                 size_t length)
{
    if (!browser_engine_input_ready(engine) || utf8 == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    /* Editing a field takes control of the page: a pending refresh would
       discard what was typed. */
    (void) navigation_cancel_refresh(&engine->navigation);
    return browser_engine_finish_input(
        engine, controller_replace_text(&engine->controller, utf8, length));
}

bool browser_engine_insert_text(BrowserEngine *engine, const char *utf8,
                                size_t length)
{
    if (!browser_engine_input_ready(engine) || utf8 == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    (void) navigation_cancel_refresh(&engine->navigation);
    return browser_engine_finish_input(
        engine, controller_insert_text(&engine->controller, utf8, length));
}

bool browser_engine_backspace(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    (void) navigation_cancel_refresh(&engine->navigation);
    return browser_engine_finish_input(
        engine, controller_backspace(&engine->controller));
}

bool browser_engine_activate(BrowserEngine *engine,
                             ControllerAction *action)
{
    if (!browser_engine_input_ready(engine) || action == NULL) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    size_t activations_before = engine->controller.activations;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t activation_begin = tilefinch_platform_monotonic_time_us();
#endif
    bool activated = controller_activate(&engine->controller, action);
    /* A layout generation can retire the last focus region immediately
       before activation. If failure occurred before the controller counted
       an activation, no page event or default action ran. Resolve the exact
       authored autofocus target and retry once so a single user press is not
       split into repair-then-activate. Script/handler failures occur after
       the activation count changes and are never replayed. */
    if (!activated
        && engine->controller.activations == activations_before
        && browser_engine_apply_autofocus(engine)) {
        engine->autofocus_pending = false;
        activated = controller_activate(&engine->controller, action);
    }
    /* A delivered user action supersedes the deferred initial autofocus.
       Otherwise the next idle turn can return to the search field while a
       provider edit is still being confirmed, moving both focus and scroll. */
    if (activated) engine->autofocus_pending = false;
    bool youtube_finished = false;
    if (browser_engine_youtube_activate(engine, action, activated,
                                        &youtube_finished))
        return youtube_finished;
    if (activated && action->type == CONTROLLER_ACTION_NAVIGATE
        && navigation_url_is_same_document(
               &engine->navigation, action->url)) {
        /* A fragment activation is a local HTML default action. Resolve it
           before returning to chrome so the PSP never associates Wi-Fi,
           leaves Reader, or paints a network loading surface for work that
           must not issue request bytes. */
        activated = browser_engine_commit_same_document_action(
            engine, action->url);
        if (activated) {
            action->type = CONTROLLER_ACTION_CONTROL;
            action->url[0] = '\0';
        }
        return activated;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t activation_dispatched = tilefinch_platform_monotonic_time_us();
#endif
    bool finished = browser_engine_finish_input(engine, activated);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    char activation_phases[160];
    snprintf(activation_phases, sizeof(activation_phases),
        "tilefinch-activation-phases: dispatch=%lluus finish=%lluus",
        (unsigned long long) (activation_dispatched - activation_begin),
        (unsigned long long) (tilefinch_platform_monotonic_time_us() - activation_dispatched));
    tilefinch_platform_log_message(activation_phases);
#endif
    return finished;
}

static bool browser_engine_adopt_script_focus_of(
    BrowserEngine *engine, bool text_only)
{
    if (!browser_engine_input_ready(engine)
        || !browser_engine_settle_bounded_layout(engine)) return false;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    static const lxb_char_t focus_attribute[] = "data-tilefinch-focus";
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        lxb_dom_node_t *node = control->node;
        if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !lxb_dom_element_has_attribute(
                   lxb_dom_interface_element(node), focus_attribute,
                   sizeof(focus_attribute) - 1)) continue;
        if (text_only && control->type != CONTROL_INPUT
            && control->type != CONTROL_TEXTAREA
            && control->type != CONTROL_EDITABLE) return false;
        return controller_focus_node(&engine->controller, node);
    }
    return false;
}

bool browser_engine_adopt_script_focus(BrowserEngine *engine)
{
    return browser_engine_adopt_script_focus_of(engine, false);
}

bool browser_engine_adopt_script_text_focus(BrowserEngine *engine)
{
    return browser_engine_adopt_script_focus_of(engine, true);
}

bool browser_engine_restore_autofocus(BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)) return false;
    browser_engine_cancel_idle_work(engine);
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    engine->autofocus_pending = false;
    return browser_engine_finish_input(
        engine, browser_engine_apply_autofocus(engine));
}

bool browser_engine_focused_provider_media_url(
    const BrowserEngine *engine, char *url, size_t capacity)
{
    if (url != NULL && capacity != 0) url[0] = '\0';
    if (engine == NULL || url == NULL || capacity == 0
        || !browser_engine_input_ready(engine)) {
        return false;
    }
    const LinkRegion *link = NULL;
    if (!controller_focused_link_region(&engine->controller, &link)) {
        return false;
    }
    lxb_dom_node_t *node = link->node;
    static const lxb_char_t provider_media_attribute[] =
        "data-tilefinch-provider-media";
    if (browser_engine_attribute_owner(
            node, provider_media_attribute,
            sizeof(provider_media_attribute) - 1u) == NULL) {
        return false;
    }
    const char *current_url =
        navigation_active_document_url(&engine->navigation);
    if (current_url != NULL
        && fetch_resolve_url(current_url, link->url, url, capacity)) {
        return true;
    }
    if (link->url_length >= capacity) return false;
    memcpy(url, link->url, link->url_length);
    url[link->url_length] = '\0';
    return true;
}

static lxb_dom_node_t *browser_engine_provider_media_thumbnail(
    const BrowserEngine *engine)
{
    if (engine == NULL || !browser_engine_input_ready(engine)) return NULL;
    const LinkRegion *link = NULL;
    if (!controller_focused_link_region(&engine->controller, &link)) {
        return NULL;
    }
    static const lxb_char_t provider_media_attribute[] =
        "data-tilefinch-provider-media";
    lxb_dom_node_t *root = browser_engine_attribute_owner(
        link->node, provider_media_attribute,
        sizeof(provider_media_attribute) - 1u);
    if (root == NULL) return NULL;

    /* Provider cards are deliberately shallow, but keep the search generic
       and bounded so malformed markup cannot turn a focus sample into a
       document traversal. */
    lxb_dom_node_t *node = root->first_child;
    for (unsigned inspected = 0; node != NULL && inspected < 64u;
         inspected++) {
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL && name_length == 3u
            && strncasecmp(name, "img", 3u) == 0) {
            return node;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return NULL;
}

bool browser_engine_prepare_focused_provider_media_thumbnail(
    BrowserEngine *engine)
{
    lxb_dom_node_t *thumbnail =
        browser_engine_provider_media_thumbnail(engine);
    if (thumbnail == NULL) return true;
    NavigationPage *page = &engine->navigation.page;
    const ImageResource *image = images_find_node(&page->images, thumbnail);
    if (image != NULL && image->pixels != NULL) {
        return true;
    }
    if (navigation_prioritize_deferred_image(
            &engine->navigation, thumbnail)) return false;
    /* The target was omitted or exhausted its bounded retry. Do not block
       video resolution forever for pixels the image pipeline has already
       classified as terminal. */
    return true;
}

bool browser_engine_consume_media_request(
    BrowserEngine *engine, ScriptMediaRequest *request)
{
    if (!browser_engine_input_ready(engine) || request == NULL
        || engine->navigation.page.runtime == NULL
        || !script_runtime_consume_media_request(
               engine->navigation.page.runtime, request)) return false;
    /* Native page media owns the scarce PSP audio hardware. Keep decoded
       game sounds resident but suspend their channel until the page
       explicitly resumes its AudioContext after returning. */
    script_runtime_suspend_game_audio(engine->navigation.page.runtime);
    bool allowed = request->source[0] != '\0'
        && tilefinch_csp_allows_request(
            &engine->navigation.page.document.content_security_policy,
            TILEFINCH_DESTINATION_MEDIA, request->source);
    if (!allowed) {
        (void) script_runtime_update_media_state(
            engine->navigation.page.runtime, request->node_handle,
            SCRIPT_MEDIA_STATE_ERROR, 0, 0);
        request->source[0] = '\0';
    }
    return true;
}

bool browser_engine_update_media_state(
    BrowserEngine *engine, int64_t node_handle, ScriptMediaState state,
    double current_time, double duration)
{
    return browser_engine_input_ready(engine) && node_handle != 0
        && engine->navigation.page.runtime != NULL
        && script_runtime_update_media_state(
               engine->navigation.page.runtime, node_handle, state,
               current_time, duration);
}

bool browser_engine_advance_runtime(BrowserEngine *engine,
                                    unsigned elapsed_ms,
                                    size_t maximum_callbacks,
                                    bool *visible_layout_changed)
{
    if (visible_layout_changed != NULL) *visible_layout_changed = false;
    if (!browser_engine_input_ready(engine)
        || maximum_callbacks == 0) return false;
    size_t before = engine->navigation.incremental_relayouts;
    bool advanced = navigation_advance_runtime(
        &engine->navigation, elapsed_ms, maximum_callbacks);
    if (advanced && !engine->navigation.defer_script_navigation) {
        /* Synchronous embedders must service submissions produced by timers
           and promise jobs too, not just forms queued during initial load.
           Follow at most one here; another committed form waits for the next
           owner turn. Deferred frontends retain ownership of their action. */
        ControllerAction action;
        if (browser_engine_collect_script_form_submission(engine, &action))
            advanced = browser_engine_execute_action(
                engine, &action, engine->config.maximum_document_bytes,
                engine->config.navigation_timeout_ms);
    }
    if (!engine->navigation.page.loaded) {
        /* Timer/microtask dispatch shares the same mutation→relayout boundary
           as direct input. A failed replacement retires the page; never leave
           the old render/controller shell externally observable. */
        return browser_engine_report_retired_update(engine, "runtime-publication");
    }
    bool changed = engine->navigation.incremental_relayouts != before;
    if (changed && engine->render_ready
        && !browser_engine_apply_layout_damage(engine)) return false;
    bool visible_changed =
        changed && engine->navigation.relayout_damage_valid;
    if (visible_layout_changed != NULL) {
        *visible_layout_changed = visible_changed;
    }
    if (changed && !visible_changed) {
        engine->unchanged_runtime_frames_suppressed++;
    }
    if (advanced) clear_error(engine);
    return advanced;
}

bool browser_engine_page_gamepad_available(const BrowserEngine *engine)
{
    return browser_engine_input_ready(engine)
        && engine->navigation.page.loaded
        && engine->navigation.page.runtime != NULL;
}

bool browser_engine_set_gamepad_state(
    BrowserEngine *engine, const TilefinchGamepadState *state)
{
    /* A pending transactional navigation makes ordinary page input not-ready,
       but its incumbent runtime is still live and may be restored on failure.
       Deliver the disconnect to that realm instead of leaving a connected
       Gamepad object behind after browser input authority has returned. */
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && navigation_set_gamepad_state(&engine->navigation, state);
}

bool browser_engine_set_page_visibility(BrowserEngine *engine, bool visible)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded)
        return false;
    ScriptRuntime *runtime = engine->navigation.page.runtime;
    return runtime == NULL
        || script_runtime_set_page_visibility(runtime, visible);
}

bool browser_engine_page_fullscreen_active(BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready && engine->navigation.page.loaded
        && script_runtime_page_fullscreen_active(
               engine->navigation.page.runtime);
}

bool browser_engine_take_page_controls_notice(BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded
        || script_runtime_take_page_controls_notice(
               engine->navigation.page.runtime);
}

bool browser_engine_exit_page_fullscreen(BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready && engine->navigation.page.loaded
        && script_runtime_exit_page_fullscreen(
               engine->navigation.page.runtime);
}

void browser_engine_suspend_page_presentations(BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded)
        return;
    ScriptRuntime *runtime = engine->navigation.page.runtime;
    script_runtime_suspend_game_audio(runtime);
    (void) script_runtime_exit_page_fullscreen(runtime);
}

bool browser_engine_execute_action(BrowserEngine *engine,
                                   const ControllerAction *action,
                                   size_t maximum_bytes,
                                   long timeout_ms)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->controller_ready || action == NULL
        || maximum_bytes == 0 || timeout_ms <= 0) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "controller-action",
            "controller action is invalid");
    }
    browser_engine_cancel_idle_work(engine);
    /* Activation has already dispatched ordinary control behavior. Only
       navigation and form submission require a second execution phase. */
    if (action->type == CONTROLLER_ACTION_NONE
        || action->type == CONTROLLER_ACTION_CONTROL
        || action->type == CONTROLLER_ACTION_MEDIA) {
        clear_error(engine);
        return true;
    }
    const char *method = action->type == CONTROLLER_ACTION_FORM_SUBMIT
        ? action->method : "GET";
    if (action->type == CONTROLLER_ACTION_NAVIGATE
        && strcasecmp(method, "GET") == 0
        && navigation_url_is_same_document(
               &engine->navigation, action->url)) {
        browser_engine_store_current_focus(engine);
        browser_engine_store_current_controls(engine);
        return browser_engine_commit_same_document_action(
            engine, action->url);
    }
    browser_engine_store_current_focus(engine);
    browser_engine_store_current_controls(engine);
    if (strcasecmp(method, "GET") == 0
        && site_adapter_handles_navigation(method, action->url)) {
        const char *current_url =
            navigation_active_document_url(&engine->navigation);
        if (current_url != NULL) {
            snprintf(engine->navigation.pending_navigation_referer,
                     sizeof(engine->navigation.pending_navigation_referer),
                     "%s", current_url);
        }
        engine->navigation.pending_navigation_user_activated =
            action->navigation_source == CONTROLLER_NAVIGATION_USER;
        return browser_engine_load_url_with_limits(
            engine, action->url, maximum_bytes, timeout_ms, true);
    }
    size_t loads_before = engine->navigation.loads_committed;
    if (!controller_execute_action(
            &engine->controller, action, maximum_bytes, timeout_ms)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_ENGINE,
            TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED, "controller-action",
            engine->navigation.last_error);
    }
    if (engine->navigation.loads_committed != loads_before
        && (!browser_engine_bind_current_full_document(engine)
            || !browser_engine_refresh_shell(engine))) return false;
    clear_error(engine);
    return true;
}

const RenderCanvasDeferral *browser_engine_canvas_deferral(
    const BrowserEngine *engine, const uint16_t *frame)
{
    if (engine == NULL || frame == NULL || frame != engine->render.frame)
        return NULL;
    return tile_cache_canvas_deferral(&engine->render);
}

bool browser_engine_canvas_materialize(BrowserEngine *engine)
{
    return engine == NULL || tile_cache_canvas_materialize(&engine->render);
}

bool browser_engine_render_frame(BrowserEngine *engine,
                                 const char *optional_ppm_path)
{
    if (engine != NULL && engine->render.preparation_polling) return false;
    if (engine == NULL || !engine->render_ready
        || !engine->navigation.page.loaded) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "frame-render",
            "render shell is not ready");
    }
    if (engine->render_relayout_generation
            != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) return false;
    if (!browser_engine_apply_focus_paint(engine)) return false;
    tile_cache_set_forced_dark(
        &engine->render,
        engine->forced_dark && !browser_engine_page_declares_dark(engine));
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    bool previous_scroll_valid = engine->render.last_frame_scroll_valid;
    int previous_scroll_y = viewport_device_to_css(
        &engine->navigation.viewport, engine->render.last_frame_scroll_y);
    {
        /* Defer only when nothing below paints into this frame. */
        int focus_x = 0, focus_y = 0, focus_width = 0, focus_height = 0;
        engine->render.canvas_defer_allowed =
            engine->find.query[0] == '\0'
            && !browser_engine_focus_indicator_rect(
                   engine, &focus_x, &focus_y, &focus_width, &focus_height);
    }
    RenderCanvasFrameResult canvas_frame =
        tile_cache_render_canvas_frame_fast(
            &engine->render, scroll_y,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height);
    bool frame_rendered = canvas_frame == RENDER_CANVAS_FRAME_COMPLETE;
    if (canvas_frame == RENDER_CANVAS_FRAME_FAILED
        || (!frame_rendered && !tile_cache_render_frame(
            &engine->render, scroll_y,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height, NULL))) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-render",
            "frame rendering failed");
    }
    browser_engine_paint_find_highlights(engine, scroll_y);
    int focus_x = 0, focus_y = 0, focus_width = 0, focus_height = 0;
    ControllerFocusOutline focus_outline = {0};
    if (browser_engine_focus_indicator_rect(
            engine, &focus_x, &focus_y, &focus_width, &focus_height)
        && controller_focused_outline_style(
            &engine->controller, &focus_outline)) {
        const ViewportContext *viewport = &engine->navigation.viewport;
        int left = viewport_css_to_device(viewport, focus_x);
        int top = viewport_css_to_device(
            viewport, focus_y - scroll_y);
        int right = viewport_css_to_device(
            viewport, focus_x + focus_width);
        int bottom = viewport_css_to_device(
            viewport, focus_y - scroll_y + focus_height);
        int outline_width = viewport_css_to_device(
            viewport, focus_outline.width);
        if (outline_width < 1) outline_width = 1;
        int outline_offset = viewport_css_to_device(
            viewport, focus_outline.offset);
        render_paint_authored_focus_outline(
            engine->render.frame, engine->render.frame_pixels,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height,
            left, top, right - left, bottom - top,
            outline_width, outline_offset,
            focus_outline.style,
            focus_outline.color, focus_outline.alpha);
    }
    if (optional_ppm_path != NULL)
        (void) tile_cache_canvas_materialize(&engine->render);
    if (optional_ppm_path != NULL
        && !render_write_frame_ppm(
            optional_ppm_path, engine->render.frame,
            engine->render.frame_pixels,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-write",
            "rendered frame could not be written");
    }
    bool scrolled = !previous_scroll_valid || scroll_y != previous_scroll_y;
    if (scrolled) {
        engine->paint_ahead_direction =
            previous_scroll_valid && scroll_y < previous_scroll_y ? -1 : 1;
    }
    /* A repaint at the same position (a toast, focus, chrome) follows an
       idle cancel on the frontend; paint ahead again unless it finished. */
    if (scrolled || !tile_cache_prefetch_satisfied(&engine->render)) {
        int prefetch_y = engine->paint_ahead_direction < 0
            ? scroll_y - 1
            : scroll_y + engine->navigation.viewport.css_height
                + TILEFINCH_TILE_SIZE - 1;
        if (prefetch_y >= 0
            && prefetch_y < engine->navigation.page.layout.height) {
            tile_cache_schedule_prefetch_row(
                &engine->render, prefetch_y,
                engine->config.device.framebuffer_width);
            /* Publication is foreground work. Leave speculative offscreen
               rasterization to the caller's idle pump after presenting. */
        }
    }
    clear_error(engine);
    size_t compile_rejections =
        engine->navigation.page.script_result.host_compile_rejections;
    if (compile_rejections > engine->observed_compile_rejections) {
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_WARNING,
            TILEFINCH_SUBSYSTEM_SCRIPT, TILEFINCH_DIAGNOSTIC_LIMIT_REACHED,
            "compile-admission",
            engine->navigation.page.script_result.error,
            compile_rejections,
            engine->navigation.page.script_result
                .host_compile_projected_rejections);
    }
    engine->observed_compile_rejections = compile_rejections;
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_DEBUG,
        TILEFINCH_SUBSYSTEM_RENDER, TILEFINCH_DIAGNOSTIC_OK,
        "frame-rendered", "", engine->render.frames_rendered,
        engine->render.max_frame_us);
    return true;
}

bool browser_engine_render_placeholder_frame(BrowserEngine *engine,
                                             size_t *placeholders)
{
    if (placeholders != NULL) *placeholders = 0;
    if (engine == NULL || !engine->render_ready
        || !engine->navigation.page.loaded) return false;
    if (engine->initial_tile_hold) {
        uint64_t now = tilefinch_platform_monotonic_time_us();
        const NavigationEntry *entry = navigation_current(&engine->navigation);
        if (entry != NULL && entry->scroll_y == 0
            && !engine->render.presented_scroll_valid
            && now >= engine->initial_tile_hold_started_us
            && now - engine->initial_tile_hold_started_us < UINT64_C(1000000))
            return false;
        engine->initial_tile_hold = false;
    }
    engine->render.placeholder_missing = true;
    bool rendered = browser_engine_render_frame(engine, NULL);
    engine->render.placeholder_missing = false;
    if (placeholders != NULL) *placeholders = engine->render.placeholder_tiles;
    return rendered;
}

BrowserRenderJobStatus browser_engine_render_frame_bounded(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units)
{
    return browser_engine_render_frame_bounded_cancelable(
        engine, budget_us, maximum_units, NULL);
}

BrowserRenderJobStatus browser_engine_render_frame_bounded_cancelable(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units,
    const TilefinchCancellation *cancellation)
{
    if (engine != NULL && engine->render.preparation_polling)
        return BROWSER_RENDER_JOB_PENDING;
    if (engine == NULL || !engine->render_ready
        || !engine->navigation.page.loaded) {
        (void) set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_INVALID_INPUT, "frame-job",
            "render shell is not ready");
        return BROWSER_RENDER_JOB_FAILED;
    }
    if (tilefinch_cancellation_requested(cancellation)) {
        browser_engine_cancel_render_job(engine);
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    if (engine->render_relayout_generation
            != engine->navigation.incremental_relayouts
        && !browser_engine_apply_layout_damage(engine)) {
        return BROWSER_RENDER_JOB_FAILED;
    }
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    if (tile_cache_canvas_frame_fast_eligible(
            &engine->render, scroll_y,
            engine->config.device.framebuffer_width,
            engine->config.device.framebuffer_height)) {
        bool rendered = browser_engine_render_frame(engine, NULL);
        if (tilefinch_cancellation_requested(cancellation)) {
            return BROWSER_RENDER_JOB_CANCELLED;
        }
        return rendered ? BROWSER_RENDER_JOB_COMPLETE
                        : BROWSER_RENDER_JOB_FAILED;
    }
    RenderFrameWorkResult prepared =
        tile_cache_prepare_frame_bounded_cancelable(
        &engine->render, scroll_y,
        engine->config.device.framebuffer_width,
        engine->config.device.framebuffer_height,
        budget_us, maximum_units, cancellation);
    if (prepared == RENDER_FRAME_WORK_CANCELLED) {
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    if (prepared == RENDER_FRAME_WORK_FAILED) {
        (void) set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED, "frame-job",
            "bounded tile preparation failed");
        return BROWSER_RENDER_JOB_FAILED;
    }
    if (prepared == RENDER_FRAME_WORK_PENDING) {
        return BROWSER_RENDER_JOB_PENDING;
    }
    bool rendered = browser_engine_render_frame(engine, NULL);
    if (tilefinch_cancellation_requested(cancellation)) {
        browser_engine_cancel_render_job(engine);
        return BROWSER_RENDER_JOB_CANCELLED;
    }
    return rendered ? BROWSER_RENDER_JOB_COMPLETE
                    : BROWSER_RENDER_JOB_FAILED;
}

void browser_engine_cancel_render_job(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready) return;
    tile_cache_cancel_frame_work(&engine->render);
}

bool browser_engine_render_frame_pending(const BrowserEngine *engine)
{
    return engine != NULL && tile_cache_frame_work_pending(&engine->render);
}

bool browser_engine_canvas_frame_pending(const BrowserEngine *engine)
{
    return engine != NULL
        && (engine->navigation.canvas_paint_damage_pending
            || tile_cache_canvas_frame_work_pending(&engine->render));
}

bool browser_engine_page_task_runnable(const BrowserEngine *engine)
{
    if (!browser_engine_input_ready(engine)
        || !engine->navigation.page.loaded) return false;
    const NavigationPage *page = &engine->navigation.page;
    if (script_runtime_task_runnable(page->runtime)) return true;
    for (size_t i = 0; i < page->frame_count; i++) {
        if (page->frames[i].loaded
            && script_runtime_task_runnable(page->frames[i].runtime))
            return true;
    }
    return false;
}

bool browser_engine_frame_clock_delay_ms(const BrowserEngine *engine,
                                         unsigned *delay_ms)
{
    return browser_engine_input_ready(engine)
        && engine->navigation.page.loaded
        && script_runtime_frame_clock_delay_ms(
            engine->navigation.page.runtime, delay_ms);
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
bool browser_engine_runnable_state(BrowserEngine *engine,
                                   ScriptRunnableState *state)
{
    if (state != NULL) *state = (ScriptRunnableState) {0};
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready && engine->navigation.page.loaded
        && script_runtime_runnable_state(
               engine->navigation.page.runtime, state);
}
#endif

#include "browser_engine/youtube.inc"

/* One idle turn runs these steps in order. A step either lets the turn
   continue or stops it with the turn's return value; the order is
   load-bearing (see browser_engine_idle_steps). The continuations a step
   leaves behind are combined into the turn's result at the end. */
typedef struct {
    bool *visual_changed;
    bool result;
    bool topics_work;
    size_t relayouts_before;
    bool font_work;
    bool font_publication;
    bool video_preview_work;
    bool layout_still_settling;
    bool retarget_work;
    bool retarget_changed;
    bool autofocus_work;
    bool cache_work;
} BrowserIdleTurn;

typedef enum {
    BROWSER_IDLE_CONTINUE,
    BROWSER_IDLE_STOP
} BrowserIdleStepResult;

static BrowserIdleStepResult browser_idle_stop(BrowserIdleTurn *turn,
                                               bool result)
{
    turn->result = result;
    return BROWSER_IDLE_STOP;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_youtube_signin_recovery(BrowserEngine *engine,
                                     BrowserIdleTurn *turn)
{
    BrowserEngineNavigationIntent intent = {0};
    switch (browser_engine_youtube_idle_recovery(engine, &intent)) {
    case BROWSER_YOUTUBE_RECOVERY_NONE:
        return BROWSER_IDLE_CONTINUE;
    case BROWSER_YOUTUBE_RECOVERY_DECLINED:
        return browser_idle_stop(turn, false);
    case BROWSER_YOUTUBE_RECOVERY_NAVIGATE:
        break;
    }
    bool started = browser_engine_begin_navigation_request(engine, intent.url,
        "GET", NULL, 0, NULL, engine->config.maximum_document_bytes,
        engine->config.navigation_timeout_ms, intent.record_history,
        intent.signin_recovery);
    browser_engine_youtube_recovery_started(engine, started);
    return browser_idle_stop(turn, started);
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_youtube_pumps(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    turn->topics_work = browser_engine_youtube_idle_pumps(engine, visual_changed);
    if (visual_changed != NULL && *visual_changed) return browser_idle_stop(turn, true);
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_web_font_memory(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    /* Dropping optional page fonts relieves pressure; it must not wait in
       the startup queue that deliberately delays acquiring more resources.
       The navigation transaction declines while DOM mutations are pending. */
    size_t font_generation = engine->navigation.incremental_relayouts;
    NavigationBackgroundWorkOutcome font_memory_work =
        navigation_run_web_font_memory_work(&engine->navigation);
    if (font_memory_work == NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL) return browser_idle_stop(turn, true);
    if (font_generation != engine->navigation.incremental_relayouts) {
        (void) controller_rebind_focus(&engine->controller);
        if (!browser_engine_apply_layout_damage(engine)) return browser_idle_stop(turn, false);
        if (visual_changed != NULL) *visual_changed = true;
        return browser_idle_stop(turn, true);
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_startup_deferral(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    /* Startup script tasks can still change the whole document's geometry.
       Do not compete with them by reading/publishing optional fonts and
       images only to reflow the same page again immediately. The committed
       baseline frame and input remain usable, and tile preparation can run.
       Timers and unrelated fetches do not delay resources. After 128 runtime
       turns even continuously appended scripts stop deferring idle work. */
    bool defer_for_startup = engine->navigation.page.runtime != NULL
        && engine->navigation.page.script_result.runtime_ticks < 128u
        && (engine->font_load != NULL
            || (engine->font_requested_faces & (FontSetFaceMask)
                ~(BROWSER_BASELINE_FONT_FACES | engine->font_failed_faces)) != 0
            || navigation_background_resources_pending(&engine->navigation))
        && script_runtime_has_pending_startup_scripts(engine->navigation.page.runtime);
    if (defer_for_startup) {
        /* Turns are not time: a device advancing 100 ms scripts reaches 128
           turns only after a quarter of a minute, while an in-flight external
           script keeps the predicate true the whole time. Bound the wait by
           the wall clock as well, per runtime. */
        uint64_t now = tilefinch_platform_monotonic_time_us();
        const void *runtime = engine->navigation.page.runtime;
        if (engine->startup_deferral_runtime != runtime
            || engine->startup_deferral_generation != engine->navigation.generation) {
            engine->startup_deferral_runtime = runtime;
            engine->startup_deferral_generation = engine->navigation.generation;
            engine->startup_deferral_started_us = now;
        }
        uint64_t limit = engine->config.startup_resource_deferral_us;
        if (limit != 0 && now >= engine->startup_deferral_started_us
            && now - engine->startup_deferral_started_us >= limit) {
            defer_for_startup = false;
        }
    }
    if (defer_for_startup) {
        (void) tile_cache_run_idle_work(&engine->render,
            engine->config.idle_work_budget_us,
            engine->config.idle_work_maximum_units);
        return browser_idle_stop(turn, true);
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_layout_completion(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    ScriptRuntime *runtime = engine->navigation.page.runtime;
    if (navigation_layout_completion_pending(&engine->navigation)
        && !browser_engine_navigation_pending(engine)
        /* A bounded focus layout's completion waits for pending author
           mutations (navigation_complete_layout_step): the relayout of
           the next runtime turn publishes them first. Until then the
           later idle steps run; stopping here would starve them. */
        && (runtime == NULL
            || !script_runtime_has_pending_mutations(runtime))) {
        /* The provisional screens of an in-place relayout are showing; the
           rest of the page is built here, one attempt per idle slice. Like
           an optional font batch it holds no author state, so a frontend
           may abandon it for new input and it simply starts again. Fonts
           and images wait: their publication would rebuild it anyway.
           A reader waiting at the bottom for the next screens would only
           cancel them again by pressing on (and never get them): in that
           phase a frontend queues further scrolling down instead and still
           yields to anything else. */
        bool awaited =
            navigation_layout_completion_awaited(&engine->navigation);
        const char *completion_phase = awaited
                ? "optional-layout-extension-awaited"
                : "optional-layout-completion";
        unsigned completion_boundary = TILEFINCH_COOPERATE_LAYOUT_COMPLETION
            | (awaited ? TILEFINCH_COOPERATE_EXTENSION_AWAITED : 0u);
        if (!tilefinch_platform_cooperate_at(
                completion_phase, 1, completion_boundary)) {
            (void) tilefinch_platform_cooperate_at(
                completion_phase, 0, completion_boundary);
            return browser_idle_stop(turn, true);
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t completion_started = tilefinch_platform_monotonic_time_us();
#endif
        engine->layout_completion_running = true;
        engine->completion_scrolled = false;
        /* A relayout's served press may have left its fill unfinished; that
           frame is the main loop's now. */
        engine->completion_frame_pending = false;
        NavigationLayoutCompletion completion =
            navigation_complete_layout(&engine->navigation);
        engine->layout_completion_running = false;
        engine->completion_frame_pending = false;
        if (engine->completion_scrolled && visual_changed != NULL)
            *visual_changed = true;
        (void) tilefinch_platform_cooperate_at(
            completion_phase, 0, completion_boundary);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char completion_message[160];
        snprintf(completion_message, sizeof(completion_message),
                 "tilefinch-layout-completion: result=%d elapsed=%lluus "
                 "limit=%d height=%d awaited=%d",
                 (int) completion,
                 (unsigned long long) (tilefinch_platform_monotonic_time_us()
                                       - completion_started),
                 engine->navigation.page.layout.content_limit_y,
                 engine->navigation.page.layout.height,
                 awaited);
        tilefinch_platform_log_message(completion_message);
#endif
        if (completion == NAVIGATION_LAYOUT_COMPLETION_ADOPTED
            || completion == NAVIGATION_LAYOUT_COMPLETION_EXTENDED) {
            (void) controller_rebind_focus(&engine->controller);
            if (!browser_engine_apply_layout_damage(engine)) return browser_idle_stop(turn, false);
            if (visual_changed != NULL) *visual_changed = true;
        }
        return browser_idle_stop(turn, true);
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_optional_fonts(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    /* Optional font and image work share one relayout generation: the
       layout-damage step below compares against it. */
    turn->relayouts_before = engine->navigation.incremental_relayouts;
    bool font_work = false;
    bool font_publication = false;
    FontSetFaceMask optional = engine->font_requested_faces
        & (FontSetFaceMask) ~(BROWSER_BASELINE_FONT_FACES
                             | engine->font_failed_faces);
    size_t publication_limit =
        engine->config.fonts.maximum_publication_work_units;
    bool font_guarded = optional != 0 && publication_limit != 0
        && engine->navigation.page.loaded
        && engine->navigation.page.layout.layout_work_units
               > publication_limit;
    if (font_guarded) {
        /* Publishing the batch reflows the whole page at roughly its initial
           layout cost. On a page this large that is seconds of blocked
           input for a font change, and each cancelled attempt would only be
           retried; keep the fallback faces for this page instead. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-font-batch: skipped faces=0x%02x work=%zu limit=%zu",
                 (unsigned) optional,
                 engine->navigation.page.layout.layout_work_units,
                 publication_limit);
        if (!engine->font_publication_guarded)
            tilefinch_platform_log_message(message);
#endif
        /* Policy refusal applies to this layout, not to the font file for
           the engine's lifetime. Later small/provider pages can load it. */
        browser_engine_discard_staged_fonts(engine);
        optional = 0;
    }
    engine->font_publication_guarded = font_guarded;
    const struct {
        FontSetFaceMask bit;
        FontFace *face;
        FontFace *staged;
        const char *path;
    } font_order[] = {
        { FONT_SET_FACE_SERIF, &engine->fonts.serif, &engine->staged_fonts.serif,
          engine->config.fonts.serif_path },
        { FONT_SET_FACE_SANS_ITALIC, &engine->fonts.sans_italic,
          &engine->staged_fonts.sans_italic,
          engine->config.fonts.sans_italic_path },
        { FONT_SET_FACE_SANS_BOLD, &engine->fonts.sans_bold,
          &engine->staged_fonts.sans_bold,
          engine->config.fonts.sans_bold_path },
        { FONT_SET_FACE_METRIC_SANS_BOLD, &engine->fonts.metric_sans_bold,
          &engine->staged_fonts.metric_sans_bold,
          engine->config.fonts.metric_sans_bold_path },
        { FONT_SET_FACE_SERIF_BOLD, &engine->fonts.serif_bold,
          &engine->staged_fonts.serif_bold,
          engine->config.fonts.serif_bold_path }
    };
    if (!browser_engine_navigation_pending(engine)) {
        for (size_t at = 0;
             at < sizeof(font_order) / sizeof(font_order[0]); at++) {
            if ((optional & font_order[at].bit) == 0
                || font_order[at].face->loaded
                || font_order[at].staged->loaded
                || (engine->font_load != NULL
                    && engine->font_loading_face != font_order[at].bit)) {
                continue;
            }
            font_work = true;
            if (engine->font_load == NULL) {
                size_t loaded = font_set_loaded_bytes(&engine->fonts);
                size_t staged_bytes = font_set_loaded_bytes(&engine->staged_fonts);
                size_t maximum = engine->config.fonts.maximum_total_bytes;
                if (loaded >= maximum || staged_bytes >= maximum - loaded) {
                    engine->font_failed_faces |= font_order[at].bit;
                    break;
                }
                engine->font_load = font_face_load_begin(
                    &engine->budget, font_order[at].path,
                    maximum - loaded - staged_bytes);
                if (engine->font_load == NULL) {
                    engine->font_failed_faces |= font_order[at].bit;
                    break;
                }
                engine->font_loading_face = font_order[at].bit;
            }
            FontFaceLoadStatus status = font_face_load_pump(
                engine->font_load, BROWSER_FONT_IDLE_READ_BYTES,
                font_order[at].staged);
            font_face_load_destroy(
                status == FONT_FACE_LOAD_PENDING ? NULL : engine->font_load);
            if (status == FONT_FACE_LOAD_PENDING) break;
            engine->font_load = NULL;
            engine->font_loading_face = 0;
            if (status != FONT_FACE_LOAD_COMPLETE) {
                engine->font_failed_faces |= font_order[at].bit;
                break;
            }
            break;
        }
        bool batch_ready = engine->font_load == NULL;
        FontSetFaceMask staged = 0;
        for (size_t at = 0; at < sizeof(font_order) / sizeof(font_order[0]); at++) {
            if (font_order[at].staged->loaded) staged |= font_order[at].bit;
            if ((optional & font_order[at].bit) != 0
                && (engine->font_failed_faces & font_order[at].bit) == 0
                && !font_order[at].face->loaded
                && !font_order[at].staged->loaded) batch_ready = false;
        }
        if (batch_ready && staged != 0) font_work = true;
        if (batch_ready && staged != 0
            && !script_runtime_has_pending_mutations(engine->navigation.page.runtime)
            && tilefinch_platform_monotonic_time_us()
                   >= engine->font_publication_retry_us) {
            /* This transaction can be abandoned without author mutation.
               Let a frontend yield to new input at layout checkpoints; the
               live page keeps its fallback metrics until adoption succeeds. */
            bool publication_allowed = tilefinch_platform_cooperate_at(
                "optional-font-publication", 1,
                TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION);
            if (!publication_allowed) {
                engine->font_publication_retry_us =
                    tilefinch_platform_monotonic_time_us() + UINT64_C(250000);
                (void) tilefinch_platform_cooperate_at(
                "optional-font-publication", 0,
                TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION);
                return browser_idle_stop(turn, true);
            }
            font_publication = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t font_started_us = tilefinch_platform_monotonic_time_us();
#endif
            for (size_t at = 0; at < sizeof(font_order) / sizeof(font_order[0]); at++) {
                if ((staged & font_order[at].bit) == 0) continue;
                *font_order[at].face = *font_order[at].staged;
                memset(font_order[at].staged, 0, sizeof(*font_order[at].staged));
            }
            /* Metrics changed in place, not CSS. Reuse font-independent
               styles but remeasure every geometry and ch-dependent value. */
            layout_reuse_cache_begin_font_publication(
                engine->navigation.page.layout_reuse);
            bool adopted = navigation_relayout(&engine->navigation);
            layout_reuse_cache_end_font_publication(
                engine->navigation.page.layout_reuse);
            if (!adopted) {
                /* A supervisor cancel (Circle during idle work) stops the
                   relayout at a cooperate checkpoint. The faces are still
                   good: return them to staging and publish on a later pump
                   instead of dropping the batch for the rest of the page.
                   A refused build keeps the fallback faces for good. */
                bool retry_later = engine->navigation.page.loaded
                    && engine->navigation.layout_build_cancelled;
                if (retry_later) engine->font_publication_retry_us =
                    tilefinch_platform_monotonic_time_us() + UINT64_C(250000);
                for (size_t at = 0; at < sizeof(font_order) / sizeof(font_order[0]); at++) {
                    if ((staged & font_order[at].bit) == 0) continue;
                    if (retry_later) {
                        *font_order[at].staged = *font_order[at].face;
                        memset(font_order[at].face, 0, sizeof(*font_order[at].face));
                    } else {
                        font_face_destroy(font_order[at].face);
                    }
                }
                if (!retry_later) engine->font_failed_faces |= staged;
                layout_reuse_cache_reset(engine->navigation.page.layout_reuse);
            } else {
                tile_cache_invalidate_glyphs(&engine->render);
                if (visual_changed != NULL) *visual_changed = true;
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            char message[128];
            snprintf(message, sizeof(message),
                     "tilefinch-font-batch: faces=0x%02x adopted=%d relayout=%lluus",
                     (unsigned) staged, adopted,
                     (unsigned long long) (tilefinch_platform_monotonic_time_us() - font_started_us));
            tilefinch_platform_log_message(message);
#endif
            font_work = true;
            (void) tilefinch_platform_cooperate_at(
                "optional-font-publication", 0,
                TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION);
        }
    } else if (engine->font_load != NULL) {
        font_work = true;
    }
    turn->font_work = font_work;
    turn->font_publication = font_publication;
    if (font_publication && !engine->navigation.page.loaded) {
        /* The in-place rebuild path retires the page when its relayout
           fails; that must surface like any other retired page rather than
           be reported as a successful idle pump. */
        browser_engine_reset_shell(engine);
        return browser_idle_stop(turn, set_error_code(
            engine, TILEFINCH_SUBSYSTEM_NETWORK,
            TILEFINCH_DIAGNOSTIC_NETWORK_FAILED, "font-publication",
            "web font publication relayout retired the page"));
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_background_resources(BrowserEngine *engine,
                                  BrowserIdleTurn *turn)
{
    NavigationBackgroundWorkOutcome background_outcome =
        turn->font_publication ? NAVIGATION_BACKGROUND_WORK_SUCCESS
        : navigation_run_background_resources(&engine->navigation);
    if (!engine->navigation.page.loaded) {
        /* Some optional continuations report a soft refusal after their
           required, mutation-driven relayout retired the DOM. A soft outcome
           must not let the controller/render shell keep its borrowed graph. */
        return browser_idle_stop(turn, browser_engine_report_retired_update(engine, "background-publication"));
    }
    if (background_outcome == NAVIGATION_BACKGROUND_WORK_HARD_FAILURE) {
        return browser_idle_stop(turn, set_error_code(
            engine, TILEFINCH_SUBSYSTEM_NETWORK,
            TILEFINCH_DIAGNOSTIC_NETWORK_FAILED, "background-resources",
            "background resource continuation failed"));
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_video_preview(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool video_preview_work = !turn->font_work
        && !browser_engine_navigation_pending(engine)
        && !navigation_background_resources_pending(&engine->navigation)
        && navigation_run_linked_video_preview(&engine->navigation);
    turn->video_preview_work = video_preview_work;
    if (video_preview_work && !engine->navigation.page.loaded) {
        browser_engine_reset_shell(engine);
        return browser_idle_stop(turn, set_error_code(engine, TILEFINCH_SUBSYSTEM_NETWORK,
            TILEFINCH_DIAGNOSTIC_NETWORK_FAILED, "video-preview-publication",
            "preview publication relayout retired the page"));
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_layout_damage(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    size_t relayouts_after = engine->navigation.incremental_relayouts;
    if (engine->navigation.incremental_relayouts
            != turn->relayouts_before) {
        /* Optional font and image work share this continuation. Rebind once
           after all of it so neither path can expose a stale focus to input. */
        (void) controller_rebind_focus(&engine->controller);
    }
    if (relayouts_after != turn->relayouts_before) {
        /* The relayout already computed the union of changed command bounds.
           Preserve decoded images, tiles outside that damage, and the active
           frame job rather than destroying the complete render shell for one
           optional font or thumbnail arrival. */
        if (!browser_engine_apply_layout_damage(engine)) return browser_idle_stop(turn, false);
        /* A background image/font continuation committed new layout and
           display commands. The shell refresh alone does not repaint the
           PSP framebuffer; tell the frontend to schedule that frame. */
        if (visual_changed != NULL) *visual_changed = true;
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_image_retarget(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    /* Autofocus is a commitment of the settled layout, not of the earlier
       parser checkpoint. Run it after optional font/resource work so a
       relayout cannot immediately retire the region we just selected. */
    bool layout_still_settling = turn->font_work || turn->video_preview_work
        || engine->font_load != NULL
        || navigation_background_resources_pending(&engine->navigation);
    turn->layout_still_settling = layout_still_settling;
    /* Once images and fonts have settled, keep only the painted size of
       each decoded raster, one surface per turn. A change publishes an
       empty-damage layout generation that the next frame applies. */
    if (!layout_still_settling) {
        turn->retarget_changed = navigation_run_image_retarget_work(
            &engine->navigation);
        turn->retarget_work = navigation_image_retarget_pending(
            &engine->navigation);
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_autofocus(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool *visual_changed = turn->visual_changed;
    bool autofocus_work = browser_engine_retry_deferred_autofocus(
        engine, turn->layout_still_settling);
    turn->autofocus_work = autofocus_work;
    if (autofocus_work) {
        if (!browser_engine_finish_input(engine, true)) return browser_idle_stop(turn, false);
        if (visual_changed != NULL) *visual_changed = true;
    }
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_tiles(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    (void) turn;
    (void) tile_cache_run_idle_work(
        &engine->render, engine->config.idle_work_budget_us,
        engine->config.idle_work_maximum_units);
    return BROWSER_IDLE_CONTINUE;
}

static TILEFINCH_OUT_OF_LINE BrowserIdleStepResult
browser_idle_script_cache(BrowserEngine *engine, BrowserIdleTurn *turn)
{
    bool cache_work = false;
    bool turn_quiet = !turn->font_work && !turn->video_preview_work && engine->font_load == NULL
        && !turn->autofocus_work && !tile_cache_idle_work_pending(&engine->render);
    bool store_pending = navigation_script_bytecode_pending(
        &engine->navigation);
    if (turn_quiet && turn->retarget_changed && !engine->idle_cache_waited
        && (store_pending
            || browser_session_lazy_bundle_pending_count(
                   &engine->session) != 0
            || browser_session_script_disk_enabled(&engine->session))) {
        /* A turn that already retargeted an image surface defers the
           compiled-script work (a store or a persistent-tier slice) by one
           turn, and not more: the two alternate while both have work, so
           neither starves the other and a turn does at most one of each. */
        engine->idle_cache_waited = true;
        cache_work = true;
    } else if (turn_quiet && store_pending
               && !browser_engine_navigation_pending(engine)) {
        /* The load queued its classic bytecode stores; serialize one per
           quiet turn. Images still arriving do not hold them back (they
           can keep arriving for as long as the page is open); a turn that
           did font, focus or tile work does.
           A large script is one unbreakable unit, so pending input gets
           the checkpoint first and the store waits a turn. */
        engine->idle_cache_waited = false;
        cache_work = !tilefinch_platform_cooperate(
                "script-bytecode-store", 1)
            || navigation_store_pending_script_bytecode(&engine->navigation)
            || navigation_script_bytecode_pending(&engine->navigation);
    } else if (turn_quiet && !browser_engine_navigation_pending(engine)) {
        /* Bundle records waiting for their digest take one slice of
           hashing per quiet turn; then the persistent tier writes what the
           stores above put in RAM, in small slices. Like them neither
           waits for images. */
        engine->idle_cache_waited = false;
        cache_work = browser_session_lazy_bundle_record_maintenance(
                &engine->session)
            || browser_session_script_disk_maintenance(&engine->session);
    }
    turn->cache_work = cache_work;
    return BROWSER_IDLE_CONTINUE;
}

typedef struct {
    const char *name;
    BrowserIdleStepResult (*run)(BrowserEngine *engine,
                                 BrowserIdleTurn *turn);
} BrowserIdleStep;

/* The order is the scheduling policy. Provider sign-in recovery and pumps
   come first; web-font memory relief precedes the startup deferral that
   holds back new resources; an unfinished in-place layout is completed
   before fonts or images publish onto it; their shared relayout is applied
   once; retargeting, autofocus, tiles and the script cache run only on the
   settled layout. A new continuation adds a row here. */
static const BrowserIdleStep browser_engine_idle_steps[] = {
    { "youtube-signin-recovery", browser_idle_youtube_signin_recovery },
    { "youtube-pumps", browser_idle_youtube_pumps },
    { "web-font-memory", browser_idle_web_font_memory },
    { "startup-deferral", browser_idle_startup_deferral },
    { "layout-completion", browser_idle_layout_completion },
    { "optional-fonts", browser_idle_optional_fonts },
    { "background-resources", browser_idle_background_resources },
    { "video-preview", browser_idle_video_preview },
    { "layout-damage", browser_idle_layout_damage },
    { "image-retarget", browser_idle_image_retarget },
    { "autofocus", browser_idle_autofocus },
    { "tiles", browser_idle_tiles },
    { "script-cache", browser_idle_script_cache }
};

#if !defined(__PSP__)
const char *browser_engine_test_idle_step_name(size_t index)
{
    return index < sizeof(browser_engine_idle_steps)
                       / sizeof(browser_engine_idle_steps[0])
        ? browser_engine_idle_steps[index].name : NULL;
}
#endif

bool browser_engine_run_idle_work(
    BrowserEngine *engine, bool *visual_changed)
{
    if (visual_changed != NULL) *visual_changed = false;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->render_ready) return false;
    BrowserIdleTurn turn = {.visual_changed = visual_changed};
    for (size_t i = 0; i < sizeof(browser_engine_idle_steps)
                               / sizeof(browser_engine_idle_steps[0]); i++) {
        if (browser_engine_idle_steps[i].run(engine, &turn)
                == BROWSER_IDLE_STOP)
            return turn.result;
    }
    return turn.topics_work || turn.cache_work || turn.autofocus_work || turn.font_work
        || turn.retarget_work || turn.video_preview_work
        || navigation_background_resources_pending(&engine->navigation)
        || tile_cache_idle_work_pending(&engine->render);
}

bool browser_engine_run_deferred_image_work(
    BrowserEngine *engine, bool *visual_changed)
{
    if (visual_changed != NULL) *visual_changed = false;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->render_ready || browser_engine_navigation_pending(engine)) {
        return false;
    }
    size_t relayouts_before = engine->navigation.incremental_relayouts;
    if (!navigation_run_deferred_image_work(&engine->navigation)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_NETWORK,
            TILEFINCH_DIAGNOSTIC_NETWORK_FAILED, "deferred-image-work",
            "deferred image continuation failed");
    }
    size_t relayouts_after = engine->navigation.incremental_relayouts;
    if (relayouts_after != relayouts_before) {
        (void) controller_rebind_focus(&engine->controller);
        if (!browser_engine_apply_layout_damage(engine)) return false;
        if (visual_changed != NULL) *visual_changed = true;
    }
    return navigation_background_resources_pending(&engine->navigation);
}

void browser_engine_cancel_idle_work(BrowserEngine *engine)
{
    if (engine == NULL || !engine->render_ready) return;
    tile_cache_cancel_idle_work(&engine->render);
}

uint64_t browser_engine_render_shell_serial(const BrowserEngine *engine)
{
    return engine == NULL ? 0 : engine->render_shell_serial;
}

bool browser_engine_set_web_font_mode(BrowserEngine *engine, WebFontMode mode)
{
    if (engine == NULL || mode < WEB_FONTS_ON || mode >= WEB_FONTS_MODE_COUNT) return false;
    engine->config.resources.web_font_mode = mode;
    return navigation_set_web_font_mode(&engine->navigation, mode);
}

bool browser_engine_reclaim_optional_memory(
    BrowserEngine *engine, BrowserOptionalMemoryReclaim *reclaim)
{
    if (reclaim != NULL) memset(reclaim, 0, sizeof(*reclaim));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) return false;

    BrowserOptionalMemoryReclaim result = {0};
    if (engine->font_load != NULL
        || font_set_loaded_bytes(&engine->staged_fonts) != 0) {
        size_t before = budget_remaining(&engine->budget);
        browser_engine_discard_staged_fonts(engine);
        size_t after = budget_remaining(&engine->budget);
        result.font_staging_bytes = after > before ? after - before : 0;
    }
    if (engine->navigation_ready && engine->navigation.page.runtime != NULL) {
        result.javascript_bytes = script_runtime_collect_and_trim(
            engine->navigation.page.runtime);
    }
    if (engine->session_ready) {
        result.session_cache_bytes = browser_session_cache_reclaim(
            &engine->session, SIZE_MAX);
    }
    if (engine->render_ready) {
        result.render_cache_bytes = tile_cache_reclaim_optional(
            &engine->render);
    }
    result.total_bytes = result.font_staging_bytes;
    if (result.total_bytes <= SIZE_MAX - result.javascript_bytes) {
        result.total_bytes += result.javascript_bytes;
    } else {
        result.total_bytes = SIZE_MAX;
    }
    if (result.total_bytes <= SIZE_MAX - result.session_cache_bytes) {
        result.total_bytes += result.session_cache_bytes;
    } else {
        result.total_bytes = SIZE_MAX;
    }
    if (result.total_bytes <= SIZE_MAX - result.render_cache_bytes) {
        result.total_bytes += result.render_cache_bytes;
    } else {
        result.total_bytes = SIZE_MAX;
    }
    if (reclaim != NULL) *reclaim = result;
    (void) emit_diagnostic(
        engine, TILEFINCH_DIAGNOSTIC_DEBUG,
        TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_OK,
        "optional-memory-reclaimed", "", result.total_bytes,
        budget_remaining(&engine->budget));
    return true;
}

static void browser_optional_reclaim_add(
    size_t *total, size_t amount)
{
    if (total == NULL) return;
    *total = amount > SIZE_MAX - *total ? SIZE_MAX : *total + amount;
}

void browser_engine_prepare_optional_memory_reclaim(
    BrowserEngine *engine, size_t target_remaining_bytes,
    BrowserOptionalMemoryReclaimJob *job)
{
    if (job == NULL) return;
    memset(job, 0, sizeof(*job));
    job->target_remaining_bytes = target_remaining_bytes;
    job->phase = BROWSER_OPTIONAL_RECLAIM_FONT_STAGING;
    job->active = engine != NULL
        && engine->state == BROWSER_ENGINE_ACTIVE
        && budget_remaining(&engine->budget) < target_remaining_bytes;
}

bool browser_engine_optional_memory_reclaim_pending(
    const BrowserOptionalMemoryReclaimJob *job)
{
    return job != NULL && job->active;
}

bool browser_engine_pump_optional_memory_reclaim(
    BrowserEngine *engine, BrowserOptionalMemoryReclaimJob *job)
{
    if (job == NULL || !job->active) return true;
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || budget_remaining(&engine->budget)
               >= job->target_remaining_bytes) {
        job->active = false;
        return true;
    }

    size_t reclaimed = 0;
    switch (job->phase) {
    case BROWSER_OPTIONAL_RECLAIM_FONT_STAGING:
        if (engine->font_load != NULL
            || font_set_loaded_bytes(&engine->staged_fonts) != 0) {
            size_t before = budget_remaining(&engine->budget);
            browser_engine_discard_staged_fonts(engine);
            size_t after = budget_remaining(&engine->budget);
            reclaimed = after > before ? after - before : 0;
            job->reclaimed.font_staging_bytes = reclaimed;
        }
        job->phase = BROWSER_OPTIONAL_RECLAIM_SESSION_CACHE;
        break;
    case BROWSER_OPTIONAL_RECLAIM_SESSION_CACHE:
        if (engine->session_ready) {
            /* Bound a frame by reclaimed bytes rather than draining the
               entire cache. One oversized oldest entry may exceed the slice,
               but the fixed entry table keeps lookup/free work finite. */
            size_t remaining = budget_remaining(&engine->budget);
            size_t deficit = remaining < job->target_remaining_bytes
                ? job->target_remaining_bytes - remaining : 0;
            size_t slice = deficit < 256u * 1024u
                ? deficit : 256u * 1024u;
            reclaimed = browser_session_cache_reclaim(
                &engine->session, slice == 0 ? 1u : slice);
            browser_optional_reclaim_add(
                &job->reclaimed.session_cache_bytes, reclaimed);
        }
        if (reclaimed == 0)
            job->phase = BROWSER_OPTIONAL_RECLAIM_RENDER_CACHE;
        break;
    case BROWSER_OPTIONAL_RECLAIM_RENDER_CACHE:
        if (engine->render_ready) {
            reclaimed = tile_cache_reclaim_optional(&engine->render);
            job->reclaimed.render_cache_bytes = reclaimed;
        }
        job->phase = BROWSER_OPTIONAL_RECLAIM_JS_FIRST_GC;
        break;
    case BROWSER_OPTIONAL_RECLAIM_JS_FIRST_GC:
        if (engine->navigation_ready
            && engine->navigation.page.runtime != NULL) {
            (void) script_runtime_collect_and_trim_phase(
                engine->navigation.page.runtime,
                SCRIPT_RUNTIME_COLLECT_FIRST_GC);
        }
        job->phase = BROWSER_OPTIONAL_RECLAIM_JS_SECOND_GC;
        break;
    case BROWSER_OPTIONAL_RECLAIM_JS_SECOND_GC:
        if (engine->navigation_ready
            && engine->navigation.page.runtime != NULL) {
            (void) script_runtime_collect_and_trim_phase(
                engine->navigation.page.runtime,
                SCRIPT_RUNTIME_COLLECT_SECOND_GC);
        }
        job->phase = BROWSER_OPTIONAL_RECLAIM_JS_TRIM;
        break;
    case BROWSER_OPTIONAL_RECLAIM_JS_TRIM:
        if (engine->navigation_ready
            && engine->navigation.page.runtime != NULL) {
            reclaimed = script_runtime_collect_and_trim_phase(
                engine->navigation.page.runtime,
                SCRIPT_RUNTIME_COLLECT_TRIM);
            job->reclaimed.javascript_bytes = reclaimed;
        }
        job->phase = BROWSER_OPTIONAL_RECLAIM_PHASE_COUNT;
        break;
    case BROWSER_OPTIONAL_RECLAIM_PHASE_COUNT:
        break;
    }
    browser_optional_reclaim_add(
        &job->reclaimed.total_bytes, reclaimed);
    if (budget_remaining(&engine->budget) >= job->target_remaining_bytes
        || job->phase >= BROWSER_OPTIONAL_RECLAIM_PHASE_COUNT) {
        job->active = false;
        (void) emit_diagnostic(
            engine, TILEFINCH_DIAGNOSTIC_DEBUG,
            TILEFINCH_SUBSYSTEM_ENGINE, TILEFINCH_DIAGNOSTIC_OK,
            "optional-memory-reclaim-complete", "",
            job->reclaimed.total_bytes, budget_remaining(&engine->budget));
    }
    return !job->active;
}

bool browser_engine_maintain_background_workers(BrowserEngine *engine)
{
    return engine == NULL || images_decode_worker_reap_cancelled(
        &engine->budget);
}

const DocumentBacking *browser_engine_document_backing(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : &engine->backing;
}

bool browser_engine_view_snapshot(
    const BrowserEngine *engine, BrowserViewSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready)
        return false;

    const NavigationSession *navigation = &engine->navigation;
    const NavigationEntry *entry = navigation_current(navigation);
    const char *pending_url = browser_engine_pending_navigation_url(engine);
    const char *url = pending_url != NULL
        ? pending_url
        : navigation_active_document_url(navigation);
    const char *title = pending_url != NULL
        ? "Opening page"
        : (navigation->page.loaded
               ? navigation->page.document.title
               : (entry == NULL ? NULL : entry->title));
    snprintf(snapshot->url, sizeof(snapshot->url), "%s",
             url == NULL ? "" : url);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s",
             title == NULL ? "" : title);
    snapshot->secure = strncmp(snapshot->url, "https://", 8) == 0;
    snapshot->can_go_back =
        navigation->history_count > 0 && navigation->history_index > 0;
    snapshot->can_go_forward =
        navigation->history_count > 0
        && navigation->history_index + 1 < navigation->history_count;
    snapshot->scroll_y = entry == NULL ? 0 : entry->scroll_y;
    snapshot->maximum_scroll_y = viewport_max_scroll_css(
        &navigation->viewport, navigation->page.layout.height);
    snapshot->loading = browser_engine_navigation_pending(engine);
    snapshot->navigation_generation = navigation->generation;
    snapshot->tls_verify_result = navigation->last_tls_verify_result;
    snapshot->tls_verify_result_available =
        navigation->last_tls_verify_result_available;
    snapshot->tls_verification_failed =
        navigation->last_tls_verification_failed;
    snprintf(snapshot->tls_version, sizeof(snapshot->tls_version), "%s",
             navigation->last_tls_version);
    snprintf(snapshot->tls_peer_issuer,
             sizeof(snapshot->tls_peer_issuer), "%s",
             navigation->last_tls_peer_issuer);

    if (engine->controller_ready) {
        int x = 0, y = 0, width = 0, height = 0;
        if (controller_focused_rect(
                &engine->controller, &x, &y, &width, &height)) {
            snapshot->has_focus = true;
            snapshot->focus_x =
                viewport_css_to_device(&navigation->viewport, x);
            snapshot->focus_y = viewport_css_to_device(
                &navigation->viewport, y - snapshot->scroll_y);
            int right = viewport_css_to_device(
                &navigation->viewport, x + width);
            int bottom = viewport_css_to_device(
                &navigation->viewport,
                y - snapshot->scroll_y + height);
            snapshot->focus_width = right - snapshot->focus_x;
            snapshot->focus_height = bottom - snapshot->focus_y;
            snapshot->focus_has_authored_outline =
                engine->controller.has_authored_focus_outline;
            lxb_dom_node_t *node = controller_focused_node(&engine->controller);
            size_t length = 0;
            const char *label = document_attribute(node, "aria-label", &length);
            if (label == NULL || length == 0)
                label = document_attribute(node, "title", &length);
            if (label != NULL) {
                size_t n = length < sizeof(snapshot->focus_label) - 1u
                    ? length : sizeof(snapshot->focus_label) - 1u;
                /* Do not split a multibyte label at the fixed snapshot bound. */
                while (n < length && n != 0 && ((unsigned char) label[n] & 0xc0u) == 0x80u) n--;
                for (size_t i = 0; i < n; i++)
                    snapshot->focus_label[i] = (unsigned char) label[i] < 0x20u
                        || label[i] == 0x7f ? ' ' : label[i];
                snapshot->focus_label[n] = '\0';
            }
        }
    }
    return true;
}

static const NavigationSession *browser_engine_active_navigation(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->navigation_ready
        ? NULL : &engine->navigation;
}

uint64_t browser_engine_page_generation(const BrowserEngine *engine)
{
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    return navigation == NULL ? 0u : navigation->generation;
}

bool browser_engine_page_loaded(const BrowserEngine *engine)
{
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    return navigation != NULL && navigation->page.loaded;
}

bool browser_engine_page_script_memory_failed(const BrowserEngine *engine)
{
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    return navigation != NULL
        && (navigation->page.script_memory_limited
            || navigation->page.script_result.host_compile_memory_refusals
                   != 0u
            || (navigation->page.runtime == NULL
                && navigation->page.script_degradation_observed));
}

static bool browser_engine_copy_entry_text(
    char *out, size_t capacity, const char *text)
{
    if (out == NULL) return true;
    if (capacity == 0u) return false;
    out[0] = '\0';
    if (text == NULL) text = "";
    size_t length = strnlen(text, capacity);
    if (length >= capacity) return false;
    memcpy(out, text, length + 1u);
    return true;
}

bool browser_engine_current_entry(
    const BrowserEngine *engine, char *url, size_t url_capacity,
    int *scroll_y)
{
    if (url != NULL && url_capacity != 0u) url[0] = '\0';
    if (scroll_y != NULL) *scroll_y = 0;
    const NavigationEntry *entry =
        navigation_current(browser_engine_active_navigation(engine));
    if (entry == NULL
        || !browser_engine_copy_entry_text(url, url_capacity, entry->url)) {
        if (url != NULL && url_capacity != 0u) url[0] = '\0';
        return false;
    }
    if (scroll_y != NULL) *scroll_y = entry->scroll_y;
    return true;
}

bool browser_engine_current_entry_text(
    const BrowserEngine *engine, BrowserHistoryEntryText *text,
    int *scroll_y)
{
    if (text != NULL) {
        text->url[0] = '\0';
        text->title[0] = '\0';
    }
    if (scroll_y != NULL) *scroll_y = 0;
    const NavigationEntry *entry =
        navigation_current(browser_engine_active_navigation(engine));
    if (entry == NULL) return false;
    if (text != NULL
        && (!browser_engine_copy_entry_text(
                text->url, sizeof(text->url), entry->url)
            || !browser_engine_copy_entry_text(
                text->title, sizeof(text->title), entry->title))) {
        text->url[0] = '\0';
        text->title[0] = '\0';
        return false;
    }
    if (scroll_y != NULL) *scroll_y = entry->scroll_y;
    return true;
}

bool browser_engine_history_peek(
    const BrowserEngine *engine, bool forward, char *url,
    size_t url_capacity)
{
    if (url != NULL && url_capacity != 0u) url[0] = '\0';
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    if (navigation == NULL || navigation->history_count == 0u) return false;
    size_t index = navigation->history_index;
    if (forward ? index + 1u >= navigation->history_count : index == 0u)
        return false;
    if (!browser_engine_copy_entry_text(
            url, url_capacity,
            navigation->history[forward ? index + 1u : index - 1u].url)) {
        if (url != NULL && url_capacity != 0u) url[0] = '\0';
        return false;
    }
    return true;
}

bool browser_engine_page_document_url(
    const BrowserEngine *engine, char *url, size_t url_capacity)
{
    if (url != NULL && url_capacity != 0u) url[0] = '\0';
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    if (navigation == NULL
        || !browser_engine_copy_entry_text(
               url, url_capacity, navigation->page.document_url)) {
        if (url != NULL && url_capacity != 0u) url[0] = '\0';
        return false;
    }
    return true;
}

long browser_engine_last_http_status(const BrowserEngine *engine)
{
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    return navigation == NULL ? 0 : navigation->last_http_status;
}

size_t browser_engine_page_incremental_relayouts(const BrowserEngine *engine)
{
    const NavigationSession *navigation =
        browser_engine_active_navigation(engine);
    return navigation == NULL ? 0u : navigation->incremental_relayouts;
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
bool browser_engine_validation_counters(
    const BrowserEngine *engine, BrowserValidationCounters *counters)
{
    if (counters == NULL) return false;
    memset(counters, 0, sizeof(*counters));
    /* Same gate as the validation reports' former browser_engine_navigation()
       reads: a navigation session, whatever the engine's lifecycle state. */
    if (engine == NULL || !engine->navigation_ready) return false;
    const NavigationSession *navigation = &engine->navigation;
    counters->generation = navigation->generation;
    counters->loaded = navigation->page.loaded;
    counters->incremental_relayouts = navigation->incremental_relayouts;
    counters->full_relayouts = navigation->performance.full_relayouts;
    counters->fast_relayouts = navigation->performance.fast_relayouts;
    counters->layout_us = navigation->performance.layout_us;
    counters->layout_height = navigation->page.layout.height;
    counters->layout_fixed_count = navigation->page.layout.fixed_count;
    counters->layout_text_fingerprint =
        navigation->page.layout.text_fingerprint;
    counters->layout_performance = navigation->page.layout.performance;
    _Static_assert(sizeof(counters->last_cf_mitigated)
                       == sizeof(navigation->last_cf_mitigated),
                   "validation counters copy the whole header value");
    memcpy(counters->last_cf_mitigated, navigation->last_cf_mitigated,
           sizeof(counters->last_cf_mitigated));
    counters->managed_challenge_heap_raises =
        navigation->managed_challenge_heap_raises;
    return true;
}
#endif

bool browser_engine_frame_view(
    const BrowserEngine *engine, BrowserFrameView *view)
{
    if (view == NULL) return false;
    memset(view, 0, sizeof(*view));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || engine->framebuffer == NULL)
        return false;
    view->pixels = engine->framebuffer;
    view->pixel_count = engine->framebuffer_pixels;
    view->width = engine->config.device.framebuffer_width;
    view->height = engine->config.device.framebuffer_height;
    view->stride = engine->config.device.framebuffer_width;
    return true;
}

bool browser_engine_frontend_snapshot(
    const BrowserEngine *engine, BrowserFrontendSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE)
        return false;
    if (engine->framebuffer != NULL) {
        snapshot->frame.pixels = engine->framebuffer;
        snapshot->frame.pixel_count = engine->framebuffer_pixels;
        snapshot->frame.width = engine->config.device.framebuffer_width;
        snapshot->frame.height = engine->config.device.framebuffer_height;
        snapshot->frame.stride = engine->config.device.framebuffer_width;
    }
    if (engine->navigation_ready)
        snapshot->navigation = &engine->navigation;
    if (engine->controller_ready)
        snapshot->controller = &engine->controller;
    return true;
}

bool browser_engine_fill_frame(BrowserEngine *engine, uint16_t color)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || engine->framebuffer == NULL)
        return false;
    for (size_t index = 0; index < engine->framebuffer_pixels; index++)
        engine->framebuffer[index] = color;
    return true;
}

bool browser_engine_set_user_css(
    BrowserEngine *engine, const char *css, size_t length)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready)
        return false;
    return navigation_set_user_css(&engine->navigation, css, length);
}

bool browser_engine_apply_user_css(
    BrowserEngine *engine, const char *css, size_t length)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || (css == NULL && length != 0)
        || browser_engine_navigation_pending(engine)
        || engine->candidate_shell_prepared
        || engine->candidate_render.budget != NULL) {
        return false;
    }
    NavigationSession *navigation = &engine->navigation;
    lxb_dom_node_t *focused_node = engine->controller_ready
        ? controller_focused_node(&engine->controller) : NULL;
    NavigationUserCssTransaction *transaction =
        navigation_apply_user_css_begin(navigation, css, length);
    if (transaction == NULL) return false;

    /* Build the replacement controller/tile cache while the incumbent shell
       and the transaction's old layout remain owned. A real Budget refusal
       therefore destroys only candidate ownership; rollback itself performs
       no allocation and the already-painted incumbent remains usable. */
    if (!browser_engine_prepare_candidate_shell(engine, navigation)) {
        browser_engine_abort_candidate_shell(engine);
        (void) navigation_apply_user_css_rollback(
            navigation, transaction);
        return false;
    }
    if (focused_node != NULL) {
        (void) controller_restore_focus_node(
            &engine->candidate_controller, focused_node);
    }
    browser_engine_commit_candidate_shell(engine);
    navigation_apply_user_css_commit(navigation, transaction);
    clear_error(engine);
    return true;
}

#include "browser_engine/reader_recovery.inc"

const NavigationSession *browser_engine_navigation_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->navigation_ready
        ? NULL : &engine->navigation;
}

const BrowserController *browser_engine_controller_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->controller_ready
        ? NULL : &engine->controller;
}

const FontFace *browser_engine_font_face(
    const BrowserEngine *engine, FontFamily family)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->fonts_ready
        ? NULL : font_set_face(&engine->fonts, family);
}

const FontFace *browser_engine_font_face_variant(
    const BrowserEngine *engine, FontFamily family, bool italic, bool bold)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->fonts_ready
        ? NULL : font_set_face_variant(
            &engine->fonts, family, italic, bold);
}

const TileCache *browser_engine_render_metrics_view(
    const BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
            || !engine->render_ready
        ? NULL : &engine->render;
}

Budget *browser_engine_budget(BrowserEngine *engine)
{
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : &engine->budget;
}

BrowserSession *browser_engine_session(BrowserEngine *engine)
{
    return engine == NULL || !engine->session_ready ? NULL : &engine->session;
}

const BrowserSession *browser_engine_session_view(const BrowserEngine *engine)
{
    return engine == NULL || !engine->session_ready ? NULL : &engine->session;
}

NavigationSession *browser_engine_navigation(BrowserEngine *engine)
{
    return engine == NULL || !engine->navigation_ready
        ? NULL : &engine->navigation;
}

bool browser_engine_take_download_offer(BrowserEngine *engine,
    char *url, size_t url_capacity, char *filename, size_t filename_capacity)
{
    if (engine == NULL || url == NULL || filename == NULL) return false;
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (!work->download_filename[0] || strlen(work->url) >= url_capacity
        || strlen(work->download_filename) >= filename_capacity) return false;
    strcpy(url, work->url);
    strcpy(filename, work->download_filename);
    work->download_filename[0] = '\0';
    return true;
}

BrowserController *browser_engine_controller(BrowserEngine *engine)
{
    return engine == NULL || !engine->controller_ready
        ? NULL : &engine->controller;
}

FontSet *browser_engine_fonts(BrowserEngine *engine)
{
    return engine == NULL || !engine->fonts_ready ? NULL : &engine->fonts;
}

TileCache *browser_engine_render_shell(BrowserEngine *engine)
{
    return engine == NULL || !engine->render_ready ? NULL : &engine->render;
}

uint16_t *browser_engine_framebuffer(BrowserEngine *engine,
                                     size_t *pixel_count)
{
    if (pixel_count != NULL)
        *pixel_count = engine == NULL ? 0 : engine->framebuffer_pixels;
    return engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        ? NULL : engine->framebuffer;
}
