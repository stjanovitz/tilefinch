#ifndef TILEFINCH_JS_RUNTIME_H
#define TILEFINCH_JS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/session.h"
#include "tilefinch/layout.h"
#include "tilefinch/style.h"
#include "tilefinch/remote_selector.h"

/* QuickJS parsing/bytecode generation is a dependency call which cannot be
   interrupted by the runtime watchdog.  This policy therefore places a hard,
   truthful byte ceiling on every source submitted by the C host before that
   call begins.  It does not claim to bound strings which author JavaScript
   later constructs and passes to eval/Function inside QuickJS. */
typedef enum {
    SCRIPT_EXECUTION_PROFILE_LAB = 0,
    SCRIPT_EXECUTION_PROFILE_PSP_STRICT,
    SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC
} ScriptExecutionProfile;

typedef struct {
    /* Zero keeps the existing unbounded lab admission behavior. */
    size_t maximum_host_compile_source_bytes;
    /* Zero selects the default 16 ms observation threshold.  These are
       telemetry thresholds, not deadlines: an uninterruptible dependency
       call can only be classified after it returns. */
    uint64_t slow_compile_threshold_us;
    uint64_t slow_callback_threshold_us;
    /* Projected-time admission is enforced only when both fields are
       nonzero: a source is rejected before QuickJS parsing begins when its
       modeled compile time (length divided by the modeled throughput in
       bytes per millisecond) exceeds the microsecond budget.  The model is
       a calibrated throughput, not a measurement; rejection is as truthful
       and observable as the byte ceiling above. */
    uint64_t maximum_host_compile_projected_us;
    size_t modeled_compile_bytes_per_ms;
} ScriptExecutionPolicy;

typedef enum {
    SCRIPT_COMPILE_SOURCE_INTERNAL = 0,
    SCRIPT_COMPILE_SOURCE_INLINE,
    SCRIPT_COMPILE_SOURCE_EXTERNAL,
    SCRIPT_COMPILE_SOURCE_MODULE,
    SCRIPT_COMPILE_SOURCE_DIAGNOSTIC,
    SCRIPT_COMPILE_SOURCE_LAZY_FACTORY
} ScriptCompileSourceKind;

typedef enum {
    SCRIPT_COMPILE_ADMISSION_NONE = 0,
    SCRIPT_COMPILE_ADMISSION_ACCEPTED,
    SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT,
    SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME,
    /* Admitted, but the watchdog (deadline or cooperate stop) aborted the
       compile through the parser's bounded interrupt polls. */
    SCRIPT_COMPILE_ADMISSION_ABORTED_WATCHDOG
} ScriptCompileAdmission;

bool script_execution_policy_for_profile(
    ScriptExecutionProfile profile, ScriptExecutionPolicy *policy);

/* Native DOM handles stay deliberately bounded on the PSP.  Generation tags
   permit safe reuse after subtree destruction or collection of an unreachable
   detached wrapper; pressure telemetry distinguishes simultaneous demand from
   lifetime churn. */
#define SCRIPT_DOM_HANDLE_INDEX_BITS 14u
#define SCRIPT_DOM_HANDLE_INDEX_MASK \
    ((1u << SCRIPT_DOM_HANDLE_INDEX_BITS) - 1u)
#define SCRIPT_DOM_HANDLE_SLOT_CAPACITY 8192u

