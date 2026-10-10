/* Shared internals for the js_runtime.c orchestrator and the translation
   units split out of it.  Not part of the public Tilefinch API: frontends and
   other subsystems must keep using tilefinch/js_runtime.h. */
#ifndef TILEFINCH_JS_RUNTIME_INTERNAL_H
#define TILEFINCH_JS_RUNTIME_INTERNAL_H

#include <stddef.h>
#include "diagnostic_trace.h"
#include "tilefinch_test_faults.h"

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
#define SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT 12u
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

/* The base table size every runtime allocates at creation; a policy may let
   it grow to DOM_BRIDGE_NODE_LIMIT_MAX (see DomBridge.node_capacity). */
#define DOM_BRIDGE_NODE_LIMIT SCRIPT_DOM_HANDLE_SLOT_CAPACITY
#define DOM_BRIDGE_NODE_LIMIT_MAX SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX
/* Live shadow roots per realm: the handle table's capacity. How many a page
   may attach scales with its page budget (js_dom_register_shadow_root):
   component sites nest them (MDN's Lit reference page attaches ~70), and
   every root and the component behind it costs memory, so the PSP app's
   32 MiB page ceiling keeps the historical 64 (more pushed MDN and Reddit
   past the 5 MiB script heap) while a larger ceiling admits up to the
   table. */
#define DOM_BRIDGE_SHADOW_ROOT_LIMIT 256u
#define DOM_BRIDGE_SHADOW_ROOT_BASE 64u
#define DOM_BRIDGE_SHADOW_ROOT_BUDGET_BYTES (512u * 1024u)
#define BRIDGE_NODE_NATIVE_PIN 0x01u
#define BRIDGE_NODE_LIVE_WRAPPER 0x02u
#define BRIDGE_NODE_PENDING_RETIRE_NOTIFY 0x04u
#define DOM_BRIDGE_NODE_INDEX_BITS SCRIPT_DOM_HANDLE_INDEX_BITS
#define DOM_BRIDGE_NODE_INDEX_MASK SCRIPT_DOM_HANDLE_INDEX_MASK
#define DOM_BRIDGE_NODE_GENERATION_MAX \
    ((uint32_t) (INT32_MAX >> DOM_BRIDGE_NODE_INDEX_BITS))
#define SCRIPT_SOURCE_NODE_LIMIT 256

_Static_assert(DOM_BRIDGE_NODE_LIMIT_MAX <= DOM_BRIDGE_NODE_INDEX_MASK,
               "DOM bridge node handles must encode every slot");
_Static_assert(DOM_BRIDGE_NODE_LIMIT_MAX < UINT16_MAX
                   && DOM_BRIDGE_NODE_LIMIT <= DOM_BRIDGE_NODE_LIMIT_MAX
                   && DOM_BRIDGE_NODE_LIMIT % 32u == 0
                   && DOM_BRIDGE_NODE_LIMIT_MAX % 32u == 0,
               "DOM bridge node index entries are 16-bit slot + 1");

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
    TilefinchRequestDestination destination;
    bool prefer_text_response;
    char *integrity;
    size_t integrity_length;
    char target_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    /* Its response bound in the realm scheduler's pool. */
    size_t response_bound;
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
    uintptr_t owner_document_identity;
    uint32_t identity;
    bool programmatic;
    bool html;
    bool force_async;
    bool already_started;
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
    ScriptResourceTiming resource_timing;
    /* Budget-owned minimized MIME type referenced by resource_timing. */
    char *resource_timing_content_type;
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

