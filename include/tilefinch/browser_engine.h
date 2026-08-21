#ifndef TILEFINCH_BROWSER_ENGINE_H
#define TILEFINCH_BROWSER_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/controller.h"
#include "tilefinch/diagnostics.h"
#include "tilefinch/document_backing.h"
#include "tilefinch/font.h"
#include "tilefinch/navigation.h"
#include "tilefinch/render.h"
#include "tilefinch/reader_mode.h"
#include "tilefinch/session.h"

#define BROWSER_ENGINE_PROFILE_NAME_LIMIT 32
#define BROWSER_ENGINE_PATH_LIMIT 512
#define BROWSER_PSP_STRICT_CONTENT_LIMIT (16u * 1024u * 1024u)
#define BROWSER_PSP_REALISTIC_CONTENT_LIMIT (24u * 1024u * 1024u)
#define BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE (8u * 1024u * 1024u)

typedef enum {
    BROWSER_PSP_MEMORY_STRICT = 0,
    BROWSER_PSP_MEMORY_REALISTIC
} BrowserPspMemoryProfile;

typedef struct {
    char name[BROWSER_ENGINE_PROFILE_NAME_LIMIT];
    unsigned target_clock_mhz;
    int framebuffer_width;
    int framebuffer_height;
    int navigation_viewport_width;
    size_t recommended_memory_limit;
    size_t minimum_non_page_reserve;
    size_t maximum_tile_capacity;
} BrowserDeviceProfile;

typedef struct {
    bool enabled;
    bool document_scripts_enabled;
    ScriptExecutionPolicy execution_policy;
    size_t heap_limit;
    unsigned runtime_timeout_ms;
    size_t maximum_scripts;
    size_t maximum_total_bytes;
    size_t maximum_file_bytes;
    long network_timeout_ms;
} BrowserJavaScriptConfig;

typedef struct {
    bool enabled;
    bool web_fonts_enabled;
    size_t maximum_stylesheets;
    size_t maximum_stylesheet_bytes;
    size_t maximum_stylesheet_file_bytes;
    size_t maximum_font_attempts;
    size_t maximum_font_bytes;
    size_t maximum_font_file_bytes;
    size_t maximum_font_face_backend_bytes;
    size_t maximum_images;
    size_t maximum_image_bytes;
    size_t maximum_image_file_bytes;
    size_t maximum_decoded_image_bytes;
    long timeout_ms;
} BrowserResourceConfig;

typedef struct {
    bool enabled;
    size_t maximum_total_bytes;
    char sans_path[BROWSER_ENGINE_PATH_LIMIT];
    char serif_path[BROWSER_ENGINE_PATH_LIMIT];
    char sans_italic_path[BROWSER_ENGINE_PATH_LIMIT];
    char sans_bold_path[BROWSER_ENGINE_PATH_LIMIT];
    char serif_bold_path[BROWSER_ENGINE_PATH_LIMIT];
    char metric_sans_path[BROWSER_ENGINE_PATH_LIMIT];
    char metric_sans_bold_path[BROWSER_ENGINE_PATH_LIMIT];
} BrowserFontConfig;

typedef struct {
    TilefinchDiagnosticCallback callback;
    void *opaque;
    TilefinchDiagnosticSeverity minimum_severity;
} BrowserDiagnosticConfig;

typedef struct {
    BrowserDeviceProfile device;
    /* Memory intentionally unavailable to page-owned allocations: PSP UI,
       thread stacks, TLS/backend state, sockets and fragmentation headroom. */
    size_t non_page_memory_reserve;
    size_t memory_limit;
    size_t history_capacity;
    size_t session_cache_limit;
    size_t maximum_document_bytes;
    long navigation_timeout_ms;
    NavigationReplacementMode navigation_replacement_mode;
    /* Optional authored CSS viewport override used by lab/device profiles.
       Zero selects the document/default viewport; otherwise both dimensions
       must be positive and bounded. */
    int declared_css_width;
    int declared_css_height;
    /* Present the authoritative first viewport before commit/history/UI
       setup. Admission remains bounded and, when the engine owns its tile
       cache, that raster work is promoted into the committed renderer. */
    bool progressive_first_paint;
    size_t tile_capacity;
    uint64_t idle_work_budget_us;
    size_t idle_work_maximum_units;
    BrowserJavaScriptConfig javascript;
    BrowserResourceConfig resources;
    BrowserFontConfig fonts;
    BrowserDiagnosticConfig diagnostics;
} BrowserConfig;