typedef struct {
    bool success;
    bool interrupted;
    size_t scripts_evaluated;
    size_t scripts_failed;
    size_t external_scripts_skipped;
    size_t external_scripts_loaded;
    size_t external_scripts_failed;
    size_t external_script_bytes;
    size_t external_script_bytecode_cache_hits;
    size_t external_script_bytecode_cache_misses;
    size_t external_script_bytecode_cache_stores;
    size_t external_script_bytecode_cache_admission_skips;
    size_t external_script_bytecode_cache_restore_failures;
    size_t external_script_bytecode_cache_bytes;
    bool dom_content_loaded_dispatched;
    bool form_submission_requested;
    bool relayout_required;
    size_t dom_mutations;
    size_t dom_handle_slots_live;
    size_t dom_handle_slots_peak;
    size_t dom_handle_slots_high_water;
    size_t dom_handle_slot_reuses;
    size_t dom_handle_exhaustions;
    size_t dom_handle_wrapper_releases;
    size_t dom_handle_connected_preserves;
    size_t dom_handle_stale_releases;
    size_t geometry_queries;
    size_t geometry_retained_fast_paths;
    size_t geometry_ancestor_visits;
    size_t geometry_synchronous_layouts;
    size_t events_dispatched;
    bool last_event_cancelled;
    size_t timer_callbacks_run;
    size_t runtime_ticks;
    size_t pending_tasks;
    size_t network_requests;
    size_t network_failures;
    size_t async_network_queued;
    size_t async_network_completed;
    size_t async_network_quota_rejected;
    size_t async_network_peak_inflight;
    size_t async_network_cancelled;
    size_t async_network_timed_out;
    size_t async_network_logical_admitted;
    size_t async_network_logical_completed;
    size_t async_network_logical_rejected;
    size_t async_network_logical_cancelled;
    size_t async_network_logical_timed_out;
    size_t async_network_logical_peak;
    size_t async_network_logical_peak_bytes;
    size_t async_network_active_native;
    size_t async_network_pending_logical;
    size_t dynamic_scripts_queued;
    size_t dynamic_scripts_started;
    size_t dynamic_scripts_completed;
    size_t dynamic_scripts_failed;
    size_t dynamic_scripts_cancelled;
    size_t dynamic_scripts_cache_hits;
    size_t dynamic_scripts_ordered_waits;
    size_t dynamic_scripts_peak_pending;
    size_t dynamic_script_bytes;
    size_t dynamic_scripts_quota_rejected;
    size_t indexed_db_opens;
    size_t indexed_db_deletes;
    size_t indexed_db_transactions;
    size_t indexed_db_requests;
    size_t indexed_db_records;
    size_t indexed_db_bytes;
    size_t indexed_db_peak_bytes;
    size_t indexed_db_quota_errors;
    size_t clipboard_writes;
    char last_clipboard_text[4096];
    long last_network_status;
    char last_network_url[256];
    size_t last_network_response_bytes;
    unsigned long long last_network_body_hash;
    char last_network_content_type[128];
    char last_network_content_encoding[64];
    char last_network_server[64];
    char last_network_cf_ray[64];
    char last_network_cf_mitigated[32];
    char last_network_body_prefix[65];
    size_t promise_rejections;
    size_t promise_rejections_created;
    size_t promise_rejections_handled;
    size_t promise_rejections_undefined;
    /* Module compilation cost is split from execution so bytecode and lazy
       activation changes have a measured target. */
    size_t module_compile_count;
    unsigned long long module_compile_us;
    char last_promise_rejection[2048];
    size_t uncaught_callback_errors;
    char last_uncaught_callback_error[512];
    /* Allocation-bounded provenance captured separately from the stack so a
       minified callback cannot truncate away the realm/task which invoked it. */
    char last_uncaught_callback_task[160];
    size_t xhr_send_calls;
    char last_xhr_error[512];
    size_t xhr_response_count;
    long last_xhr_status;
    size_t last_xhr_response_text_units;
    size_t last_xhr_response_bytes;
    char last_xhr_response_type[16];
    char last_xhr_ready_states[32];
    char last_reject_stack[512];
    char summary[2048];
    char error[2048];
    char error_source_context[2048];
    char frame_eval_telemetry[128];
    size_t performance_now_calls;
    double performance_now_last_ms;
    size_t date_now_calls;
    double date_now_last_ms;
    size_t watchdog_polls;
    unsigned long watchdog_elapsed_ms;
    uint64_t script_max_slice_us;
    size_t script_work_units;
    size_t script_max_slice_work_units;
    size_t script_cooperative_yields;
    uint64_t max_nonpreemptible_compile_us;
    size_t max_nonpreemptible_compile_bytes;
    size_t nonpreemptible_compile_count;
    size_t host_compile_attempts;
    size_t host_compile_source_bytes;
    uint64_t host_compile_total_us;
    size_t bootstrap_bytecode_restores;
    size_t bootstrap_bytecode_restore_failures;
    size_t bootstrap_bytecode_source_preferences;
    size_t bootstrap_bytecode_preferred_source_bytes;
    size_t bootstrap_bytecode_stored_bytes;
    size_t bootstrap_bytecode_bytes;
    uint64_t bootstrap_bytecode_restore_us;
    size_t bootstrap_lazy_module_loads;
    size_t bootstrap_lazy_module_failures;
    size_t host_compile_rejections;
    size_t host_compile_rejected_source_bytes;
    size_t host_compile_projected_rejections;
    size_t host_compile_projected_rejected_bytes;
    size_t host_compile_watchdog_aborts;
    size_t host_compile_watchdog_aborted_bytes;
    size_t host_compile_source_limit_bytes;
    ScriptCompileAdmission last_compile_admission;
    ScriptCompileSourceKind last_compile_source_kind;
    size_t last_compile_source_bytes;
    ScriptCompileSourceKind max_nonpreemptible_compile_source_kind;
    char last_compile_source_name[256];
    char max_nonpreemptible_compile_source_name[256];
    /* Callback timing covers each complete C-to-JavaScript host call.  A call
       with one or more QuickJS interrupt polls is classified as preemptible;
       the embedding API cannot expose smaller native subsegments. */
    uint64_t max_nonpreemptible_callback_us;
    size_t nonpreemptible_callback_count;
    size_t host_callback_calls;
    size_t host_callback_calls_with_interrupt_polls;
    uint64_t host_callback_total_us;
    char max_nonpreemptible_callback_name[96];
    size_t retention_wrapper_evictions;
    size_t retention_listener_drops;
    size_t retention_handler_drops;
    size_t retention_observer_drops;
    size_t retention_record_drops;
    size_t retention_dirty_drops;
    size_t retention_state_evictions;
    size_t retention_control_drops;
    /* Current bounded JavaScript root census. These are container/root counts,
       not heap estimates, and are refreshed with the ordinary ScriptResult. */
    size_t root_timers;
    size_t root_node_wrappers;
    size_t root_stable_wrappers;
    size_t root_stable_listener_targets;
    size_t root_stable_handler_targets;
    size_t root_mutation_observers;
    size_t root_pending_network;
    size_t root_frame_windows;
    size_t lazy_webpack_candidates;
    size_t lazy_webpack_applied;
    size_t lazy_webpack_fallbacks;
    size_t lazy_webpack_factories_deferred;
    size_t lazy_webpack_factory_source_bytes;
    size_t lazy_webpack_compressed_source_bytes;
    size_t lazy_webpack_bytecode_bytes;
    size_t lazy_webpack_compressed_bytecode_bytes;
    size_t lazy_webpack_source_compiles;
    size_t lazy_webpack_bytecode_restores;
    size_t lazy_webpack_bytecode_demotions;
    size_t lazy_webpack_bytecode_bytes_released;
    size_t lazy_webpack_factories_compiled;
    size_t lazy_webpack_compiled_factory_evictions;
    size_t lazy_webpack_factory_compile_failures;
    size_t lazy_webpack_resident_source_limit_bytes;
    size_t lazy_webpack_residency_rejections;
    size_t lazy_webpack_compile_pressure_collections;
    size_t lazy_webpack_compile_cache_reclaimed_bytes;
    size_t lazy_webpack_compile_admission_rejections;
    size_t lazy_webpack_syntax_preflight_attempts;
    size_t lazy_webpack_syntax_preflight_failures;
    size_t lazy_webpack_syntax_preflight_source_bytes;
    uint64_t lazy_webpack_syntax_preflight_total_us;
} ScriptResult;

/* Cheap, allocation-free counters for attributing one host evaluation
   boundary. Unlike ScriptResult, this intentionally does not refresh or copy
   the JavaScript-visible diagnostic census. */
typedef struct {
    uint64_t host_compile_total_us;
    uint64_t host_callback_total_us;
    size_t scripts_evaluated;
    size_t dom_mutations;
} ScriptRuntimeTelemetry;

