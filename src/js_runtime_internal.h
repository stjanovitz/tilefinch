/* Shared internals for the js_runtime.c orchestrator and the translation
   units split out of it.  Not part of the public Tilefinch API: frontends and
   other subsystems must keep using tilefinch/js_runtime.h. */
#ifndef TILEFINCH_JS_RUNTIME_INTERNAL_H
#define TILEFINCH_JS_RUNTIME_INTERNAL_H

#include <ctype.h>
#include <stddef.h>
#include "diagnostic_trace.h"
#include "tilefinch_test_faults.h"

#include "tilefinch/media_capture.h"
#include "tilefinch/page_dialog.h"
#include "tilefinch/js_runtime.h"
#ifndef __PSP__
/* Numeric ownership observation for host tests, never a page hook. */
bool js_rt_audio_slot_test_snapshot(ScriptRuntime *runtime, unsigned counts[5]);
#endif
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/fetch.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/request_context.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/style.h"
#include "tilefinch/url.h"
#include "style_cache_internal.h"
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
#include "validation_cpu_internal.h"
#endif

#include <quickjs.h>
#include <lexbor/dom/interface.h>

#if defined(CONFIG_PROPERTY_FAULT_TRACE)
/* Lab-only extension supplied by the optional pinned-Bellard diagnostic
   patch. It is deliberately absent from upstream quickjs.h. */
void JS_SetPropertyFaultTraceLimit(JSRuntime *runtime, uint32_t limit);
#endif

/* Host and validation builds carry the realm's test and lab instruments:
   the bootstrap's test-only probes (`__tilefinchTestProbes`), the lab
   summary channel (`pocSummary`) and frame-eval telemetry. The shipping PSP
   build compiles them out. */
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define JS_RT_TEST_INSTRUMENTS 1
#endif

/* The private-session XHR history (script_runtime_xhr_diagnostics) is host
   lab instrumentation; no PSP build has a reader for it. */
#if !defined(__PSP__)
#define JS_RT_XHR_DIAGNOSTICS 1
#define JS_RT_XHR_COUNT(owner, field)                                       \
    do {                                                                    \
        const ScriptRuntime *xhr_owner_ = (owner);                          \
        if (xhr_owner_ != NULL && xhr_owner_->xhr_diagnostics != NULL)      \
            js_rt_saturating_add_size(                                      \
                &xhr_owner_->xhr_diagnostics->field, 1);                    \
    } while (0)
#else
#define JS_RT_XHR_COUNT(owner, field) ((void) (owner))
#endif

/* Trusted bootstrap sources and authored page scripts have deliberately
   separate ceilings. Standards shims may consume the user-approved 3 KiB of
   internal headroom without silently widening a page's longest
   non-preemptible compile unit. */
#define SCRIPT_BOOTSTRAP_STRICT_MAXIMUM_HOST_COMPILE_BYTES (275u * 1024u)
#define SCRIPT_PSP_STRICT_MAXIMUM_HOST_COMPILE_BYTES (256u * 1024u)
/* The realistic profile's compile unit is the source ceiling; memory, not
   size, decides admission (script_admission.h). */
#define SCRIPT_PSP_MAXIMUM_HOST_COMPILE_BYTES (4u * 1024u * 1024u)

typedef struct {
    const unsigned char *data;
    size_t bytecode_length;
    size_t source_length;
    uint64_t source_hash;
} BrowserBootstrapBytecode;

/* The atoms the embedded bytecode names by index: pinned first in every
   runtime that restores it (JS_PinAtomTable), so its functions run from
   ROM without a per-realm copy. */
extern const unsigned char browser_bootstrap_atom_table[];
extern const size_t browser_bootstrap_atom_table_length;
extern const uint32_t browser_bootstrap_atom_count;

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
#define DOM_MUTATION_BATCH_LIMIT 128u
#define DOM_SCROLL_INTENT_LIMIT 16u
#define SCRIPT_DEFAULT_SLOW_DEPENDENCY_US UINT64_C(16000)
#define SCRIPT_CRYPTO_DIGEST_INPUT_LIMIT (1024u * 1024u)
/* A Web Crypto slice's time (PBKDF2 and the jobs of src/web_crypto.c). */
#define SCRIPT_CRYPTO_SLICE_NS UINT64_C(8000000)
#define SCRIPT_LAZY_MAX_RETAINED_SOURCE_BYTES (6u * 1024u * 1024u)
#define SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT 8192u
/* Browser-authored, non-page-controlled constructor facade installed into a
   fresh DedicatedWorkerGlobalScope before it is published. Keep it bounded
   independently of author script admission; the facade includes the local
   XHR/FileReader/WebSocket/Worker and realm-owned encoding compatibility
   surfaces. The source is already resident in the lazy bootstrap; this cap
   bounds only its temporary handoff into the fresh realm. */
#define SCRIPT_WORKER_REALM_INITIALIZER_MAX_BYTES (32u * 1024u)
#define SCRIPT_LAZY_RESIDENT_BUNDLE_LIMIT 64u
#define SCRIPT_DYNAMIC_TASK_LIMIT 64u
#define SCRIPT_DYNAMIC_NODE_LIMIT 256u
/* The script-clone census (clones of started scripts and their
   suppressed insertions) is a host-lab diagnostic; the shipping PSP build
   compiles it out. */
#if defined(__PSP__)
#define TILEFINCH_SCRIPT_CLONE_CENSUS 0
#else
#define TILEFINCH_SCRIPT_CLONE_CENSUS 1
#endif
#define SCRIPT_DYNAMIC_DEFAULT_FILE_BYTES (512u * 1024u)
#define SCRIPT_DYNAMIC_DEFAULT_TOTAL_BYTES (2u * 1024u * 1024u)
/* Concurrent async chunk fetches (bounded by the 8-entry async bridge)
   plus a synchronous module-loader fetch must all hold slots at once on
   script-heavy SPAs. */
#define SCRIPT_RUNTIME_FETCH_CONCURRENCY 12u
/* Dynamic script requests in flight at once (the network pump stops
   starting queued scripts at this many pending requests). The realm's
   fetch scheduler bounds the sum of their response bounds at this many
   times the realm's source total, so one stalled response never holds the
   bound the others need (m.vk.ru: a tracker host that answered nothing for
   over 5 s used to serialize every later dynamic script behind it). */
#define SCRIPT_DYNAMIC_INFLIGHT_REQUESTS 4u
/* The share of that pool fetch() responses may hold at once. */
#define SCRIPT_RUNTIME_FETCH_POOL_BYTES (2u * 1024u * 1024u)
/* fetch() requests that always fit that share at once, however large one
   response's bound is: each in-flight request is charged at most
   1/SCRIPT_RUNTIME_FETCH_POOL_MINIMUM_REQUESTS of the pool. */
#define SCRIPT_RUNTIME_FETCH_POOL_MINIMUM_REQUESTS 2u
/* Keep a useful bounded script unit admissible when retained page state
   consumes the presentation reserve. A 64 KiB floor truncated ordinary
   late dependencies even with several MiB free. This is a response ceiling,
   not a reservation: source quotas, actual Budget allocations and compiler
   admission still apply. Larger responses keep the presentation reserve:
   script_admission_affordable_bytes. */
#define SCRIPT_DYNAMIC_MINIMUM_RESPONSE_BYTES (256u * 1024u)
#define SCRIPT_REALM_MAXIMUM_SCRIPTS 256u
#define SCRIPT_REALM_MAXIMUM_FILE_BYTES (8u * 1024u * 1024u)
#define SCRIPT_REALM_MAXIMUM_TOTAL_BYTES (128u * 1024u * 1024u)
#define SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT 29u
/* A fully hydrated long article exceeds 4096 nodes several times
   over; a truncated walk silently drops querySelectorAll matches (the
   mobile section transform only saw the first few sections).
   Still bounded per call to keep PSP worst cases finite. */
#define DOM_TRAVERSAL_VISIT_LIMIT 32768u

/* The heap composition census (JS_ComputeMemoryDetail): the host lab's
   TILEFINCH_TRACE_JS_HEAP_DETAIL, and PSP validation builds' report-time
   census (validation_js_heap_detail). */
#if defined(CONFIG_TILEFINCH_HEAP_DETAIL) \
    && (!defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG))
#define TILEFINCH_JSMEM_CENSUS 1
#endif

#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
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
    /* The realm's pool and its refusal count when this entry began: an
       entry the pool keeps refusing is out of memory (interrupt_handler). */
    const BudgetQuickJSPool *pool;
    size_t pool_rejections_started;
    bool memory_exhausted;
} Watchdog;

/* Refused heap requests within one entry into JavaScript after which the
   entry stops as out of memory. Page code that catches the out-of-memory
   error and tries again at the realm's limit otherwise spins until the
   execution deadline (136 million refusals in 60 s in one replay). An entry that
   recovers from a few refusals (a smaller retry, a cache dropped) stays
   far below it. */
#define SCRIPT_ENTRY_REFUSAL_LIMIT 256u

/* The table's default ceiling; below it a full table grows before it
   reclaims (js_rt_bridge_register_node). A policy may raise the ceiling to
   DOM_BRIDGE_NODE_LIMIT_MAX (see DomBridge.node_capacity); growth past
   DOM_BRIDGE_NODE_BUDGET_CAPACITY is memory-admitted
   (bridge_node_table_grow). */
#define DOM_BRIDGE_NODE_LIMIT SCRIPT_DOM_HANDLE_SLOT_CAPACITY
#define DOM_BRIDGE_NODE_LIMIT_MAX SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX
#define DOM_BRIDGE_NODE_BUDGET_CAPACITY SCRIPT_DOM_HANDLE_SLOT_BUDGET_CAPACITY
#define DOM_BRIDGE_NODE_GROWTH_STEP SCRIPT_DOM_HANDLE_SLOT_GROWTH_STEP
#define DOM_BRIDGE_WRAPPER_PLAIN_KEYS 7
/* Live shadow roots per realm. Admission is memory-based, like script
   admission (js_dom_register_shadow_root): a root is admitted while the
   page Budget keeps SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES free (for
   the layout and stylesheet work the new content needs) beside
   DOM_BRIDGE_SHADOW_ROOT_COST_BYTES. That cost is an upper bound on what a
   component root takes with its content: the 64-bit host measures a
   shared-prototype root with one adopted sheet and a small subtree at
   6,757 bytes, layout included (test_shadow_root_memory_admission), and
   the PSP's 32-bit build needs less. The first DOM_BRIDGE_SHADOW_ROOT_BASE roots are always
   admitted; their allocations still need the Budget like any others.
   Component sites nest roots (the Internet Archive's search results
   attach six per result tile, and its infinite scroll keeps adding
   tiles), so a fixed share of the budget per root refused real content
   while memory was free.

   DOM_BRIDGE_SHADOW_ROOT_LIMIT is the hard bound the indices need: the
   registry below (Budget-allocated, grown on demand) and the tables that
   mirror it, shadow.js's identity table (ROOT_LIMIT),
   DOCUMENT_ADOPTION_ROOT_LIMIT (one adopted-sheet list per root) and
   STYLE_SHADOW_SCOPE_LIMIT (one <style> scope per root). At the measured
   cost 2048 roots need 14 MiB, so on a 32 MiB page memory decides first. */
#define DOM_BRIDGE_SHADOW_ROOT_LIMIT 2048u
#define DOM_BRIDGE_SHADOW_ROOT_COST_BYTES (16u * 1024u)
/* Tainted canvases a realm can hold; past it a cross-origin draw is refused. */
#define DOM_BRIDGE_CANVAS_TAINT_LIMIT 64u
/* Native stores of ImageBitmaps that are not origin-clean; canvas.js keeps
   at most this many bitmaps alive. */
#define DOM_BRIDGE_CANVAS_BITMAP_LIMIT 8u
#define DOM_BRIDGE_SHADOW_ROOT_BASE 64u
#define BRIDGE_NODE_NATIVE_PIN 0x01u
#define BRIDGE_NODE_LIVE_WRAPPER 0x02u
#define BRIDGE_NODE_PENDING_RETIRE_NOTIFY 0x04u
/* Where the bootstrap keeps the node's event state (listener and handler
   maps): in its per-handle maps (ROOTED), in a holder only the live
   wrappers of the node's detached tree reference (HELD), or on the node's
   own wrapper (KEPT: past the maps' admitted capacity), which the bridge
   then holds one reference to, released when the state moves or the slot
   ends. ROOTED and HELD do not retain the node. STUCK marks a ROOTED or
   KEPT detached node whose tree could not be held (a frozen wrapper): the
   sweep leaves it until a wrapper of its tree is released. See
   __tilefinchHoldEventState in dom.js. */
#define BRIDGE_NODE_EVENT_ROOTED 0x08u
#define BRIDGE_NODE_EVENT_HELD 0x10u
#define BRIDGE_NODE_EVENT_KEPT 0x20u
#define BRIDGE_NODE_EVENT_STUCK 0x40u
/* A node of a detached tree a hold walked (it set this on every node of
   the tree with a handle), a subtree that joined such a tree, or a node a
   new wrapper found in one: an insertion under it may concern a holder.
   It may outlive the hold (the bridge clears it when it finds otherwise),
   but no node of a held tree that has a handle lacks it. A node without a
   handle has no flags; bridge_event_state_joining never sees one as the
   parent, since every insertion names its parent by handle (a wrapper,
   which bridge_wrapper_event_holder marks as it is made). */
#define BRIDGE_NODE_EVENT_IN_HELD_TREE 0x80u
#define BRIDGE_NODE_EVENT_STATE \
    (BRIDGE_NODE_EVENT_ROOTED | BRIDGE_NODE_EVENT_HELD \
     | BRIDGE_NODE_EVENT_KEPT)
/* The bits that keep a node's handle (and its detached tree) alive. */
#define BRIDGE_NODE_RETAINING \
    (BRIDGE_NODE_NATIVE_PIN | BRIDGE_NODE_LIVE_WRAPPER \
     | BRIDGE_NODE_PENDING_RETIRE_NOTIFY)
#define DOM_BRIDGE_NODE_INDEX_BITS SCRIPT_DOM_HANDLE_INDEX_BITS
#define DOM_BRIDGE_NODE_INDEX_MASK SCRIPT_DOM_HANDLE_INDEX_MASK
#define DOM_BRIDGE_NODE_GENERATION_MAX \
    ((uint32_t) (INT32_MAX >> DOM_BRIDGE_NODE_INDEX_BITS))

_Static_assert(DOM_BRIDGE_NODE_LIMIT_MAX <= DOM_BRIDGE_NODE_INDEX_MASK,
               "DOM bridge node handles must encode every slot");
_Static_assert(DOM_BRIDGE_NODE_LIMIT_MAX < UINT16_MAX
                   && DOM_BRIDGE_NODE_LIMIT <= DOM_BRIDGE_NODE_LIMIT_MAX
                   && DOM_BRIDGE_NODE_LIMIT % 32u == 0
                   && DOM_BRIDGE_NODE_LIMIT_MAX % 32u == 0,
               "DOM bridge node index entries are 16-bit slot + 1");