typedef enum {
    BROWSER_ENGINE_ACTIVE = 0,
    BROWSER_ENGINE_SHUTDOWN
} BrowserEngineState;

typedef struct {
    BrowserEngineState state;
    size_t control_bytes;
    size_t framebuffer_bytes;
    size_t non_page_memory_reserve;
    size_t budget_limit;
    size_t budget_current;
    size_t budget_peak;
    size_t budget_external_reserved;
    size_t budget_external_reserved_peak;
    size_t session_inline_accounting_bytes;
    size_t budget_active_allocations;
    size_t budget_largest_allocation;
    size_t allocation_failures;
    bool page_loaded;
    bool fonts_ready;
    bool controller_ready;
    bool render_ready;
    DocumentBackingKind backing_kind;
    size_t section_count;
    size_t current_section;
    size_t loads_started;
    size_t loads_committed;
    size_t loads_cancelled;
    size_t pages_destroyed;
    size_t controller_focus_moves;
    size_t controller_activations;
    size_t controller_text_edits;
    size_t rendered_frames;
    size_t unchanged_runtime_frames_suppressed;
    uint64_t maximum_render_us;
    size_t idle_jobs_scheduled;
    size_t idle_jobs_completed;
    size_t idle_jobs_cancelled;
    size_t idle_slices;
    size_t idle_slice_overruns;
    size_t idle_image_admission_skips;
    size_t idle_glyphs_prewarmed;
    size_t idle_glyph_cache_misses;
    uint64_t idle_work_us;
    uint64_t maximum_idle_slice_us;
    uint64_t maximum_idle_unit_us;
    size_t startup_visual_slices;
    uint64_t startup_visual_us;
    uint64_t maximum_startup_visual_slice_us;
    uint64_t maximum_startup_visual_unit_us;
    size_t diagnostics_emitted;
    size_t diagnostics_filtered;
    TilefinchDiagnosticSubsystem last_diagnostic_subsystem;
    TilefinchDiagnosticCode last_diagnostic_code;
    NavigationPerformance navigation;
} BrowserEngineMetrics;

typedef struct {
    size_t javascript_bytes;
    size_t session_cache_bytes;
    size_t render_cache_bytes;
    size_t total_bytes;
} BrowserOptionalMemoryReclaim;

typedef struct BrowserEngine BrowserEngine;

typedef enum {
    BROWSER_NAVIGATION_JOB_IDLE = 0,
    BROWSER_NAVIGATION_JOB_PENDING,
    BROWSER_NAVIGATION_JOB_SUCCEEDED,
    BROWSER_NAVIGATION_JOB_FAILED,
    BROWSER_NAVIGATION_JOB_CANCELLED
} BrowserNavigationJobStatus;

typedef struct {
    NavigationLoadQuota load;
} BrowserNavigationJobQuota;

typedef struct {
    BrowserNavigationJobStatus status;
    NavigationLoadMetrics load;
    /* Monotonic overall navigation completion, from 0 through 1000. It
       combines transport progress with explicit parse/build/commit phases. */
    size_t completion_per_mille;
    uint64_t elapsed_us;
    uint64_t navigation_session_started_us;
    uint64_t provisional_capture_started_us;
    uint64_t provisional_first_present_us;
    uint64_t maximum_transform_slice_us;
    uint64_t maximum_irreducible_unit_us;
    size_t pump_calls;
    size_t incumbent_pages_preserved;
    size_t provisional_paints;
    size_t provisional_scrolls;
    size_t provisional_frame_count;
    size_t provisional_bytes;
    int provisional_scroll_y;
    size_t transform_slices;
    size_t transform_quota_overruns;
    size_t irreducible_unit_overruns;
} BrowserNavigationJobMetrics;

typedef struct {
    const uint16_t *pixels;
    size_t pixel_count;
    size_t frame_count;
    size_t current_frame;
    int scroll_y;
    int maximum_scroll_y;
    bool ready;
} BrowserProvisionalViewport;

