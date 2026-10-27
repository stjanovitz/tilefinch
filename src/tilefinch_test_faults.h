#ifndef TILEFINCH_TEST_FAULTS_H
#define TILEFINCH_TEST_FAULTS_H

/*
 * Host-only deterministic fault injection.
 *
 * Every switch a test can flip lives in this one struct so the engine
 * carries no per-feature test statics and the seams are discoverable in one
 * place. Production code reads a field at the exact point where the real
 * failure would surface; a test sets the field and runs the ordinary path.
 * The struct does not exist in device builds.
 */
#ifndef __PSP__
#include <stdbool.h>

typedef struct {
    /* Host-only native audio allocation/publication refusal gates. */
    unsigned audio_slot_allocation_fail_at;
    unsigned audio_slot_install_fail_at;
    unsigned audio_slot_publish_fail_at;
    /* Expire the host watchdog at the Nth WebAssembly task checkpoint. */
    unsigned wasm_task_checkpoints;
    /* Refuse the next WebAssembly memory alias buffer allocation once. */
    bool refuse_next_wasm_alias;
    /* Refuse the next dedicated-worker realm creation once. */
    bool refuse_next_worker_realm;
    /* Backdate both halves of the Window performance origin by this many
       milliseconds. Tests use it to expose epoch/monotonic skew without a
       real sleep inside runtime startup. Consumed once. */
    unsigned runtime_performance_origin_backdate_ms;
    /* Fail the next document refresh after a DOM mutation once. */
    bool refuse_next_document_refresh;
    /* Refuse the next streaming resource scheduler creation once. */
    bool refuse_next_stream_scheduler;
    /* Refuse the next static-fallback layout once. */
    bool refuse_next_static_fallback_layout;
    /* Refuse the next same-document relayout once. */
    bool refuse_next_same_document_relayout;
    /* Refuse the next provisional layout completion's build once, as a
       budget refusal would (NAVIGATION_LAYOUT_COMPLETION_FAILED). */
    bool refuse_next_layout_completion;
    /* Refuse every script geometry build over a bounded focus layout while
       set (a complete build the budget cannot hold); counts the attempts. */
    bool refuse_bounded_geometry_builds;
    unsigned bounded_geometry_build_attempts;
    /* Refuse the next adopted-sheet root metadata allocation once. */
    bool refuse_next_adopted_scope_table;
    /* Simulate one pack I/O read failure, separately from Budget refusal. */
    bool fail_next_script_cache_read;
    unsigned script_cache_read_attempts;
    /* Fail this many upcoming script-cache publications (index, then
       pack) at the point where FAT has removed the old file. */
    unsigned fail_script_cache_index_renames;
    unsigned fail_script_cache_pack_renames;
    /* Refuse the next child-frame presentation refresh once. */
    bool refuse_next_frame_presentation;
    /* Refuse the next background web-font relayout once. */
    bool refuse_next_background_font_relayout;
    /* Observe cache churn across identical versus distinct script plans. */
    unsigned web_font_kerning_cache_resets;
    unsigned web_font_charmap_lookups;
    /* Host observation: jar reads that derive the YouTube login identity
       (cache misses). Reset by the test. */
    unsigned youtube_login_identity_builds;
    /* Host observation: full scans of the chrome glyph cache for the media
       raster's ink check. Reset by the test. */
    unsigned chrome_media_ink_scans;
    /* Host observation: retained Unicode chrome glyph lookups, the full
       table scans among them, and the scans that found the entry (its
       hint had been overwritten). Reset by the test. */
    unsigned long chrome_unicode_lookups;
    unsigned long chrome_unicode_scans;
    unsigned long chrome_unicode_scan_hits;
    /* Outline loads made only to measure an advance. */
    unsigned web_font_advance_loads;
    /* Clip and mask coverage rebuilds during SVG rasterization. */
    unsigned svg_clip_mask_builds;
    /* Sprite bytes scanned linearly to find external <use> symbols. */
    unsigned long external_svg_symbol_scan_bytes;
    /* Refuse one validated optional image-loader setup. */
    bool refuse_next_image_load_setup;
    /* Report the image decoder busy (a worker decode in flight) while set. */
    bool image_decode_busy;
    /* Hold the detached-image pump's sweep of dead slots while set, as
       when a node is released and its address reused before that pump. */
    bool defer_image_preload_sweep;
    /* Refuse every growth of the bootstrap's event-target maps while set
       (js_dom_grow_event_targets). */
    bool refuse_event_target_growth;
    /* Refuse the Nth (1-based) JavaScript heap allocation made inside the
       next call of the bootstrap's event-state hold hook
       (__tilefinchHoldEventState) or release hook
       (__tilefinchReleaseEventHolders), once each, as a full page heap
       would (JS_TestRefuseAllocation); event_hook_refused reports whether
       that call reached the refusal. */
    unsigned event_hold_refuse_allocation;
    unsigned event_release_refuse_allocation;
    bool event_hook_refused;
    /* Refuse the next render-shell initialization once. */
    bool refuse_next_render_shell_init;
    /* Make the next layout cooperate checkpoint decline to continue once,
       the way a supervisor cancel (Circle press) does on the device. */
    bool cancel_next_layout_cooperate;
    /* Inject an allocator refusal at the job context, after optional caches. */
    bool refuse_next_layout_context;
    /* Refuse the next minimal-stylesheet pressure fallback once. */
    bool refuse_next_minimal_stylesheet;
    /* A NavigationParserCheckpointTestFault to inject at the next matching
       parser checkpoint (0 = none). Consumed once. */
    unsigned parser_checkpoint_fault;
    /* Fail this many upcoming Memory Stick site-storage record appends. */
    unsigned fail_site_storage_appends;
    /* tilefinch_platform_replace_file: while set, rename refuses to
       replace an existing file, as FAT does. */
    bool replace_file_no_replace;
    /* tilefinch_platform_replace_file with a backup, consumed once: fail
       the rename that installs the temporary file (the previous file is
       moved back), or stop right after the previous file moved aside, as a
       power loss there would. */
    bool replace_file_fail_install;
    bool replace_file_crash_after_backup;
    bool refuse_next_youtube_feed_build;
    bool expire_youtube_history;
    bool refuse_youtube_signin_recovery;
    /* Rasterize box shadows pixel by pixel, without the fully covered
       core run, as the reference the core run must reproduce. */
    bool shadow_reference_raster;
    /* Compose a canvas page with the canvas path only over a valid
       previous frame (the tile path otherwise): the reference a
       full-viewport canvas frame must reproduce. */
    bool canvas_frame_reference;
    /* Rasterize fills and box-shadow cores pixel by pixel, without
       interior spans, core runs or the translucent blend memo: the
       reference those must reproduce. */
    bool raster_span_reference;
    /* Host observation for index-refusal tests: linear node-box lookups.
       Reset by the test; never present on the PSP. */
    unsigned long long layout_node_box_scans;
    /* Host work observation: display-list commands the colour-only focus
       recolour visits. Reset by the test. */
    unsigned long long focus_color_command_visits;
    /* Host work observations: CSS source handed to the statement scanner
       and body text inspected by the visible-content census. */
    unsigned long long css_statement_max_source_bytes;
    unsigned long long document_body_text_bytes_inspected;
    /* Host work observation: ancestor cascades run by the simplified-view
       image filter. Reset by the test. */
    unsigned long long reader_image_style_resolutions;
    /* Host observation for per-site identity tests: the request key and
       effective User-Agent of each request trace replay answered, as a
       ring of the latest 16. Reset by the test. */
    struct {
        char url[256];
        char user_agent[192];
        bool cookie_present;
        unsigned fetch_site; /* 0 unknown, 1 none, 2 origin, 3 site, 4 cross-site. */
    } replayed_requests[16];
    unsigned replayed_request_count;
} TilefinchTestFaults;

TilefinchTestFaults *tilefinch_test_faults(void);
#endif

#endif