_Static_assert(DOM_BRIDGE_NODE_LIMIT <= DOM_BRIDGE_NODE_BUDGET_CAPACITY
                   && DOM_BRIDGE_NODE_BUDGET_CAPACITY <= DOM_BRIDGE_NODE_LIMIT_MAX
                   && DOM_BRIDGE_NODE_BUDGET_CAPACITY % 32u == 0
                   && DOM_BRIDGE_NODE_GROWTH_STEP % 32u == 0
                   && DOM_BRIDGE_NODE_GROWTH_STEP != 0,
               "memory-admitted growth starts within the ceiling, in whole bit words");
_Static_assert(SCRIPT_DOM_HANDLE_SLOT_INITIAL_TOP_LEVEL % 32u == 0
                   && SCRIPT_DOM_HANDLE_SLOT_INITIAL_CHILD_FRAME % 32u == 0
                   && SCRIPT_DOM_HANDLE_SLOT_INITIAL_CHILD_FRAME >= 32u
                   && SCRIPT_DOM_HANDLE_SLOT_INITIAL_TOP_LEVEL
                          <= DOM_BRIDGE_NODE_LIMIT,
               "initial handle tables are whole bit words within the ceiling");
/* The bound of the native ancestor walks that stand in for script's own
   256-step walks (getRootNode, the transition watcher's display check). */
#define DOM_ROOT_NODE_ANCESTOR_LIMIT 256u

typedef struct {
    uint64_t id;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    TilefinchRequestDestination destination;
    bool prefer_text_response;
    char *integrity;
    size_t integrity_length;
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    /* Its response bound in the realm scheduler's pool. */
    size_t response_bound;
} ScriptAsyncFetch;

/* A realm's CORS preflights: one in flight on the realm scheduler, or a
   decided result kept for its owner to claim (and an allowed one reused
   until its Access-Control-Max-Age runs out). The key is Budget-owned
   (bridge->budget): credentials mode, method, origin, sorted header names
   and URL, each NUL-terminated, compared whole. */
#define SCRIPT_CORS_PREFLIGHT_SLOTS 4u
typedef enum {
    SCRIPT_CORS_PREFLIGHT_FREE = 0,
    SCRIPT_CORS_PREFLIGHT_PENDING,
    SCRIPT_CORS_PREFLIGHT_ALLOWED,
    SCRIPT_CORS_PREFLIGHT_REFUSED
} ScriptCorsPreflightState;

typedef struct {
    char *key;
    size_t key_length;
    /* The OPTIONS request while PENDING. */
    uint64_t request_id;
    /* A decided entry is dropped at this time. */
    uint64_t expires_us;
    uint32_t last_use;
    uint8_t state;
    /* Access-Control-Max-Age: 0 allowed only the request that asked. */
    bool single_use;
} ScriptCorsPreflightEntry;

#define SCRIPT_EVENT_SOURCE_LIMIT 2u
#define SCRIPT_EVENT_SOURCE_PENDING_BYTES (64u * 1024u)

