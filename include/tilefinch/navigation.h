#ifndef TILEFINCH_NAVIGATION_H
#define TILEFINCH_NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/reader_mode.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/layout.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/resources.h"
#include "tilefinch/section_router.h"
#include "tilefinch/style.h"

#define NAVIGATION_URL_LIMIT 2048
#define NAVIGATION_TITLE_LIMIT 256
#define NAVIGATION_FRAME_LIMIT 2
#define NAVIGATION_FRAME_DISCOVERY_LIMIT 8
#define NAVIGATION_MESSAGE_LIMIT 16
#define NAVIGATION_STYLESHEET_EVENT_LIMIT 32
#define NAVIGATION_BLOCKING_SCRIPT_SAMPLE_LIMIT 24
/* Resource, frame, and asynchronous page-runtime scheduler views share this
   cap. The independent top-level NavigationLoad transport adds at most one
   transient slot while a document is being fetched. */
#define NAVIGATION_PAGE_FETCH_SLOT_LIMIT 16

typedef struct {
    uint64_t total_us;
    uint64_t max_us;
    size_t slices;
    size_t work_units;
    size_t max_work_units;
    size_t cooperative_yields;
    size_t restarts;
    size_t discarded_stale;
} NavigationSliceStats;

typedef enum {
    NAVIGATION_SLICE_NETWORK = 0,
    NAVIGATION_SLICE_PARSER,
    NAVIGATION_SLICE_SCRIPT,
    NAVIGATION_SLICE_STYLE,
    NAVIGATION_SLICE_RESOURCE,
    NAVIGATION_SLICE_LAYOUT,
    NAVIGATION_SLICE_PAINT,
    NAVIGATION_SLICE_RUNTIME,
    NAVIGATION_SLICE_TEARDOWN,
    NAVIGATION_SLICE_COUNT
} NavigationSlicePhase;

typedef struct {
    PocDocument document;
    ScriptRuntime *runtime;
    ScriptResult script_result;
    lxb_dom_node_t *element;
    long parent_handle;
    char parent_url[NAVIGATION_URL_LIMIT];
    char url[NAVIGATION_URL_LIMIT];
    char referrer_policy[128];
    long http_status;
    char server[64];
    char cf_ray[64];
    char capability_trace[2048];
    char reject_source_context[3072];
    char callback_error_source_context[3072];
    uint64_t fetch_request_id;
    uint32_t sandbox_flags;
    uint64_t lifecycle_generation;
    bool fetch_pending;
    bool client_hint_retried;
    bool loaded;
    bool retired;
    bool opaque_origin;
} NavigationFrame;

typedef struct {
    ScriptRuntime *source;
    uint64_t sequence;
    uint64_t source_generation;
    uint64_t target_generation;
    long source_frame_handle;
    long target_frame_handle;
    char source_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char *json;
    char *target_origin;
} NavigationMessage;

typedef void (*NavigationScriptFrontierCallback)(
    void *opaque, size_t script_index, bool active);

typedef struct {
    char *url;
    char *title;
    int scroll_y;
    int focus_kind;
    /* Semantic ordinal within focus_kind; independent of inline fragments. */
    size_t focus_index;
} NavigationEntry;

typedef struct {
    const char *url;
    const char *title;
    int scroll_y;
    int focus_kind;
    size_t focus_index;
} NavigationHistoryRecord;

typedef struct {
    lxb_dom_node_t *element;
    uint64_t resolved_url_hash;
} NavigationStylesheetEventRecord;

typedef struct {
    PocDocument document;
    ReaderDocumentAnalysis reader_analysis;
    Stylesheet stylesheet;
    StylesheetDocumentResources stylesheet_resources;
    ExternalStylesheetStats external_stylesheets;
    ExternalFontStats external_fonts;
    ExternalFontLoader external_font_loader;
    ImageResources images;
    LayoutDocument layout;
    LayoutReuseCache *layout_reuse;
    char referrer_policy[128];
    char resource_base_url[NAVIGATION_URL_LIMIT];
    /* Full document URL of the committed page, retained so a
       resource-driven style rebuild can re-request external CSS, images,
       and fonts without depending on a history entry being present. */
    char document_url[NAVIGATION_URL_LIMIT];
    ScriptRuntime *runtime;
    ScriptResult script_result;
    FetchSchedulerDomain *fetch_domain;
    FetchScheduler *resource_scheduler;
    /* Optional visual resources continue only from owner idle ticks. The
       legacy full-document continuation is stateless; simple static pages
       additionally retain a bounded viewport-ordered document-image queue
       whose one active request is externally pumped. */
    bool image_continuation_pending;
    ImagePriorityTarget *deferred_image_targets;
    size_t deferred_image_count;
    size_t deferred_image_cursor;
    ImagePriorityLoadJob *deferred_image_job;
    bool loaded;
    NavigationFrame frames[NAVIGATION_FRAME_LIMIT];
    size_t frame_count;
    lxb_dom_node_t *discovered_frame_elements[NAVIGATION_FRAME_DISCOVERY_LIMIT];
    size_t discovered_frame_element_count;
    NavigationStylesheetEventRecord stylesheet_events[
        NAVIGATION_STYLESHEET_EVENT_LIMIT];
    size_t stylesheet_event_count;
    NavigationMessage messages[NAVIGATION_MESSAGE_LIMIT];
    size_t message_count;
    uint64_t compiled_stylesheet_fingerprint;
    uint64_t compiled_stylesheet_resource_signature;
    uint64_t compiled_stylesheet_build_generation;
    bool compiled_stylesheet_cacheable;
} NavigationPage;