typedef struct {
    lxb_dom_node_t *node;
    uintptr_t owner_document_identity;
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
    ScriptMutationJournal mutations;
    BrowserSession *session;
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
    /* Native URL publication precedes advisory same-document callbacks.  A
       generation lets the Location facade detect and repair a callback/OOM
       split without allocating a URL string on ordinary property reads. */
    uint32_t document_url_revision;
    char referrer_policy[128];
    /* The slot table: parallel arrays of node_capacity entries carved from
       one Budget block (bridge_node_table_resize), allocated at runtime
       creation with DOM_BRIDGE_NODE_LIMIT slots and grown, never beyond
       node_capacity_limit, only when registration finds every slot live. */
    lxb_dom_node_t **nodes;
    /* Owner identities are captured at registration so whole-document
       retirement never has to inspect a node which author mutation may have
       destroyed already. */
    uintptr_t *node_owner_document_identities;
    uint32_t *node_generations;
    uint32_t *node_wrapper_leases;
    /* Bit 0 pins a handle for native work; bit 1 records that the newest JS
       wrapper lease is still live. Sharing the byte avoids enlarging the
       bounded PSP handle table to track detached-node correctness. */
    unsigned char *node_retention_flags;
    /* NULL slots whose generation can still advance, one bit per slot. */
    uint32_t *node_reusable_bits;
    size_t node_capacity;
    size_t node_capacity_limit;
    /* Per handle slot, the WeakRef the script wrapper cache holds for the
       live wrapper (a strong reference to the WeakRef only), so native
       getters can return an existing wrapper. Budget-allocated on first
       use; NULL when unavailable (the getters then ask script to wrap). */
    JSValue *wrapper_refs;
    /* Per handle slot, the adopted owner document's tag: 0 unknown, 1 the
       script's document, 2 the template contents owner, 3-255 a script-side
       document the bootstrap names. Keeping it with the native node lets
       it outlive wrappers. Budget-allocated on the first adoption away from
       the document (or template contents read); until then every node is
       the document's, template contents aside. */
    unsigned char *node_owner_tags;
    /* Set once a remote node writer was installed or a remote wrapper
       made: from then on native getters defer to script, which knows
       virtual (section-remote) nodes. */
    bool remote_mode_seen;
    size_t node_count;
    /* Every non-NULL slot is indexed by its node pointer, so registering a
       node and finding the handles inside a detached subtree do not scan the
       slot table. Only register/invalidate write `nodes`, and both keep this
       index and the reusable-slot bitmap (NULL slots whose generation can
       still advance) in step. */
    uint16_t *node_index;
    /* A power of two of at least twice node_capacity, so the load factor
       stays at or below one half. */
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
        size_t next;
        size_t hits;
        size_t misses;
        size_t lookup_probes;
        /* Index+1 chains over the existing 64-entry replacement ring.
           Zero terminates; this does not enlarge the retained style set. */
        uint8_t buckets[64];
        uint8_t links[64];
        struct ComputedStyleCacheEntry *entries;
        /* Connected mutations since the last read, as subtrees whose
           cached styles they can change (see
           bridge_computed_style_note_mutation); `dirty_all` when that is
           unknown or the list overflowed. */
        lxb_dom_node_t *dirty[16];
        size_t dirty_count;
        bool dirty_all;
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
       (anything but id, form, type or an internal state attribute). */
    uint64_t dom_structure_version;
    /* Native setAttribute calls: total, those writing the value already
       present, and time spent in the native setter. */
    size_t attribute_writes;
    size_t attribute_writes_unchanged;
    uint64_t attribute_write_ns;
    /* Exhaustion-time slot reclamation: a reentry guard, the exhaustions
       still to handle without a collection after a futile one (cleared at
       each entry into JavaScript), and the next such backoff. */
    bool node_reclaim_active;
    uint16_t node_reclaim_gc_skip;
    uint16_t node_reclaim_gc_backoff;
    /* Shadow carriers are native DOM nodes, but ordinary document/element
       selectors must not cross their tree boundary.  Store encoded handles,
       rather than pointers, so slot generation prevents allocator reuse from
       granting a stale carrier identity. */
    uint32_t shadow_root_handles[DOM_BRIDGE_SHADOW_ROOT_LIMIT];
    size_t shadow_root_count;
    ScriptRuntime *host;
    ScriptPostMessageCallback post_message;
    void *post_message_opaque;
    uint64_t performance_origin_ns;
    unsigned fetch_timeout_ms;
    bool deterministic_entropy;
    uint64_t entropy_state;
    uint64_t replay_seed;
    bool deterministic_clock;
    bool wall_clock_timers;
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
    int64_t fullscreen_node_handle;
    /* Page controls entry notice (navigator.tilefinch.requestPageControls
       {notice: "once"}): the page's pending request to skip a repeat, and
       whether this document has already shown the notice once. */
    bool page_controls_notice_once;
    bool page_controls_notice_shown;
    bool user_activation_active;
    bool user_activation_has_been_active;
    uint64_t user_activation_expires_ms;
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
    ImageResources *images;
    ScriptSynchronousLayoutCallback synchronous_layout;
    void *synchronous_layout_opaque;
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
    ScriptEventSource event_sources[SCRIPT_EVENT_SOURCE_LIMIT];
    size_t event_source_count;
    ScriptWebSocket websockets[SCRIPT_WEBSOCKET_LIMIT];
    size_t websocket_count;
    unsigned char *websocket_event_scratch;
    ScriptMultiplayer multiplayer;
    unsigned char *multiplayer_event_scratch;
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
        size_t largest_unit_bytes;
        /* Source whose bytecode was restored instead of compiled: from the
           session's RAM tables (about 50 ms per MiB instead of 4 s), and
           the part of it read from the Memory Stick tier (about 1 s per
           MiB more). */
        size_t restored_bytes;
        size_t disk_restored_bytes;
        size_t visible_text_bytes;
        bool visible_text_known;
    } heavy;
    bool allow_test_network_primitive_overrides;
    uint64_t dynamic_script_sequence;
    ScriptExecutionPolicy execution_policy;
    JSValue current_script;
    uintptr_t current_script_owner_document_identity;
    /* Descendant CSP grant of the script being evaluated (module roots and
       classic import() inherit it); zero outside a script element. */
    uint8_t current_script_csp_grant;
    JSValue trusted_node_wrap;
    JSValue trusted_stable_script_wrap;
    JSValue trusted_retire_native_node_state;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t webgl_native_total_us;
    size_t webgl_native_frames;