typedef struct {
    bool active;
    bool headers_pending;
    bool headers_valid;
    bool terminal_response;
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

#define SCRIPT_WEBSOCKET_LIMIT FETCH_WEBSOCKET_LIMIT

typedef struct {
    bool active;
    uint64_t id;
} ScriptWebSocket;

typedef struct {
    bool active;
    uint64_t id;
} ScriptMultiplayer;

typedef enum {
    SCRIPT_DYNAMIC_QUEUED = 0,
    SCRIPT_DYNAMIC_FETCHING,
    SCRIPT_DYNAMIC_READY
} ScriptDynamicState;

typedef struct {
    lxb_dom_node_t *node;
    uint32_t identity;
    bool programmatic;
    bool html;
    bool force_async;
    bool already_started;
    /* Created by innerHTML-style fragment parsing, not the document's own
       parser: never registers an import map. */
    bool fragment_parsed;
#if TILEFINCH_SCRIPT_CLONE_CENSUS
    /* A clone of a started script not yet seen connected (lab census). */
    bool census_started_clone;
#endif
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
    ScriptResourceTiming resource_timing;
    /* Budget-owned minimized MIME type referenced by resource_timing. */
    char *resource_timing_content_type;
    /* Budget-owned copy of the element's integrity attribute, taken when
       the script was prepared: SRI and the CSP integrity grant both use
       it, so a later attribute change cannot split the two decisions. */
    char *integrity;
    size_t integrity_length;
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
    bool resource_timing_recorded;
    /* The response's Cache-Control carried no-store: its compiled classic
       bytecode is not kept (classic_bytecode_store). */
    bool response_no_store;
    /* Counted toward the page's heavy-script weight (bridge heavy). */
    bool heavy_counted;
    TilefinchResourceGrant stale_resource_grant;
} ScriptDynamicTask;

typedef enum {
    SCRIPT_PRELOADED_SCRIPT_EMPTY = 0,
    /* The preload's request is still in flight: a consumer waits for it
       (HTML's onResponseAvailable) instead of fetching the URL itself. */
    SCRIPT_PRELOADED_SCRIPT_PENDING,
    SCRIPT_PRELOADED_SCRIPT_READY
} ScriptPreloadedScriptState;

/* One entry of the document's map of preloaded resources (HTML, <link
   rel=preload>) for a classic script: the response of a preload whose key
   is url, destination "script", mode and credentials, kept for the first
   consumer (script_runtime_offer_preloaded_script). A module entry (HTML's
   module map, which a modulepreload fills) is an adopted request: while
   PENDING it goes to the first module fetch of the same key
   (script_runtime_take_module_preload); completed, its response is in the
   module HTTP cache that module fetches consult, and READY for a
   script-inserted module script. */
typedef struct {
    char *url;
    /* The link's integrity metadata; NULL when it had none. */
    char *integrity;
    /* NULL when the response came from url itself. */
    char *response_url;
    BrowserSharedBody *body;
    size_t length;
    char content_type[128];
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    ScriptPreloadedScriptState state;
    /* READY with no body: the response failed the preload's own integrity
       metadata, so Fetch made it a network error, which is what a
       consumer gets. */
    bool network_error;
    /* PENDING: the navigation's request, which this entry now owns. The
       scheduler (the page's resource scheduler) outlives the realm. */
    FetchScheduler *scheduler;
    uint64_t request_id;
    uint8_t csp_grant;
    /* The document's CSP policy count when the request was made. */
    uint8_t csp_policies;
    bool module;
    /* A module response's Referrer-Policy (its code; 0: none), which the
       module's imports use. */
    uint8_t referrer_policy;
} ScriptPreloadedScript;
/* How long after the load event an unconsumed entry is kept: Chromium's
   kUnusedPreloadTimeout (resource_fetcher.cc), after which it reports the
   preload as unused. Chromium's memory cache can still serve it later;
   this engine has no such cache for CORS scripts, so the entry goes. */
#define SCRIPT_PRELOADED_SCRIPT_UNUSED_MS 3000u

typedef struct {
    lxb_dom_node_t *node;
    uint32_t sequence;
    int scroll_x;
    int scroll_y;
} ElementScrollIntent;

/* What a retained computed style depends on besides connected DOM
   content (which the bridge's mutation notes and content generation track):
   the sheet and its build, the document, its container geometry (compared
   by content, since every layout pass rebuilds it), the fullscreen element,
   and the host style generation (document_style_generation(): parser,
   controller and other native DOM writes, colour scheme). */
typedef struct {
    const void *sheet;
    const void *dom;
    uint64_t sheet_generation;
    uint64_t container_generation;
    uint64_t container_signature;
    uint64_t host_generation;
    const lxb_dom_node_t *fullscreen_node;
} ComputedStyleInputs;

/* Event-state bookkeeping of the DOM bridge (js_event_state.inc): which
   handle slots hold event state for a detached tree, and the retry state
   of the bootstrap's hold and release hooks. */
typedef struct {
    /* Slots whose event state is HELD, and a count that changes whenever
       that set changes or held state moves to another tree; with the last
       root bridge_tree_held_handle walked, it lets a repeat query on an
       unchanged tree skip the walk. */
    size_t held_slots;
    /* Slots marked BRIDGE_NODE_EVENT_STUCK. */
    size_t stuck_slots;
    uint32_t holder_epoch;
    uint32_t holder_probe_epoch;
    const lxb_dom_node_t *holder_probe_root;
    int64_t holder_probe_handle;
    /* A hold or release hook that threw (the realm's heap refused an
       allocation) left the state where it was. Holds then wait, and the
       bootstrap's unfinished releases (release_pending) are retried, once
       the heap has collected retry_backoff more times than
       retry_collection, doubling to 64 while they keep failing; a backoff
       of 0 lets them run. */
    size_t retry_collection;
    uint8_t retry_backoff;
    bool release_pending;
#ifdef JS_RT_TEST_INSTRUMENTS
    /* Host-test counters: calls into the bootstrap's hold and release
       hooks, slots that left HELD, wrapper releases that reopened STUCK
       trees for the sweep, hook calls that threw, and retries of
       unfinished releases. */
    uint64_t hook_calls;
    uint64_t held_releases;
    uint64_t stuck_reopens;
    uint64_t hook_failures;
    uint64_t release_retries;
#endif
} DomEventState;

/* getElementById answers, found or not, for the bridge's document. They
   hold while neither the structure version (script child-list, text and
   id writes; see dom_structure_version) nor the host style generation
   (parser and other native writes, through Lexbor's callbacks) moves.
   Script class and style writes move neither, so lookups between them
   stay answered. Inline in the budget-owned runtime: a fixed 8 entries. */
#define DOM_ID_MEMO_ENTRIES 8u
#define DOM_ID_MEMO_KEY_BYTES 40u
typedef struct {
    lxb_dom_node_t *node; /* NULL: no element has the id. */
    uint8_t length;       /* 0: the entry is unused. */
    char key[DOM_ID_MEMO_KEY_BYTES];
} DomIdMemoEntry;

typedef struct {
    uint64_t structure_version;
    uint64_t host_generation;
    uint8_t next;
    DomIdMemoEntry entries[DOM_ID_MEMO_ENTRIES];
} DomIdMemo;

/* The pixels of a tainted canvas, or of an ImageBitmap that is not
   origin-clean. They live only here, never in a script-visible buffer:
   script names a canvas's store by the element's handle and a bitmap's by
   its negative id. `pixels` is width * height RGBA bytes allocated from the
   realm's JavaScript heap (the limit a Uint8ClampedArray of the same size is
   charged to), or NULL while there is no surface. */
typedef struct {
    lxb_dom_node_t *node; /* the canvas element; NULL for a bitmap store */
    int32_t bitmap_id;    /* > 0 for a bitmap store */
    int32_t width;
    int32_t height;
    uint8_t *pixels;
} DomBridgeCanvasStore;

typedef struct DomBridge {
    PocDocument *document;
    Budget *budget;
    ScriptResult *result;
    /* Process-issued identity for native WebGL cache authority.  This is a
       two-word counter so the 32-bit PSP never relies on a tearing 64-bit
       atomic.  Page code cannot observe or choose either word. */
    uint32_t webgl_realm_epoch_high;
    uint32_t webgl_realm_epoch_low;
    ScriptWebglGeometryCacheState webgl_geometry_cache;
    void *webgl_geometry_vertices[SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT];
    /* Retained vertex blocks the GE drew from directly and that were
       evicted later in the same frame: freed once that frame's list has
       completed (at the next render, or when the cache is cleared). */
    void *webgl_geometry_retired[SCRIPT_WEBGL_GEOMETRY_DIRECT_DRAW_LIMIT];
    size_t webgl_geometry_retired_count;
    bool *relayout_dirty;
    /* What changed since the host last consumed it, allocated on the first
       connected mutation (js_rt_bridge_journal): many realms (most child
       frames) never mutate their document. NULL is an empty journal. */
    ScriptMutationJournal *mutations;
    /* The journal could not be allocated: changes went unrecorded until the
       next consume, which reports an overflowed journal whose roots were
       lost (layout resets, resources are rescanned). */
    bool mutations_unrecorded;
    BrowserSession *session;
    uint32_t session_storage_scope;
    const FontSet *fonts;
    char *document_url;
    const char *top_level_url;
    bool opaque_origin;
    char calculated_base_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool document_base_dirty;
    /* A connected <meta http-equiv=refresh> may have been inserted (or its
       http-equiv/content changed): navigation looks for a declarative
       refresh again (script_runtime_take_refresh_meta_mutation). */
    bool refresh_meta_mutated;
    /* The write being journaled is ScriptMutationRecord.keeps_images. */
    bool mutation_keeps_images;
    /* Native URL publication precedes advisory same-document callbacks.  A
       generation lets the Location facade detect and repair a callback/OOM
       split without allocating a URL string on ordinary property reads. */
    uint32_t document_url_revision;
    char referrer_policy[128];
    /* The slot table: parallel arrays of node_capacity entries carved from
       one Budget block (bridge_node_table_resize), allocated at runtime
       creation with SCRIPT_DOM_HANDLE_SLOT_INITIAL_* slots and grown,
       never beyond node_capacity_limit, only when registration finds every
       slot live: doubled up to DOM_BRIDGE_NODE_BUDGET_CAPACITY, then by
       DOM_BRIDGE_NODE_GROWTH_STEP (bridge_node_table_next_capacity). */
    lxb_dom_node_t **nodes;
    /* Owner identities are captured at registration so whole-document
       retirement never has to inspect a node which author mutation may have
       destroyed already. */
    uintptr_t *node_owner_document_identities;
    /* At most DOM_BRIDGE_NODE_GENERATION_MAX (32,767). */
    uint16_t *node_generations;
    uint32_t *node_wrapper_leases;
    /* Bit 0 pins a handle for native work; bit 1 records that the newest JS
       wrapper lease is still live. Sharing the byte avoids enlarging the
       bounded PSP handle table to track detached-node correctness. */
    unsigned char *node_retention_flags;
    /* NULL slots whose generation can still advance, one bit per slot. */
    uint32_t *node_reusable_bits;
    /* Every node_reusable_bits word below this one is clear. */
    size_t node_reusable_low_word;
    /* Slots whose wrapper one collection is holding; clear between
       collections (js_rt_bridge_collection_holds). */
    uint32_t *node_wrapper_held_bits;
    /* Slots whose handle number is kept somewhere other than the node's
       wrapper, one bit per slot: in native state (a weak or pinned native
       handle, an image or script record) or in a handle-keyed bootstrap
       table (form control state, focus, frame windows, shadow hosts).
       Sticky until the slot retires. A connected node's slot is released
       when its wrapper dies only while its bit is clear
       (bridge_connected_slot_releasable). */
    uint32_t *node_handle_kept_bits;
    /* The bootstrap holds raw handles of connected nodes across owner turns
       (a suspended motion scan, or a test realm's cleared wrapper cache):
       connected slots are not released meanwhile. */
    bool connected_release_held;
    size_t node_wrappers_held;
    /* A wrapper's own keys that a new wrapper would have again
       (bridge_note_plain_wrapper_keys). */
    JSAtom wrapper_plain_keys[DOM_BRIDGE_WRAPPER_PLAIN_KEYS];
    int wrapper_plain_key_count;
    size_t node_capacity;
    size_t node_capacity_limit;
    /* Per handle slot, a weak target (JS_NewWeakTarget) of the live
       wrapper: the wrapper cache, which native getters and the bootstrap
       (__tilefinchNodeWrapper) answer from. A weak target is the object
       itself, so a slot keeps only its pointer (NULL for none; read it with
       js_rt_bridge_wrapper_ref). Budget-allocated on first use; NULL when
       unavailable (the getters then ask script to wrap). */
    void **wrapper_refs;
    /* Per handle slot, the adopted owner document's tag: 0 unknown, 1 the
       script's document, 2 the template contents owner, 3-255 a script-side
       document the bootstrap names. Keeping it with the native node lets
       it outlive wrappers. Budget-allocated on the first adoption away from
       the document (or template contents read); until then every node is
       the document's, template contents aside. */
    unsigned char *node_owner_tags;
    size_t node_count;
    /* Every non-NULL slot is indexed by its node pointer, so registering a
       node and finding the handles inside a detached subtree do not scan the
       slot table. Only register/invalidate write `nodes`, and both keep this
       index and the reusable-slot bitmap (NULL slots whose generation can
       still advance) in step. */
    uint16_t *node_index;
    /* Twice node_capacity, so the load factor stays at or below one half
       (bridge_node_index_home scales the hash to it). */
    size_t node_index_capacity;
    /* The last element getComputedStyle() cascaded, for consecutive
       property reads of one declaration. Valid while none of the cascade's
       inputs moved: connected DOM content (every bridge mutation, including
       focus, custom-element and form state, bumps the content generation)
       and ComputedStyleInputs, which cover native changes made between
       entries into JavaScript and synchronous layout within one. */
    struct {
        const lxb_dom_node_t *node;
        ComputedStyleInputs inputs;
        uint64_t content_generation;
        ComputedStyle style;
    } computed_style_memo;
    /* Whether any element from `parent` up is display:none, as the last
       transition snapshot resolved it, under the memo's validity above:
       watched siblings share that ancestor chain. Kept only while the
       style cache's removal listener can forget `parent`. */
    struct {
        const lxb_dom_node_t *parent;
        ComputedStyleInputs inputs;
        uint64_t content_generation;
        bool rendered;
    } transition_ancestors_memo;
#define COMPUTED_STYLE_CUSTOM_KEY_LIMIT 32u
    /* Resolved ancestor styles under the memo's validity key: a read starts
       from its nearest cached ancestor instead of re-running the cascade for
       the whole chain (siblings share parents, and pages read several nodes
       per task). Allocated from the page Budget on first use. */
    struct {
        ComputedStyleInputs inputs;
        uint64_t content_generation;
        /* Registered for node-removal eviction (see document.h); the ring
           is unusable without it. */
        bool removal_listening;
        /* Replacement candidates drawn so far: the sequence the next
           victim is drawn from (see computed_style_cache_victim). */
        size_t next;
        size_t hits;
        size_t misses;
        size_t lookup_probes;
        /* Used-geometry reads answered from the geometry records. */
        size_t geometry_hits;
        /* Index+1 chains over the 128 retained styles.
           Zero terminates; this does not enlarge the retained style set. */
        uint8_t buckets[128];
        uint8_t links[128];
        /* Bit i set while slot i holds a style. Drops and node removals
           vacate slots between reads; a new style takes a vacant slot
           before it evicts a live one. */
        uint8_t filled[128 / 8];
        /* Bit i set when slot i's style was used since it was stored or
           last passed over: a replacement candidate with the bit gets a
           second chance. */
        uint8_t referenced[128 / 8];
        struct ComputedStyleCacheEntry *entries;
        /* What a used-geometry read (width, height, padding) needs of each
           ancestor (whether it is a containing block, its horizontal
           padding), as resolved in the epoch it is stamped with. An
           open-addressed table beside the ring, so that a page reading
           the sizes of many elements does not re-run the cascade for
           whole ancestor chains the ring has cycled out. A walk allocates
           it from the page Budget at its first store; a connected
           mutation, a forced layout build or the end of the task releases
           it. */
        struct ComputedStyleGeometryRecord *geometry;
        /* Set for good once a read meets an element with more ancestors
           than a walk takes (64): such a walk resolves its top without
           the parent style, and records must not stand in for it. */
        bool geometry_off;
        /* Connected mutations since the last read, as subtrees whose
           cached styles they can change (see
           bridge_computed_style_note_mutation); `dirty_all` when that is
           unknown or the list overflowed. */
        lxb_dom_node_t *dirty[16];
        size_t dirty_count;
        bool dirty_all;
        /* Drops queued without a content-generation move (a sheet change
           navigation scoped, a host style write the bridge described:
           js_rt_bridge_computed_style_drop_within): the next read applies
           them as it applies a mutation's. */
        bool drops_pending;
        /* Calls of js_rt_bridge_computed_style_drop_within since the sheet
           settle callback started, and whether one could not be scoped:
           the settle keeps the retained styles outside the reported
           subtrees only when navigation reported and none went whole. */
        uint8_t scope_reports;
        bool scope_whole;
        /* Elements whose own matched rules may have changed although
           their descendants' inputs did not (a sibling test, a :has()
           answer): only their entries go. A kept descendant is checked
           against its parent's current style before use (parent_key),
           as the layout reuse cache re-keys off parent styles. */
        lxb_dom_node_t *shallow[24];
        size_t shallow_count;
        /* Keyed :has() subjects of the pending changes
           (style_has_note_change); `has_active` while it holds any. */
        StyleHasPending has_pending;
        bool has_active;
        /* Bumped by every drop: an entry validated in this epoch is valid
           without checking its ancestors. */
        uint32_t epoch;
        size_t scoped_clears;
        size_t full_clears;
        /* var() lookups under the same validity, cleared with the
           entries (any change can alter a custom property somewhere).
           The table exists only once a read resolved a var() against a
           sheet that declares custom properties. */
        StyleVariableCacheLease variables;
        /* Selector dependencies of the sheet generation below. */
        const void *flags_sheet;
        uint64_t flags_generation;
        /* The generation the key sets below were built for (they cost
           several times the flags and are built only when a change
           reaches a structural test); whole-sheet scans of each kind, for
           the lab's style-cache report and tests. */
        const void *keys_sheet;
        uint64_t keys_generation;
        size_t flag_scans;
        size_t key_scans;
        bool flags_has;
        bool flags_structure;
        bool flags_focus;
        /* Sibling combinators or `of S` counts, which make one element's
           attributes part of its siblings' matches (positional
           pseudo-classes read only positions); :empty or :blank beside a
           sibling test, through which a child list can restyle the
           parent's siblings. */
        bool flags_sibling;
        bool flags_empty_sibling;
        /* A custom-property rule with :has(): a subject's drop takes its
           subtree. */
        bool flags_custom_has;
        bool flags_has_escaped;
        /* Structural tests that reach descendants (style.h), as keys of
           the children they concern: all of them for child-list changes,
           the sibling ones for attribute changes. */
        StyleStructureKeys structure;
        StyleStructureKeys sibling_structure;
        /* Keys of the elements a custom-property rule's structural test
           (all; sibling only) can switch custom properties on. A custom
           property is not in ComputedStyle, so such a sibling's subtree
           goes; `_any` when some test has no key. */
        uint32_t custom_structure_keys[COMPUTED_STYLE_CUSTOM_KEY_LIMIT];
        uint32_t custom_sibling_keys[COMPUTED_STYLE_CUSTOM_KEY_LIMIT];
        uint8_t custom_structure_key_count;
        uint8_t custom_sibling_key_count;
        bool custom_structure_any;
        bool custom_sibling_any;
        /* Keys of the elements whose :empty a sibling test reads. */
        uint32_t empty_keys[COMPUTED_STYLE_CUSTOM_KEY_LIMIT];
        uint8_t empty_key_count;
        bool empty_any;
        /* Host style generations that emptied a warm ring, for the lab's
           style-cache report. */
        size_t host_clears;
    } computed_style_cache;
    /* The last container-state signature computed, by the generation it
       was computed for (shared by the memo and the ring). */
    const void *computed_style_container_sheet;
    uint64_t computed_style_container_generation;
    uint64_t computed_style_container_signature;
    /* Set while the transition watcher snapshots an element: computed
       values resolve from the cascade as it stands rather than forcing the
       synchronous layout a class change would otherwise cost. */
    bool computed_style_without_layout;
    /* Connected mutations that can change document statistics (anything
       but attribute, inline-style and canvas-pixel changes); the runtime
       refreshes the document only when this advances. */
    size_t stats_mutations;
    uint64_t dom_version;
    /* Advances with dom_version except for attribute, inline-style and
       canvas changes that cannot move a form control between owners
       (anything but id, form, name, type or an internal state
       attribute). */
    uint64_t dom_structure_version;
    /* Advances with every script mutation but attribute, inline-style and
       canvas ones, and with document.write(): what can change how many
       nodes the document holds. */
    uint64_t tree_version;
    /* The document's parser_writes the two versions above last took in
       (js_rt_bridge_note_parser_writes). */
    uint64_t parser_writes_seen;
    /* The same for the layout (js_rt_bridge_note_parser_writes_for_layout). */
    uint64_t parser_writes_laid_out;
    /* A script the whole-document loader runs (a page committed whole, not
       streamed): document.write() has no parser to feed, so its markup
       waits here until the loader takes it once the script returns
       (script_runtime_whole_document_write_end). Budget-allocated. */
    const lxb_dom_node_t *whole_document_writer;
    char *whole_document_written;
    size_t whole_document_written_length;
    DomIdMemo id_memo;
    /* Native setAttribute calls: total, those writing the value already
       present, and time spent in the native setter. */
    size_t attribute_writes;
    size_t attribute_writes_unchanged;
    uint64_t attribute_write_ns;
    /* Exhaustion-time slot reclamation: a reentry guard, the exhaustions
       still to handle without a collection after a futile one (cleared at
       each entry into JavaScript), and the next such backoff. */
    bool node_reclaim_active;
    /* Guarded DOM natives (JS_RT_GUARDED_NATIVE) on the stack. A native
       keeps the node pointers it resolved across any page code it calls
       (argument coercion, an accessor, an inline script), so while one
       runs a discard retires its subtree's handles at once but leaves the
       destroy to the outermost native's return: deferred_discards holds
       those roots (Budget-grown up to JS_RT_DEFERRED_DISCARD_LIMIT). A
       root the list cannot take stays undestroyed until teardown and is
       counted in deferred_discard_refusals. */
    bool deferred_draining;
    uint32_t native_depth;
    size_t deferred_discard_count;
    size_t deferred_discard_capacity;
    size_t deferred_discard_refusals;
    lxb_dom_node_t **deferred_discards;
    uint16_t node_reclaim_gc_skip;
    uint16_t node_reclaim_gc_backoff;
    uint16_t node_wrapper_sweep_cursor;
    uint16_t shadow_reclaim_gc_skip;
    uint16_t shadow_reclaim_gc_backoff;
    size_t node_wrapper_sweep_remaining;
    /* Event-state bookkeeping (js_event_state.inc). The wrap, insertion
       and sweep paths read it. */
    DomEventState events;
    /* Shadow carriers are native DOM nodes, but ordinary document/element
       selectors must not cross their tree boundary.  Store encoded handles,
       rather than pointers, so slot generation prevents allocator reuse from
       granting a stale carrier identity. */
    uint32_t *shadow_root_handles;
    size_t shadow_root_count;
    size_t shadow_root_capacity;
    ScriptRuntime *host;
    ScriptPostMessageCallback post_message;
    void *post_message_opaque;
    uint64_t performance_origin_ns;
    unsigned fetch_timeout_ms;
    bool deterministic_entropy;
    uint64_t entropy_state;
    uint64_t replay_seed;
    bool deterministic_clock;
    bool fixed_wall_clock;
    bool wall_clock_timers;
    uint64_t clock_origin_ms;
    uint64_t clock_host_elapsed_ms;
    uint64_t clock_wall_elapsed_ms;
    uint64_t clock_monotonic_elapsed_ms;
    uint64_t clock_source_counts[SCRIPT_CLOCK_SOURCE_COUNT];
    /* Mirrors the native QuickJS CSP gate for delayed frame-eval helpers.
       A trusted evaluator is compiled during bootstrap, before author policy
       is armed, so every later invocation must independently consult this
       non-page-writable bit. */
    bool dynamic_code_allowed;
    size_t console_messages;
    bool navigation_requested;
    bool navigation_replace;
    bool navigation_user_activated;
    char navigation_url[2048];
    bool media_requested;
    bool media_audio_only;
    ScriptMediaCommand media_command;
    TilefinchRequestMode media_mode;
    TilefinchCredentialsMode media_credentials;
    int64_t media_node_handle;
    double media_value;
    char *media_source;
    char media_title[SCRIPT_MEDIA_TITLE_CAPACITY];
    int64_t fullscreen_node_handle;
    /* Page controls entry notice (navigator.tilefinch.requestPageControls
       {notice: "once"}): the page's pending request to skip a repeat, and
       whether this document has already shown the notice once. */
    bool page_controls_notice_once;
    bool page_controls_notice_shown;
    bool user_activation_active;
    bool user_activation_has_been_active;
    uint64_t user_activation_expires_ms;
    /* Advances with every trusted activation: a page dialog's gesture limit
       restarts when it changes (page_dialogs.inc). */
    uint32_t user_activation_generation;
    /* Popunder guard (docs/SECURITY_MODEL.md): a new window this document
       asked for during a user activation (Tilefinch opens none), kept while
       that activation is transient, and the hyperlink the activation
       targeted, resolved before the page's listeners could rewrite it.
       activation_link_default: the browser follows that hyperlink itself
       (the click was not cancelled), so the guard does not. The pending
       navigation_url is the guard's own follow while
       navigation_follows_link. */
    uint64_t new_window_expires_ms;
    uint64_t new_window_site;
    char *activation_link_url;
    bool activation_link_default;
    bool navigation_follows_link;
    bool scroll_requested;
    int scroll_y;
    LayoutDocument *layout;
    /* A truncated `layout` (a bounded focus relayout's) is the complete
       layout's own above this y, its proven region; 0 when unproven. */
    int layout_region_end;
    ImageResources *images;
    ScriptSynchronousLayoutCallback synchronous_layout;
    void *synchronous_layout_opaque;
    bool synchronous_layout_images_only;
    /* The load lifecycle's layout allowance (script_runtime_finish_loading):
       the work its builds after the first may still do, and the work of
       its first build (0 until that ran). Only builds spend it. */
    uint64_t loading_layout_allowance;
    uint64_t loading_layout_first;
    /* The live document's nodes and elements as the load allowance's
       estimate last counted them (up to its cap), and the tree_version
       they were counted at; loading_count_valid is cleared at each load. */
    size_t loading_count_nodes;
    size_t loading_count_elements;
    uint64_t loading_count_version;
    bool loading_count_valid;
    bool synchronous_layout_loading;
    bool synchronous_layout_geometry_only;
    bool expect_navigation_layout;
    /* script_runtime_set_sheet_settle_callback; a refusal stands until
       dom_version moves past `sheet_settle_refused_version`. */
    ScriptSynchronousLayoutCallback sheet_settle;
    void *sheet_settle_opaque;
    bool sheet_settle_refused;
    uint64_t sheet_settle_refused_version;
    /* Layout the page forced from script (geometry reads, scrollIntoView):
       engine work inside a script entry that its watchdog also counts. */
    size_t synchronous_layout_calls;
    uint64_t synchronous_layout_us;
    uint64_t synchronous_layout_max_us;
    ScriptNodeRetirementCallback node_retirement;
    void *node_retirement_opaque;
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
    ScriptCorsPreflightEntry cors_preflights[SCRIPT_CORS_PREFLIGHT_SLOTS];
    uint32_t cors_preflight_uses;
    /* Counts preflights that left PENDING; the JavaScript FIFO resubmits a
       request parked on its preflight when this moves. */
    uint32_t cors_preflight_decisions;
    ScriptEventSource event_sources[SCRIPT_EVENT_SOURCE_LIMIT];
    size_t event_source_count;
    ScriptWebSocket websockets[SCRIPT_WEBSOCKET_LIMIT];
    size_t websocket_count;
    /* Slot js_rt_websockets_deliver tries first; it moves past the slot
       that delivered so one busy socket cannot starve the other. */
    size_t websocket_deliver_next;
    unsigned char *websocket_event_scratch;
    ScriptMultiplayer multiplayer;
    unsigned char *multiplayer_event_scratch;
    ScriptDynamicTask dynamic_scripts[SCRIPT_DYNAMIC_TASK_LIMIT];
    ScriptPreloadedScript preloaded_scripts[SCRIPT_PRELOADED_SCRIPT_LIMIT];
    /* Sum of the READY entries' lengths, at most
       SCRIPT_PRELOADED_SCRIPT_BYTE_LIMIT. */
    size_t preloaded_script_bytes;
    /* js_rt_bridge_elapsed_ms at the load event plus
       SCRIPT_PRELOADED_SCRIPT_UNUSED_MS; 0 before the load event. */
    uint64_t preloaded_scripts_unused_at_ms;
    ScriptElementState script_elements[SCRIPT_DYNAMIC_NODE_LIMIT];
    size_t script_element_count;
    uint32_t next_script_element_identity;
    /* A fragment-parsed (innerHTML) script found the state table full, so
       it carries no "already started" state and cannot be told from a
       parser-inserted one. The import map scan then trusts no stateless
       script (js_module_loader.c). */
    bool fragment_script_state_overflow;
    size_t dynamic_script_count;
    size_t dynamic_script_bytes;
    size_t script_quota_count;
    size_t script_quota_bytes;
    size_t script_quota_reserved_bytes;
    size_t maximum_scripts;
    size_t maximum_script_bytes;
    size_t script_quota_pressure_raises;
    size_t maximum_script_file_bytes;
    /* Heavy pages (script_runtime_heavy_state). */
    struct {
        ScriptHeavyPolicy policy;
        ScriptHeavyClass page_class;
        bool answered;
        bool allowed;
        size_t script_bytes;
        size_t waiting_bytes;
        size_t waiting_scripts;
        size_t refused_scripts;
        size_t oversized_scripts;
        size_t oversized_bytes;
        /* script_runtime_note_memory_refusal() */
        size_t memory_refusals;
        size_t largest_unit_bytes;
        /* Source whose bytecode was restored instead of compiled: from the
           session's RAM tables (about 50 ms per MiB instead of 4 s), and
           the part of it read from the Memory Stick tier (about 1 s per
           MiB more). */
        size_t restored_bytes;
        size_t disk_restored_bytes;
        size_t visible_text_bytes;
        bool visible_text_known;
        /* A child frame's realm (script_runtime_heavy_frame_begin): its
           big scripts are asked about by size alone, and a parser or
           deferred one that must wait stops the frame's load
           (frame_held, script_runtime_heavy_frame_held). A response is
           required to hold; after earlier scripts, a bounded completed
           handle list permits resuming the live realm without replay. */
        bool frame_gate;
        bool frame_can_hold;
        bool frame_held;
        long *frame_completed_scripts;
        size_t frame_completed_count;
        /* The top-level realm only: the page's answer for its frames'
           big scripts (script_runtime_answer_heavy_frames), kept apart
           from the page's own answer above. */
        bool frames_answered;
        bool frames_allowed;
        size_t frame_held_bytes;
    } heavy;
    bool allow_test_network_primitive_overrides;
    uint64_t dynamic_script_sequence;
    ScriptExecutionPolicy execution_policy;
    JSValue current_script;
    /* Descendant CSP grant of the script being evaluated (module roots and
       classic import() inherit it); zero outside a script element. */
    uint8_t current_script_csp_grant;
    /* HTML's ignore-destructive-writes counter: external classic and module
       scripts running now. document.write() with no insertion point is
       ignored while it is not zero. */
    uint16_t destructive_writes_ignored;
    /* Canvas elements that have drawn pixels which are not origin-clean,
       each with its native pixel store, and the stores of ImageBitmaps that
       are not origin-clean. Natives that hand a canvas's pixels to script
       refuse these, and the bootstrap's readback gates ask here, so no
       script-visible field decides it. A taint is never cleared while the
       element lives (HTML: resetting the bitmap keeps the flag); a
       discarded subtree drops its entries and frees their stores
       (js_rt_bridge_canvas_taints_retire_subtree). Budget-allocated on the
       first taint, at most DOM_BRIDGE_CANVAS_TAINT_LIMIT canvases plus
       DOM_BRIDGE_CANVAS_BITMAP_LIMIT bitmaps. */
    DomBridgeCanvasStore *canvas_taints;
    size_t canvas_taint_count;
    size_t canvas_taint_capacity;
    int32_t canvas_bitmap_next_id;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t webgl_native_total_us;
    size_t webgl_native_frames;
#endif
} DomBridge;

/* A slot's weak wrapper target as a value: the object, or undefined. */
static inline JSValue js_rt_bridge_wrapper_ref(const DomBridge *bridge,
                                               size_t slot)
{
    void *object = bridge->wrapper_refs[slot];
    return object == NULL ? JS_UNDEFINED : JS_MKPTR(JS_TAG_OBJECT, object);
}

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
    /* Set only for a module first requested by a classic script's
       import(): that script's base URL, the request's referrer. */
    char *classic_referrer_url;
    TilefinchCredentialsMode credentials;
    uint16_t parent_index;
    uint8_t effective_referrer_policy;
    uint8_t root_state;
    /* CSP grant the module's requests carry: its root script's nonce and
       parser metadata, inherited along the graph. */
    uint8_t csp_grant;
} ScriptModuleBaseEntry;