typedef struct NavigationSession NavigationSession;

/* Cross-document URL loads are transactional by default.  The explicit
   low-memory mode trades rollback after incumbent retirement for a lower
   peak: the response is downloaded first, the visible frame may be frozen by
   an owner hook, and only then is the incumbent page retired before parsing
   or starting a candidate runtime. */
typedef enum {
    NAVIGATION_REPLACEMENT_TRANSACTIONAL = 0,
    NAVIGATION_REPLACEMENT_LOW_MEMORY
} NavigationReplacementMode;

/* Both replacement hooks run at no-allocation boundaries.  freeze is called
   while the incumbent page is still valid and should only retain already
   provisioned visual state; it returns whether a frozen frame is available.
   outcome is called after all failed candidate staging has been released, or
   after a successful candidate has been published. */
typedef bool (*NavigationReplacementFreezeCallback)(
    void *opaque, const NavigationSession *incumbent);
typedef void (*NavigationReplacementOutcomeCallback)(
    void *opaque, const NavigationSession *session, bool success);

/* Optional owner hook for fallible state which must be staged against a
   candidate page before that page becomes observable.  prepare may allocate
   and must leave abort able to release any partial staging even when prepare
   returns false.  commit is called only after every remaining fallible commit
   step has succeeded and therefore must not allocate or fail. */
typedef bool (*NavigationCandidatePrepareCallback)(
    void *opaque, NavigationSession *candidate);
typedef void (*NavigationCandidateFinalizeCallback)(void *opaque);

/* Optional best-effort presentation before commit/history/UI setup. The
   layout may be a bounded top-of-document preview; it is owned by navigation
   and valid only during the callback. Returning false does not fail
   navigation. If a presented candidate subsequently fails, the callback is
   invoked once with a NULL layout to roll back that provisional frame. */
typedef bool (*NavigationProgressivePaintCallback)(
    void *opaque, NavigationSession *candidate,
    const LayoutDocument *layout);

typedef struct {
    size_t ordinal;
    size_t node_count;
    size_t source_bytes;
    size_t dom_mutations;
    uint64_t total_us;
    uint64_t metadata_us;
    uint64_t runtime_startup_us;
    uint64_t stylesheet_us;
    uint64_t stylesheet_fingerprint_us;
    uint64_t process_us;
    uint64_t compile_us;
    uint64_t host_callback_us;
    /* Remaining loader/evaluation boundary after compilation. For an
       external script this can also include bounded fetch and admission. */
    uint64_t execute_us;
    bool external;
    bool succeeded;
} NavigationBlockingScriptSample;