#endif
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
    SCRIPT_HOST_SET_FRAME_WINDOW_STATE,
    SCRIPT_HOST_INTERSECTION_RECHECK,
    SCRIPT_HOST_MEDIA_RECHECK,
    SCRIPT_HOST_RECORD_RESOURCE_TIMING,
    SCRIPT_HOST_RECORD_NAVIGATION_TIMING,
    SCRIPT_HOST_DELIVER_NETWORK,
    SCRIPT_HOST_DETACH_NETWORK,
    SCRIPT_HOST_PUMP_TIMERS,
    SCRIPT_HOST_SCHEDULER_SNAPSHOT,
    SCRIPT_HOST_XHR_DIAGNOSTICS,
    SCRIPT_HOST_REBIND_DOCUMENT,
    SCRIPT_HOST_COMMIT_SAME_DOCUMENT,
    SCRIPT_HOST_RESTORE_SAME_DOCUMENT,
    SCRIPT_HOST_SAVE_SECTION_STATE,
    SCRIPT_HOST_RESTORE_SECTION_STATE,
    SCRIPT_HOST_UPDATE_GAMEPAD,
    SCRIPT_HOST_CALLBACK_COUNT
} ScriptHostCallback;

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
#define SCRIPT_FRAME_REALM_LIMIT 16
#define SCRIPT_CHECKPOINT_CONTINUATION_LIMIT 8
/* Consecutive turns FinalizationRegistry cleanup may go without a task slot
   before one is admitted ahead of due tasks. */