typedef enum {
    SCRIPT_HOST_REFRESH_NAMED_PROPERTIES = 0,
    SCRIPT_HOST_PARSER_MUTATION_CHECKPOINT,
    SCRIPT_HOST_RECEIVE_MESSAGE,
    SCRIPT_HOST_RECEIVE_KEY_MESSAGE,
    SCRIPT_HOST_SET_FRAME_WINDOW_STATE,
    SCRIPT_HOST_INTERSECTION_RECHECK,
    SCRIPT_HOST_MEDIA_RECHECK,
    SCRIPT_HOST_RECORD_RESOURCE_TIMING,
    SCRIPT_HOST_RECORD_NAVIGATION_TIMING,
    SCRIPT_HOST_DELIVER_NETWORK,
    SCRIPT_HOST_PUMP_TIMERS,
    SCRIPT_HOST_SCHEDULER_SNAPSHOT,
    SCRIPT_HOST_COMMIT_SAME_DOCUMENT,
    SCRIPT_HOST_RESTORE_SAME_DOCUMENT,
    SCRIPT_HOST_UPDATE_GAMEPAD,
    /* Called by native code through runtime_call_captured(), or by the
       bridge, rather than through runtime_call_host(). Captured with the
       rest, before author code, and some are deleted from the global right
       after (runtime_creation.inc). */
    SCRIPT_HOST_APPLY_PAGE_SCROLL,
    SCRIPT_HOST_FLUSH_PAGE_SCROLL,
    SCRIPT_HOST_DISPATCH_AT,
    SCRIPT_HOST_DISPATCH_HANDLE,
    SCRIPT_HOST_DISPATCH_ACTIVATION_HANDLE,
    SCRIPT_HOST_DISPATCH_INPUT_HANDLE,
    SCRIPT_HOST_DISPATCH_SUBMIT_HANDLE,
    SCRIPT_HOST_DISPATCH_DOM_CONTENT_LOADED,
    /* Native <video> activation needs the bootstrap's private WeakMap
       record even when play() was not the entry point. */
    SCRIPT_HOST_MEDIA_STATE_FOR,
    /* The page lifecycle queues native visibility edges, then invokes this
       private compatibility closure at a normal author-task checkpoint. */
    SCRIPT_HOST_APPLY_PAGE_VISIBILITY,
    /* Used when native lifecycle observes that the fullscreen element
       detached. */
    SCRIPT_HOST_EXIT_FULLSCREEN,
    /* The bridge's hooks into the wrapper cache and the event-state store
       (dom.js); js_rt_bridge_trusted_hook() reads them. */
    SCRIPT_HOST_WRAP_NODE,
    SCRIPT_HOST_RETIRE_NATIVE_NODE_STATE,
    SCRIPT_HOST_HOLD_EVENT_STATE,
    SCRIPT_HOST_RELEASE_EVENT_HOLDERS,
    /* compat.js: a microphone permission changed; statuses re-read. */
    SCRIPT_HOST_PERMISSIONS_CHANGED,
    /* platform.js's parent and top senders, kept last: deleted from the
       global once captured and handed only to the child messaging set-up
       (script_runtime_configure_messaging). */
    SCRIPT_HOST_POST_PARENT_MESSAGE,
    SCRIPT_HOST_POST_TOP_MESSAGE,
    SCRIPT_HOST_CALLBACK_COUNT
} ScriptHostCallback;

typedef struct ScriptImageLoadState {
    long handle;
    uint64_t source_hash;
    /* What script keeps reading while the table has made way for the image. */
    int natural_width;
    int natural_height;
    bool pending;
    bool delivered;
} ScriptImageLoadState;

/* Slots of ScriptRuntime.host_state, which scheduler.js sees as a
   Float64Array (its HOST_* constants): facts the event loop needs every
   turn, written by the bootstrap when they change. */
typedef enum {
    SCRIPT_HOST_STATE_TIMERS = 0,
    SCRIPT_HOST_STATE_EARLIEST_DUE,
    SCRIPT_HOST_STATE_NOW,
    SCRIPT_HOST_STATE_SAMPLED_CLOCK,
    SCRIPT_HOST_STATE_SCROLL_PENDING,
    /* The earliest timer's kind (ScriptHostTimerKind), 0 without one. */
    SCRIPT_HOST_STATE_EARLIEST_KIND,
    /* The earliest due time of a task (any timer but a frame callback),
       Infinity without one: a due animation frame at the head must not
       hide an overdue timeout behind it. */
    SCRIPT_HOST_STATE_EARLIEST_TASK_DUE,
    /* Timer callbacks that threw (the scheduler reports and swallows the
       exception; see runtime_pump_timers). */
    SCRIPT_HOST_STATE_TIMER_EXCEPTIONS,
    SCRIPT_HOST_STATE_COUNT
} ScriptHostStateSlot;

/* scheduler.js's timer kinds as SCRIPT_HOST_STATE_EARLIEST_KIND codes (its
   TIMER_KIND_CODES). The first three are frame callbacks, which a browser
   runs at a rendering opportunity rather than as soon as they are due. */
typedef enum {
    SCRIPT_HOST_TIMER_NONE = 0,
    SCRIPT_HOST_TIMER_ANIMATION_FRAME,
    SCRIPT_HOST_TIMER_RENDER_OBSERVER,
    SCRIPT_HOST_TIMER_RENDER_FIXUP,
    SCRIPT_HOST_TIMER_TIMEOUT,
    SCRIPT_HOST_TIMER_INTERVAL,
    SCRIPT_HOST_TIMER_MESSAGE,
    SCRIPT_HOST_TIMER_PLATFORM_TASK,
    SCRIPT_HOST_TIMER_IDLE,
    SCRIPT_HOST_TIMER_OTHER
} ScriptHostTimerKind;

#define SCRIPT_WORKER_REALM_LIMIT 4
bool js_rt_geometry_opacity_write_begin(DomBridge *bridge, lxb_dom_node_t *node);
void js_rt_geometry_opacity_write_end(DomBridge *bridge);
/* An attribute write no style or box reads keeps script's current geometry
   (see dispatch_state.inc). begin, before the DOM write: whether the write
   may qualify (it fills `write`). end, after the write was journaled. */
typedef struct {
    lxb_dom_node_t *node;
    /* The element's style before the write, and its parent's, when a
       selector tests the attribute in the element's own compound. */
    bool compare;
    bool has_parent;
    ComputedStyle before;
    ComputedStyle parent;
} JsRtGeometryAttributeWrite;
bool js_rt_geometry_attribute_write_begin(DomBridge *bridge,
                                          lxb_dom_node_t *node,
                                          const char *name, size_t length,
                                          JsRtGeometryAttributeWrite *write);
void js_rt_geometry_attribute_write_end(DomBridge *bridge,
                                        const JsRtGeometryAttributeWrite *write);
/* A class or inline style write under a display:none ancestor keeps
   script's current geometry (see dispatch_state.inc). `relational`: the
   write may change a :has() answer. end, after the write was journaled. */
bool js_rt_geometry_hidden_write_begin(DomBridge *bridge, lxb_dom_node_t *node,
                                       bool relational);
void js_rt_geometry_hidden_write_end(DomBridge *bridge);
#define SCRIPT_FRAME_REALM_LIMIT 16
#define SCRIPT_CHECKPOINT_CONTINUATION_LIMIT 8
/* Consecutive turns FinalizationRegistry cleanup may go without a task slot
   before one is admitted ahead of due tasks. */
#define SCRIPT_CLEANUP_STARVATION_ADVANCES 4u

/* The body of the external classic script running now, which
   js_rt_heap_growth_hook may release (release is then cleared and
   `released` set). */
typedef struct {
    ScriptSourceReleaseFn release;
    void *opaque;
    size_t length;
    bool released;
} ScriptRunningSource;

