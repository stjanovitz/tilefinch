/* Shared internals for the js_runtime.c orchestrator and the translation
   units split out of it.  Not part of the public Tilefinch API: frontends and
   other subsystems must keep using tilefinch/js_runtime.h. */
#ifndef TILEFINCH_JS_RUNTIME_INTERNAL_H
#define TILEFINCH_JS_RUNTIME_INTERNAL_H

#include <stddef.h>

#include "tilefinch/js_runtime.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/fetch.h"
#include "tilefinch/request_context.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/style.h"
#include "tilefinch/url.h"

#include <quickjs.h>
#include <lexbor/dom/interface.h>

#if defined(CONFIG_PROPERTY_FAULT_TRACE)
/* Lab-only extension supplied by the optional pinned-Bellard diagnostic
   patch. It is deliberately absent from upstream quickjs.h. */
void JS_SetPropertyFaultTraceLimit(JSRuntime *runtime, uint32_t limit);
#endif

/* Public PSP execution profiles refuse to compile any single script larger
   than this before calling into QuickJS; the bootstrap arrays below are
   statically bounded by the same ceiling. */
#define SCRIPT_PSP_MAXIMUM_HOST_COMPILE_BYTES (256u * 1024u)

typedef struct {
    const unsigned char *data;
    size_t bytecode_length;
    size_t source_length;
    uint64_t source_hash;
} BrowserBootstrapBytecode;

/* Bootstrap declarations share the generation manifest, preventing authored
   modules, embedded sources, and embedded bytecode from drifting apart. */
#define BOOTSTRAP_SOURCE(source_symbol, file_name, source_name,              \
                         bytecode_symbol)                                   \
    extern const char source_symbol[];                                      \
    extern const size_t source_symbol##_length;                             \
    extern const BrowserBootstrapBytecode bytecode_symbol;
#define BOOTSTRAP_DIAGNOSTIC(source_symbol, file_name, source_name)           \
    extern const char source_symbol[];                                      \
    extern const size_t source_symbol##_length;
#include "bootstrap/sources.def"
#undef BOOTSTRAP_DIAGNOSTIC
#undef BOOTSTRAP_SOURCE

/*
 * Release PSP builds do not retain a second, authored copy of every
 * bootstrap program.  Keep the call sites shared with host builds while
 * ensuring they contain no relocation to the source arrays; --gc-sections
 * can then discard src/generated/js_bootstrap.c's generated fallback sections.
 */
#if defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
#define TILEFINCH_BOOTSTRAP_SOURCE_ARGS(source_symbol, bytecode_symbol)      \
    NULL, (bytecode_symbol).source_length
