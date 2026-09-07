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
    /* Expire the host watchdog at the Nth WebAssembly task checkpoint. */
    unsigned wasm_task_checkpoints;
    /* Refuse the next WebAssembly memory alias buffer allocation once. */
    bool refuse_next_wasm_alias;
    /* Refuse the next dedicated-worker realm creation once. */
    bool refuse_next_worker_realm;
    /* Fail the next document refresh after a DOM mutation once. */
    bool refuse_next_document_refresh;
    /* Refuse the next streaming resource scheduler creation once. */
    bool refuse_next_stream_scheduler;
    /* Refuse the next static-fallback layout once. */
    bool refuse_next_static_fallback_layout;
    /* Refuse the next same-document relayout once. */
    bool refuse_next_same_document_relayout;
    /* Refuse the next background web-font relayout once. */
    bool refuse_next_background_font_relayout;
    /* Refuse the next render-shell initialization once. */
    bool refuse_next_render_shell_init;
    /* Make the next layout cooperate checkpoint decline to continue once,
       the way a supervisor cancel (Circle press) does on the device. */
    bool cancel_next_layout_cooperate;
    /* A NavigationParserCheckpointTestFault to inject at the next matching
       parser checkpoint (0 = none). Consumed once. */
    unsigned parser_checkpoint_fault;
    /* Host observation for index-refusal tests: linear node-box lookups.
       Reset by the test; never present on the PSP. */
    unsigned long long layout_node_box_scans;
} TilefinchTestFaults;

TilefinchTestFaults *tilefinch_test_faults(void);
#endif

#endif