struct ScriptRuntime {
    Budget *budget;
    PocDocument *document;
    /* A forced geometry read must not replace the browser's presentation
       or consume its resource/mutation work while author code is active. */
    LayoutDocument *geometry_layout;
    LayoutDocument *geometry_incumbent;
    uint64_t geometry_content_generation;
    uint64_t geometry_style_generation;
    uint64_t geometry_sheet_generation;
    uint64_t geometry_font_generation;
    uint64_t geometry_container_generation;
    /* The five generations above as geometry_incumbent had them, kept
       while a scratch snapshot holds them (geometry_inputs_save). */
    uint64_t geometry_incumbent_inputs[5];
    /* Bumped whenever the geometry's inputs are captured or restored: the
       display:none ancestor last found for a kept write (hidden_root) is
       known only for the geometry of its serial. */
    uint64_t geometry_serial;
    uint64_t geometry_hidden_serial;
    lxb_dom_node_t *geometry_hidden_root;
    bool geometry_paint_stale;
    /* The inputs and free budget of the geometry build the budget last
       refused (script_runtime_flush_geometry_layout_reuse). */
    bool geometry_refused;
    uint64_t geometry_refused_content_generation;
    uint64_t geometry_refused_style_generation;
    uint64_t geometry_refused_sheet_generation;
    uint64_t geometry_refused_font_generation;
    uint64_t geometry_refused_container_generation;
    int geometry_refused_css_width;
    int geometry_refused_css_height;
    size_t geometry_refused_room;
    ScriptDocumentScope document_scope;
    JSRuntime *runtime;
    JSContext *context;
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    JSAtom validation_frame_packet_atom;
#endif
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    struct ValidationPhaseCapture *validation_phases;
    ValidationCpuCapture validation_cpu;
#endif
    BudgetQuickJSPool *quickjs_pool;
    /* Dedicated workers run in their own QuickJS contexts (realms) inside
       this runtime: a real global object, genuine top-level `this`, and
       importScripts that evaluates into that global. Slots are freed when
       the worker terminates or closes, and at runtime teardown. */
    struct {
        JSContext *context;
        JSValue global;
    } worker_realms[SCRIPT_WORKER_REALM_LIMIT];
    struct {
        JSContext *context;
        JSValue global;
        /* Core context owns the permanent global; other intrinsics are lazy.
           0 core-only, 1 initializing, 2 ready, 3 failed/retired. */
        uint8_t intrinsic_state;
        /* Eval runs under the policy of the frame whose eval was called, not
           the caller's. A new realm inherits its parent's, as about:blank and
           srcdoc do; committing a fetched document replaces it natively. */
        bool dynamic_code_allowed;
        long frame_handle;
    } frame_realms[SCRIPT_FRAME_REALM_LIMIT];
    /* Boot-window donation experiment (TILEFINCH_JS_BOOT_WINDOW_KB): the page
       heap opens at base_memory_limit plus the donated window; once the
       hydration transient collects back under the base, the limit shrinks
       to the base and the window returns to the shared engine budget. */
    size_t base_memory_limit;
    size_t boot_window_bytes;
    bool boot_window_active;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool heap_failure_census_reported;
    bool checkpoint_trace_suppressed;
#endif
    size_t boot_window_peak;
    uint64_t boot_window_advances;
    uint64_t boot_window_next_check;
    size_t boot_window_checks;
    uint64_t boot_window_returned_advance;
    /* Full JS_ComputeMemoryUsage walks paid on this realm. Decisions read
       the allocator-maintained count; only diagnostics take a census. */
    size_t heap_censuses;
#if defined(TILEFINCH_JSMEM_CENSUS)
    /* Heap census state: TILEFINCH_TRACE_JS_HEAP_DETAIL in the host lab,
       validation_js_heap_detail in a PSP validation build. */
    size_t jsmem_rejections_seen;
    size_t jsmem_next_high_water;
    unsigned jsmem_sequence;
    unsigned jsmem_oom_censuses;
    const char *jsmem_compile_name;
    size_t jsmem_compile_bytes;
#endif
    /* Memory-pressure growth (script_runtime_enable_heap_growth): the
       configured limit is a floor; the realm grows while the page Budget
       keeps its work reserve free, and gives growth back once collection
       shows it unused. */
    size_t reentrant_checkpoints_skipped;
    /* Interrupt-time sampling profiler (validation builds, or the host with
       TILEFINCH_TRACE_JS_PROFILE): wall time between polls charged to the
       innermost script line (self) and to each distinct function on the
       top frames (inclusive). Bounded; atoms are duplicated on insert. */
    struct ScriptProfile *profile;
    bool heap_growth_enabled;
    size_t heap_growth_floor;
    size_t heap_growth_ceiling;
    size_t heap_growth_reserve;
    size_t heap_growth_bytes;
    size_t heap_growth_peak_limit;
    size_t heap_growth_raises;
    /* Limit raises made ahead of need for collection headroom. */
    size_t heap_growth_pregrows;
    /* QuickJS heap bytes right after the latest collection: the live graph
       collection pacing grows from (0 before the first). */
    size_t heap_live_after_gc;
    size_t heap_collections;
    /* Page Budget refusals of the host memory a network delivery needs
       (js_rt_note_delivery_refusal); counted with the realm's own. */
    size_t delivery_refusals;
    /* Collection pacing (js_rt_gc_pacing): collections the allocation
       threshold ran, collections that left less than an amortized
       allocation step, and times it backed off for the rest of an advance.
       gc_task_starved counts this advance's consecutive starved
       collections that freed little. */
    size_t gc_threshold_collections;
    size_t gc_pacing_starved;
    size_t gc_pacing_backoffs;
    size_t gc_task_starved;
    /* The most collections one advance and one microtask checkpoint ran,
       and the count when the current advance began. */
    size_t gc_advance_collections_max;
    size_t gc_checkpoint_collections_max;
    size_t gc_advance_collections_begin;
    /* heap_growth_refusals when the current advance began. */
    size_t gc_advance_refusals;
    /* Collection time, timed builds only (host lab, validation). */
    uint64_t gc_total_ns;
    uint64_t gc_begin_ns;
    /* QuickJS heap bytes as the latest collection began. */
    size_t gc_live_before;
    /* setAttribute calls already cleared from the bridge by profile
       reports: the tilefinch-work record adds them back. */
    uint64_t work_attribute_writes_reported;
    size_t heap_growth_refusals;
    /* Last refused request: bytes it needed past the limit, and why
       (1 = page reserve reached, 2 = ceiling, 3 = request exceeds spare). */
    size_t heap_growth_refused_bytes;
    uint8_t heap_growth_refused_reason;
    /* Heap rejections inside compiles abandoned as memory refusals
       (js_rt_compile_source_type); not counted as realm exhaustion. */
    size_t compile_heap_rejections_absorbed;
    /* Memory rescue (script_runtime_arm_memory_rescue). */
    DocumentBodySnapshot memory_rescue;
    bool memory_rescue_armed;
    uint8_t memory_rescue_captures;
    size_t heap_growth_returns;
    uint64_t heap_growth_advances;
    uint64_t heap_growth_next_check;
    /* Consecutive return checks that returned nothing: each doubles the
       interval to the next full collection, as the boot window does. */
    size_t heap_growth_fruitless_checks;
    uint64_t deterministic_gc_advances;
    /* 0=deferred, 1=loading, 2=loaded, 3=failed. Uncommon platform modules
       keep their bytecode in ROM but do not occupy the page heap until a page
       first touches their standards-visible surface. */
    uint8_t lazy_bootstrap_state[SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT];
    /* The innermost lazy module loading, plus one (0: none), and whether
       it has taken its one hook claim (js_runtime_lazy_loading). */
    uint8_t lazy_loading_feature;
    bool lazy_hook_claimed;
    /* A bit per lazy placeholder: an assignment through it is calling a
       setter (runtime_lazy_bootstrap_assign). */
    uint8_t lazy_assigning[16];
    /* Open while lazy modules load (and the step that builds their
       objects runs): what the interfaces held when it opened, so that only
       what those modules define is marked when it closes. NULL inside an
       open window when the snapshot could not be taken. Loaded: some
       module evaluated in the open window. */
    bool platform_window_open;
    bool platform_window_loaded;
    JSPlatformSnapshot *platform_window;
    /* The realm's interfaces are snapshotted when its bootstrap ends
       (runtime_snapshot_interfaces). */
    bool interface_snapshot;
#ifdef JS_RT_TEST_INSTRUMENTS
    /* The bootstrap, eager and lazy, publishes its test-only probes. Off
       only when a host test asks for the shipping realm surface. */
    bool test_probes;
    /* Frame realm setups still to refuse (__tilefinchRefuseFrameSetups). */
    unsigned refused_frame_setups;
    /* A test's Web Crypto slice length in ns; 0 is SCRIPT_CRYPTO_SLICE_NS. */
    uint64_t crypto_slice_ns;
#endif
    Watchdog watchdog;
    DomBridge bridge;
    ScriptResult result;
    PromiseRejectionState promise_rejection_state;
    /* While an external classic script fetched cross-origin without CORS
       runs: its errors report muted (js_rt_report_script_error). */
    bool script_errors_muted;
    /* Set when an external classic or module script's own exception (a
       throw, or a SyntaxError in its source or its module graph) went to
       the window error event: that element then gets `load`, not `error`
       (HTML "execute the script element"). The external evaluation clears
       it before running and restores the outer value after. */
    bool script_error_reported;
    /* The same for any page script, inline ones included: set when its own
       exception went to the window error event. Each script evaluation
       entry clears it before running and restores the outer value after,
       so a nested script's report stays its own; the entry leaves its
       outcome in last_script_author_error
       (script_runtime_last_script_author_error). */
    bool script_author_error;
    bool last_script_author_error;
    /* Module imports the host could not fetch or admit
       (runtime_module_load_record): not author errors. */
    size_t module_host_failures;
    unsigned timeout_ms;
    /* Optional browser-owned absolute cap for the currently executing
       parser stage.  A later JS slice may arm after a blocking fetch, so a
       smaller per-call timeout alone cannot enforce a cumulative deadline. */
    uint64_t execution_deadline_cap_ms;
    size_t refreshed_mutations;
    /* A connected author mutation may already have destroyed storage borrowed
       by the incumbent layout before document_refresh() is asked to rebuild
       the document indices. Embedders must retire that page if the refresh is
       refused; ordinary script failures may still preserve static content. */
    bool document_refresh_failed_after_mutation;
    bool relayout_dirty;
    /* A blank-page recovery needs to distinguish finite author work that can
       still reveal the initial document from ambient duplex transports. The
       public pending_tasks census intentionally includes both. */
    size_t pending_timer_tasks;
    size_t pending_network_tasks;
    BrowserSession *session;
    char document_url[2048];
    char top_level_url[2048];
    ScriptModuleLoadCallback module_load;
    ScriptModuleFreeCallback module_release;
    void *module_opaque;
    ScriptModuleOpaqueDestroyCallback module_opaque_destroy;
    struct ScriptImportMapEntry *import_map;
    size_t import_map_count;
    bool import_map_prepared;
    ScriptModuleBaseEntry *module_bases;
    size_t module_base_count;
    size_t module_base_capacity;
    TilefinchCredentialsMode active_module_credentials;
    /* Wall time spent in nested module loads (QuickJS resolves imports
       inside the JS_Eval that compiles the importer), so each module's
       compile time can exclude its dependencies' fetch and compile. */
    uint64_t module_nested_ns;
    size_t inline_module_sequence;
    /* This realm's module bytecode generation; 0 until its first use. */
    uint32_t module_bytecode_generation;
    /* The root module being evaluated came from a no-store response. */
    bool module_root_no_store;
    /* Classic bytecode stores deferred to idle work (js_module_loader.c):
       compiled scripts, oldest first, until script_runtime_store_pending_
       bytecode() serializes them into the session table. NULL when none;
       `pending_bytes` is their sources' total, a stand-in for the
       bytecode they will add to the table. */
    struct ScriptBytecodePending *bytecode_pending;
    size_t bytecode_pending_count;
    size_t bytecode_pending_bytes;
    /* A store is serializing the oldest entry: collections must not
       release queued stores (script_runtime_gc_hook). */
    bool bytecode_pending_busy;
    /* The owner of the external classic script about to be evaluated can
       let go of its whole body once it is compiled
       (script_runtime_offer_source_release). Taken (and cleared) by the
       evaluator, so a nested evaluation never sees it; armed as the
       running script's while that script runs (innermost run only), where
       js_rt_heap_growth_hook may call it. */
    ScriptSourceReleaseFn source_release;
    void *source_release_opaque;
    bool source_released;
    ScriptRunningSource running_source;
    /* At most one source completion per tracked image; weak identities are
       checked before touching DOM across author tasks. Allocated on demand. */
    struct ScriptImageLoadState *image_load_states;
    size_t image_load_state_count;
    size_t image_load_state_capacity;
    size_t image_load_pending;
    struct ScriptImagePreloads *image_preloads;
    struct ScriptImageDecodeTickets *image_decode_tickets;
    /* Created lazily as a private callable, never published to page script. */
    JSValue media_update;
    /* The private WeakMap from each Document to its document.all
       collection (js_runtime/legacy_surface.inc). */
    JSValue document_all_cache;
    /* window.name and window.status (js_runtime/legacy_surface.inc); the
       name is mirrored to window_name_slot when the navigation keeps one. */
    JSValue window_name;
    JSValue window_status;
    ScriptWindowName *window_name_slot;
    /* The page window's own string names when author code starts, less the
       private __tilefinch bridges: what a frame WindowProxy reports as its
       own beyond its realm's (host_primitives.inc frame_scope_keys). */
    /* Element getAttribute, setAttribute, removeAttribute, URL and
       DOMException as the bootstrap left them, for the native reflected
       attributes (js_runtime/reflected_attributes.inc). */
    JSValue reflect_hooks[5];
    JSPropertyEnum *frame_page_names;
    uint32_t frame_page_name_count;
    /* Captured before author code can replace Window.navigator. The lazy
       OPFS module receives this identity through a temporary native-only
       handoff so StorageManager branding never consults a mutable global. */
    JSValue opfs_storage_manager;
    /* streams.js's WritableStream, captured as the lazy module defines it,
       before page script can replace the global: opfs.js extends this
       identity, handed over the same way. */
    JSValue streams_writable_stream;
    /* Installed by the lazy game-audio module, captured immediately, and
       removed before evaluation returns to author code. */
    JSValue game_audio_host_suspend;
    JSValue game_audio_host_complete;
    /* Installed by the lazy media-capture module and captured the same way:
       ends this realm's live tracks after the browser stopped capture, and
       names the track a MediaStreamAudioSourceNode takes from a stream. */
    JSValue media_capture_host_event;
    JSValue media_capture_stream_track;
    JSValue function_to_string;
    /* Trusted bootstrap callbacks retained before author code runs. The page
       may observe compatibility globals, but native scheduling, input
       dispatch, viewport updates and document lifecycle never look them up
       through the mutable Window object. */
    JSValue host_global;
    JSValue host_visual_viewport;
    JSValue host_window_dispatch;
    JSValue host_viewport_dispatch;
    JSValue host_event_constructor;
    JSValue host_trusted_event;
    /* dom.js's private share of browser-only helpers (reserved timers, the
       timer cancel, the crypto-scope stop), taken off the global before
       author code: lent to motion.js while it loads, and passed to the
       Worker installer. */
    JSValue dom_share;
    /* First resize only: do not charge every tiny realm for this code. */
    JSValue viewport_resize_dispatch;
    /* CONTENT_BLOCKER_SHIM_* answers this realm already gave for refused
       scripts (js_rt_answer_refused_script). */
    unsigned refused_script_shims;
    JSValue host_callbacks[SCRIPT_HOST_CALLBACK_COUNT];
#ifdef JS_RT_XHR_DIAGNOSTICS
    ScriptXHRDiagnostics *xhr_diagnostics;
    /* compat.js's recorder installer, handed over through the host channel
       in test-probe realms; JS_UNDEFINED otherwise. */
    JSValue xhr_diagnostics_install;
    JSValue private_outcome_classifier;
    bool private_outcome_classifying;
    uint32_t xhr_diagnostic_sequence;
    size_t xhr_diagnostic_cursor;
#endif
    /* Native-readable bootstrap state, so the per-turn loop and the result
       snapshot do not enter JS: the timer wheel and scroll flag (external
       Float64Array memory), and the plain records behind the network-queue
       and IndexedDB stat views (read as data properties, no getter call). */
    double host_state[SCRIPT_HOST_STATE_COUNT];
    JSValue host_network_queue_stats;
    JSValue host_indexed_db_stats;
    JSValue motion_style_flushed;
    /* Browser-delivered callbacks run with no enclosing author script. Resume
       their bounded listener dispatch only after QuickJS has drained the
       complete microtask checkpoint, before admitting another task. */
    JSValue checkpoint_continuations[
        SCRIPT_CHECKPOINT_CONTINUATION_LIMIT];
    uint8_t checkpoint_continuation_head;
    uint8_t checkpoint_continuation_count;
    TilefinchGamepadState gamepad_state;
    bool gamepad_state_valid;
    bool page_visibility_desired;
    /* window.alert/confirm/prompt (page_dialogs.inc). The top-level realm
       owns its page load's policy; a child frame's realm shares the policy
       of dialog_parent, the parent realm navigation names when the frame
       may open dialogs at all (NULL refuses every dialog). */
    PageDialogPolicy page_dialogs;
    struct ScriptRuntime *dialog_parent;
    /* Microphone capture (js_media_capture.c). A child frame reaches its
       top-level owner through media_capture_parent only while navigation
       found the frame allowed by Permissions Policy. The owner keeps the
       page load's "Allow this time" grant and prompt limits; every realm
       counts its own live tracks of session media_capture_generation, and
       names each by a serial in media_capture_tracks (0 = a free slot)
       with its enabled state (media_capture_disabled, a bit per slot), so a
       MediaStreamAudioSourceNode follows its own track. */
    struct ScriptRuntime *media_capture_parent;
    bool media_capture_frame_allowed;
    bool media_capture_session_grant;
    bool media_capture_permission_watch;
    uint8_t media_capture_dismissals;
    uint8_t media_capture_live_tracks;
    bool media_capture_prompting;
    uint32_t media_capture_dismiss_gesture;
    uint32_t media_capture_generation;
    uint32_t media_capture_permission_epoch;
    uint32_t media_capture_revoke_epoch;
    uint32_t media_capture_tracks[MEDIA_CAPTURE_TRACK_LIMIT];
    uint32_t media_capture_last_track;
    /* Sessions whose tracks this realm forgot before "ended" reached
       them (a request or release ran first); delivered next, oldest
       first, 0 = none. */
    uint32_t media_capture_ended_pending[2];
    uint8_t media_capture_disabled;
    bool page_visibility_queue[2];
    uint8_t page_visibility_queue_head;
    uint8_t page_visibility_queue_count;
    /* 0 idle, 1 Window, 2 VisualViewport. Each stage waits for the previous
       native delivery's complete microtask/continuation checkpoint. */
    uint8_t viewport_resize_phase;
    bool diagnostic_mobile_safari;
    /* Consecutive advances which ended with FinalizationRegistry cleanup
       queued but admitted none because due tasks took every slot. */
    uint8_t cleanup_starved_advances;
    TilefinchGameAudio *game_audio;
    bool game_audio_internal_slots;
    struct AudioSlotRuntime *game_audio_slots;
    struct ScriptLazyRuntimeBundle *lazy_webpack_bundles;
    uint32_t next_lazy_webpack_bundle_id;
    bool lazy_factory_recovery_pending;
    /* Program text of lazy-webpack factories whose functions refer to it by
       external id (it would otherwise be in the heap), the plain copies of
       it held, and the clock that orders those copies by last read. */
    size_t lazy_webpack_external_text_bytes;
    size_t lazy_webpack_plain_text_bytes;
    uint64_t lazy_webpack_plain_clock;
    /* Text of classic scripts compiled without toString() text in the
       heap (js_source_store.h; created by the first such compile). */
    struct ScriptSourceStore *source_store;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    ScriptRuntimeTimingMetrics timing_metrics;
#endif
#if defined(TILEFINCH_MEMORY_DIAGNOSTICS)
    /* QuickJS heap and this realm's Budget bytes when the browser
       bootstrap had run, before any page script (memory study). */
    size_t heap_after_bootstrap;
    size_t budget_after_bootstrap;
#endif
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
    /* Where the factory starts in its bundle (1-based line, and column in
       code points as QuickJS counts them), to report a SyntaxError found
       when the factory first compiles at the bundle's position. */
    uint32_t source_line;
    uint32_t source_column;
    JSValue compiled;
    JSValue wrapper;
    /* Factory text outside the heap (js_lazy_webpack.c): compiles whose
       functions still name this factory's program text by its external id,
       and a plain copy of that text (the compile's program buffer, or text
       decoded from compressed_source), valid up to plain_length, or NULL. */
    uint32_t external_owners;
    uint8_t plain_ascii;
    unsigned char *plain;
    size_t plain_length;
    size_t plain_input;
    uint64_t plain_used;
#ifndef TILEFINCH_NO_TRACE
    /* Factory-cache measurement (TILEFINCH_TRACE_FACTORY_CACHE, lab only):
       the compiled program held from compile to the end of its first call,
       when it is serialized again, and how often this factory compiled. */
    JSValue census_program;
    uint32_t census_compiles;
#endif
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
#ifndef TILEFINCH_NO_TRACE
    uint64_t census_digest; /* first 8 bytes of the bundle's SHA-256 */
#endif
} ScriptLazyRuntimeBundle;