#else
#define TILEFINCH_BOOTSTRAP_SOURCE_ARGS(source_symbol, bytecode_symbol)      \
    (source_symbol), (source_symbol##_length)
#endif

JSValue js_dom_set_custom_state(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_get_custom_state(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_parse_color(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);

/* Category-tagged allocation wrappers and bounded-capacity limits
   shared by the js_runtime translation units. */
/* Some mobile collapsible wrappers run whole article sections through a
   fragment builder; 64KB rejected the larger sections and
   left pages expanded.  Budget accounting still bounds the memory. */
#define DOM_INNER_HTML_LIMIT (256u * 1024u)
/* Text nodes carry no parse cost; some module loaders flush CSS as one
   buffered <style> text that routinely exceeds the
   innerHTML bound. */
#define DOM_TEXT_NODE_LIMIT (256u * 1024u)
#define DOM_MUTATION_BATCH_LIMIT 128u
#define DOM_SCROLL_INTENT_LIMIT 16u
#define SCRIPT_DEFAULT_SLOW_DEPENDENCY_US UINT64_C(16000)
#define SCRIPT_CRYPTO_DIGEST_INPUT_LIMIT (1024u * 1024u)
#define SCRIPT_LAZY_MAX_RETAINED_SOURCE_BYTES (6u * 1024u * 1024u)
#define SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT 8192u
#define SCRIPT_LAZY_RESIDENT_BUNDLE_LIMIT 64u
#define SCRIPT_LAZY_COMPILE_HEAP_MULTIPLIER 4u
#define SCRIPT_LAZY_COMPILE_FIXED_HEAP_BYTES (256u * 1024u)
#define SCRIPT_LAZY_COMPILE_EXECUTION_RESERVE_BYTES (256u * 1024u)
#define SCRIPT_LAZY_COMPILE_PAGE_RESERVE_BYTES (512u * 1024u)
#define SCRIPT_DYNAMIC_TASK_LIMIT 64u
#define SCRIPT_DYNAMIC_NODE_LIMIT 256u
#define SCRIPT_DYNAMIC_DEFAULT_FILE_BYTES (512u * 1024u)
#define SCRIPT_DYNAMIC_DEFAULT_TOTAL_BYTES (2u * 1024u * 1024u)
/* Concurrent async chunk fetches (bounded by the 8-entry async bridge)
   plus a synchronous module-loader fetch must all hold slots at once on
   script-heavy SPAs. */
#define SCRIPT_RUNTIME_FETCH_CONCURRENCY 12u
#define SCRIPT_REALM_MAXIMUM_SCRIPTS 256u
#define SCRIPT_REALM_MAXIMUM_FILE_BYTES (8u * 1024u * 1024u)
#define SCRIPT_REALM_MAXIMUM_TOTAL_BYTES (128u * 1024u * 1024u)
#define SCRIPT_DYNAMIC_EXECUTION_RESERVE_BYTES (512u * 1024u)
#define SCRIPT_DYNAMIC_LAZY_MINIMUM_BYTES (128u * 1024u)
#define SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT 4u
/* A fully hydrated long article exceeds 4096 nodes several times
   over; a truncated walk silently drops querySelectorAll matches (the
   mobile section transform only saw the first few sections).
   Still bounded per call to keep PSP worst cases finite. */
#define DOM_TRAVERSAL_VISIT_LIMIT 32768u

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (p), (s))

typedef struct {
    uint64_t started_ms;
    uint64_t deadline_ms;
#if defined(CONFIG_EXECUTION_PROFILE)
    uint64_t next_profile_ms;
#endif
    size_t polls;
    bool interrupted;
    ScriptResult *result;
    uint64_t slice_started_ns;
    size_t slice_work_units;
} Watchdog;

#define DOM_BRIDGE_NODE_LIMIT SCRIPT_DOM_HANDLE_SLOT_CAPACITY
#define BRIDGE_NODE_NATIVE_PIN 0x01u
#define BRIDGE_NODE_LIVE_WRAPPER 0x02u
#define BRIDGE_NODE_PENDING_RETIRE_NOTIFY 0x04u
#define DOM_BRIDGE_NODE_INDEX_BITS SCRIPT_DOM_HANDLE_INDEX_BITS
#define DOM_BRIDGE_NODE_INDEX_MASK SCRIPT_DOM_HANDLE_INDEX_MASK
#define DOM_BRIDGE_NODE_GENERATION_MAX \
    ((uint32_t) (INT32_MAX >> DOM_BRIDGE_NODE_INDEX_BITS))
#define SCRIPT_SOURCE_NODE_LIMIT 256

_Static_assert(DOM_BRIDGE_NODE_LIMIT <= DOM_BRIDGE_NODE_INDEX_MASK,
               "DOM bridge node handles must encode every slot");

typedef struct {
    lxb_dom_node_t *node;
    uintptr_t owner_document_identity;
    size_t section;
    char key[96];
} ScriptSourceNode;

typedef struct {
    uint64_t id;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
} ScriptAsyncFetch;

#define SCRIPT_EVENT_SOURCE_LIMIT 2u
#define SCRIPT_EVENT_SOURCE_PENDING_BYTES (64u * 1024u)

typedef struct {
    bool active;
    bool headers_pending;
    bool headers_valid;
    uint64_t id;
    struct DomBridge *bridge;
    TilefinchCredentialsMode credentials;
    unsigned char *pending;
    size_t pending_length;
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char response_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char error[160];
    FetchStreamOptions stream;
} ScriptEventSource;

typedef enum {
    SCRIPT_DYNAMIC_QUEUED = 0,
    SCRIPT_DYNAMIC_FETCHING,
    SCRIPT_DYNAMIC_READY
} ScriptDynamicState;

typedef struct {
    lxb_dom_node_t *node;
    uintptr_t owner_document_identity;
    uint32_t identity;
    bool programmatic;
    bool html;
    bool force_async;
    bool already_started;
} ScriptElementState;

typedef struct {
    int64_t node_handle;
    uint32_t identity;
} ScriptElementSnapshot;

typedef struct {
    ScriptDynamicState state;
    uint64_t sequence;
    uint64_t request_id;
    int64_t node_handle;
    lxb_dom_node_t *node;
    uintptr_t owner_document_identity;
    char *request_url;
    char *response_url;
    BrowserSharedBody *source_body;
    BrowserSharedBody *stale_body;
    ScriptResourceLoaderPlan resource_loader_plan;
    char *resource_loader_source;
    size_t resource_loader_source_capacity;
    ScriptQuotaReservation quota_reservation;
    size_t source_length;
    size_t resource_loader_preflight_statement;
    size_t resource_loader_statement;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    uint8_t incoming_referrer_policy;
    uint8_t effective_referrer_policy;
    bool active;
    bool module;
    bool ordered;
    bool success;
    bool stale_module_validated;
    bool stale_resource_grant_valid;
    TilefinchResourceGrant stale_resource_grant;
} ScriptDynamicTask;

typedef struct {
    lxb_dom_node_t *node;
    uintptr_t owner_document_identity;
    uint32_t sequence;
    int scroll_x;
    int scroll_y;
} ElementScrollIntent;

typedef struct DomBridge {
    PocDocument *document;
    Budget *budget;
    ScriptResult *result;
    bool *relayout_dirty;
    ScriptMutationJournal mutations;
    BrowserSession *session;
    char *document_url;
    const char *top_level_url;
    bool opaque_origin;
    char calculated_base_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool document_base_dirty;
    char referrer_policy[128];
    lxb_dom_node_t *nodes[DOM_BRIDGE_NODE_LIMIT];
    /* Owner identities are captured at registration so whole-document
       retirement never has to inspect a node which author mutation may have
       destroyed already. */
    uintptr_t node_owner_document_identities[DOM_BRIDGE_NODE_LIMIT];
    uint32_t node_generations[DOM_BRIDGE_NODE_LIMIT];
    uint32_t node_wrapper_leases[DOM_BRIDGE_NODE_LIMIT];
    /* Bit 0 pins a handle for native work; bit 1 records that the newest JS
       wrapper lease is still live. Sharing the byte avoids enlarging the
       bounded PSP handle table to track detached-node correctness. */
    unsigned char node_retention_flags[DOM_BRIDGE_NODE_LIMIT];
    size_t node_count;
    ScriptRuntime *host;
    ScriptPostMessageCallback post_message;
    void *post_message_opaque;
    uint64_t performance_origin_ns;
    unsigned fetch_timeout_ms;
    bool deterministic_entropy;
    uint64_t entropy_state;
    uint64_t replay_seed;
    bool deterministic_clock;
    uint64_t clock_origin_ms;
    uint64_t clock_host_elapsed_ms;
    uint64_t clock_wall_elapsed_ms;
    uint64_t clock_monotonic_elapsed_ms;
    uint64_t clock_source_counts[SCRIPT_CLOCK_SOURCE_COUNT];
    uint64_t frame_eval_count;
    /* Mirrors the native QuickJS CSP gate for delayed frame-eval helpers.
       A trusted evaluator is compiled during bootstrap, before author policy
       is armed, so every later invocation must independently consult this
       non-page-writable bit. */
    bool dynamic_code_allowed;
    size_t console_messages;
    bool navigation_requested;
    bool navigation_replace;
    char navigation_url[2048];
    bool media_requested;
    bool media_audio_only;
    ScriptMediaCommand media_command;
    TilefinchRequestMode media_mode;
    TilefinchCredentialsMode media_credentials;
    int64_t media_node_handle;
    double media_value;
    char *media_source;
    bool scroll_requested;
    int scroll_y;
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
    size_t remote_element_sections[8];
    char remote_element_identifiers[8][129];
    size_t remote_element_head;
    size_t remote_element_count;
    bool remote_lookup_suppressed;
    size_t section_identity;
    bool current_script_source_active;
    char current_script_source_key[96];
    size_t current_script_source_section;
    ScriptSourceNode *script_source_nodes;
    size_t script_source_node_count;
    size_t script_source_node_capacity;
    LayoutDocument *layout;
    const ImageResources *images;
    ScriptSynchronousLayoutCallback synchronous_layout;
    void *synchronous_layout_opaque;
    ElementScrollIntent scroll_intents[DOM_SCROLL_INTENT_LIMIT];
    size_t scroll_intent_count;
    uint32_t scroll_intent_sequence;
    const Stylesheet *stylesheet;
    int viewport_width;
    int viewport_height;
    int page_scroll_y;
    FetchSchedulerDomain *fetch_scheduler_domain;
    FetchScheduler *fetch_scheduler;
    ScriptAsyncFetch async_fetches[8];
    size_t async_fetch_count;
    ScriptEventSource event_sources[SCRIPT_EVENT_SOURCE_LIMIT];
    size_t event_source_count;
    ScriptDynamicTask dynamic_scripts[SCRIPT_DYNAMIC_TASK_LIMIT];
    ScriptElementState script_elements[SCRIPT_DYNAMIC_NODE_LIMIT];
    size_t script_element_count;
    uint32_t next_script_element_identity;
    size_t dynamic_script_count;
    size_t dynamic_script_bytes;
    size_t script_quota_count;
    size_t script_quota_bytes;
    size_t script_quota_reserved_bytes;
    size_t maximum_scripts;
    size_t maximum_script_bytes;
    size_t maximum_script_file_bytes;
    bool allow_test_network_primitive_overrides;
    uint64_t dynamic_script_sequence;
    ScriptExecutionPolicy execution_policy;
    JSValue current_script;
    uintptr_t current_script_owner_document_identity;
    JSValue trusted_node_wrap;
    JSValue trusted_stable_script_wrap;
    JSValue trusted_retire_native_node_state;
} DomBridge;

_Static_assert(SCRIPT_CLOCK_SOURCE_COUNT == 12,
               "deterministic clock source evidence layout changed");

typedef struct {
    ScriptResult *result;
    void *promises[32];
    size_t count;
    void *last_reported;
    const char *active_source;
} PromiseRejectionState;

typedef struct {
    char *request_url;
    char *response_url;
    TilefinchCredentialsMode credentials;
    uint16_t parent_index;
    uint8_t effective_referrer_policy;
    uint8_t root_state;
} ScriptModuleBaseEntry;

typedef enum {
    SCRIPT_HOST_REFRESH_NAMED_PROPERTIES = 0,
    SCRIPT_HOST_PARSER_MUTATION_CHECKPOINT,
    SCRIPT_HOST_RECEIVE_MESSAGE,
    SCRIPT_HOST_SET_FRAME_WINDOW_STATE,
    SCRIPT_HOST_INTERSECTION_RECHECK,
    SCRIPT_HOST_MEDIA_RECHECK,
    SCRIPT_HOST_RECORD_RESOURCE_TIMING,
    SCRIPT_HOST_PENDING_TIMERS,
    SCRIPT_HOST_PENDING_NETWORK_REQUESTS,
    SCRIPT_HOST_DELIVER_NETWORK,
    SCRIPT_HOST_PUMP_TIMERS,
    SCRIPT_HOST_REBIND_DOCUMENT,
    SCRIPT_HOST_COMMIT_SAME_DOCUMENT,
    SCRIPT_HOST_RESTORE_SAME_DOCUMENT,
    SCRIPT_HOST_SAVE_SECTION_STATE,
    SCRIPT_HOST_RESTORE_SECTION_STATE,
    SCRIPT_HOST_CALLBACK_COUNT
} ScriptHostCallback;

struct ScriptRuntime {
    Budget *budget;
    PocDocument *document;
    ScriptDocumentScope document_scope;
    JSRuntime *runtime;
    JSContext *context;
    BudgetQuickJSPool *quickjs_pool;
    /* Boot-window donation experiment (TILEFINCH_JS_BOOT_WINDOW_KB): the page
       heap opens at base_memory_limit plus the donated window; once the
       hydration transient collects back under the base, the limit shrinks
       to the base and the window returns to the shared engine budget. */
    size_t base_memory_limit;
    size_t boot_window_bytes;
    bool boot_window_active;
    size_t boot_window_peak;
    uint64_t boot_window_advances;
    uint64_t boot_window_returned_advance;
    uint64_t deterministic_gc_advances;
    /* 0=deferred, 1=loading, 2=loaded, 3=failed. Uncommon platform modules
       keep their bytecode in ROM but do not occupy the page heap until a page
       first touches their standards-visible surface. */
    uint8_t lazy_bootstrap_state[SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT];
    Watchdog watchdog;
    DomBridge bridge;
    ScriptResult result;
    PromiseRejectionState promise_rejection_state;
    unsigned timeout_ms;
    size_t refreshed_mutations;
    bool relayout_dirty;
    BrowserSession *session;
    char document_url[2048];
    char top_level_url[2048];
    ScriptModuleLoadCallback module_load;
    ScriptModuleFreeCallback module_release;
    void *module_opaque;
    ScriptModuleOpaqueDestroyCallback module_opaque_destroy;
    ScriptModuleBaseEntry *module_bases;
    size_t module_base_count;
    size_t module_base_capacity;
    TilefinchCredentialsMode active_module_credentials;
    size_t inline_module_sequence;
    /* Captured before author scripts run. Keeping the callable values here
       avoids a global lookup and prevents author replacement of private host
       bridge properties from intercepting native viewport updates. */
    JSValue page_scroll_apply;
    JSValue page_scroll_flush;
    JSValue dispatch_at;
    JSValue dispatch_handle;
    JSValue dispatch_activation_handle;
    JSValue dispatch_input_handle;
    JSValue dispatch_submit_handle;
    JSValue dom_content_loaded_dispatch;
    /* Captured and removed before author code. Native <video> activation
       needs the bootstrap's private WeakMap record even when play() was not
       the entry point. */
    JSValue media_state_for;
    /* Created lazily as a private callable, never published to page script. */
    JSValue media_update;
    JSValue function_to_string;
    /* Trusted bootstrap callbacks retained before author code runs. The page
       may observe compatibility globals, but native scheduling and document
       lifecycle never look them up through the mutable Window object. */
    JSValue host_global;
    JSValue host_callbacks[SCRIPT_HOST_CALLBACK_COUNT];
    struct ScriptLazyRuntimeBundle *lazy_webpack_bundles;
    uint32_t next_lazy_webpack_bundle_id;
    bool lazy_factory_recovery_pending;
};

typedef struct {
    size_t key_offset;
    size_t key_length;
    size_t source_offset;
    size_t source_length;
    unsigned char *compressed_source;
    size_t compressed_source_length;
    size_t compressed_source_capacity;
    uint8_t arity;
    ScriptLazyFactoryKind kind;
    JSValue compiled;
    JSValue wrapper;
} ScriptLazyRuntimeFactory;

typedef struct ScriptLazyRuntimeBundle {
    uint32_t identifier;
    const char *source;
    size_t source_length;
    size_t compressed_source_length;
    bool strict_mode;
    char *source_url;
    ScriptLazyRuntimeFactory *factories;
    size_t factory_count;
    size_t compiled_count;
    void *source_lease;
    ScriptSourceLeaseReleaseCallback release_source;
    struct ScriptLazyRuntimeBundle *next;
} ScriptLazyRuntimeBundle;

typedef struct {
    JSValue value;
    uintptr_t owner_document_identity;
} ScriptCurrentScriptScope;

/* Cross-module helpers defined in js_runtime.c. */
void js_rt_saturating_add_u64(uint64_t *value, uint64_t amount);
bool js_rt_bridge_node_slot_for_handle(const DomBridge *bridge,
                                       int64_t handle, size_t *slot);
int64_t js_rt_bridge_register_node(DomBridge *bridge, lxb_dom_node_t *node);
void js_rt_record_exception(JSContext *context, ScriptResult *result);
void js_rt_capture_error_source_context(const char *source, size_t length,
                                        const char *source_url,
                                        ScriptResult *result);
bool js_rt_current_script_scope_begin(JSContext *context,
                                      lxb_dom_node_t *node, bool module,
                                      ScriptCurrentScriptScope *previous,
                                      ScriptResult *result);
bool js_rt_current_script_scope_end(JSContext *context,
                                    ScriptCurrentScriptScope *previous,
                                    ScriptResult *result);
void js_rt_runtime_arm_watchdog(ScriptRuntime *runtime);
bool js_rt_runtime_run_jobs(ScriptRuntime *runtime);
bool js_rt_runtime_refresh(ScriptRuntime *runtime);
void js_rt_runtime_update_result(ScriptRuntime *runtime,
                                 ScriptResult *result);
uint64_t js_rt_monotonic_time_ns(void);
void js_rt_saturating_add_size(size_t *value, size_t amount);
void js_rt_trace_script_quota_rejection(DomBridge *bridge,
                                        const char *reason);
bool js_rt_runtime_script_checkpoint(ScriptRuntime *runtime,
                                     size_t work_units);
bool js_rt_runtime_callback_checkpoint(ScriptRuntime *runtime);
JSValue js_rt_compile_source_type(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   int evaluation_type,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result, bool *admitted);
bool js_rt_admit_cached_compile_source(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result);
bool js_rt_evaluate_compiled_source(
    JSContext *context, JSValue compiled, const char *name,
    size_t source_length, ScriptResult *result);
void js_rt_runtime_record_host_callback(ScriptRuntime *runtime,
                                        const char *name,
                                        uint64_t started_ns,
                                        size_t polls_before);

bool js_rt_evaluate_source_type_at(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   const char *module_base_url,
                                   const char *module_referrer_policy,
                                   TilefinchCredentialsMode module_credentials,
                                   int evaluation_type,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result);
bool js_rt_evaluate_source(JSContext *context, const char *source,
                           size_t length, const char *name,
                           ScriptResult *result);
const char *js_rt_bridge_calculated_base_url(DomBridge *bridge);

/* js_module_loader.c helpers used by the sibling translation units. */
bool js_rt_module_set_import_meta(JSContext *context, JSValueConst module,
                                  const char *response_url, bool is_main);
uint8_t js_rt_runtime_module_referrer_policy_code(const char *policy);
const char *js_rt_runtime_module_referrer_policy_text(uint8_t code);
TilefinchCredentialsMode js_rt_module_credentials_for_node(
    lxb_dom_node_t *node);
void js_rt_module_referrer_policy_for_node(
    lxb_dom_node_t *node, const char *fallback,
    char output[BROWSER_MODULE_REFERRER_POLICY_LIMIT]);
void js_rt_runtime_module_metadata_clear(ScriptRuntime *runtime);
bool js_rt_runtime_module_root_register(
    ScriptRuntime *runtime, const char *request_url, const char *response_url,
    const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials);
bool js_rt_runtime_module_root_state_set(
    ScriptRuntime *runtime, const char *request_url,
    ScriptModuleMapStatus state);
const char *js_rt_runtime_module_base_lookup(const ScriptRuntime *runtime,
                                             const char *request_url);

bool js_rt_install_function(JSContext *context, JSValue global,
                            const char *name, JSCFunction *function,
                            int arguments);
char *js_rt_dynamic_copy_text(Budget *budget, const char *text);
void js_rt_dynamic_task_clear(DomBridge *bridge, ScriptDynamicTask *task,
                              bool cancelled, bool cancel_request);

ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve(
    DomBridge *bridge, ScriptQuotaCountMode count_mode,
    size_t requested_max_bytes, ScriptQuotaReservation *reservation);
bool js_rt_bridge_script_quota_commit(
    DomBridge *bridge, ScriptQuotaReservation *reservation,
    size_t actual_source_bytes);
void js_rt_bridge_script_quota_abort(
    DomBridge *bridge, ScriptQuotaReservation *reservation);
lxb_dom_node_t *js_rt_bridge_node_arg(JSContext *context,
                                      DomBridge *bridge,
                                      JSValueConst value);
bool js_rt_network_error_is_timeout(const char *error);
int js_rt_dynamic_prepare_subtree(JSContext *context,
                                  lxb_dom_node_t *root);

/* js_fetch_cors.c entry points. */
bool js_fetch_cors_install(JSContext *context, JSValue global);
void js_rt_event_sources_destroy(DomBridge *bridge);
bool js_rt_event_sources_deliver(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 size_t *author_tasks);
void js_rt_record_network_response(ScriptResult *result,
                                   const FetchResult *fetched);
void js_rt_script_set_response_body(JSContext *context, JSValue response,
                                    const FetchResult *fetched);
void js_rt_script_store_response_cookies(
    DomBridge *bridge, const FetchResult *fetched, const char *fallback_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    TilefinchRequestDestination destination);
bool js_rt_script_response_origin_allowed(
    const DomBridge *bridge, const FetchResult *fetched,
    const char *request_url_or_origin,
    TilefinchRequestDestination destination, TilefinchRequestMode mode,
    TilefinchCredentialsMode credentials);
size_t js_rt_script_visible_response_headers(
    const DomBridge *bridge, const FetchResult *fetched,
    TilefinchCredentialsMode credentials, char *output, size_t capacity);
bool js_rt_dynamic_start_task(ScriptRuntime *runtime,
                              ScriptDynamicTask *task, bool *deferred);
bool js_rt_dynamic_take_completion(ScriptRuntime *runtime,
                                   ScriptDynamicTask *task);
bool js_rt_dynamic_execute_ready(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 size_t *completed_out);
bool js_rt_dynamic_classic_continuation_pending(
    const ScriptRuntime *runtime);
ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve_bounded(
    DomBridge *bridge, ScriptQuotaCountMode count_mode,
    size_t requested_max_bytes, size_t reservation_ceiling,
    ScriptQuotaReservation *reservation);
ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve_known(
    DomBridge *bridge, ScriptQuotaCountMode count_mode,
    size_t exact_source_bytes, ScriptQuotaReservation *reservation);
bool js_rt_bridge_script_quota_expand(
    DomBridge *bridge, ScriptQuotaReservation *reservation,
    size_t exact_source_bytes);
bool js_rt_evaluate_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool final_segment);
bool js_rt_preflight_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url);