/* The DOM bridge records a small, allocation-free description of mutations
   between host relayout boundaries.  Known layout-only changes can avoid a
   whole-document resource fingerprint walk; resource-sensitive changes still
   request the existing full style/resource pipeline, and ambiguous changes
   retain the conservative fingerprint oracle. */
/* Hydrated acceptance pages can commit just over 64 distinct connected-node
   changes in one author turn after detached-tree coalescing. 128 keeps those
   batches typed instead of forcing a whole-document oracle/rebuild, for
   5,376 additional fixed bytes per active realm over the former 32-record
   limit (0.021% of the realistic 24 MiB engine ceiling). */
#define SCRIPT_MUTATION_JOURNAL_LIMIT 128u
#define SCRIPT_MUTATION_ATTRIBUTE_LIMIT 32u

typedef enum {
    SCRIPT_MUTATION_UNKNOWN = 0,
    SCRIPT_MUTATION_TEXT,
    SCRIPT_MUTATION_INNER_HTML,
    SCRIPT_MUTATION_ATTRIBUTE,
    SCRIPT_MUTATION_INLINE_STYLE,
    SCRIPT_MUTATION_CHILD_LIST
} ScriptMutationKind;

typedef struct {
    ScriptMutationKind kind;
    lxb_dom_node_t *node;
    /* Captured while node is live.  Document retirement compares this opaque
       identity instead of dereferencing a target which an earlier mutation in
       the same author turn may already have destroyed. */
    uintptr_t owner_document_identity;
    char attribute[SCRIPT_MUTATION_ATTRIBUTE_LIMIT];
} ScriptMutationRecord;

typedef struct {
    ScriptMutationRecord records[SCRIPT_MUTATION_JOURNAL_LIMIT];
    size_t count;
    bool overflowed;
    bool resource_rebuild_required;
    bool image_resource_scan_required;
    bool image_resource_refresh_required;
    bool conservative_resource_scan;
    bool relational_selector_sensitive;
} ScriptMutationJournal;

typedef struct ScriptRuntime ScriptRuntime;

#define SCRIPT_MEDIA_SOURCE_CAPACITY 4096

typedef enum {
    SCRIPT_MEDIA_COMMAND_LOAD = 0,
    SCRIPT_MEDIA_COMMAND_PLAY,
    SCRIPT_MEDIA_COMMAND_PAUSE,
    SCRIPT_MEDIA_COMMAND_SEEK
} ScriptMediaCommand;

typedef struct {
    ScriptMediaCommand command;
    int64_t node_handle;
    double value;
    char source[SCRIPT_MEDIA_SOURCE_CAPACITY];
} ScriptMediaRequest;

typedef enum {
    SCRIPT_MEDIA_STATE_LOADING = 0,
    SCRIPT_MEDIA_STATE_PAUSED,
    SCRIPT_MEDIA_STATE_PLAYING,
    SCRIPT_MEDIA_STATE_ENDED,
    SCRIPT_MEDIA_STATE_ERROR
} ScriptMediaState;

/* Executable script resources share one cumulative per-realm quota. Static
   and dynamic roots claim their count before reaching the loader, while each
   transitive module is a new executable. Byte reservations bound concurrent
   fetches before cache bodies or network responses are acquired. */
typedef enum {
    SCRIPT_QUOTA_NEW_EXECUTABLE = 0,
    SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE
} ScriptQuotaCountMode;

typedef enum {
    SCRIPT_QUOTA_RESERVE_GRANTED = 0,
    SCRIPT_QUOTA_RESERVE_DEFERRED,
    SCRIPT_QUOTA_RESERVE_REJECTED
} ScriptQuotaReserveResult;

typedef enum {
    SCRIPT_QUOTA_PROGRESS_FAILED = 0,
    SCRIPT_QUOTA_PROGRESS_EXHAUSTED,
    SCRIPT_QUOTA_PROGRESS_PENDING,
    SCRIPT_QUOTA_PROGRESS_SETTLED
} ScriptQuotaProgressResult;

typedef struct {
    size_t reserved_bytes;
    bool active;
} ScriptQuotaReservation;
struct ScriptLazyWebpackPlan;
typedef void (*ScriptSourceLeaseReleaseCallback)(void *opaque);
typedef enum {
    /* No author code ran and ownership stayed with the caller. */
    SCRIPT_LAZY_EVALUATION_FALLBACK = 0,
    /* The transformed registration and its ordinary load event completed. */
    SCRIPT_LAZY_EVALUATION_SUCCEEDED,
    /* Author-visible transformed execution began and then failed. */
    SCRIPT_LAZY_EVALUATION_FAILED
} ScriptLazyEvaluation;
typedef bool (*ScriptPostMessageCallback)(void *opaque,
                                          ScriptRuntime *source,
                                          long target_frame_handle,
                                          const char *json,
                                          const char *target_origin);
/* CSSOM View and getComputedStyle() are synchronous APIs. Embedders with a
   staged parser/layout pipeline provide this narrowly scoped callback so a
   read after author mutation can flush style and layout without re-entering
   JavaScript. */
typedef bool (*ScriptSynchronousLayoutCallback)(void *opaque);
typedef struct {
    /* Absolute module-map request URL. */
    const char *request_url;
    /* Final response/base URL of the referring module. */
    const char *referrer_url;
    /* Effective policy inherited from the referring module response. */
    const char *referrer_policy;
    TilefinchCredentialsMode credentials;
} ScriptModuleLoadRequest;
typedef struct {
    char *source;
    size_t source_length;
    /* Absolute final response URL. The runtime keeps the requested URL as the
       QuickJS/module-map identity and uses this URL only as the module base and
       import.meta.url. Both strings remain owned by the loader until release. */
    char *response_url;
    /* Normalized policy selected from the final response only. Empty means
       that the referring module's effective policy remains inherited. */
    char response_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
} ScriptModuleLoadResult;
typedef bool (*ScriptModuleLoadCallback)(void *opaque,
                                         const ScriptModuleLoadRequest *request,
                                         ScriptModuleLoadResult *result);