typedef struct {
    JSValue value;
    uint8_t csp_grant;
    /* The scope counts in DomBridge.destructive_writes_ignored. */
    bool ignores_writes;
} ScriptCurrentScriptScope;

/* Cross-module helpers defined in js_runtime.c. */
void js_rt_saturating_add_u64(uint64_t *value, uint64_t amount);
bool js_rt_bridge_node_slot_for_handle(const DomBridge *bridge,
                                       int64_t handle, size_t *slot);
int64_t js_rt_bridge_register_node(DomBridge *bridge, lxb_dom_node_t *node);
/* The node's current handle, or 0 when it has none; never registers. */
/* Marks the handle's slot as named by native state or a handle-keyed
   bootstrap table: its connected node keeps the slot when its wrapper dies
   (DomBridge.node_handle_kept_bits). */
void js_rt_bridge_keep_node_handle(DomBridge *bridge, int64_t handle);
int64_t js_rt_bridge_find_node_handle(const DomBridge *bridge,
                                      const lxb_dom_node_t *node);
void js_rt_record_exception(JSContext *context, ScriptResult *result);
/* HTML "report an exception" for `error`, which an author script named
   `name` threw while it compiled or ran: the Window's onerror and error
   event, then the host log unless one cancels it (dom.js
   __tilefinchReportUncaught, which timers and event handlers share).
   False, reporting nothing, for the browser's own failures: an
   interrupted or memory-exhausted run, a lazy factory (its requirer
   reports it), or a script that is not the page's. */
bool js_rt_report_script_error(JSContext *context, JSValueConst error,
                               const char *name,
                               ScriptCompileSourceKind source_kind);
/* js_rt_record_exception for the pending exception of such a script's
   compile or run, which is then reported as above (a refused allocation
   is not); true when it was reported. */
bool js_rt_record_script_exception(JSContext *context, const char *name,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result);
/* Whether the classic script `script_url` that `script_node` loaded is
   cross-origin and was fetched without CORS (no crossorigin attribute). */
bool js_rt_script_errors_muted(const ScriptRuntime *runtime,
                               lxb_dom_node_t *script_node,
                               const char *script_url);
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
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
void js_rt_validation_cpu_enter(ScriptRuntime *runtime, unsigned phase);
void js_rt_validation_cpu_leave(ScriptRuntime *runtime, unsigned phase);
#endif
bool js_rt_runtime_checkpoint_pending(const ScriptRuntime *runtime);
size_t js_rt_prepare_network_response_delivery(ScriptRuntime *runtime,
                                               size_t response_bytes);
bool js_rt_runtime_refresh(ScriptRuntime *runtime);
void js_rt_runtime_update_result(ScriptRuntime *runtime,
                                 ScriptResult *result);
/* The per-frame subset of js_rt_runtime_update_result(): no telemetry. */
void js_rt_runtime_update_frame_result(ScriptRuntime *runtime,
                                       ScriptResult *result);
uint64_t js_rt_monotonic_time_ns(void);
/* JS_ComputeMemoryUsage walks every live object; diagnostics only. */
void js_rt_heap_census(ScriptRuntime *runtime, JSMemoryUsage *usage);
#if defined(CONFIG_TILEFINCH_HEAP_DETAIL) && !defined(TILEFINCH_NO_TRACE)
/* Heap composition census (host lab only); `safe` says the
   caller is at a point where a forced collection is allowed. */
void js_rt_jsmem_census_check(ScriptRuntime *runtime, const char *where,
                              bool safe);
bool js_rt_jsmem_enabled(void);
#endif
#if defined(TILEFINCH_JSMEM_CENSUS)
void js_rt_jsmem_census(ScriptRuntime *runtime, const char *reason, bool gc);
#endif
#if defined(TILEFINCH_MEMORY_DIAGNOSTICS)
/* Records heap_after_bootstrap and budget_after_bootstrap. */
void js_rt_note_bootstrap_memory(ScriptRuntime *runtime);
#endif
/* HTML deliberately leaves the transient-activation duration to the user
   agent. Five seconds matches the interoperability window used by major
   engines while keeping privileged actions tightly bounded on the PSP. */
#define SCRIPT_TRANSIENT_USER_ACTIVATION_MS UINT64_C(5000)
/* ASCII case-insensitive equality of two CSS property names (inline style
   declarations, SVG presentation attributes). */
static inline bool js_rt_property_name_equal(const char *first,
                                             size_t first_length,
                                             const char *second,
                                             size_t second_length)
{
    if (first_length != second_length) return false;
    for (size_t i = 0; i < first_length; i++) {
        if (tolower((unsigned char) first[i])
            != tolower((unsigned char) second[i])) return false;
    }
    return true;
}

/* Fill a result object or array a native has just created. JS_SetProperty*
   is an ordinary [[Set]]: an accessor a page put on Object.prototype (named
   keys) or Array.prototype (indices) would run author code in the middle
   of the native, while it still holds node, layout or image pointers.
   Defining an own data property never consults the prototype chain. Both
   consume `value`, as JS_SetProperty* does; -1 means an exception. */
static inline int js_rt_define_field(JSContext *context, JSValueConst object,
                                     const char *name, JSValue value)
{
    return JS_DefinePropertyValueStr(context, object, name, value,
                                     JS_PROP_C_W_E);
}

static inline int js_rt_define_index(JSContext *context, JSValueConst array,
                                     uint32_t index, JSValue value)
{
    return JS_DefinePropertyValueUint32(context, array, index, value,
                                        JS_PROP_C_W_E);
}

/* The DOM-native guard (DomBridge.native_depth). A guarded native's body
   is NAME_unguarded; NAME enters, runs it and leaves, and the outermost
   leave destroys the subtrees discarded meanwhile. A plain C wrapper per
   native keeps the engine's direct native call path. */
#define JS_RT_DEFERRED_DISCARD_LIMIT 4096u
void js_rt_bridge_drain_deferred_discards(JSContext *context,
                                          DomBridge *bridge);
static inline DomBridge *js_rt_native_enter(JSContext *context)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL) bridge->native_depth++;
    return bridge;
}

static inline void js_rt_native_leave(JSContext *context, DomBridge *bridge)
{
    if (bridge == NULL) return;
    if (--bridge->native_depth == 0 && bridge->deferred_discard_count != 0)
        js_rt_bridge_drain_deferred_discards(context, bridge);
}

/* `storage` is empty or static: JS_RT_GUARDED_NATIVE(, js_dom_x)(args). */
#define JS_RT_GUARDED_NATIVE(storage, name)                                  \
    static JSValue name##_unguarded(JSContext *, JSValueConst, int,          \
                                    JSValueConst *);                         \
    storage JSValue name(JSContext *context, JSValueConst this_value,        \
                         int argc, JSValueConst *argv)                       \
    {                                                                        \
        DomBridge *guarded = js_rt_native_enter(context);                    \
        JSValue value = name##_unguarded(context, this_value, argc, argv);   \
        js_rt_native_leave(context, guarded);                                \
        return value;                                                        \
    }                                                                        \
    static JSValue name##_unguarded

/* Whether `scope` is `node` or one of its first 256 ancestors. Shared by
   the mutation journal's overflow roots and the computed-style ring's
   scoped invalidation. */
static inline bool js_rt_node_within(const lxb_dom_node_t *node,
                                     const lxb_dom_node_t *scope)
{
    for (unsigned depth = 0; node != NULL && depth < 256u;
         node = node->parent, depth++)
        if (node == scope) return true;
    return false;
}

static inline uint64_t js_rt_bridge_elapsed_ms(const DomBridge *bridge)
{
    if (bridge == NULL) return 0;
    if (bridge->deterministic_clock) return bridge->clock_host_elapsed_ms;
    uint64_t now_ns = js_rt_monotonic_time_ns();
    return now_ns > bridge->performance_origin_ns
        ? (now_ns - bridge->performance_origin_ns) / UINT64_C(1000000)
        : 0;
}
static inline void js_rt_bridge_notify_user_activation(DomBridge *bridge)
{
    if (bridge == NULL) return;
    uint64_t now_ms = js_rt_bridge_elapsed_ms(bridge);
    bridge->user_activation_active = true;
    bridge->user_activation_has_been_active = true;
    bridge->user_activation_generation++;
    bridge->user_activation_expires_ms =
        now_ms > UINT64_MAX - SCRIPT_TRANSIENT_USER_ACTIVATION_MS
        ? UINT64_MAX : now_ms + SCRIPT_TRANSIENT_USER_ACTIVATION_MS;
}
static inline bool js_rt_bridge_user_activation_is_active(DomBridge *bridge)
{
    if (bridge == NULL || !bridge->user_activation_active) return false;
    if (js_rt_bridge_elapsed_ms(bridge)
        < bridge->user_activation_expires_ms) return true;
    bridge->user_activation_active = false;
    return false;
}
/* A bootstrap hook the bridge calls (SCRIPT_HOST_WRAP_NODE and the other
   bridge hooks), captured with the runtime's host callbacks; undefined for
   a bridge outside a runtime. */
static inline JSValueConst js_rt_bridge_trusted_hook(const DomBridge *bridge,
                                                     ScriptHostCallback hook)
{
    return bridge->host != NULL ? bridge->host->host_callbacks[hook]
                                : JS_UNDEFINED;
}

static inline void js_rt_bridge_consume_user_activation(DomBridge *bridge)
{
    if (bridge == NULL) return;
    bridge->user_activation_active = false;
    bridge->user_activation_expires_ms = 0;
}
void js_rt_saturating_add_size(size_t *value, size_t amount);
#ifdef JS_RT_XHR_DIAGNOSTICS
int js_rt_private_classify(ScriptRuntime *runtime, JSValueConst value,
                          JSValueConst parse, int event);