typedef struct {
    uint64_t network_us;
    uint64_t parse_us;
    uint64_t parser_feed_us;
    uint64_t parser_callback_us;
    uint64_t parser_metadata_us;
    uint64_t parser_runtime_startup_us;
    uint64_t parser_stylesheet_us;
    uint64_t parser_script_us;
    uint64_t parser_script_compile_us;
    uint64_t parser_script_execute_us;
    uint64_t parser_script_rescan_us;
    size_t parser_script_mutations;
    NavigationBlockingScriptSample blocking_script_samples[
        NAVIGATION_BLOCKING_SCRIPT_SAMPLE_LIMIT];
    size_t blocking_script_sample_count;
    size_t blocking_script_samples_dropped;
    uint64_t parser_eof_metadata_us;
    uint64_t parser_eof_preload_us;
    uint64_t parser_finish_us;
    uint64_t script_us;
    uint64_t style_us;
    uint64_t resource_us;
    uint64_t stylesheet_resource_us;
    uint64_t image_resource_us;
    uint64_t font_resource_us;
    uint64_t resource_fingerprint_us;
    uint64_t layout_us;
    uint64_t final_layout_us;
    uint64_t relayout_us;
    uint64_t runtime_us;
    size_t fast_relayouts;
    size_t full_relayouts;
    size_t resource_fingerprint_scans;
    size_t blocking_stylesheet_builds;
    size_t blocking_stylesheet_reuses;
    size_t blocking_stylesheet_adoptions;
    size_t blocking_stylesheet_final_reuses;
    size_t compiled_stylesheet_cache_hits;
    size_t compiled_stylesheet_cache_misses;
    size_t compiled_stylesheet_cache_stores;
    size_t compiled_stylesheet_cache_evictions;
    size_t compiled_stylesheet_cache_retained_bytes;
    size_t compiled_stylesheet_cache_unmarked_skips;
    size_t compiled_stylesheet_cache_generation_skips;
    size_t compiled_stylesheet_cache_size_skips;
    size_t compiled_stylesheet_cache_pressure_skips;
    uint64_t compiled_stylesheet_cache_last_stored_signature;
    uint64_t compiled_stylesheet_cache_last_observed_signature;
    uint64_t compiled_stylesheet_cache_last_stored_fingerprint;
    uint64_t compiled_stylesheet_cache_last_observed_fingerprint;
    size_t blocking_stylesheet_continuations;
    size_t blocking_stylesheet_continuation_fallbacks;
    size_t blocking_stylesheet_continuation_rules;
    size_t blocking_stylesheet_continuation_prefix_inputs;
    size_t blocking_stylesheet_continuation_suffix_inputs;
    size_t blocking_stylesheet_continuation_rules_before;
    uint64_t blocking_stylesheet_continuation_us;
    uint64_t blocking_stylesheet_continuation_discovery_us;
    uint64_t blocking_stylesheet_continuation_context_us;
    uint64_t blocking_stylesheet_continuation_append_us;
    uint64_t blocking_stylesheet_fingerprint_us;
    size_t mutation_fast_relayouts;
    size_t mutation_resource_rebuilds;
    size_t mutation_image_resource_scans;
    size_t mutation_conservative_scans;
    size_t mutation_journal_overflows;
    size_t semantic_relayout_skips;
    size_t focus_outline_relayout_skips;
    size_t focus_paint_relayout_skips;
    size_t layout_reuse_style_hits;
    size_t layout_reuse_style_misses;
    size_t layout_reuse_intrinsic_hits;
    size_t layout_reuse_intrinsic_misses;
    size_t layout_reuse_table_row_hits;
    size_t layout_reuse_table_row_misses;
    size_t layout_reuse_scoped_invalidations;
    size_t layout_reuse_full_resets;
    size_t layout_reuse_pressure_evictions;
    size_t layout_reuse_retained_bytes;
    uint64_t response_headers_us;
    uint64_t first_body_byte_us;
    size_t stylesheet_preload_timeout_bytes;
    size_t stylesheet_preload_timeout_requests;
    size_t stylesheet_preload_timeout_responses;
    size_t stylesheet_preload_failure_stage;
    size_t stylesheet_preload_failure_bytes;
    long stylesheet_preload_failure_status;
    char stylesheet_preload_failure_error[96];
    size_t stylesheet_preload_completed_bytes;
    uint64_t stylesheet_preload_last_completed_us;
    uint64_t first_dom_us;
    uint64_t first_layout_us;
    uint64_t first_paint_us;
    uint64_t completion_us;
    size_t partial_layouts;
    size_t partial_paints;
    size_t progressive_paint_rollbacks;
    uint64_t progressive_layout_us;
    uint64_t progressive_paint_us;
    size_t progressive_layout_attempts;
    size_t progressive_layout_skips;
    size_t progressive_layout_failures;
    size_t progressive_layout_adoptions;
    size_t progressive_visual_readiness_skips;
    size_t streaming_preview_checks;
    size_t streaming_preview_attempts;
    size_t streaming_preview_paints;
    size_t streaming_preview_pre_script_checks;
    size_t streaming_preview_pre_script_paints;
    size_t streaming_preview_visibility_checks;
    size_t streaming_preview_visibility_skips;
    size_t streaming_preview_visibility_nodes;
    size_t streaming_preview_style_mismatches;
    size_t streaming_preview_style_refresh_attempts;
    size_t streaming_preview_style_refreshes;
    size_t streaming_preview_style_refresh_failures;
    size_t streaming_preview_partial_stylesheet_paints;
    size_t streaming_preview_pressure_skips;
    size_t streaming_preview_visual_readiness_skips;
    size_t streaming_preview_empty_raster_skips;
    uint64_t streaming_preview_first_attempt_us;
    uint64_t streaming_preview_last_attempt_us;
    uint64_t streaming_preview_content_ready_us;
    size_t streaming_preview_source_bytes;
    size_t streaming_preview_node_count;
    uint64_t streaming_preview_layout_us;
    uint64_t streaming_preview_paint_us;
    uint64_t streaming_preview_frame_alloc_us;
    uint64_t streaming_preview_cache_init_us;
    uint64_t streaming_preview_raster_us;
    uint64_t streaming_preview_present_us;
    uint64_t streaming_preview_style_refresh_us;
    size_t progressive_preview_commands;
    size_t progressive_preview_node_boxes;
    size_t progressive_image_priority_nodes;
    size_t progressive_image_priority_loaded;
    size_t markup_image_priority_nodes;
    size_t stylesheet_pressure_fallbacks;
    size_t image_pressure_fallbacks;
    size_t background_image_batches;
    size_t background_images_loaded;
    size_t background_image_relayouts;
    size_t background_image_failures;
    size_t background_font_slices;
    size_t background_fonts_loaded;
    size_t background_font_relayouts;
    size_t background_font_failures;
    int progressive_preview_y_limit;
    size_t progressive_decoded_image_builds;
    size_t progressive_scaled_image_builds;
    size_t staged_body_slides_avoided;
    size_t staged_body_slide_bytes_avoided;
    size_t staged_body_compactions;
    size_t staged_body_compaction_bytes;
    uint64_t max_slice_us;
    size_t max_slice_work_units;
    NavigationSlicePhase max_slice_phase;
    NavigationSliceStats slices[NAVIGATION_SLICE_COUNT];
    uint64_t last_swap_teardown_us;
    uint64_t last_swap_gc_us;
    uint64_t last_swap_parse_us;
    uint64_t last_swap_commit_us;
    size_t swap_gc_runs;
    size_t swap_gc_skips;
    size_t swap_gc_trimmed_bytes;
    size_t swaps_since_gc;
    size_t low_memory_replacements_started;
    size_t low_memory_replacements_succeeded;
    size_t low_memory_replacements_failed;
    size_t low_memory_transactional_fallbacks;
    size_t low_memory_last_released_bytes;
    size_t low_memory_peak_released_bytes;
    bool low_memory_last_rollback_available;
    bool low_memory_last_frozen_frame;
} NavigationPerformance;