typedef void (*ScriptModuleFreeCallback)(void *opaque,
                                         ScriptModuleLoadResult *result);
typedef void (*ScriptModuleOpaqueDestroyCallback)(void *opaque);
/* Module scripts require a JavaScript MIME essence; missing and generic text
   types fail even without X-Content-Type-Options. Parameters are ignored. */
bool script_module_mime_type_allowed(const char *content_type);
/* A 304 inherits the stored representation MIME only when the response omits
   Content-Type. Any supplied conflicting/non-JavaScript type fails closed. */
bool script_module_revalidated_mime_allowed(
    const char *cached_content_type, const char *response_content_type);
typedef bool (*ScriptRemoteElementLookupCallback)(
    void *opaque, const char *identifier, size_t length,
    size_t *section_index, char tag_name[32], char stable_key[96]);
typedef bool (*ScriptRemoteSelectorLookupCallback)(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96]);
typedef bool (*ScriptRemoteSelectorCollectCallback)(
    void *opaque, const char *selector, size_t length,
    TilefinchRemoteSelectorMatch *matches, size_t capacity, size_t *match_count);
typedef bool (*ScriptRemoteDescendantCollectCallback)(
    void *opaque, unsigned root_special, int what_to_show,
    TilefinchRemoteTraversalNode *nodes, size_t capacity, size_t *node_count);

typedef enum {
    SCRIPT_REMOTE_NODE_TEXT = 0,
    SCRIPT_REMOTE_NODE_INNER_HTML,
    SCRIPT_REMOTE_NODE_ATTRIBUTE,
    SCRIPT_REMOTE_NODE_ATTRIBUTES,
    SCRIPT_REMOTE_NODE_MATCHES,
    SCRIPT_REMOTE_NODE_GEOMETRY,
    SCRIPT_REMOTE_NODE_PARENT_ELEMENT,
    SCRIPT_REMOTE_NODE_FIRST_ELEMENT_CHILD,
    SCRIPT_REMOTE_NODE_NEXT_ELEMENT_SIBLING,
    SCRIPT_REMOTE_NODE_PREVIOUS_ELEMENT_SIBLING,
    SCRIPT_REMOTE_NODE_FIRST_CHILD,
    SCRIPT_REMOTE_NODE_NEXT_SIBLING,
    SCRIPT_REMOTE_NODE_PREVIOUS_SIBLING,
    SCRIPT_REMOTE_NODE_LAST_CHILD
} ScriptRemoteNodeReadKind;

typedef enum {
    SCRIPT_REMOTE_NODE_WRITE_TEXT = 0,
    SCRIPT_REMOTE_NODE_WRITE_INNER_HTML,
    SCRIPT_REMOTE_NODE_WRITE_SET_ATTRIBUTE,
    SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE
} ScriptRemoteNodeWriteKind;

typedef struct {
    char *name;
    size_t name_length;
    char *value;
    size_t value_length;
} ScriptRemoteNodeAttribute;

typedef struct {
    char *value;
    size_t length;
    bool is_null;
    ScriptRemoteNodeAttribute *attributes;
    size_t attribute_count;
    char related_stable_key[193];
    char related_tag_name[32];
    size_t related_section_index;
    unsigned related_special;
    unsigned related_node_type;
    int geometry_x;
    int geometry_y;
    int geometry_width;
    int geometry_height;
    int geometry_client_width;
    int geometry_client_height;
    int geometry_scroll_width;
    int geometry_scroll_height;
    int geometry_scroll_left;
    int geometry_scroll_top;
} ScriptRemoteNodeReadResult;

/* A successful callback may return values and attribute storage allocated from
   the runtime's shared Budget.  The runtime releases all returned storage
   after copying it into the JavaScript heap.  is_null distinguishes a missing
   attribute or relation from a present empty string. */
typedef bool (*ScriptRemoteNodeReadCallback)(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeReadKind kind,
    const char *name, size_t name_length, ScriptRemoteNodeReadResult *result);
typedef bool (*ScriptRemoteNodeWriteCallback)(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeWriteKind kind,
    const char *name, size_t name_length,
    const char *value, size_t value_length);
typedef bool (*ScriptNodeVisibilityCallback)(
    void *opaque, size_t section_index, size_t element_ordinal,
    size_t node_ordinal, unsigned node_type);

/* Reserved section used to ask whether a streaming parser has entered body. */
#define SCRIPT_NODE_VISIBILITY_DOCUMENT_BODY_SECTION ((size_t) -1)

/* Remote-document operations belong only to the top-level document realm.
   Child-frame runtimes have independent documents and origins, so a child
   scope always rejects and clears a non-empty binding. */
typedef enum {
    SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL = 0,
    SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME
} ScriptDocumentScope;

/* `dual-domain-ms-call-v2`: deterministic wall and monotonic observations
   return their current realm-local millisecond and then advance only that
   domain by one. Stable timestamp samples are counted but do not advance the
   monotonic domain. Event-loop advances add their explicit elapsed_ms to both
   domains before callbacks. QuickJS watchdog polls never affect page-visible
   time. */
#define SCRIPT_DETERMINISTIC_CLOCK_CONTRACT "dual-domain-ms-call-v2"
#define SCRIPT_DETERMINISTIC_CLOCK_CALL_QUANTUM_MS UINT64_C(1)
#define SCRIPT_DETERMINISTIC_CLOCK_MAX_HOST_ELAPSED_MS UINT64_C(60000000)
#define SCRIPT_DETERMINISTIC_CLOCK_MAX_SOURCE_EVENTS UINT64_C(1000000)
#define SCRIPT_DETERMINISTIC_CLOCK_MAX_DOMAIN_ELAPSED_MS \
    (SCRIPT_DETERMINISTIC_CLOCK_MAX_HOST_ELAPSED_MS \
     + SCRIPT_DETERMINISTIC_CLOCK_MAX_SOURCE_EVENTS)