#endif
void js_rt_trace_script_quota_rejection(DomBridge *bridge,
                                        const char *reason);
bool js_rt_runtime_script_checkpoint(ScriptRuntime *runtime,
                                     size_t work_units);
bool js_rt_runtime_callback_checkpoint(ScriptRuntime *runtime);
bool js_rt_runtime_native_checkpoint(ScriptRuntime *runtime);
JSValue js_rt_throw_task_interruption(JSContext *context, const char *message);
JSValue js_rt_compile_source_type(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   int evaluation_type,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result, bool *admitted);
bool js_rt_admit_cached_compile_source(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result);
/* Whether module text goes to the function-source store (on unless the lab
   switch TILEFINCH_JS_MODULE_SOURCE_STORE=0 turns it off). */
bool js_rt_module_source_store_enabled(void);
/* Function source outside the heap (evaluation.inc, "Function source
   outside the heap"): bytecode restored from the classic or module cache
   refers to its script's text by offset. Call begin before JS_ReadObject()
   with the script text (NULL if not at hand: functions that need it then
   do not restore) and end after it with what begin returned and the same
   text. A false end means the restored functions cannot read their text
   (the Budget refused the copy): discard the restored value as a failed
   restore. */
uint32_t js_rt_source_store_begin_restore(ScriptRuntime *runtime,
                                          const char *source, size_t length);
bool js_rt_source_store_end_restore(ScriptRuntime *runtime,
                                    uint32_t source_id, const char *source);
/* Gives up about `needed` bytes of retained function source (compressing,
   then dropping text no uncompiled function needs); allocates only Budget
   memory, never the QuickJS heap; returns the bytes released. */
size_t js_rt_source_store_relieve(ScriptRuntime *runtime, size_t needed);
/* A safe point (advance boundary): compresses retained function source the
   page's room no longer allows plain, all of it when `refused` (the realm
   was refused growth since the last one). */
void js_rt_source_store_safe_point(ScriptRuntime *runtime, bool refused);
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
/* As js_rt_evaluate_source_type_at for an external module root: compiles it
   through js_rt_module_compile_external(). */
bool js_rt_evaluate_external_module_at(
    JSContext *context, const char *source, size_t length, const char *name,
    const char *response_url, const char *module_referrer_policy,
    TilefinchCredentialsMode module_credentials, ScriptResult *result);
bool js_rt_evaluate_source(JSContext *context, const char *source,
                           size_t length, const char *name,
                           ScriptResult *result);
/* As js_rt_evaluate_source for the browser's own script: it and the
   functions it defines are browser code and read the interface snapshot
   (JS_EVAL_FLAG_TRUSTED); code it compiles at run time is not. */
bool js_rt_evaluate_browser_source(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   ScriptResult *result);
const char *js_rt_bridge_calculated_base_url(DomBridge *bridge);
bool js_rt_image_preload_changed(DomBridge *bridge, lxb_dom_node_t *node);
int js_rt_image_decode_ticket(DomBridge *bridge, long handle, int op, int ticket);
const ImageResource *js_rt_image_preload_find(DomBridge *bridge,
                                             const lxb_dom_node_t *node);
ImageResources *js_rt_image_preload_resources(DomBridge *bridge);
/* Before pinning js_rt_image_preload_resources(bridge)->items[index]. */
void js_rt_image_preload_prepare_pin(DomBridge *bridge, size_t index);
int js_rt_image_preload_status(DomBridge *bridge, const lxb_dom_node_t *node);
/* Whether the page was told `node`'s current source loaded, or is about to be,
   and the natural size it reported: an image's load outlives its table entry,
   which viewport admission can remove and bring back. */
bool js_rt_image_load_remembered(DomBridge *bridge, const lxb_dom_node_t *node,
                                 int *natural_width, int *natural_height);
/* Whether `node` is a detached image refused a load slot whose error event
   has been dispatched: broken until its source changes. */
bool js_rt_image_preload_broken(DomBridge *bridge, const lxb_dom_node_t *node);
/* After node handles are invalidated and before their nodes are freed:
   forget every preload pointer to them, so an image allocated at a reused
   address never matches stale entries. */
void js_rt_image_preload_forget_released(DomBridge *bridge);

/* js_module_loader.c helpers used by the sibling translation units. */
/* Compile one external module record (a root or an imported dependency) the
   way JS_Eval(JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY) does,
   including synchronous resolution of its imports, and applying the page's
   module source-retention policy. Returns the module value or JS_EXCEPTION;
   *admitted reports whether the compile-admission policy accepted it. */
JSValue js_rt_module_compile_external(
    ScriptRuntime *runtime, JSContext *context, const char *source,
    size_t source_length, const char *module_name, const char *response_url,
    bool response_no_store, ScriptResult *result, bool *admitted);
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
/* A root's grant; descendants registered later inherit it. */
void js_rt_runtime_module_csp_grant_set(ScriptRuntime *runtime,
                                        const char *request_url,
                                        uint8_t grant);
bool js_rt_runtime_module_root_register(
    ScriptRuntime *runtime, const char *request_url, const char *response_url,
    const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials);
bool js_rt_runtime_module_root_state_set(
    ScriptRuntime *runtime, const char *request_url,
    ScriptModuleMapStatus state);
const char *js_rt_runtime_module_base_lookup(const ScriptRuntime *runtime,
                                             const char *request_url);

/* One native entry point the realm installs on a global (or the host
   channel). Each owner keeps a static table of these; js_rt_install_natives
   installs a table in row order. The flags are the native's whole
   descriptor policy, so the row is the only place a native is described. */
enum {
    /* Configurable, so the bootstrap can delete it once captured. */
    JS_RT_NATIVE_TRANSIENT = 1u << 0,
    /* Writable and configurable in a realm created with
       allow_test_network_primitive_overrides; otherwise frozen. */
    JS_RT_NATIVE_TEST_OVERRIDABLE = 1u << 1,
    /* A plain function, never the profiling trampoline. */
    JS_RT_NATIVE_UNPROFILED = 1u << 2,
    /* Left out under the TILEFINCH_DISABLE_COMPRESSION_STREAMS lab switch. */
    JS_RT_NATIVE_COMPRESSION_STREAMS = 1u << 3,
};
typedef struct {
    const char *name;
    JSCFunction *function;
    uint8_t arguments;
    uint8_t flags;
} JsRtNative;

bool js_rt_install_native(JSContext *context, JSValue target,
                          const JsRtNative *native);
bool js_rt_install_natives(JSContext *context, JSValue target,
                           const JsRtNative *natives, size_t count);
#define JS_RT_INSTALL_NATIVES(context, target, table) \
    js_rt_install_natives((context), (target), (table), \
                          sizeof(table) / sizeof((table)[0]))
char *js_rt_dynamic_copy_text(Budget *budget, const char *text);
size_t js_rt_dynamic_requeue_fetching(DomBridge *bridge);
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
/* After inserting the siblings first..last under parent: prepares the
   scripts that insertion made ready (parent too, when it is a script).
   first NULL means parent's whole subtree. */
int js_rt_dynamic_prepare_insertion(JSContext *context,
                                    lxb_dom_node_t *parent,
                                    lxb_dom_node_t *first,
                                    lxb_dom_node_t *last);

/* js_fetch_cors.c entry points. */
bool js_fetch_cors_install(JSContext *context, JSValue global);
void js_rt_event_sources_destroy(DomBridge *bridge);
/* Cancels in-flight CORS preflights and frees the realm's preflight keys;
   before the realm scheduler is destroyed. */
void js_rt_cors_preflights_destroy(DomBridge *bridge);
void js_rt_preloaded_scripts_destroy(DomBridge *bridge);
/* Once per advance, before dynamic scripts start: completes pending
   entries whose requests finished, and releases the unconsumed ones
   SCRIPT_PRELOADED_SCRIPT_UNUSED_MS after the load event. */
void js_rt_preloaded_scripts_step(DomBridge *bridge);
/* The load event fired now. */
void js_rt_preloaded_scripts_note_load(DomBridge *bridge);
/* A queued dynamic script whose preload is still in flight: it waits. */
bool js_rt_dynamic_task_awaits_preload(DomBridge *bridge,
                                       const ScriptDynamicTask *task);
/* Decides completed preflights, releasing their scheduler entries; once
   per host turn, so one whose request was aborted does not hold its entry
   until the realm's next request. */
void js_rt_cors_preflights_settle(DomBridge *bridge);
bool js_rt_event_sources_deliver(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 size_t *author_tasks);
void js_rt_websockets_destroy(DomBridge *bridge);
/* Shared owner-thread entry for fully constructed native network events. */
bool js_rt_network_call_global(ScriptRuntime *runtime, const char *name,
                              int argc, JSValueConst *argv);
size_t js_rt_websockets_abort(DomBridge *bridge);
/* Owner-thread delivery of an already-taken transport event. Message bytes
   are in bridge.websocket_event_scratch and copied before author callbacks. */
bool js_rt_websocket_deliver_event(
    ScriptRuntime *runtime, size_t slot, const FetchWebSocketEvent *event);
bool js_rt_websockets_deliver(ScriptRuntime *runtime,
                              size_t completion_budget,
                              size_t *author_tasks);
void js_rt_multiplayer_destroy(DomBridge *bridge);
size_t js_rt_multiplayer_abort(DomBridge *bridge);
bool js_rt_multiplayer_deliver(ScriptRuntime *runtime,
                               size_t completion_budget,
                               size_t *author_tasks);
/* Fills ScriptResult's last_network_* fields, which only the host lab,
   tests and the validation input-script harness read; builds without
   tracing or the validation log leave them untouched. */
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
void js_rt_record_network_response(ScriptResult *result,
                                   const FetchResult *fetched);
#else
static inline void js_rt_record_network_response(ScriptResult *result,
                                                 const FetchResult *fetched)
{
    (void) result;
    (void) fetched;
}
#endif
bool js_rt_script_set_response_body(JSContext *context, JSValue response,
                                    const FetchResult *fetched,
                                    bool prefer_text);
/* Offers a fetch/XHR response the page can read (same-origin, or
   cross-origin and CORS-readable; never opaque) to media discovery
   (media_json_response_remember): top-level documents only. */
void js_rt_remember_media_json(DomBridge *bridge, const FetchResult *fetched,
                               TilefinchRequestDestination destination);
void js_rt_script_store_response_cookies(
    DomBridge *bridge, const FetchResult *fetched, const char *fallback_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    TilefinchRequestDestination destination);
bool js_rt_script_response_origin_allowed(
    const DomBridge *bridge, const FetchResult *fetched,
    const char *request_url_or_origin,
    TilefinchRequestDestination destination, TilefinchRequestMode mode,
    TilefinchCredentialsMode credentials);
bool js_rt_script_resource_timing_allowed(
    const DomBridge *bridge, const FetchResult *fetched,
    const char *response_url);
uint8_t js_rt_script_resource_timing_protocol(long version);
bool js_rt_resource_timing_body_info_visible(
    const DomBridge *bridge, TilefinchRequestMode mode,
    const char *response_url, bool redirect_origin_tainted);
bool js_rt_resource_timing_minimize_content_type(
    const char *source, char *output, size_t capacity);
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
/* Raises the realm's source total past its configured floor while the page
   Budget keeps the realm's growth reserve free (see bridge_state.inc). */
void js_rt_bridge_script_bytes_admit(DomBridge *bridge, size_t bytes);
/* Response-cache reclamation is safe only without borrowed cache metadata. */
size_t js_rt_dynamic_response_bound(DomBridge *bridge, bool reclaim_responses);
bool js_rt_heavy_gate_holds(ScriptRuntime *runtime, ScriptDynamicTask *task);
/* Heap growth refusal: copy the body for memory rescue when armed. */
void js_rt_memory_rescue_capture(ScriptRuntime *runtime);
/* Records `source_bytes` of script restored from cached bytecode (RAM, or
   the Memory Stick tier when `from_disk`) for the heavy-page estimate. */
void js_rt_heavy_note_restored(ScriptRuntime *runtime, size_t source_bytes,
                               bool from_disk);
ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve_known(
    DomBridge *bridge, ScriptQuotaCountMode count_mode,
    size_t exact_source_bytes, ScriptQuotaReservation *reservation);
bool js_rt_bridge_script_quota_expand(
    DomBridge *bridge, ScriptQuotaReservation *reservation,
    size_t exact_source_bytes);
/* One ResourceLoader segment of a response; segment_index (its statement
   index) keys its classic bytecode, which a no-store response never keeps. */
bool js_rt_evaluate_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    size_t segment_index, bool response_no_store, bool final_segment);
bool js_rt_preflight_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    size_t segment_index, bool response_no_store);
/* A dynamically inserted classic script's response. */
bool js_rt_evaluate_external_classic_dynamic(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool response_no_store);

JSValue js_rt_wrap_dom_handle(JSContext *context, JSValueConst handle);

typedef struct {
    lxb_dom_node_t *next;
    const lxb_dom_node_t *boundary;
    /* Nodes the walk may visit (and levels it may climb at once); zero
       selects DOM_TRAVERSAL_VISIT_LIMIT. `exhausted` is set when the walk
       stopped at that limit rather than at the end of the subtree. */
    size_t visit_limit;
    bool exhausted;
    size_t visited;
    size_t next_depth;
    size_t current_depth;
} DomDocumentOrderTraversal;

/* document.all's view of a tree: its elements in tree order, exactly as
   querySelectorAll("*") lists them, visited without registering handles. */
typedef struct {
    DomBridge *bridge;
    DomDocumentOrderTraversal traversal;
    const lxb_dom_node_t *boundary;
    size_t shadow_depth;
} DomElementWalk;
/* scope 0 walks the realm's document; otherwise scope is a Document node
   handle. False when it names no document. */
bool js_rt_element_walk_init(DomElementWalk *walk, DomBridge *bridge,
                             int64_t scope);
lxb_dom_node_t *js_rt_element_walk_next(DomElementWalk *walk);
/* The trusted wrapper of `node`, registering its handle; a RangeError when
   the handle table is exhausted. */
JSValue js_rt_bridge_wrap_node(JSContext *context, DomBridge *bridge,
                               lxb_dom_node_t *node);
/* The native tree behind a Document object: 0 for the realm's document, a
   Document node handle, -1 for a script-side document (or any other
   object), or -2 with an exception pending. */
int64_t js_rt_document_scope(JSContext *context, JSValueConst value);

uintptr_t js_rt_node_owner_identity(const lxb_dom_node_t *node);
bool js_rt_node_is_strict_descendant(const lxb_dom_node_t *node,
                                     const lxb_dom_node_t *ancestor);
ScriptElementState *js_rt_script_element_state_find(
    DomBridge *bridge, const lxb_dom_node_t *node);
ScriptElementState *js_rt_script_element_state_register(
    DomBridge *bridge, lxb_dom_node_t *node, bool html);
bool js_rt_script_element_parser_started(lxb_dom_node_t *node);
void js_rt_script_element_states_purge_marked(
    DomBridge *bridge,
    const unsigned char marked[SCRIPT_DYNAMIC_NODE_LIMIT]);

/* js_dom_bindings.c symbols referenced by the orchestrator (install
   chain and shared node/mutation machinery). */