void js_rt_bridge_queue_remote_element(DomBridge *bridge,
                                       const char *identifier,
                                       size_t length, size_t section);

JSValue js_rt_wrap_dom_handle(JSContext *context, JSValueConst handle);

/* js_remote_bindings.c entry points. */
void js_rt_remote_node_read_result_destroy(
    DomBridge *bridge, ScriptRemoteNodeReadResult *read);
bool js_remote_bindings_install(JSContext *context, JSValue global);
JSValue js_remote_node_writer_active(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv);
JSValue js_remote_lookup_suppress(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv);
JSValue js_remote_selector_result(JSContext *context,
                                  JSValueConst selector,
                                  bool earlier_only);
JSValue js_remote_selector_collection(JSContext *context,
                                      JSValueConst selector,
                                      JSValueConst local_values,
                                      uint32_t local_length,
                                      uint32_t *result_length);
JSValue js_remote_descendant_collection(JSContext *context,
                                        DomBridge *bridge,
                                        unsigned root_special,
                                        int32_t what_to_show);

typedef struct {
    lxb_dom_node_t *next;
    const lxb_dom_node_t *boundary;
    size_t visited;
    size_t next_node_ordinal;
    size_t next_element_ordinal;
    size_t current_node_ordinal;
    size_t current_element_ordinal;
    size_t next_depth;
    size_t current_depth;
    bool track_ordinals;
} DomDocumentOrderTraversal;