void browser_device_profile_psp3000(BrowserDeviceProfile *profile);
void browser_config_init(BrowserConfig *config,
                         const BrowserDeviceProfile *profile);
bool browser_config_apply_psp_memory_profile(
    BrowserConfig *config, BrowserPspMemoryProfile profile);
bool browser_config_set_font_paths(
    BrowserConfig *config, const char *sans_path, const char *serif_path,
    const char *sans_italic_path, const char *sans_bold_path,
    const char *serif_bold_path, const char *metric_sans_path,
    const char *metric_sans_bold_path, size_t maximum_total_bytes);
bool browser_config_validate(const BrowserConfig *config,
                             char *error, size_t error_capacity);

/*
 * BrowserEngine is heap-resident so its fixed control block never consumes a
 * PSP thread stack.  The content budget begins after that fixed control block
 * and accounts for the framebuffer, session, page, fonts, and render caches.
 * Lexbor's allocator installation is process-global, so only one active
 * BrowserEngine may exist. A second create fails until the first engine has
 * shut down; shutdown releases that process-global ownership.
 */
BrowserEngine *browser_engine_create(const BrowserConfig *config,
                                     char *error, size_t error_capacity);
bool browser_engine_shutdown(BrowserEngine *engine);
void browser_engine_destroy(BrowserEngine *engine);

const BrowserConfig *browser_engine_config(const BrowserEngine *engine);
/* Changes the global author-script policy for subsequent navigations. The
   frontend reloads the active page after changing it. */
bool browser_engine_set_javascript_enabled(
    BrowserEngine *engine, bool enabled);
const char *browser_engine_last_error(const BrowserEngine *engine);
TilefinchDiagnosticCode browser_engine_last_diagnostic_code(
    const BrowserEngine *engine);
bool browser_engine_metrics(const BrowserEngine *engine,
                            BrowserEngineMetrics *metrics);
bool browser_engine_content_blocker_configure(
    BrowserEngine *engine, ContentBlockerMode mode,
    const char *custom_path);
bool browser_engine_content_blocker_set_allowed_sites(
    BrowserEngine *engine, const char *const *sites, size_t count);
bool browser_engine_content_blocker_metrics(
    const BrowserEngine *engine, ContentBlockerMetrics *metrics);

bool browser_engine_commit_html(BrowserEngine *engine, const char *url,
                                const char *html, size_t html_length,
                                bool record_history);
bool browser_engine_load_url(BrowserEngine *engine, const char *url,
                             bool record_history);
bool browser_engine_load_url_with_limits(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history);
/*
 * Responsive URL/action/history navigation. The incumbent page and its
 * controller remain usable until the candidate's no-fail commit boundary.
 * Each pump performs at most one fetch/parser slice or one transactional
 * finalization phase.
 */
bool browser_engine_begin_navigation_url(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history);
bool browser_engine_begin_navigation_action(
    BrowserEngine *engine, const ControllerAction *action,
    size_t maximum_bytes, long timeout_ms);
bool browser_engine_begin_navigation_history(
    BrowserEngine *engine, bool forward, size_t maximum_bytes,
    long timeout_ms);
BrowserNavigationJobStatus browser_engine_pump_navigation(
    BrowserEngine *engine, const BrowserNavigationJobQuota *quota);
void browser_engine_cancel_navigation(BrowserEngine *engine,
                                      const char *reason);
/*
 * Retire transport work owned by the committed page while retaining each
 * cancellation result for its normal resource/runtime consumer. This is the
 * safe boundary before a platform tears down and recreates its network stack.
 */
size_t browser_engine_cancel_network_work(
    BrowserEngine *engine, const char *reason);
BrowserNavigationJobStatus browser_engine_navigation_status(
    const BrowserEngine *engine);
bool browser_engine_navigation_pending(const BrowserEngine *engine);
/* Borrows the in-flight destination until the job reaches a terminal state. */
const char *browser_engine_pending_navigation_url(
    const BrowserEngine *engine);
bool browser_engine_navigation_job_metrics(
    const BrowserEngine *engine, BrowserNavigationJobMetrics *metrics);