struct NavigationSession {
    Budget *budget;
    NavigationEntry *history;
    size_t history_capacity;
    size_t history_count;
    size_t history_index;
    NavigationPage page;
    /* One exact compiled sheet retained in RAM between same-site
       navigations. It is deliberately outside BrowserSession's persistent
       HTTP cache and therefore can never reach the Memory Stick. */
    Stylesheet compiled_stylesheet_cache;
    ExternalStylesheetStats compiled_stylesheet_cache_external_stats;
    uint64_t compiled_stylesheet_cache_fingerprint;
    uint64_t compiled_stylesheet_cache_resource_signature;
    size_t compiled_stylesheet_cache_retained_bytes;
    bool compiled_stylesheet_cache_ready;
    uint64_t generation;
    bool cancelled;
    size_t loads_started;
    size_t loads_committed;
    size_t loads_cancelled;
    size_t page_destroys;
    size_t history_pruned;
    bool scripts_enabled;
    size_t js_memory_limit;
    unsigned js_timeout_ms;
    ScriptExecutionPolicy script_execution_policy;
    ViewportContext viewport;
    /* Optional declared layout viewport (the fidelity oracle's emulated
       CSS viewport).  When nonzero it overrides the document's
       meta-derived resolution; the device frame keeps its size. */
    int declared_css_width;
    int declared_css_height;
    const FontSet *fonts;
    const ImageResources *images;
    bool external_resources_enabled;
    size_t maximum_stylesheets;
    size_t maximum_stylesheet_bytes;
    size_t maximum_stylesheet_file_bytes;
    bool web_fonts_enabled;
    size_t maximum_font_attempts;
    size_t maximum_font_bytes;
    size_t maximum_font_file_bytes;
    size_t maximum_font_face_backend_bytes;
    long font_timeout_ms;
    size_t maximum_images;
    size_t maximum_image_bytes;
    size_t maximum_image_file_bytes;
    size_t maximum_decoded_image_bytes;
    long resource_timeout_ms;
    size_t incremental_relayouts;
    NavigationPerformance performance;
    FetchStreamMetrics last_stream;
    FetchStreamOptions stream_delivery;
    unsigned progressive_preview_lookahead_percent;
    uint64_t resource_fingerprint;
    bool style_rebuild_required;
    bool relayout_damage_valid;
    int relayout_damage_left;
    int relayout_damage_top;
    int relayout_damage_right;
    int relayout_damage_bottom;
    /* A successful outline-only focus relayout gate already resolved the
       focused style. Preserve its compact paint result so the controller
       does not repeat the same bounded selector/style work before present. */
    lxb_dom_node_t *focus_outline_cache_node;
    uint64_t focus_outline_cache_stylesheet_generation;
    size_t focus_outline_cache_relayout_generation;
    uint32_t focus_outline_cache_color;
    int focus_outline_cache_offset;
    uint8_t focus_outline_cache_alpha;
    uint8_t focus_outline_cache_width;
    uint8_t focus_outline_cache_style;
    bool focus_outline_cache_valid;
    /* Paint-only focus changes mutate retained command colours in place.
       BrowserEngine mirrors those colours into a scaled visual clone before
       presenting and invalidates only this bounded damage union. */
    size_t focus_paint_generation;
    bool focus_paint_damage_valid;
    int focus_paint_damage_left;
    int focus_paint_damage_top;
    int focus_paint_damage_right;
    int focus_paint_damage_bottom;
    BrowserSession *browser_session;
    ScriptRemoteElementLookupCallback remote_element_lookup;
    void *remote_element_opaque;
    ScriptRemoteSelectorLookupCallback remote_selector_lookup;
    void *remote_selector_opaque;
    ScriptRemoteSelectorCollectCallback remote_selector_collect;
    void *remote_selector_collect_opaque;
    ScriptRemoteDescendantCollectCallback remote_descendant_collect;
    void *remote_descendant_collect_opaque;
    ScriptRemoteNodeReadCallback remote_node_read;
    void *remote_node_read_opaque;
    ScriptRemoteNodeWriteCallback remote_node_write;
    void *remote_node_write_opaque;
    ScriptNodeVisibilityCallback node_visibility;
    void *node_visibility_opaque;
    size_t runtime_section_identity;
    bool document_scripts_enabled;
    bool defer_document_script_pipeline;
    size_t maximum_scripts;
    size_t maximum_script_bytes;
    size_t maximum_script_file_bytes;
    long script_timeout_ms;
    size_t script_discovered;
    size_t script_attempted;
    size_t script_loaded;
    size_t script_failed;
    size_t script_skipped_cross_origin;
    size_t script_skipped_module;
    size_t script_skipped_quota;
    size_t script_skipped_pressure;
    size_t script_pressure_collections;
    size_t script_pressure_reclaimed_bytes;
    size_t script_pressure_capped_requests;
    size_t script_bytes;
    size_t script_cache_hits;
    size_t script_parser_blocking;
    size_t script_deferred;
    size_t script_asynchronous;
    size_t script_modules;
    size_t script_module_map_hits;
    size_t script_inline_data_fast_paths;
    size_t script_inline_data_fast_path_bytes;
    size_t script_inline_data_quota_exemptions;
    size_t script_cost_class_rejections;
    size_t script_watchdog_classification_misses;
    size_t script_watchdog_classification_miss_bytes;
    size_t script_watchdog_classification_miss_loops;
    unsigned script_watchdog_classification_miss_flags;
    size_t preloads_discovered;
    size_t preloads_launched;
    size_t preloads_completed;
    size_t preloads_cache_hits;
    size_t preloads_failed;
    size_t preloads_deferred;
    /* Launch sweeps stopped because the scheduler byte pool no longer held
       the headroom reserved for authoritative fetches. The preloads stay
       queued, so this is not a terminal outcome like preloads_deferred. */
    size_t preloads_headroom_skipped;
    long last_http_status;
    long last_transport_code;
    long last_tls_verify_result;
    bool last_transport_timed_out;
    bool last_tls12_compatibility_retry;
    char last_tls_version[16];
    size_t client_hint_retries;
    char last_cf_mitigated[32];
    char last_server[64];
    char last_error[512];
    char last_script_error_context[2048];
    char last_page_trace[2048];
    char previous_page_trace[2048];
    char last_frame_trace[2048];
    char last_frame_trace_url[NAVIGATION_URL_LIMIT];
    size_t frames_discovered;
    size_t frames_loaded;
    size_t frames_failed;
    size_t frame_messages_posted;
    size_t frame_messages_delivered;
    size_t frame_messages_to_parent;
    size_t frame_messages_to_child;
    size_t frame_messages_dropped;
    size_t frames_detached;
    uint64_t next_frame_message_sequence;
    uint64_t next_frame_lifecycle_generation;
    uint64_t last_frame_message_sequence;
    long last_frame_message_source;
    long last_frame_message_target;
    char last_frame_message_event[96];
    char frame_message_event_log[512];
    char last_frame_reject_reason[128];
    bool trace_frame_capabilities;
    bool trace_page_capabilities;
    bool diagnostic_frame_safari;
    char pending_navigation_referer[NAVIGATION_URL_LIMIT];
    bool pending_navigation_same_origin;
    char pending_response_referrer_policy[128];
    char *user_css;
    size_t user_css_length;
    NavigationCandidatePrepareCallback candidate_prepare;
    NavigationCandidateFinalizeCallback candidate_abort;
    NavigationCandidateFinalizeCallback candidate_commit;
    void *candidate_commit_opaque;
    NavigationProgressivePaintCallback progressive_paint;
    void *progressive_paint_opaque;
    bool progressive_paint_preserves_incumbent;
    /* Prepare content-shape Reader markers on a completed candidate DOM
       before its first authoritative stylesheet/layout pass. */
    bool prepare_reader_candidates;
    uint64_t navigation_started_us;
    NavigationReplacementMode replacement_mode;
    NavigationReplacementFreezeCallback replacement_freeze;
    NavigationReplacementOutcomeCallback replacement_outcome;
    void *replacement_hook_opaque;
    bool replacement_failure_overlay_active;
    char replacement_failure_message[256];
};