#define SCRIPT_DETERMINISTIC_CLOCK_EVIDENCE_SCOPE "top-level-realm-v1"
#define SCRIPT_DETERMINISTIC_REPLAY_ENVIRONMENT \
    "deterministic-hermetic-v3"
#define SCRIPT_DETERMINISTIC_REPLAY_SEED_SOURCE "trace-origin-ms-v1"
#define SCRIPT_DETERMINISTIC_CONFIGURED_SEED_SOURCE "configured-u64-v1"
#define SCRIPT_DETERMINISTIC_ENTROPY_CONTRACT \
    "splitmix64-url-scope-v1"
#define SCRIPT_DETERMINISTIC_PERFORMANCE_ENTRIES \
    "normalized-empty-v1"
#define SCRIPT_DETERMINISTIC_INTL_SURFACE "bounded-en-us-utc-v1"

typedef enum {
    SCRIPT_CLOCK_SOURCE_DATE_NOW = 0,
    SCRIPT_CLOCK_SOURCE_DATE_FUNCTION,
    SCRIPT_CLOCK_SOURCE_DATE_CONSTRUCTOR,
    SCRIPT_CLOCK_SOURCE_PERFORMANCE_NOW,
    SCRIPT_CLOCK_SOURCE_PERFORMANCE_MARK,
    SCRIPT_CLOCK_SOURCE_PERFORMANCE_MEASURE,
    SCRIPT_CLOCK_SOURCE_ANIMATION_TIMELINE,
    SCRIPT_CLOCK_SOURCE_IDLE_DEADLINE_TIME_REMAINING,
    SCRIPT_CLOCK_SOURCE_ANIMATION_FRAME,
    SCRIPT_CLOCK_SOURCE_EVENT_TIMESTAMP,
    SCRIPT_CLOCK_SOURCE_INTERSECTION_OBSERVER,
    SCRIPT_CLOCK_SOURCE_IDLE_CALLBACK_START,
    SCRIPT_CLOCK_SOURCE_COUNT
} ScriptDeterministicClockSource;

typedef struct {
    uint64_t date_now;
    uint64_t date_function;
    uint64_t date_constructor;
    uint64_t performance_now;
    uint64_t performance_mark;
    uint64_t performance_measure;
    uint64_t animation_timeline;
    uint64_t idle_deadline_time_remaining;
    uint64_t animation_frame;
    uint64_t event_timestamp;
    uint64_t intersection_observer;
    uint64_t idle_callback_start;
} ScriptDeterministicClockSourceCounts;

typedef struct {
    uint64_t seed;
    uint64_t clock_origin_ms;
    uint64_t host_elapsed_ms;
    uint64_t wall_elapsed_ms;
    uint64_t monotonic_elapsed_ms;
    uint64_t wall_observations;
    uint64_t monotonic_observations;
    uint64_t monotonic_samples;
    ScriptDeterministicClockSourceCounts sources;
} ScriptDeterministicReplayDiagnostics;

typedef struct {
    ScriptRemoteElementLookupCallback element_lookup;
    void *element_opaque;
    ScriptRemoteSelectorLookupCallback selector_lookup;
    void *selector_opaque;
    ScriptRemoteSelectorCollectCallback selector_collect;
    void *selector_collect_opaque;
    ScriptRemoteDescendantCollectCallback descendant_collect;
    void *descendant_collect_opaque;
    ScriptRemoteNodeReadCallback node_read;
    void *node_read_opaque;
    ScriptRemoteNodeWriteCallback node_write;
    void *node_write_opaque;
    ScriptNodeVisibilityCallback node_visibility;
    void *node_visibility_opaque;
    size_t section_identity;
} ScriptRemoteDocumentBinding;

typedef struct {
    BrowserSession *session;
    /* Immutable top-level document URL for network partitioning. Child
       runtimes receive their embedder's top-level URL, not their own URL. */
    const char *top_level_url;
    /* Optional borrowed page-wide scheduler domain from the same Budget. The
       runtime retains it for its lifetime; standalone runtimes lazily create
       a private bounded domain only if asynchronous network work begins. */
    struct FetchSchedulerDomain *fetch_scheduler_domain;
    /* Borrowed current cascade for CSS.supports() during bootstrap-time
       author execution. Deferred parsers may attach it before scripts run. */
    const Stylesheet *stylesheet;
    ViewportContext viewport;
    ScriptExecutionPolicy execution_policy;
    /* Zero selects the bounded standalone defaults. Navigation supplies the
       same limits used by its parser-oriented document script pipeline. */
    size_t maximum_scripts;
    size_t maximum_script_bytes;
    size_t maximum_script_file_bytes;
    bool defer_document_scripts;
    /* Host-test seam for deterministic transport fakes. Production and lab
       callers must leave this false: native realm primitives are otherwise
       installed non-writable, non-configurable, and non-enumerable. */
    bool allow_test_network_primitive_overrides;
    /* Applied only after trusted browser bootstrap evaluation. */
    bool dynamic_code_disabled;
    ScriptDocumentScope document_scope;
    const char *referrer_policy;
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
    size_t section_identity;
} ScriptRuntimeOptions;

/* Lab-only repeatability hook. It affects JavaScript entropy sources for
   subsequently-created runtimes; production/live callers leave it disabled.
   Each realm's stream is derived from the numeric seed, its serialized
   document URL, and its top-level/child-frame scope, so creation order and
   unrelated runtime scheduling cannot alter replay entropy. */
void script_runtime_configure_deterministic_replay(bool enabled,
                                                   unsigned long long seed);
/* Pure, allocation-free reference for the native
   `splitmix64-url-scope-v1` derivation. Exposing the initial state keeps lab
   capture tooling and C/JavaScript conformance vectors pinned to one exact
   byte-level contract without exposing mutable runtime state. The URL must
   already be the exact canonical serialized HTTP(S) URL; invalid,
   non-canonical, or overlong input returns zero. V1 intentionally gives
   same-URL sibling realms of the same scope the same stream because no stable
   frame-tree identity is yet available; creation order is never used. */
uint64_t script_runtime_deterministic_entropy_state_v1(
    uint64_t seed, const char *serialized_document_url,
    ScriptDocumentScope scope);