/*
 * A provisional viewport is an immutable raster snapshot, never candidate
 * DOM/controller state. It is available only while an asynchronous
 * navigation remains pending. Page-step scrolling selects another bounded
 * pre-rasterized snapshot; final commit carries only its clamped document
 * scroll coordinate into the authoritative page.
 */
bool browser_engine_provisional_viewport(
    const BrowserEngine *engine, BrowserProvisionalViewport *viewport);
bool browser_engine_scroll_provisional_page(
    BrowserEngine *engine, int direction);
bool browser_engine_history_move(BrowserEngine *engine, bool forward);
bool browser_engine_replace_history(
    BrowserEngine *engine, const NavigationHistoryRecord *records,
    size_t count, size_t current_index);
bool browser_engine_restore_view(
    BrowserEngine *engine, int scroll_y, int focus_kind,
    size_t focus_index);
bool browser_engine_execute_action(BrowserEngine *engine,
                                   const ControllerAction *action,
                                   size_t maximum_bytes,
                                   long timeout_ms);
/* Semantic input commands retain no queued input allocation. DOM mutation,
 * script dispatch, and relayout still use their ordinary bounded page budget.
 * Device-space scroll and pointer coordinates use the configured framebuffer
 * dimensions. Activation returns a navigation/form action for policy-aware
 * callers to inspect before passing it to browser_engine_execute_action(). */
bool browser_engine_focus_move(BrowserEngine *engine, bool forward);
bool browser_engine_focus_direction(BrowserEngine *engine,
                                    ControllerFocusDirection direction);
bool browser_engine_focus_at(BrowserEngine *engine, int x, int y);
bool browser_engine_pointer_event(BrowserEngine *engine,
                                  ControllerPointerPhase phase,
                                  int x, int y, bool *activate,
                                  bool *page_changed);
void browser_engine_pointer_discard_click(BrowserEngine *engine);
bool browser_engine_pointer_commit_click(BrowserEngine *engine);
bool browser_engine_focus_node(BrowserEngine *engine, lxb_dom_node_t *node);
bool browser_engine_scroll_by(BrowserEngine *engine, int delta_y);
bool browser_engine_scroll_settle(BrowserEngine *engine);
LayoutCursor browser_engine_pointer_cursor(const BrowserEngine *engine);
LayoutScrollbarWidth browser_engine_root_scrollbar_width(
    const BrowserEngine *engine);
bool browser_engine_scroll_step(BrowserEngine *engine, int direction,
                                unsigned held_frames);
bool browser_engine_scroll_page(BrowserEngine *engine, int direction);
bool browser_engine_scroll_to_edge(BrowserEngine *engine, bool bottom);

#define BROWSER_FIND_QUERY_LIMIT 96u
#define BROWSER_TEXT_ANCHOR_LIMIT 64u
typedef struct {
    char query[BROWSER_FIND_QUERY_LIMIT + 1u];
    size_t match_count;
    size_t selected;
    bool truncated;
    bool wrapped;
} BrowserFindSnapshot;

/* Find operates on the retained visible-text display list. A query performs
   one bounded index build; next/previous are O(1), scroll to the match with
   context, and leave layout/DOM untouched. */
bool browser_engine_find_begin(BrowserEngine *engine, const char *query,
                               BrowserFindSnapshot *snapshot);
bool browser_engine_find_move(BrowserEngine *engine, int direction,
                              BrowserFindSnapshot *snapshot);
bool browser_engine_find_snapshot(BrowserEngine *engine,
                                  BrowserFindSnapshot *snapshot);
bool browser_engine_capture_text_anchor(
    const BrowserEngine *engine,
    char anchor[BROWSER_TEXT_ANCHOR_LIMIT + 1u], int *document_y);
bool browser_engine_restore_text_anchor(
    BrowserEngine *engine, const char *anchor, int nearest_y);
void browser_engine_find_clear(BrowserEngine *engine);
bool browser_engine_text_value(const BrowserEngine *engine, char *output,
                               size_t capacity, size_t *length);
bool browser_engine_text_input_info(
    const BrowserEngine *engine, ControllerTextInputInfo *info);
bool browser_engine_replace_text(BrowserEngine *engine, const char *utf8,
                                 size_t length);