typedef struct NavigationLoad NavigationLoad;

typedef enum {
    NAVIGATION_LOAD_PENDING = 0,
    NAVIGATION_LOAD_READY_TO_FINISH,
    NAVIGATION_LOAD_FINALIZING,
    NAVIGATION_LOAD_SUCCEEDED,
    NAVIGATION_LOAD_FAILED,
    NAVIGATION_LOAD_CANCELLED
} NavigationLoadStatus;

typedef struct {
    FetchPumpQuota fetch;
    /* Response callbacks only stage bytes. Parsing consumes at most this many
       staged bytes in one navigation pump. Zero selects 64 KiB. */
    size_t maximum_parser_body_bytes;
    /* Parser callbacks and dependency APIs may contain non-resumable native
       calls. This is an honest overrun threshold, not a false preemption
       claim. Zero selects 10 ms. */
    uint64_t maximum_parser_time_us;
    /* Final parse/style/layout remains transactional. These limits are
       measured honestly; an overrun is reported rather than exposing a
       half-committed page. Zero selects no advisory ceiling. */
    size_t maximum_finalize_work_units;
    uint64_t maximum_finalize_time_us;
} NavigationLoadQuota;

typedef struct {
    size_t pump_calls;
    size_t quota_yields;
    size_t completion_per_mille;
    size_t body_bytes;
    size_t body_callbacks;
    size_t peak_buffered_bytes;
    uint64_t total_pump_us;
    uint64_t maximum_pump_us;
    size_t staged_body_bytes;
    size_t maximum_staged_body_bytes;
    uint64_t parser_pump_us;
    uint64_t maximum_parser_pump_us;
    size_t parser_quota_yields;
    bool parser_time_quota_exceeded;
    uint64_t finalize_us;
    size_t finalize_work_units;
    size_t finalize_slices;
    size_t finalize_yields;
    size_t finalize_phase;
    bool finalize_quota_exceeded;
    size_t safe_sections_ready;
    size_t safe_section_source_bytes;
    FetchStreamMetrics stream;
} NavigationLoadMetrics;

