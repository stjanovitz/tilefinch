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
    /* Refuse the next adopted-sheet root metadata allocation once. */
    bool refuse_next_adopted_scope_table;
    /* Simulate one pack I/O read failure, separately from Budget refusal. */
    bool fail_next_script_cache_read;
    unsigned script_cache_read_attempts;
    /* Refuse the next child-frame presentation refresh once. */
    bool refuse_next_frame_presentation;
    /* Refuse the next background web-font relayout once. */
    bool refuse_next_background_font_relayout;
    /* Refuse one validated optional image-loader setup. */
    bool refuse_next_image_load_setup;
    /* Refuse the next render-shell initialization once. */
    bool refuse_next_render_shell_init;
    /* Make the next layout cooperate checkpoint decline to continue once,
       the way a supervisor cancel (Circle press) does on the device. */
    bool cancel_next_layout_cooperate;
    /* Inject an allocator refusal at the job context, after optional caches. */
    bool refuse_next_layout_context;
    /* A NavigationParserCheckpointTestFault to inject at the next matching
       parser checkpoint (0 = none). Consumed once. */
    unsigned parser_checkpoint_fault;
    /* Fail this many upcoming Memory Stick site-storage record appends. */
    unsigned fail_site_storage_appends;
    /* Fail the rename that puts a compacted site log in place, once. */
    bool fail_next_site_storage_compact_rename;
    /* Model FAT's refusal to rename over an existing credential file. */
    bool youtube_login_no_replace_rename;
    /* Stop the next compaction right after the original log is moved
       aside, as a power loss there would. */
    bool crash_next_site_storage_compact;
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
    /* Host work observations: CSS source handed to the statement scanner
       and body text inspected by the visible-content census. */
    unsigned long long css_statement_max_source_bytes;
    unsigned long long document_body_text_bytes_inspected;
    /* Host observation for per-site identity tests: the request key and
       effective User-Agent of each request trace replay answered, as a
       ring of the latest 16. Reset by the test. */
    struct {
        char url[256];
        char user_agent[192];
    } replayed_requests[16];
    unsigned replayed_request_count;
} TilefinchTestFaults;

TilefinchTestFaults *tilefinch_test_faults(void);
#endif

#endif