bool browser_engine_insert_text(BrowserEngine *engine, const char *utf8,
                                size_t length);
bool browser_engine_backspace(BrowserEngine *engine);
bool browser_engine_activate(BrowserEngine *engine,
                             ControllerAction *action);
/* Page media is consumed by a platform player rather than by navigation.
   The facade keeps DOM/CSP ownership on the engine side and exposes only an
   already-resolved, bounded request plus state feedback. */
bool browser_engine_consume_media_request(
    BrowserEngine *engine, ScriptMediaRequest *request);
bool browser_engine_update_media_state(
    BrowserEngine *engine, int64_t node_handle, ScriptMediaState state,
    double current_time, double duration);
/*
 * Advances page callbacks and commits any resulting layout. The optional
 * output reports whether the committed display list has visible damage, not
 * merely whether a layout generation advanced; callers can therefore avoid
 * presenting an unchanged frame after a no-op style/layout mutation.
 */
bool browser_engine_advance_runtime(BrowserEngine *engine,
                                    unsigned elapsed_ms,
                                    size_t maximum_callbacks,
                                    bool *visible_layout_changed);
/*
 * Commits one already-materialized compressed section and keeps the facade's
 * backing, controller, and render shell synchronized with the navigation
 * page.  When selection is non-NULL, the facade commits it only after the
 * candidate document succeeds; on failure it remains active for the caller
 * to abort or use while restoring the previous section.
 */
bool browser_engine_commit_section_html(
    BrowserEngine *engine, DocumentBacking *backing,
    DocumentBackingSelection *selection, const char *url,
    const char *html, size_t html_length, bool preserve_runtime,
    bool record_history);
bool browser_engine_refresh_shell(BrowserEngine *engine);
/*
 * Rebuild only the engine-owned render shell after a caller performs a
 * transitional direct relayout. Controller focus and interaction state are
 * preserved. New engine commands should normally refresh both sides
 * internally instead.
 */
bool browser_engine_refresh_render_shell(BrowserEngine *engine);
/*
 * Apply the committed navigation session's latest bounded relayout-damage
 * rectangle to the engine-owned render shell without discarding unaffected
 * tiles.
 */
bool browser_engine_apply_layout_damage(BrowserEngine *engine);
/*
 * Select forced-dark page painting without altering authored image/video/
 * canvas pixels. The choice persists across navigation render-shell swaps.
 */
bool browser_engine_set_forced_dark(BrowserEngine *engine, bool enabled);
/* Full invalidation after the application-level provider changes. */
bool browser_engine_optional_glyphs_updated(BrowserEngine *engine);
/* Repaint after one deferred block read without evicting glyphs that are
   already independent of the provider's small block cache. */
bool browser_engine_optional_glyph_payloads_ready(BrowserEngine *engine);
/* Visible Unicode-script hints gathered without a second DOM walk. The
   frontend may use these to attach installed optional glyph packs lazily. */
uint8_t browser_engine_glyph_script_mask(const BrowserEngine *engine);
/* Choose the generated YouTube search-result density. This is an engine
   preference, not a query parameter exposed by the provider page. */
bool browser_engine_set_youtube_compact_results(
    BrowserEngine *engine, bool compact);
bool browser_engine_render_frame(BrowserEngine *engine,
                                 const char *optional_ppm_path);
typedef enum {
    BROWSER_RENDER_JOB_CANCELLED = -2,
    BROWSER_RENDER_JOB_FAILED = -1,
    BROWSER_RENDER_JOB_PENDING = 0,
    BROWSER_RENDER_JOB_COMPLETE = 1
} BrowserRenderJobStatus;
/*
 * Prepare at most maximum_units expensive tiles and publish the completed
 * frame only after all selected resident tiles are ready. Repeated calls are
 * latest-wins when scroll or layout state changes.
 */
BrowserRenderJobStatus browser_engine_render_frame_bounded(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units);
/*
 * Cancellation is cooperative at tile/compositor boundaries. Callers that
 * receive CANCELLED should retain their last presented surface even though a
 * just-completed compositor may have updated the engine-owned candidate.
 */
BrowserRenderJobStatus browser_engine_render_frame_bounded_cancelable(
    BrowserEngine *engine, uint64_t budget_us, size_t maximum_units,
    const TilefinchCancellation *cancellation);