bool navigation_init(NavigationSession *session, Budget *budget,
                     size_t history_capacity);
void navigation_enable_scripts(NavigationSession *session,
                               size_t js_memory_limit,
                               unsigned timeout_ms);
/* Applies to subsequently-created page and frame runtimes.  To avoid a
   partially-applied policy, changing it while a page runtime is live fails. */
bool navigation_set_script_execution_policy(
    NavigationSession *session, const ScriptExecutionPolicy *policy);
void navigation_attach_browser_session(NavigationSession *session,
                                       BrowserSession *browser_session);
/* Installs one complete prepare/abort/commit set, or clears it when all three
   callbacks are NULL. Partial hook sets are rejected. */
bool navigation_set_candidate_commit_hooks(
    NavigationSession *session, NavigationCandidatePrepareCallback prepare,
    NavigationCandidateFinalizeCallback abort,
    NavigationCandidateFinalizeCallback commit, void *opaque);
/* Installs or clears a best-effort first-viewport preview.  It is attempted
   only for streamed documents with deferred visual resources and sufficient
   measured budget headroom. */
bool navigation_set_progressive_paint_hook(
    NavigationSession *session, NavigationProgressivePaintCallback paint,
    void *opaque);
/* Bounded CSS-height lookahead used only by a transient streaming preview.
   Fifty preserves the subsystem default; BrowserEngine uses one additional
   viewport so a handheld can expose one provisional page-down snapshot. */
bool navigation_set_progressive_preview_lookahead(
    NavigationSession *session, unsigned percent);
/* Declares that the installed paint callback uses separate snapshot storage
   and cannot overwrite or retain pointers into the incumbent page. */
void navigation_set_progressive_paint_preserves_incumbent(
    NavigationSession *session, bool preserves);
/* This is never enabled implicitly. It affects only subsequent streamed,
   non-sectioned cross-document URL loads which replace a live page. */
bool navigation_set_replacement_mode(
    NavigationSession *session, NavigationReplacementMode mode);
/* Installs or clears the complete optional visual freeze/outcome pair.
   Supplying only one callback is rejected. */
bool navigation_set_replacement_hooks(
    NavigationSession *session, NavigationReplacementFreezeCallback freeze,
    NavigationReplacementOutcomeCallback outcome, void *opaque);
/* Replaces the complete remote-document contract and immediately rebinds the
   already-live top-level runtime. NULL clears callbacks, opaque values,
   section identity, and pending remote materialization requests together.
   Child-frame runtimes are deliberately outside this scope. */