void bridge_invalidate_node_slot(DomBridge *bridge, size_t slot);
void bridge_release_native_node_pin(DomBridge *bridge, int64_t handle);
void bridge_pin_node_slot(DomBridge *bridge, size_t slot);
/* Takes document.write()'s insertions since the last call into the DOM
   versions and the computed-style cache (js_dom_bindings.c); every read
   that compares those calls it first. The _for_layout form, called by
   style and geometry reads, also marks the layout dirty once per write, so
   the read rebuilds it (and a sheet a written <style> or link changed):
   only a script that reads layout after writing pays for it. */
void js_rt_bridge_note_parser_writes(DomBridge *bridge);
void js_rt_bridge_note_parser_writes_for_layout(DomBridge *bridge);
/* Before a read resolves style: install a pending stylesheet change
   (script_runtime_set_sheet_settle_callback). */
void js_rt_bridge_settle_pending_sheets(DomBridge *bridge);
bool js_rt_bridge_flush_synchronous_layout(DomBridge *bridge,
                                           lxb_dom_node_t *node);
/* Script never observes a truncated layout (see bridge_state.inc). */
const LayoutDocument *js_rt_bridge_layout(const DomBridge *bridge);
bool js_rt_bridge_layout_truncated(const DomBridge *bridge);
const LayoutNodeBox *js_rt_bridge_proven_box(const DomBridge *bridge,
                                             const lxb_dom_node_t *node);
bool js_rt_notify_style_flushed(JSContext *context);
bool bridge_node_is_connected(const lxb_dom_node_t *node);
void js_rt_bridge_note_canvas_mutation(DomBridge *bridge,
                                       lxb_dom_node_t *node,
                                       bool paint_only);
JSValue js_canvas_commit_surface(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_canvas_image_source(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
/* Bootstrap-only (installed only while canvas.js is evaluated, which
   captures them): taint a canvas and move its pixels into its native store,
   create or release an ImageBitmap store, the taint query, pattern fill,
   and the PNG encoder's checksums. */
JSValue js_canvas_native_surface(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_canvas_bitmap_surface(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_canvas_origin_clean(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_canvas_raster_pattern(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_canvas_png_checksum(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
bool js_rt_bridge_canvas_tainted(const DomBridge *bridge,
                                 const lxb_dom_node_t *node);
void js_rt_bridge_canvas_taints_retire_subtree(DomBridge *bridge,
                                               const lxb_dom_node_t *root);
void js_rt_bridge_canvas_taints_free(DomBridge *bridge);
JSValue js_webgl_render(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_webgl_index_maximum(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_webgl_finite_float32(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_webgl_combine_matrix4(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_webgl_read_pixels(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_webgl_snapshot(JSContext *context,
                          JSValueConst this_value,
                          int argc, JSValueConst *argv);
JSValue js_webgl_release_surface(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_webgl_publish_frame(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
bool js_webgl_realm_epoch_advance(DomBridge *bridge);
void js_webgl_realm_epoch_release(DomBridge *bridge);
bool js_wasm_runtime_init(JSRuntime *runtime);
void js_wasm_release_idle_runtime(void);
#if defined(TILEFINCH_HAVE_WAMR_COMPONENT)
void js_wasm_component_configure(const char *program_directory);
#endif
JSValue js_wasm_available(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv);
JSValue js_wasm_compile(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_wasm_validate(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_wasm_instantiate(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_wasm_module_info(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_wasm_memory_grow(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_wasm_global_get(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_wasm_global_set(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_wasm_detach_buffer(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv);
/* Whether value is an attached ArrayBuffer aliasing WebAssembly linear
   memory; such a buffer cannot be transferred or detached by script. */
bool js_wasm_buffer_aliases_memory(JSContext *context, JSValueConst value);
/* Guards the realm's ArrayBuffer.prototype.transfer and
   transferToFixedLength against WebAssembly.Memory buffers. Run once per
   realm, after its typed-array intrinsics exist and before author code. */
bool js_wasm_guard_buffer_transfer(JSContext *realm);
JSValue js_wasm_snapshot_source(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_set_fullscreen(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_page_controls_notice(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
TilefinchGameAudio *js_rt_game_audio_engine(ScriptRuntime *runtime);
/* A browser modal opened at OPENED_MS (js_rt_monotonic_time_ns() / 1e6)
   inside script has closed: its time is moved out of the watchdog's
   deadlines (page_dialogs.inc). */
void js_rt_extend_watchdog_after_modal(ScriptRuntime *runtime,
                                      uint64_t opened_ms);
/* Microphone capture natives and lifecycle (js_media_capture.c). */
JSValue js_media_capture_command(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_media_capture_permission(JSContext *context,
                                    JSValueConst this_value, int argc,
                                    JSValueConst *argv);
/* Task-boundary work (runtime_loop.inc runs the author code): the
   generation of a session the browser stopped while this realm held live
   tracks (0 when none; the realm's count is cleared), and whether a
   permissions.query() status must re-read its state. */
uint32_t js_rt_media_capture_take_ended(ScriptRuntime *runtime);
bool js_rt_media_capture_take_permission_change(ScriptRuntime *runtime);
bool js_rt_media_capture_event_pending(const ScriptRuntime *runtime);
/* Releases the realm's live tracks (teardown). */
void js_rt_media_capture_release(ScriptRuntime *runtime);
/* What a MediaStreamAudioSourceNode bound to TRACK (a serial the realm
   handed to its track) plays: nothing once the track or its session ended,
   silence while the page disabled it, else the microphone. */
typedef enum {
    JS_RT_MEDIA_TRACK_ENDED = 0,
    JS_RT_MEDIA_TRACK_DISABLED,
    JS_RT_MEDIA_TRACK_LIVE
} JsRtMediaTrackState;
JsRtMediaTrackState js_rt_media_capture_track_state(
    const ScriptRuntime *runtime, uint32_t track);
bool js_rt_game_audio_lifecycle(ScriptRuntime *runtime, bool destroy);
/* The JS Web Audio reference (game-audio.js over the command channel below)
   serves host differential tests and explicit validation A/B runs. Shipping
   PSP builds have only the native audio slots. */
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_GAME_AUDIO_REFERENCE 1
JSValue js_game_audio_decode(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_game_audio_command(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
#endif
JSValue js_canvas_raster_rect(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_canvas_raster_rect_batch(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
JSValue js_canvas_measure_text(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_canvas_raster_path(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_canvas_raster_paint_batch(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv);
JSValue js_canvas_raster_image(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_canvas_raster_image_batch(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv);
lxb_dom_node_t *dom_document_order_next(
    DomDocumentOrderTraversal *traversal);
/* Continue after `node`'s subtree instead of inside it; `node` is the node
   dom_document_order_next just returned. */
void dom_document_order_skip_children(DomDocumentOrderTraversal *traversal,
                                      lxb_dom_node_t *node);
JSValue js_computed_style_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_computed_style_read(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_computed_style_values(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_rendered_text(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_transition_discover(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_transition_snapshot(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_computed_style_support(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv);
JSValue js_timing_function(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_attribute_change_may_affect_has(JSContext *context,
                                           JSValueConst this_value,
                                           int argc, JSValueConst *argv);
JSValue js_style_reach(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_append(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv);
JSValue js_dom_prepare_dynamic_subtree(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv);
JSValue js_dom_attributes(JSContext *context,
                          JSValueConst this_value,
                          int argc, JSValueConst *argv);
JSValue js_dom_body(JSContext *context, JSValueConst this_value,
                    int argc, JSValueConst *argv);
JSValue js_dom_parser_write(JSContext *context, JSValueConst this_value,
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
JSValue js_dom_custom_element_candidates(JSContext *context,
                                         JSValueConst this_value,
                                         int argc, JSValueConst *argv);
JSValue js_dom_traverse(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv);
JSValue js_dom_observe_parser_insertions(JSContext *context,
                                         JSValueConst this_value,
                                         int argc, JSValueConst *argv);
JSValue js_dom_named_element_ids(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_frame_handles(JSContext *context, JSValueConst this_value,
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
JSValue js_dom_get_text_prefix(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_dom_get_style_attribute_prefix(
    JSContext *context, JSValueConst this_value,
    int argc, JSValueConst *argv);
JSValue js_dom_insert_before(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_is_connected(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_dom_shadow_relevant(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_dom_root_node(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_node_owner(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv);
void js_rt_bridge_computed_style_cache_free(DomBridge *bridge);
/* Releases the used-geometry records (at a task boundary or before a
   layout build). */
void js_rt_bridge_computed_style_release_records(DomBridge *bridge);
/* Drops the computed-style memo and the cached ancestor styles a connected
   script mutation can change (js_computed_style.c); the mutation journal
   calls it for every recorded mutation. */
void js_rt_bridge_computed_style_note_mutation(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length, bool relational,
    const uint32_t *changed_tokens, size_t changed_token_count);
/* The cascade changed only within the subtrees of `roots` (and what they
   pass on by inheritance), or anywhere when `roots` is NULL: the retained
   styles there go at the next read. False when the change could not be
   scoped (NULL roots, a full queue): every retained style goes. */
bool js_rt_bridge_computed_style_drop_within(DomBridge *bridge,
                                             lxb_dom_node_t *const *roots,
                                             size_t count);
/* After a host style write (document_style_changed) that the bridge made
   with the generation folded to `before` just ahead of it, and whose every
   effect on computed styles drops already queued cover: retained styles
   keep following the host generation instead of all going at the next
   read. Nothing changes when another host change came first. */
void js_rt_bridge_computed_style_host_described(DomBridge *bridge,
                                                uint64_t before);
/* Bytes the getComputedStyle caches hold (ancestor styles plus the var()
   table), for the profile report and tests. */
size_t js_rt_bridge_computed_style_cache_bytes(const DomBridge *bridge);
/* Whether a retained computed style (ring or memo) names `node`. */
bool js_rt_bridge_computed_style_cache_holds(const DomBridge *bridge,
                                             const lxb_dom_node_t *node);
/* The QuickJS pool's limit-growth hook: `growth` more bytes on top of
   `live` against `limit`; returns the raised limit, or 0 when refused.
   Exposed for tests. */
size_t js_rt_heap_growth_hook(void *opaque, size_t live, size_t growth,
                              size_t limit);
/* Collection amortization: the allocation a full collection of a `live`
   byte heap must follow to pay for itself, and whether at least that much
   has been allocated since the realm's last collection. A collection made
   only in case it helps ("speculative") waits until it is due; one that
   would otherwise end in a refusal runs regardless. */
size_t js_rt_gc_amortized_step(size_t live);
bool js_rt_gc_due(const ScriptRuntime *runtime);
/* A completed response the realm could not take for memory stays queued
   for a later turn; the refusal is judged like the realm's own
   (script_runtime_allocation_refusals), so a page whose memory stays
   exhausted has its realm kept or stopped by the memory stop rule instead
   of failing the turn. */
void js_rt_note_delivery_refusal(ScriptRuntime *runtime);
/* Gives up the session's cached responses (browser_session_cache_reclaim)
   for only the page Budget the realm still lacks to obtain `wanted` more
   heap, and nothing when Budget cannot raise what it obtains (the heap is
   at its limit or ceiling), when the whole cache could not cover the
   shortfall (browser_session_cache_reclaimable_bytes), or when the session
   draws on another Budget. Returns the bytes freed. */
size_t js_rt_reclaim_cached_responses_for_heap(ScriptRuntime *runtime,
                                               size_t wanted);
JSValue js_dom_make_fast_getter(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_make_fast_method(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
void js_rt_bridge_wrapper_refs_free(DomBridge *bridge);
void js_rt_bridge_hold_stateful_wrappers(DomBridge *bridge, bool hold);
void js_rt_bridge_sweep_node_wrappers(DomBridge *bridge);
/* Allocates the slot table with `initial` slots (rounded down to a multiple
   of 32, at least 32, at most `limit`); `limit` (0 for the default) is the
   most slots registration may later grow it to. Freed by
   js_rt_bridge_node_table_free after the realm's finalizers have run. */
bool js_rt_bridge_node_table_init(DomBridge *bridge, size_t limit,
                                  size_t initial);
void js_rt_bridge_node_table_free(DomBridge *bridge);
/* Whether a full table could take one more growth step now: below its
   ceiling, and past the Budget capacity with the step's memory free beside
   the presentation reserve. Allocates nothing and frees no cache. */
bool js_rt_bridge_node_table_can_grow(const DomBridge *bridge);
/* The realm's mutation journal, allocated on first use; NULL (with
   mutations_unrecorded set) when the Budget refused it. Freed with the
   handle table. */
ScriptMutationJournal *js_rt_bridge_journal(DomBridge *bridge);
JSValue js_dom_closest(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_matches(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_node_type(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_query(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv);
JSValue js_dom_query_all(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_register_shadow_root(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
JSValue js_dom_assign_slot(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_constructed_sheet_text(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv);
JSValue js_dom_sheet_revision(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_css_statement_ends(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_font_shorthand_valid(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_css_register_property(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_set_adopted_sheets(JSContext *context,
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
JSValue js_dom_compare_position(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_is_ancestor(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv);
JSValue js_dom_selectors_test_attribute(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv);
JSValue js_dom_next_ancestor_entry(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv);
#ifdef JS_RT_TEST_INSTRUMENTS
/* __tilefinchReleaseNodeWrapper: host and validation realms only. Tests
   use it to release one wrapper's handle lease by hand; the bootstrap has
   no caller. */
JSValue js_dom_release_node_wrapper(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
#endif
JSValue js_dom_remove(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv);
JSValue js_dom_remove_attribute(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_pre_insert_clear(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_make_wrapper(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_dom_keep_node_handle(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_hold_handle_release(JSContext *context, JSValueConst this_value,
                                   int argc, JSValueConst *argv);
JSValue js_dom_retain_node_wrapper(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv);
JSValue js_dom_node_wrapper(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv);
JSValue js_dom_note_event_state(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_grow_event_targets(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv);
JSValue js_dom_set_attribute(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_set_control_value(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_control_checked(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_form_direction(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_dom_parser_form_owner(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv);
JSValue js_dom_version(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv);
JSValue js_dom_has_parser_form_owners(JSContext *context,
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
JSValue js_dom_node_identity(JSContext *context,
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
/* Factory text outside the heap: the external-source hooks
   (JS_SetExternalSourceHooks()) route ids at or above
   SCRIPT_SOURCE_STORE_ID_LIMIT here; same contracts as the store's
   (js_source_store.h). */
int js_lazy_webpack_text_read(ScriptRuntime *runtime, uint32_t id,
                              uint32_t offset, uint32_t length, char *dest);
int js_lazy_webpack_text_query(ScriptRuntime *runtime, uint32_t id,
                               uint32_t offset, uint32_t length);
void js_lazy_webpack_text_release(ScriptRuntime *runtime, uint32_t id);
/* Drops plain factory text, least recently read first, until at most
   `allowance` bytes stay plain; returns the Budget bytes released. The
   compressed copy stays, so nothing is lost. */
size_t js_lazy_webpack_text_trim(ScriptRuntime *runtime, size_t allowance);
/* True once the realm's external-source hooks are installed
   (evaluation.inc). */
bool js_rt_external_source_hooks(ScriptRuntime *runtime);

#endif