void browser_engine_cancel_render_job(BrowserEngine *engine);
bool browser_engine_run_idle_work(BrowserEngine *engine);
void browser_engine_cancel_idle_work(BrowserEngine *engine);
/*
 * Make room for a short-lived device service such as voice recognition.
 * This preserves the committed document, layout, script realm, controller,
 * focus, scroll position, resident tiles, and presented framebuffer.
 */
bool browser_engine_reclaim_optional_memory(
    BrowserEngine *engine, BrowserOptionalMemoryReclaim *reclaim);
bool browser_engine_bind_document_backing(BrowserEngine *engine,
                                          const DocumentBacking *backing);
const DocumentBacking *browser_engine_document_backing(
    const BrowserEngine *engine);

/*
 * Stable read-only frontend view. Strings and geometry are copied so a UI
 * never retains engine-owned pointers across a navigation commit.
 */
typedef struct {
    char url[NAVIGATION_URL_LIMIT];
    char title[256];
    bool secure;
    bool can_go_back;
    bool can_go_forward;
    bool has_focus;
    bool loading;
    int scroll_y;
    int maximum_scroll_y;
    int focus_x;
    int focus_y;
    int focus_width;
    int focus_height;
    uint64_t navigation_generation;
} BrowserViewSnapshot;

bool browser_engine_view_snapshot(
    const BrowserEngine *engine, BrowserViewSnapshot *snapshot);

typedef struct {
    const uint16_t *pixels;
    size_t pixel_count;
    int width;
    int height;
    int stride;
} BrowserFrameView;

/* One frontend sampling boundary for engine-owned views. Every member expires
   together at the next engine mutation; callers refresh the whole snapshot
   rather than retaining independently sampled navigation/controller state. */
typedef struct {
    BrowserFrameView frame;
    const NavigationSession *navigation;
    const BrowserController *controller;
} BrowserFrontendSnapshot;

bool browser_engine_frame_view(
    const BrowserEngine *engine, BrowserFrameView *view);
bool browser_engine_frontend_snapshot(
    const BrowserEngine *engine, BrowserFrontendSnapshot *snapshot);
bool browser_engine_fill_frame(BrowserEngine *engine, uint16_t color);
bool browser_engine_set_user_css(
    BrowserEngine *engine, const char *css, size_t length);
bool browser_engine_apply_user_css(
    BrowserEngine *engine, const char *css, size_t length);
bool browser_engine_prepare_reader(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis);
void browser_engine_set_reader_candidate_mode(BrowserEngine *engine,
                                              bool enabled);
bool browser_engine_reader_analysis(
    const BrowserEngine *engine, ReaderDocumentAnalysis *analysis);

/*
 * Read-only transitional views. They expire at the next engine mutation and
 * exist for diagnostics/policy migration; frontends must not retain them.
 */
const NavigationSession *browser_engine_navigation_view(
    const BrowserEngine *engine);
const BrowserController *browser_engine_controller_view(
    const BrowserEngine *engine);
const FontFace *browser_engine_font_face(
    const BrowserEngine *engine, FontFamily family);
const FontFace *browser_engine_font_face_variant(
    const BrowserEngine *engine, FontFamily family, bool italic, bool bold);
const TileCache *browser_engine_render_metrics_view(
    const BrowserEngine *engine);

/*
 * Deliberately shared service state. Device media and voice reservations use
 * the same bounded envelope as the page; this is not a presentation API.
 */
Budget *browser_engine_budget(BrowserEngine *engine);
BrowserSession *browser_engine_session(BrowserEngine *engine);
const BrowserSession *browser_engine_session_view(const BrowserEngine *engine);

/* Transitional mutable accessors retained for tests and unmigrated tools. */
NavigationSession *browser_engine_navigation(BrowserEngine *engine);
BrowserController *browser_engine_controller(BrowserEngine *engine);
FontSet *browser_engine_fonts(BrowserEngine *engine);
TileCache *browser_engine_render_shell(BrowserEngine *engine);
uint16_t *browser_engine_framebuffer(BrowserEngine *engine,
                                     size_t *pixel_count);

#endif