/* Returns a stable snapshot only for a successfully-created deterministic
   runtime. Before uint64_t saturation, each domain elapsed value is its host
   elapsed value plus the advancing observations assigned to that domain;
   stable monotonic samples are accounted separately. */
bool script_runtime_deterministic_replay_diagnostics(
    const ScriptRuntime *runtime,
    ScriptDeterministicReplayDiagnostics *diagnostics);

ScriptRuntime *script_runtime_create(PocDocument *document, Budget *budget,
                                     size_t js_memory_limit,
                                     unsigned timeout_ms,
                                     const char *document_url,
                                     ScriptResult *result);
bool script_runtime_rebind_document(ScriptRuntime *runtime,
                                    PocDocument *document,
                                    ScriptResult *result);
/* Retires every native reference owned by `document` while its Lexbor tree is
   still alive. Embedders must call this before destroying a document whose
   JavaScript realm will outlive it. The operation is bounded, allocation-free,
   and does not run author JavaScript. */
void script_runtime_detach_document(ScriptRuntime *runtime,
                                    const PocDocument *document);
/* Repairs host pointers after an embedding struct containing an unchanged
   document/runtime pair is moved. This is allocation-free and does not run
   author JavaScript or invalidate still-valid DOM handles. */
void script_runtime_relocate_document_storage(ScriptRuntime *runtime,
                                              PocDocument *document);
/* Replaces the complete live top-level binding in one non-allocating step.
   Pending materialization requests are invalidated before the old opaque
   values can be released. NULL clears the binding. Child-frame runtimes
   remain unbound and return false when passed a non-empty binding. */
bool script_runtime_rebind_remote_document(
    ScriptRuntime *runtime, const ScriptRemoteDocumentBinding *binding);
size_t script_runtime_collect_and_trim(ScriptRuntime *runtime);
/* Remaining active QuickJS heap allowance. Unlike the browser Budget this
   excludes allocator cache capacity and reflects JS_SetMemoryLimit(). */
size_t script_runtime_heap_remaining(const ScriptRuntime *runtime);
/* Inline bootstraps can expand DOM-embedded state independently of source
   length. This returns the bounded reserve selected from the configured heap
   ceiling; it never changes that ceiling. */
size_t script_runtime_inline_execution_reserve(
    const ScriptRuntime *runtime, size_t source_length);
void script_runtime_bound_allocator_cache(ScriptRuntime *runtime,
                                          size_t small_class_limit,
                                          size_t large_class_limit);
bool script_runtime_commit_same_document(ScriptRuntime *runtime,
                                         const char *url,
                                         const char *old_url,
                                         ScriptResult *result);
bool script_runtime_restore_same_document(ScriptRuntime *runtime,
                                          const char *url,
                                          const char *old_url,
                                          ScriptResult *result);
bool script_runtime_save_section_state(ScriptRuntime *runtime,
                                       size_t section, size_t *count,
                                       ScriptResult *result);
bool script_runtime_restore_section_state(ScriptRuntime *runtime,
                                          size_t section, size_t *count,
                                          ScriptResult *result);
bool script_runtime_take_remote_element_request(
    ScriptRuntime *runtime, char *identifier, size_t capacity,
    size_t *section_index);
bool script_runtime_set_section_identity(ScriptRuntime *runtime,
                                         size_t section_identity);
bool script_runtime_set_current_script_source(
    ScriptRuntime *runtime, size_t section_identity, size_t element_ordinal);
void script_runtime_clear_current_script_source(ScriptRuntime *runtime);
bool script_runtime_register_script_source_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node,
    size_t section_identity, size_t element_ordinal);
void script_runtime_clear_script_source_nodes(ScriptRuntime *runtime);
ScriptRuntime *script_runtime_create_with_session(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, ScriptResult *result);
ScriptRuntime *script_runtime_create_configured(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ScriptRuntimeOptions *options, ScriptResult *result);
ScriptRuntime *script_runtime_create_with_session_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, int css_viewport_width,
    int css_viewport_height, int device_width, int device_height,
    ScriptResult *result);
ScriptRuntime *script_runtime_create_deferred(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, ScriptResult *result);
ScriptRuntime *script_runtime_create_deferred_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, int css_viewport_width,
    int css_viewport_height, int device_width, int device_height,
    ScriptResult *result);
bool script_runtime_evaluate_inline(ScriptRuntime *runtime,
                                    lxb_dom_node_t *script_node,
                                    bool module, ScriptResult *result);
typedef enum {
    SCRIPT_INLINE_DATA_FALLBACK = 0,
    SCRIPT_INLINE_DATA_APPLIED,
    SCRIPT_INLINE_DATA_FAILED
} ScriptInlineDataEvaluation;
/* Applies a deliberately narrow classic-script subset without invoking the
   JavaScript compiler: up to eight window/globalThis property assignments
   whose values are strict JSON objects or arrays. Ambiguous syntax returns
   FALLBACK without author-visible effects; APPLIED and FAILED are terminal. */
ScriptInlineDataEvaluation script_runtime_evaluate_inline_data(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    size_t *source_bytes);
/* Refreshes Window named properties from the currently parsed DOM without
   retaining wrappers for unrelated descendants. Parser-driven loaders call
   this immediately before each author script so earlier markup is visible. */
bool script_runtime_refresh_named_properties(ScriptRuntime *runtime);
bool script_runtime_finish_loading(ScriptRuntime *runtime,
                                   ScriptResult *result);
/* Parser and native DOM owners call this after mutations which bypass the JS
   bridge so the next baseURI/resource preparation observes the live tree. */
void script_runtime_invalidate_document_base(ScriptRuntime *runtime);
/* Copies the current same-document URL after history.push/replaceState.
   Loader owners use this instead of the navigation's original URL when a
   later parser/deferred script resolves a relative resource. */
bool script_runtime_copy_document_url(
    const ScriptRuntime *runtime, char *output, size_t output_size);
bool script_runtime_copy_top_level_url(
    const ScriptRuntime *runtime, char *output, size_t output_size);