uintptr_t js_rt_node_owner_identity(const lxb_dom_node_t *node);
bool js_rt_node_is_strict_descendant(const lxb_dom_node_t *node,
                                     const lxb_dom_node_t *ancestor);
ScriptElementState *js_rt_script_element_state_find(
    DomBridge *bridge, const lxb_dom_node_t *node);
ScriptElementState *js_rt_script_element_state_register(
    DomBridge *bridge, lxb_dom_node_t *node, bool html);
void js_rt_script_element_states_mark_descendants(
    const DomBridge *bridge, const lxb_dom_node_t *ancestor,
    unsigned char marked[SCRIPT_DYNAMIC_NODE_LIMIT]);
void js_rt_script_element_states_purge_marked(
    DomBridge *bridge,
    const unsigned char marked[SCRIPT_DYNAMIC_NODE_LIMIT]);

/* js_dom_bindings.c symbols referenced by the orchestrator (install
   chain and shared node/mutation machinery). */
void bridge_invalidate_node_slot(DomBridge *bridge, size_t slot);
void bridge_release_native_node_pin(DomBridge *bridge, int64_t handle);
bool js_rt_bridge_flush_synchronous_layout(DomBridge *bridge);
bool bridge_node_is_connected(const lxb_dom_node_t *node);
lxb_dom_node_t *dom_document_order_next(
    DomDocumentOrderTraversal *traversal);