bool navigation_rebind_top_level_remote_document(
    NavigationSession *session,
    const ScriptRemoteDocumentBinding *binding);
void navigation_set_remote_element_lookup(
    NavigationSession *session, ScriptRemoteElementLookupCallback callback,
    void *opaque);
void navigation_set_remote_selector_lookup(
    NavigationSession *session, ScriptRemoteSelectorLookupCallback callback,
    void *opaque);
void navigation_set_remote_selector_collect(
    NavigationSession *session, ScriptRemoteSelectorCollectCallback callback,
    void *opaque);
void navigation_set_remote_descendant_collect(
    NavigationSession *session,
    ScriptRemoteDescendantCollectCallback callback, void *opaque);
void navigation_set_remote_node_read(
    NavigationSession *session, ScriptRemoteNodeReadCallback callback,
    void *opaque);
void navigation_set_remote_node_write(
    NavigationSession *session, ScriptRemoteNodeWriteCallback callback,
    void *opaque);
void navigation_set_node_visibility(
    NavigationSession *session, ScriptNodeVisibilityCallback callback,
    void *opaque);
bool navigation_set_runtime_section_identity(NavigationSession *session,
                                             size_t section_identity);
void navigation_enable_document_scripts(
    NavigationSession *session, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes,
    long timeout_ms);
void navigation_enable_external_resources(
    NavigationSession *session, size_t maximum_stylesheets,
    size_t maximum_stylesheet_bytes, size_t maximum_stylesheet_file_bytes,
    size_t maximum_images, size_t maximum_image_bytes,
    size_t maximum_image_file_bytes, size_t maximum_decoded_image_bytes,
    long timeout_ms);
/* Page fonts are separately bounded and opt-in. This keeps the established
   external stylesheet/image API stable for embedders which do not need a
   downloadable-font backend. Failed or unsupported faces remain a normal CSS
   fallback rather than failing navigation. */
void navigation_enable_web_fonts(
    NavigationSession *session, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, long timeout_ms);
bool navigation_set_user_css(NavigationSession *session, const char *css,
                             size_t length);
void navigation_set_reader_candidate_mode(NavigationSession *session,
                                          bool enabled);
/* Rebuilds user presentation CSS into staged stylesheet/layout values and
   adopts them only after every fallible step succeeds. */
bool navigation_apply_user_css(NavigationSession *session, const char *css,
                               size_t length);
void navigation_enable_frame_capability_trace(NavigationSession *session,
                                              bool enabled);
void navigation_enable_page_capability_trace(NavigationSession *session,
                                             bool enabled);
void navigation_enable_diagnostic_frame_safari(NavigationSession *session,
                                               bool enabled);
void navigation_set_stream_delivery(
    NavigationSession *session, size_t chunk_bytes, uint64_t irregular_seed,
    size_t irregular_max_chunk_bytes, size_t stall_every_chunks,
    size_t cancel_after_bytes, size_t truncate_after_bytes);
bool navigation_collect_frame_capability_trace(NavigationSession *session);
bool navigation_collect_page_capability_trace(NavigationSession *session);
uint64_t navigation_begin(NavigationSession *session);
void navigation_cancel(NavigationSession *session, uint64_t generation);
bool navigation_commit_html(NavigationSession *session, uint64_t generation,
                            const char *url, const char *html,
                            size_t html_length, int viewport_width,
                            const FontSet *fonts,
                            const ImageResources *images,
                            bool record_history);
/* Commits trusted, browser-authored markup without creating a JavaScript
   realm. Native site adapters use this for documents that cannot contain
   author script; ordinary direct HTML commits retain their existing runtime
   semantics. */
bool navigation_commit_static_html(
    NavigationSession *session, uint64_t generation,
    const char *url, const char *html, size_t html_length,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history);
bool navigation_commit_html_preserve_runtime(
    NavigationSession *session, uint64_t generation, const char *url,
    const char *html, size_t html_length, int viewport_width,
    const FontSet *fonts, const ImageResources *images, bool record_history);
bool navigation_execute_current_body_scripts(NavigationSession *session);
bool navigation_load_url(NavigationSession *session, uint64_t generation,
                         const char *url, size_t maximum_bytes,
                         long timeout_ms, int viewport_width,
                         const FontSet *fonts,
                         const ImageResources *images,
                         bool record_history);
bool navigation_load_request(NavigationSession *session,
                             uint64_t generation, const char *url,
                             const char *method, const char *body,
                             size_t body_length, const char *content_type,
                             size_t maximum_bytes, long timeout_ms,
                             int viewport_width, const FontSet *fonts,
                             const ImageResources *images,
                             bool record_history);