/* Returns the realm's lazily-created bounded network scheduler view. The
   runtime owns the view and its retained domain; loader contexts may borrow it
   until runtime destruction, including across document replacement. */
struct FetchScheduler *script_runtime_fetch_scheduler(
    ScriptRuntime *runtime);
void script_runtime_set_module_loader(
    ScriptRuntime *runtime, ScriptModuleLoadCallback load,
    ScriptModuleFreeCallback release, void *opaque);
void script_runtime_set_module_loader_owned(
    ScriptRuntime *runtime, ScriptModuleLoadCallback load,
    ScriptModuleFreeCallback release, void *opaque,
    ScriptModuleOpaqueDestroyCallback destroy_opaque);
/* child_context irreversibly changes the realm to child scope before its
   fallible global setup. parent_same_origin controls whether that child may
   replace the exposed parent.postMessage property; a cross-origin child gets
   the callable cross-origin surface but assignment raises SecurityError. The
   transition clears top-level remote bindings and queues; callback/opaque
   become callable only after setup succeeds. A child realm rejects a later
   child_context=false configuration. */
bool script_runtime_configure_messaging(
    ScriptRuntime *runtime, bool child_context, bool parent_same_origin,
    bool child_opaque_origin,
    ScriptPostMessageCallback callback, void *opaque);
/* Native loaders use this immutable realm property when deriving credentials
   and CORS origins for parser-inserted and module scripts. */
bool script_runtime_origin_is_opaque(const ScriptRuntime *runtime);
/* Updates only the already-configured native callback opaque after an
   embedding owner move; no JavaScript setup or callback is performed. */
void script_runtime_relocate_messaging_opaque(ScriptRuntime *runtime,
                                              void *opaque);
bool script_runtime_dispatch_message(ScriptRuntime *runtime,
                                     const char *json,
                                     const char *source_origin,
                                     long source_frame_handle,
                                     ScriptResult *result);
bool script_runtime_set_frame_window_state(ScriptRuntime *runtime,
                                           long frame_handle, bool active,
                                           bool same_origin,
                                           bool opaque_origin,
                                           const char *committed_url,
                                           ScriptResult *result);
void script_runtime_report_memory(ScriptRuntime *runtime, FILE *output);
/* Emits one fixed-size phase/realm census without evaluating author code or
   forcing collection. Suitable for high-water and pre-teardown PSP logging. */
void script_runtime_report_roots(ScriptRuntime *runtime, FILE *output,
                                 const char *phase, const char *realm,
                                 long realm_handle);
void script_runtime_intersection_recheck(ScriptRuntime *runtime);
/* Re-evaluate retained MediaQueryList listeners after viewport/layout state
   becomes authoritative. A realm without listeners makes this a cheap no-op. */
void script_runtime_media_recheck(ScriptRuntime *runtime);
/* Reconcile parser-owned DOM insertions with JavaScript MutationObservers
   before the next parser-blocking script executes, then perform the required
   microtask checkpoint. */
bool script_runtime_parser_mutation_checkpoint(ScriptRuntime *runtime);
bool script_runtime_evaluate_diagnostic(ScriptRuntime *runtime,
                                        const char *source,
                                        const char *source_url,
                                        ScriptResult *result);
bool script_runtime_record_resource_timing(ScriptRuntime *runtime,
                                           const char *url,
                                           const char *initiator_type);
long script_runtime_node_handle(ScriptRuntime *runtime,
                                lxb_dom_node_t *node);
/* Weak snapshots are generation-safe but do not retain a detached subtree.
   Use them for controller/layout and script-discovery frontiers. */
long script_runtime_node_weak_handle(ScriptRuntime *runtime,
                                     lxb_dom_node_t *node);
/* Resolves only a still-live generation-tagged native handle.  The check does
   not dereference a caller-retained DOM pointer, so snapshots remain safe
   after destructive author mutations invalidate and free their old nodes. */
lxb_dom_node_t *script_runtime_node_handle_resolve(
    const ScriptRuntime *runtime, long handle);
/* True after a post-parse script element has been admitted to the native
   dynamic loader. Document/section rescans use this to avoid executing the
   same node through the parser-oriented loader a second time. */
bool script_runtime_dynamic_script_is_scheduled(
    const ScriptRuntime *runtime, const lxb_dom_node_t *node);
/* Reserve before acquiring a cache body or starting a fetch. Borrowed cache
   metadata may first be inspected to request its exact body length. A
   DEFERRED result means another in-flight script temporarily owns all
   remaining byte capacity; callers that can wait must retry. REJECTED means
   the committed document limit is exhausted. NEW_EXECUTABLE claims one
   cumulative count on success; aborting releases bytes only, so failed
   attempts cannot bypass count limits. Commit charges the original,
   uninstrumented source length and consumes the reservation. */
ScriptQuotaReserveResult script_runtime_script_quota_reserve(
    ScriptRuntime *runtime, ScriptQuotaCountMode count_mode,
    size_t requested_max_bytes, ScriptQuotaReservation *reservation);
/* Allocation-free count admission probe for loader checkpoints. This is
   advisory until reserve/commit, but on the single-threaded parser boundary
   no author task can consume a slot between the probe and the claim. */
bool script_runtime_script_slot_available(const ScriptRuntime *runtime);
bool script_runtime_script_quota_commit(
    ScriptRuntime *runtime, ScriptQuotaReservation *reservation,
    size_t actual_source_bytes);
void script_runtime_script_quota_abort(
    ScriptRuntime *runtime, ScriptQuotaReservation *reservation);
/* Performs only bounded native fetch progress needed to settle dynamic-script
   byte reservations.  It never starts queued tasks, executes READY scripts,
   dispatches events, resolves author fetches, or runs JavaScript jobs. */
ScriptQuotaProgressResult script_runtime_script_quota_progress(
    ScriptRuntime *runtime, unsigned maximum_wait_ms);
bool script_runtime_advance(ScriptRuntime *runtime, unsigned elapsed_ms,
                            size_t callback_budget, ScriptResult *result);