JSValue js_computed_style_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_append(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv);
JSValue js_dom_prepare_dynamic_subtree(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv);
JSValue js_dom_append_many(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_attributes(JSContext *context,
                          JSValueConst this_value,
                          int argc, JSValueConst *argv);
JSValue js_dom_body(JSContext *context, JSValueConst this_value,
                    int argc, JSValueConst *argv);
JSValue js_dom_child_nodes(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_document_child_nodes(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
JSValue js_dom_children(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_dom_clone(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv);
JSValue js_dom_content(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_create(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv);
JSValue js_dom_create_comment(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_create_fragment(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_dom_create_text(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_descendants(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_named_element_ids(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_document_element(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_get_attribute(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_image_property(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_get_element_by_id_method(
JSContext *context, JSValueConst this_value,
int argc, JSValueConst *argv);
JSValue js_dom_get_inner_html(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_get_text(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_dom_insert_before(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_is_connected(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_dom_matches(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_node_type(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_namespace_uri(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_query(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv);
JSValue js_dom_query_all(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_query_count(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_query_selector_all_method(JSContext *context,
                                         JSValueConst this_value,
                                         int argc,
                                         JSValueConst *argv);
JSValue js_dom_query_selector_method(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv);
JSValue js_dom_record_event(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_dom_record_event_handler(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
JSValue js_dom_relation(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_dom_release_node_wrapper(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
JSValue js_dom_remove(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv);
JSValue js_dom_remove_attribute(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_retain_node_wrapper(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv);
JSValue js_dom_set_attribute(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_set_control_value(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_parser_form_owner(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_has_parser_form_owners(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv);
JSValue js_runtime_heap_remaining(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv);
JSValue js_dom_set_inner_html(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_set_text(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_dom_tag_name(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_find_stable_node(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_section_identity(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_stable_node_key(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_style_get(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv);
JSValue js_style_set(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv);

/* js_lazy_webpack.c entry points used by the runtime orchestrator. */
void js_lazy_webpack_recover_failure(ScriptRuntime *runtime);
void js_lazy_webpack_bundles_destroy(ScriptRuntime *runtime);
bool js_lazy_webpack_install(ScriptRuntime *runtime, JSValue global);

#endif