NavigationLoad *navigation_load_begin_url(
    NavigationSession *session, uint64_t generation, const char *url,
    size_t maximum_bytes, long timeout_ms, int viewport_width,
    const FontSet *fonts, const ImageResources *images,
    bool record_history);
NavigationLoad *navigation_load_begin_request(
    NavigationSession *session, uint64_t generation, const char *url,
    const char *method, const char *body, size_t body_length,
    const char *content_type, size_t maximum_bytes, long timeout_ms,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history);
/* Section-routed loads share the exact fetch pump. The caller owns store and
   builder; on success they contain the completed backing store. Before EOF,
   only sections explicitly reported as safe by the store are observable. */
NavigationLoad *navigation_load_begin_sectioned(
    NavigationSession *session, uint64_t generation, const char *url,
    size_t maximum_bytes, long timeout_ms, int viewport_width,
    const FontSet *fonts, const ImageResources *images, bool record_history,
    CompressedSectionStore *store, SectionStoreStreamBuilder *builder,
    size_t block_bytes, size_t maximum_section_bytes,
    size_t initial_section);
NavigationLoadStatus navigation_load_pump(
    NavigationLoad *load, const NavigationLoadQuota *quota);
NavigationLoadStatus navigation_load_status(const NavigationLoad *load);
bool navigation_load_metrics(const NavigationLoad *load,
                             NavigationLoadMetrics *metrics);
/* With a non-NULL quota, advances exactly one transactional finalization
   phase and may return false with status NAVIGATION_LOAD_FINALIZING. Callers
   should yield and call again. A NULL quota retains the synchronous wrapper
   behavior and runs all remaining phases. */
bool navigation_load_finish(NavigationLoad *load,
                            const NavigationLoadQuota *quota);
void navigation_load_cancel(NavigationLoad *load, const char *reason);
void navigation_load_destroy(NavigationLoad *load);
bool navigation_back(NavigationSession *session, const NavigationEntry **entry);
bool navigation_forward(NavigationSession *session,
                        const NavigationEntry **entry);
const NavigationEntry *navigation_current(const NavigationSession *session);
/*
 * Transactionally replaces only the bounded top-level history list. Every
 * string is copied before the incumbent list is changed, so OOM leaves the
 * active page and its history untouched.
 */
bool navigation_replace_history(
    NavigationSession *session, const NavigationHistoryRecord *records,
    size_t count, size_t current_index);
bool navigation_commit_same_document_url(NavigationSession *session,
                                         const char *url);
bool navigation_restore_same_document_url(NavigationSession *session,
                                          const char *url,
                                          const char *old_url);
void navigation_discard_current_page(NavigationSession *session);
bool navigation_set_scroll(NavigationSession *session, int scroll_y);
bool navigation_advance_runtime(NavigationSession *session,
                                unsigned elapsed_ms,
                                size_t callback_budget);
/* Runs one bounded background-resource unit. Simple static pages admit one
   viewport-ranked document image, pump transport without waiting, or consume
   one completed decode; the legacy full-document continuation remains a
   lab-only compatibility experiment. A completed resource may replace the
   current layout, so owners refresh their render shell when relayout counters
   change. */
bool navigation_run_background_resources(NavigationSession *session);
bool navigation_background_resources_pending(
    const NavigationSession *session);
/* Close page and frame transport handles before the platform network stack
   is rebuilt. Completed cancellations are consumed by their normal owners. */
size_t navigation_cancel_network_work(
    NavigationSession *session, const char *reason);
bool navigation_dispatch_event(NavigationSession *session,
                               const char *selector,
                               const char *event_type);
bool navigation_dispatch_node_event(NavigationSession *session,
                                    lxb_dom_node_t *node,
                                    const char *event_type);
bool navigation_dispatch_node_activation(NavigationSession *session,
                                         lxb_dom_node_t *node);
bool navigation_dispatch_node_pointer(
    NavigationSession *session, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons);
bool navigation_dispatch_node_input_event(NavigationSession *session,
                                          lxb_dom_node_t *node,
                                          const char *event_type,
                                          const char *data,
                                          const char *input_type,
                                          const char *current_value);
bool navigation_dispatch_node_submit_event(NavigationSession *session,
                                           lxb_dom_node_t *form,
                                           lxb_dom_node_t *submitter);
bool navigation_evaluate_external_script(NavigationSession *session,
                                         lxb_dom_node_t *script_node,
                                         const char *source,
                                         size_t source_length,
                                         const char *source_url);
bool navigation_relayout(NavigationSession *session);
bool navigation_execute_document_scripts_from(
    NavigationSession *session, PocDocument *document);
bool navigation_execute_document_scripts_streaming_from(
    NavigationSession *session, PocDocument *document,
    NavigationScriptFrontierCallback frontier, void *frontier_opaque);
void navigation_destroy(NavigationSession *session);
const char *navigation_slice_phase_name(NavigationSlicePhase phase);

#endif