bool script_runtime_dispatch(ScriptRuntime *runtime, const char *selector,
                             const char *event_type, ScriptResult *result);
bool script_runtime_dispatch_node(ScriptRuntime *runtime,
                                  lxb_dom_node_t *node,
                                  const char *event_type,
                                  ScriptResult *result);
bool script_runtime_dispatch_activation_node(ScriptRuntime *runtime,
                                             lxb_dom_node_t *node,
                                             ScriptResult *result);
bool script_runtime_dispatch_pointer_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons, ScriptResult *result);
bool script_runtime_dispatch_input_node(ScriptRuntime *runtime,
                                        lxb_dom_node_t *node,
                                        const char *event_type,
                                        const char *data,
                                        const char *input_type,
                                        const char *current_value,
                                        ScriptResult *result);
bool script_runtime_dispatch_submit_node(ScriptRuntime *runtime,
                                         lxb_dom_node_t *form,
                                         lxb_dom_node_t *submitter,
                                         ScriptResult *result);
bool script_runtime_evaluate_external(ScriptRuntime *runtime,
                                      lxb_dom_node_t *script_node,
                                      const char *source,
                                      size_t source_length,
                                      const char *source_url,
                                      ScriptResult *result);
bool script_runtime_evaluate_external_typed(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool module, ScriptResult *result);
bool script_runtime_evaluate_external_classic_cached(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, ScriptResult *result);
typedef enum {
    SCRIPT_MODULE_MAP_MISSING = 0,
    SCRIPT_MODULE_MAP_LOADING,
    SCRIPT_MODULE_MAP_EVALUATED,
    SCRIPT_MODULE_MAP_FAILED
} ScriptModuleMapStatus;
/* A module-map entry is keyed by its resolved request URL. Settled entries
   are reusable by later <script type=module> elements without another
   fetch, compile, evaluation, or executable-quota charge. */
ScriptModuleMapStatus script_runtime_module_map_status(
    const ScriptRuntime *runtime, const char *request_url);
/* External module identity and response base differ after redirects. Keep the
   request URL as the module-map key while resolving its imports and exposing
   import.meta.url from response_url. Classic callers may use the typed API. */
bool script_runtime_evaluate_external_module(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, ScriptResult *result);
bool script_runtime_evaluate_external_module_credentials(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, TilefinchCredentialsMode credentials,
    ScriptResult *result);
/* The response policy is the root module's already-computed effective policy:
   its final response override, when present, otherwise the document policy. */
bool script_runtime_evaluate_external_module_context(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials, ScriptResult *result);
/* The module loader reads this while compiling an active root module so the
   root script's credentials mode also applies to every transitive import. */
TilefinchCredentialsMode script_runtime_module_credentials(
    const ScriptRuntime *runtime);
/* Evaluates a preplanned canonical Webpack chunk with its module factories
   deferred. The runtime consumes source_lease only after the transformed
   registration has compiled and the native source registry is installed;
   FALLBACK guarantees that no author code ran and ownership was not taken. */
ScriptLazyEvaluation script_runtime_evaluate_external_lazy_webpack(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    const struct ScriptLazyWebpackPlan *plan,
    void *source_lease, ScriptSourceLeaseReleaseCallback release_source,
    ScriptResult *result);
bool script_runtime_consume_relayout(ScriptRuntime *runtime);
void script_runtime_telemetry(const ScriptRuntime *runtime,
                              ScriptRuntimeTelemetry *telemetry);
/* Moves the bounded mutation journal into result and clears the runtime copy.
   Returns false when no mutations were recorded. */
bool script_runtime_consume_mutations(ScriptRuntime *runtime,
                                      ScriptMutationJournal *result);
bool script_runtime_consume_navigation(ScriptRuntime *runtime, char *url,
                                       size_t url_size, bool *replace);
bool script_runtime_consume_media_request(ScriptRuntime *runtime,
                                          ScriptMediaRequest *request);
bool script_runtime_update_media_state(
    ScriptRuntime *runtime, int64_t node_handle, ScriptMediaState state,
    double current_time, double duration);
/* Consumes the successful default action of form.submit()/requestSubmit().
   The returned nodes belong to the current document and remain valid only
   until the caller starts another navigation. */
bool script_runtime_take_form_submission(
    ScriptRuntime *runtime, lxb_dom_node_t **form,
    lxb_dom_node_t **submitter);
bool script_runtime_consume_scroll(ScriptRuntime *runtime, int *scroll_y);
void script_runtime_set_layout(ScriptRuntime *runtime, LayoutDocument *layout,
                               int viewport_height);
void script_runtime_set_images(ScriptRuntime *runtime,
                               const ImageResources *images);
void script_runtime_set_stylesheet(ScriptRuntime *runtime,
                                   const Stylesheet *stylesheet);
void script_runtime_set_synchronous_layout_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque);
/* Updates the host-backed page viewport without compiling source or walking
   element scroll containers.  A changed position queues one coalesced window
   and visualViewport scroll notification for the next runtime tick. */
bool script_runtime_set_page_scroll(ScriptRuntime *runtime, int scroll_y);
/* Cancel transport-owned author work before a platform network stack is
   destroyed. Promise/event delivery remains on the next ordinary tick. */
size_t script_runtime_cancel_network(
    ScriptRuntime *runtime, const char *reason);
void script_runtime_destroy(ScriptRuntime *runtime);

bool scripts_run_document(PocDocument *document, Budget *budget,
                          size_t js_memory_limit, unsigned timeout_ms,
                          ScriptResult *result);
bool scripts_run_document_at(PocDocument *document, Budget *budget,
                             size_t js_memory_limit, unsigned timeout_ms,
                             const char *document_url,
                             ScriptResult *result);
bool scripts_run_document_at_context(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ViewportContext *viewport, ScriptResult *result);
bool scripts_run_document_at_context_with_policy(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ViewportContext *viewport,
    const ScriptExecutionPolicy *execution_policy, ScriptResult *result);
bool scripts_run_document_at_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    int css_viewport_width, int css_viewport_height,
    int device_width, int device_height, ScriptResult *result);

#endif