#define SCRIPT_CLEANUP_STARVATION_ADVANCES 4u

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
    Watchdog watchdog;
    DomBridge bridge;
    ScriptResult result;
    PromiseRejectionState promise_rejection_state;
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
    /* The page lifecycle queues native visibility edges, then invokes this
       private compatibility closure at a normal author-task checkpoint. */
    JSValue page_visibility_host_apply;
    /* Captured before author code and removed from the global object. Used
       when native lifecycle observes that the fullscreen element detached. */
    JSValue fullscreen_host_exit;
    /* The private WeakMap from each Document to its document.all
       collection (js_runtime/legacy_surface.inc). */
    JSValue document_all_cache;
    /* Captured before author code can replace Window.navigator. The lazy
       OPFS module receives this identity through a temporary native-only
       handoff so StorageManager branding never consults a mutable global. */
    JSValue opfs_storage_manager;
    /* Installed by the lazy game-audio module, captured immediately, and
       removed before evaluation returns to author code. */
    JSValue game_audio_host_suspend;
    JSValue game_audio_host_complete;
    JSValue function_to_string;
    /* Trusted bootstrap callbacks retained before author code runs. The page
       may observe compatibility globals, but native scheduling and document
       lifecycle never look them up through the mutable Window object. */
    JSValue host_global;
    JSValue host_callbacks[SCRIPT_HOST_CALLBACK_COUNT];
    ScriptXHRDiagnostics *xhr_diagnostics;
    JSValue private_outcome_classifier;
    bool private_outcome_classifying;
    uint32_t xhr_diagnostic_sequence;
    size_t xhr_diagnostic_cursor;
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
    bool page_visibility_queue[2];
    uint8_t page_visibility_queue_head;
    uint8_t page_visibility_queue_count;
    /* Consecutive advances which ended with FinalizationRegistry cleanup
       queued but admitted none because due tasks took every slot. */
    uint8_t cleanup_starved_advances;
    TilefinchGameAudio *game_audio;
    bool game_audio_internal_slots;
    struct AudioSlotRuntime *game_audio_slots;
    struct ScriptLazyRuntimeBundle *lazy_webpack_bundles;
    uint32_t next_lazy_webpack_bundle_id;
    bool lazy_factory_recovery_pending;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    ScriptRuntimeTimingMetrics timing_metrics;
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
    uintptr_t owner_document_identity;
    uint8_t csp_grant;
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
/* HTML deliberately leaves the transient-activation duration to the user
   agent. Five seconds matches the interoperability window used by major
   engines while keeping privileged actions tightly bounded on the PSP. */
#define SCRIPT_TRANSIENT_USER_ACTIVATION_MS UINT64_C(5000)
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
static inline void js_rt_bridge_consume_user_activation(DomBridge *bridge)
{
    if (bridge == NULL) return;
    bridge->user_activation_active = false;
    bridge->user_activation_expires_ms = 0;
}
void js_rt_saturating_add_size(size_t *value, size_t amount);
int js_rt_private_classify(ScriptRuntime *runtime, JSValueConst value,
                          JSValueConst parse, int event);
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
const char *js_rt_bridge_calculated_base_url(DomBridge *bridge);

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
void js_rt_websockets_destroy(DomBridge *bridge);
size_t js_rt_websockets_abort(DomBridge *bridge);
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
bool js_rt_bridge_flush_synchronous_layout(DomBridge *bridge);
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
JSValue js_wasm_snapshot_source(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_set_fullscreen(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_dom_page_controls_notice(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv);
TilefinchGameAudio *js_rt_game_audio_engine(ScriptRuntime *runtime);
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
JSValue js_computed_style_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv);
JSValue js_computed_style_read(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_dom_rendered_text(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv);
JSValue js_transition_snapshot(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv);
JSValue js_computed_style_support(JSContext *context,
                                  JSValueConst this_value,
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
JSValue js_dom_root_node(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv);
JSValue js_dom_node_owner(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv);
void js_rt_bridge_computed_style_cache_free(DomBridge *bridge);
void js_rt_bridge_computed_style_cache_forget(DomBridge *bridge);
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
JSValue js_dom_make_fast_getter(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_make_fast_method(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv);
JSValue js_dom_note_remote_wrapper(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv);
void js_rt_bridge_wrapper_refs_free(DomBridge *bridge);
/* Allocates the base slot table; `limit` (0 for the default) is the most
   slots registration may later grow it to. Freed by
   js_rt_bridge_node_table_free after the realm's finalizers have run. */
bool js_rt_bridge_node_table_init(DomBridge *bridge, size_t limit);
void js_rt_bridge_node_table_free(DomBridge *bridge);
JSValue js_dom_closest(JSContext *context, JSValueConst this_value,
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
JSValue js_dom_register_shadow_root(JSContext *context,
                                    JSValueConst this_value,
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
JSValue js_dom_nearest_id(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv);
JSValue js_dom_next_ancestor_entry(JSContext *context,
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
JSValue js_dom_version(JSContext *context, JSValueConst this_value,
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
JSValue js_dom_node_identity(JSContext *context,
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
