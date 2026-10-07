void script_runtime_detach_document(ScriptRuntime *runtime,
                                    const PocDocument *document)
{
    uintptr_t owner = document_owner_identity(document);
    if (runtime == NULL || owner == 0) return;
    DomBridge *bridge = &runtime->bridge;

    if (runtime->document == document) {
        /* The realm may outlive this tree, but page-authorized requests may
           not. Reject JS fetch promises while the CSP/budget authority is
           still live, then cancel every native request before clearing the
           document pointers used by completion delivery. */
        (void) runtime_call_host(
            runtime, SCRIPT_HOST_DETACH_NETWORK, 0, NULL, NULL);
        (void) script_runtime_cancel_network(runtime, "document detached");
        js_rt_event_sources_destroy(bridge);
        js_rt_websockets_destroy(bridge);
        js_rt_multiplayer_destroy(bridge);
    }

    bool preserve_foreign_source_scope = false;
    if (bridge->current_script_source_active) {
        for (size_t i = 0; i < bridge->script_source_node_count; i++) {
            const ScriptSourceNode *source = &bridge->script_source_nodes[i];
            if (source->section == bridge->current_script_source_section
                && strcmp(source->key,
                          bridge->current_script_source_key) == 0
                && source->owner_document_identity != owner) {
                preserve_foreign_source_scope = true;
                break;
            }
        }
    }

    /* Clear work before invalidating its handle so request cancellation and
       pin accounting still see the slot. Owner identities were captured while
       nodes were live; no retirement decision dereferences a DOM pointer. */
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        bool owned = task->active
            && task->owner_document_identity == owner;
        if (!owned && task->active && task->owner_document_identity == 0) {
            size_t slot = 0;
            owned = js_rt_bridge_node_slot_for_handle(
                        bridge, task->node_handle, &slot)
                && bridge->node_owner_document_identities[slot] == owner;
        }
        if (owned) {
            js_rt_dynamic_task_clear(bridge, task, true, true);
        }
    }
    for (size_t i = 0; i < bridge->node_count; i++) {
        if (bridge->node_owner_document_identities[i] == owner) {
            bridge_invalidate_node_slot(bridge, i);
        }
    }
    bridge_scroll_intents_detach_document(bridge, owner);
    /* Retained computed styles hold raw node addresses of whichever
       document was bound; a detached tree can be destroyed without a
       per-node removal. */
    js_rt_bridge_computed_style_cache_forget(bridge);
    script_element_states_detach_document(bridge, document);

    size_t source_output = 0;
    for (size_t i = 0; i < bridge->script_source_node_count; i++) {
        if (bridge->script_source_nodes[i].owner_document_identity == owner) {
            continue;
        }
        if (source_output != i) {
            bridge->script_source_nodes[source_output] =
                bridge->script_source_nodes[i];
        }
        source_output++;
    }
    if (source_output != bridge->script_source_node_count) {
        memset(bridge->script_source_nodes + source_output, 0,
               (bridge->script_source_node_count - source_output)
                   * sizeof(bridge->script_source_nodes[0]));
    }
    bridge->script_source_node_count = source_output;

    if (bridge->current_script_owner_document_identity == owner) {
        (void) document_current_script_set(
            runtime->context, JS_NULL, &runtime->result);
    }
    if (bridge->current_script_source_active
        && !preserve_foreign_source_scope) {
        bridge->current_script_source_active = false;
        bridge->current_script_source_key[0] = '\0';
    }

    if (runtime->document == document) {
        memset(&bridge->mutations, 0, sizeof(bridge->mutations));
        bridge->document = NULL;
        bridge->layout = NULL;
        bridge->images = NULL;
        bridge->stylesheet = NULL;
        runtime->document = NULL;
        runtime->relayout_dirty = false;
    } else {
        size_t mutation_output = 0;
        for (size_t i = 0; i < bridge->mutations.count; i++) {
            if (bridge->mutations.records[i].owner_document_identity
                == owner) {
                continue;
            }
            if (mutation_output != i) {
                bridge->mutations.records[mutation_output] =
                    bridge->mutations.records[i];
            }
            mutation_output++;
        }
        if (mutation_output == 0) {
            memset(&bridge->mutations, 0, sizeof(bridge->mutations));
        } else {
            memset(bridge->mutations.records + mutation_output, 0,
                   (bridge->mutations.count - mutation_output)
                       * sizeof(bridge->mutations.records[0]));
            bridge->mutations.count = mutation_output;
        }
    }
}

bool script_runtime_rebind_document(ScriptRuntime *runtime,
                                    PocDocument *document,
                                    ScriptResult *result)
{
    if (runtime == NULL || document == NULL || document->html == NULL)
        return false;
    /* Direct embedders may rebind while the old tree is still live.  Retire
       only that document's state.  A caller which used the required explicit
       pre-destruction detach leaves runtime->document NULL; in that case the
       remaining handles intentionally belong to other live documents (for
       example, the streamed script corpus driving a section replacement). */
    if (runtime->document != NULL && runtime->document != document) {
        script_runtime_detach_document(runtime, runtime->document);
    }
    runtime->document = document;
    runtime->bridge.document = document;
    runtime->bridge.document_base_dirty = true;
    runtime->relayout_dirty = false;
    bool rebound = runtime_call_host(
        runtime, SCRIPT_HOST_REBIND_DOCUMENT, 0, NULL, NULL);
    js_rt_runtime_update_result(runtime, result);
    return rebound;
}

void script_runtime_relocate_document_storage(ScriptRuntime *runtime,
                                              PocDocument *document)
{
    if (runtime == NULL || document == NULL) return;
    runtime->document = document;
    runtime->bridge.document = document;
    runtime->bridge.document_base_dirty = true;
}

static size_t script_runtime_current_limit(const ScriptRuntime *runtime);

size_t script_runtime_collect_and_trim_phase(
    ScriptRuntime *runtime, ScriptRuntimeCollectPhase phase)
{
    if (runtime == NULL || runtime->runtime == NULL
        || runtime->quickjs_pool == NULL
        || phase >= SCRIPT_RUNTIME_COLLECT_PHASE_COUNT) return 0;
    if (phase == SCRIPT_RUNTIME_COLLECT_FIRST_GC
        || phase == SCRIPT_RUNTIME_COLLECT_SECOND_GC) {
        JS_RunGC(runtime->runtime);
        return 0;
    }
    /* The trim phase follows both graph passes. Re-arm automatic collection
       from the post-collection live heap before returning cached pages to the
       shared browser Budget. The allocator maintains malloc_size and every
       limit change mirrors script_runtime_current_limit(), so this needs no
       heap census. */
    uint64_t current =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    uint64_t growth = current >> 1;
    if (growth < 256u * 1024u) growth = 256u * 1024u;
    uint64_t threshold = current > UINT64_MAX - growth
        ? UINT64_MAX : current + growth;
    uint64_t limit = script_runtime_current_limit(runtime);
    if (limit > 0 && threshold >= limit) {
        uint64_t reserve = limit / 8u;
        if (reserve > 256u * 1024u) reserve = 256u * 1024u;
        threshold = limit > reserve ? limit - reserve : current;
        if (threshold < current) threshold = current;
    }
    JS_SetGCThreshold(
        runtime->runtime,
        threshold > SIZE_MAX ? SIZE_MAX : (size_t) threshold);
    return budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
}

size_t script_runtime_collect_and_trim(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || runtime->quickjs_pool == NULL) return 0;
    /* A DOM wrapper can participate in a self-cycle (classList and author
       listeners are common examples).  Bellard QuickJS removes that cycle
       in one pass and clears its WeakRef/FinalizationRegistry target in the
       next.  This explicit full-collection boundary therefore needs both
       bounded passes before allocator trimming. */
    (void) script_runtime_collect_and_trim_phase(
        runtime, SCRIPT_RUNTIME_COLLECT_FIRST_GC);
    (void) script_runtime_collect_and_trim_phase(
        runtime, SCRIPT_RUNTIME_COLLECT_SECOND_GC);
    /* QuickJS raises its automatic-GC threshold from the allocation peak
       which triggered a collection. An embedding-requested full collection
       can reduce the live heap dramatically while leaving that old threshold
       above the hard memory limit, so a later synchronous allocation burst
       reaches OOM without another automatic collection. Re-arm the ordinary
       1.5x heuristic from post-collection live usage, retaining at least the
       dependency's default 256 KiB growth window and a bounded pre-limit
       reserve. */
    size_t reclaimed = script_runtime_collect_and_trim_phase(
        runtime, SCRIPT_RUNTIME_COLLECT_TRIM);
    /* The collection above may have finalized the last instance. */
    js_wasm_release_idle_runtime();
    return reclaimed;
}

size_t js_rt_prepare_network_response_delivery(ScriptRuntime *runtime,
                                               size_t response_bytes)
{
    /* Below this size the collection cost is more likely to dominate the
       callback than the response's temporary parser/compiler expansion. */
    if (response_bytes < 512u * 1024u) return 0;
    size_t reclaimed = script_runtime_collect_and_trim(runtime);
    /* A large author response is already leased by the completion which is
       about to receive it.  Older HTTP and provider entries are optional at
       this boundary and otherwise compete with the transient JS string,
       decoder, and compiler expansion of this response.  Eviction is safe:
       shared bodies which still have a live lease remain alive, and the
       session helper reports only bytes physically returned to the common
       page Budget. */
    if (runtime != NULL && runtime->session != NULL
        && runtime->session->budget == runtime->budget) {
        size_t cache_reclaimed = browser_session_cache_reclaim(
            runtime->session, response_bytes);
        reclaimed = cache_reclaimed > SIZE_MAX - reclaimed
            ? SIZE_MAX : reclaimed + cache_reclaimed;
    }
    return reclaimed;
}

void js_rt_heap_census(ScriptRuntime *runtime, JSMemoryUsage *usage)
{
    runtime->heap_censuses++;
    JS_ComputeMemoryUsage(runtime->runtime, usage);
}

size_t script_runtime_heap_used(const ScriptRuntime *runtime)
{
    return runtime == NULL || runtime->quickjs_pool == NULL ? 0
        : budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
}

size_t script_runtime_heap_remaining(const ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || runtime->quickjs_pool == NULL) return 0;
    size_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > SIZE_MAX - limit
            ? SIZE_MAX : limit + runtime->boot_window_bytes;
    }
    /* The budget allocator maintains the exact QuickJS malloc_size at every
       allocation/free, so admission can be O(1). JS_ComputeMemoryUsage walks
       the entire live heap and is reserved for explicit diagnostics. */
    size_t current =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    return current < limit ? limit - current : 0;
}

#define SCRIPT_HEAP_GROWTH_STEP (512u * 1024u)

static size_t script_runtime_current_limit(const ScriptRuntime *runtime)
{
    size_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > SIZE_MAX - limit
            ? SIZE_MAX : limit + runtime->boot_window_bytes;
    }
    return limit;
}

/* Called by the allocator (no JavaScript, and no allocation except the
   memory-rescue copy on refusal) when a request would pass the limit.
   Grows in whole steps so the next allocations do not each ask again, and
   only while the page Budget keeps its reserve. */
size_t js_rt_heap_growth_hook(void *opaque, size_t live, size_t growth,
                              size_t limit)
{
    ScriptRuntime *runtime = opaque;
    if (runtime == NULL || !runtime->heap_growth_enabled) return 0;
    /* Only the part of the request the current headroom cannot hold. */
    size_t needed;
    uint8_t reason = 3;
    if (live >= limit) {
        needed = live - limit;
        if (growth > SIZE_MAX - needed) goto refuse;
        needed += growth;
    } else {
        needed = growth > limit - live ? growth - (limit - live) : 0;
    }
    if (needed == 0) return limit;
    /* A request the ceiling cannot hold must not cost the optional caches
       anything. */
    reason = 2;
    if (limit >= runtime->heap_growth_ceiling
        || needed > runtime->heap_growth_ceiling - limit) goto refuse;
    size_t step = needed <= SCRIPT_HEAP_GROWTH_STEP
        ? SCRIPT_HEAP_GROWTH_STEP
        : needed + (SCRIPT_HEAP_GROWTH_STEP - 1u)
              - (needed + (SCRIPT_HEAP_GROWTH_STEP - 1u))
                  % SCRIPT_HEAP_GROWTH_STEP;
    if (step > runtime->heap_growth_ceiling - limit)
        step = runtime->heap_growth_ceiling - limit;
    reason = 1;
    /* Optional caches (module bytecode, responses) give way to the live
       page before growth is refused. */
    if (runtime->heap_growth_reserve <= SIZE_MAX - step)
        (void) budget_make_room(runtime->budget,
                                runtime->heap_growth_reserve + step);
    size_t free_bytes = budget_remaining(runtime->budget);
    /* Headroom already granted under the current limit can still be drawn
       from the Budget after the raise, so it is not spare: counting it let
       the realm end up to one unused step below the page reserve. */
    size_t granted = live < limit ? limit - live : 0;
    if (free_bytes <= runtime->heap_growth_reserve
        || free_bytes - runtime->heap_growth_reserve <= granted) goto refuse;
    size_t spare = free_bytes - runtime->heap_growth_reserve - granted;
    reason = 3;
    if (step > spare) step = spare;
    if (step < needed || step > SIZE_MAX - runtime->base_memory_limit)
        goto refuse;
    runtime->base_memory_limit += step;
    runtime->heap_growth_bytes += step;
    runtime->heap_growth_raises++;
    size_t raised = script_runtime_current_limit(runtime);
    if (raised > runtime->heap_growth_peak_limit)
        runtime->heap_growth_peak_limit = raised;
    return raised;
refuse:
    runtime->heap_growth_refusals++;
    runtime->heap_growth_refused_bytes = needed;
    runtime->heap_growth_refused_reason = reason;
    /* The one exception to "no allocation" here: a bounded copy of the body
       from the page Budget (never the QuickJS heap), at most a few times
       per realm. */
    js_rt_memory_rescue_capture(runtime);
    return 0;
}

/* Collection pacing wants garbage headroom above the live graph, but the
   limit only grows when an allocation fails it, and a page that mostly
   allocates garbage never fails it: every collection then reclaims just
   below a limit that sits a sliver above live, and the realm collects its
   whole heap every few hundred KiB. Raise the limit toward `want` at a safe
   point while the page Budget has spare memory above its reserve; under
   pressure this does nothing and pacing stays tight. Never evicts caches. */
static void script_runtime_heap_pregrow(ScriptRuntime *runtime, size_t want)
{
    if (runtime == NULL || !runtime->heap_growth_enabled
        || runtime->budget == NULL) return;
    size_t limit = script_runtime_current_limit(runtime);
    if (want > runtime->heap_growth_ceiling)
        want = runtime->heap_growth_ceiling;
    if (want <= limit) return;
    size_t free_bytes = budget_remaining(runtime->budget);
    size_t granted = script_runtime_heap_remaining(runtime);
    if (free_bytes <= runtime->heap_growth_reserve
        || free_bytes - runtime->heap_growth_reserve <= granted) return;
    size_t step = want - limit;
    size_t spare = free_bytes - runtime->heap_growth_reserve - granted;
    if (step > spare) step = spare - spare % SCRIPT_HEAP_GROWTH_STEP;
    if (step < SCRIPT_HEAP_GROWTH_STEP) return;
    runtime->base_memory_limit += step;
    runtime->heap_growth_bytes += step;
    runtime->heap_growth_pregrows++;
    size_t raised = script_runtime_current_limit(runtime);
    if (raised > runtime->heap_growth_peak_limit)
        runtime->heap_growth_peak_limit = raised;
    JS_SetMemoryLimit(runtime->runtime, raised);
}

/* The automatic-collection growth percentage knob
   (TILEFINCH_JS_GC_GROWTH_PCT, which the PSP app exports from boot.cfg),
   read once for the process: -1 when unset, otherwise at least 0. Every
   reader (the pacing hook, the threshold re-arm, growth returns and
   deterministic replay) goes through here. */
static long js_rt_gc_growth_knob(void)
{
    static bool resolved;
    static long knob;
    if (!resolved) {
        const char *value = getenv("TILEFINCH_JS_GC_GROWTH_PCT");
        knob = value == NULL ? -1 : atol(value);
        if (value != NULL && knob < 0) knob = 0;
        resolved = true;
    }
    return knob;
}

/* QuickJS's unconstrained threshold growth can reach a realm's hard
   ceiling before collecting dead cyclic browser state. A 125% live-heap
   threshold remains logarithmic while there is room, then the reserve
   clamp (runtime_apply_gc_slack) leaves ordinary event construction
   headroom. 0 leaves QuickJS's own pacing. */
#define SCRIPT_GC_DEFAULT_GROWTH_PCT 125
static long js_rt_gc_growth_pct(void)
{
    long knob = js_rt_gc_growth_knob();
    return knob < 0 ? SCRIPT_GC_DEFAULT_GROWTH_PCT : knob;
}

/* Lab: a fixed GC-threshold slack in KiB (TILEFINCH_JS_GC_SLACK_KB),
   0 when unset. */
static long js_rt_gc_slack_kb(void)
{
    static bool resolved;
    static long slack_kb;
    if (!resolved) {
        const char *value = tilefinch_lab_getenv("TILEFINCH_JS_GC_SLACK_KB");
        slack_kb = value == NULL ? 0 : atol(value);
        if (slack_kb < 0) slack_kb = 0;
        resolved = true;
    }
    return slack_kb;
}

/* A full collection marks the whole live graph, so it pays for itself only
   after enough allocation: at least a sixteenth of the live heap (and 256
   KiB). Less than that between collections is collection thrash. */
#define SCRIPT_GC_AMORTIZED_MINIMUM (256u * 1024u)
/* Starved automatic collections (less than an amortized step of headroom
   left below the limit) that also freed little, per advance and stage, in
   a realm that cannot grow: first at QuickJS's own spacing, then only at
   exhaustion, then not at all until the next advance, when the refusal
   reports exhaustion. A starved collection that frees a useful amount
   (garbage churn near the limit) is progress and resets the count. */
#define SCRIPT_GC_STARVED_STAGE 8u
#define SCRIPT_GC_INEFFECTIVE_MINIMUM (64u * 1024u)

/* Lab A/B switch: TILEFINCH_JS_GC_PACING=0 restores the collection
   behavior before amortized pacing (QuickJS's threshold re-arm and the
   pressure collections before every compile). */
static bool js_rt_gc_pacing_disabled(void)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *value = tilefinch_lab_getenv("TILEFINCH_JS_GC_PACING");
        disabled = value != NULL && strcmp(value, "0") == 0;
    }
    return disabled != 0;
}

size_t js_rt_gc_amortized_step(size_t live)
{
    size_t step = live / 16u;
    return step < SCRIPT_GC_AMORTIZED_MINIMUM ? SCRIPT_GC_AMORTIZED_MINIMUM
                                              : step;
}

bool js_rt_gc_due(const ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->heap_collections == 0
        || js_rt_gc_pacing_disabled()) return true;
    size_t now = script_runtime_heap_used(runtime);
    size_t after = runtime->heap_live_after_gc;
    return now > after && now - after >= js_rt_gc_amortized_step(after);
}

/* After every collection the allocation threshold runs (JS_SetGCPacingHook).
   QuickJS re-arms at half the remaining headroom, so as live data
   approaches the limit (a growable realm's limit rises only 512 KiB at a
   time, when an allocation fails it) the heap is collected every few
   hundred KiB, each time a full mark of 10-16 MB that frees nothing
   (xe.com on the PSP: 165 collections, 25.6 s of one 27.7 s task).

   QuickJS's spacing stands while it leaves an amortized step (a sixteenth
   of the live heap). Below that the heap is starved. Where collections
   still free garbage (churn), keep QuickJS's spacing: collecting early is
   what keeps that garbage under the limit. Where they free almost nothing
   (live data growing toward the limit), collecting early is pure cost: a
   realm that can still grow collects once per growth step instead of a
   dozen times; a realm that cannot stops collecting for the rest of the
   advance after a bounded number, so a heap that truly cannot fit reaches
   the allocation refusal (and memory rescue) promptly instead of
   thrashing. The limit itself is never raised here: growth stays on
   demand, under the page reserve. */
static size_t js_rt_gc_pacing(void *opaque, size_t live, size_t limit,
                              size_t proposed)
{
    ScriptRuntime *runtime = opaque;
    if (runtime == NULL || js_rt_gc_growth_pct() == 0
        || js_rt_gc_pacing_disabled())
        return proposed;
    /* QuickJS's own re-arm (half the live heap, or half the remaining
       headroom near the limit) stands while it leaves an amortized step,
       and always far from the limit (a small heap early in startup). */
    if ((proposed > live && proposed - live >= js_rt_gc_amortized_step(live))
        || (live < limit && live <= limit - live)) {
        runtime->gc_task_starved = 0;
        return proposed;
    }
    runtime->gc_pacing_starved++;
    /* Growable: the limit can still take a whole growth step, and has not
       refused one this advance. */
    size_t available = script_runtime_heap_available(runtime);
    size_t remaining = script_runtime_heap_remaining(runtime);
    bool growable = runtime->heap_growth_enabled
        && available > remaining
        && available - remaining >= SCRIPT_HEAP_GROWTH_STEP
        && runtime->heap_growth_refusals == runtime->gc_advance_refusals;
    size_t freed = runtime->gc_live_before > live
        ? runtime->gc_live_before - live : 0;
    size_t ineffective = live / 64u < SCRIPT_GC_INEFFECTIVE_MINIMUM
        ? SCRIPT_GC_INEFFECTIVE_MINIMUM : live / 64u;
    /* Churn: collecting early is what keeps the garbage out of the
       limit. Keep QuickJS's spacing. */
    if (freed >= ineffective) {
        runtime->gc_task_starved = 0;
        return proposed;
    }
    runtime->gc_task_starved++;
    /* A realm that can still grow collects next one growth step past its
       current limit: the step is granted on demand (the check above), and
       a collection then comes before the step after it, so garbage never
       buys more than one step of limit and a refused step is not preceded
       by a pointless collection. (A threshold at the limit itself fired at
       every object allocation once live data sat a few bytes below it.) */
    if (growable)
        return limit > SIZE_MAX - SCRIPT_HEAP_GROWTH_STEP
            ? SIZE_MAX : limit + SCRIPT_HEAP_GROWTH_STEP;
    /* JS_SetGCThreshold semantics: a threshold at the limit collects only
       when an object allocation would otherwise not fit. */
    if (runtime->gc_task_starved <= SCRIPT_GC_STARVED_STAGE) return proposed;
    if (runtime->gc_task_starved == SCRIPT_GC_STARVED_STAGE + 1u)
        runtime->gc_pacing_backoffs++;
    if (runtime->gc_task_starved <= 2u * SCRIPT_GC_STARVED_STAGE)
        return limit;
    return SIZE_MAX;
}

void script_runtime_enable_heap_growth(ScriptRuntime *runtime,
                                       size_t ceiling, size_t page_reserve)
{
    if (runtime == NULL || runtime->quickjs_pool == NULL) return;
    runtime->heap_growth_enabled = ceiling > runtime->base_memory_limit;
    runtime->heap_growth_floor = runtime->base_memory_limit;
    runtime->heap_growth_ceiling = ceiling;
    runtime->heap_growth_reserve = page_reserve;
    runtime->heap_growth_peak_limit = script_runtime_current_limit(runtime);
    budget_quickjs_pool_set_limit_growth(
        runtime->quickjs_pool,
        runtime->heap_growth_enabled ? js_rt_heap_growth_hook : NULL,
        runtime);
}

size_t script_runtime_heap_available(const ScriptRuntime *runtime)
{
    size_t remaining = script_runtime_heap_remaining(runtime);
    if (runtime == NULL || !runtime->heap_growth_enabled
        || runtime->budget == NULL) return remaining;
    size_t limit = script_runtime_current_limit(runtime);
    if (limit >= runtime->heap_growth_ceiling) return remaining;
    size_t free_bytes = budget_remaining(runtime->budget);
    if (free_bytes <= runtime->heap_growth_reserve) return remaining;
    size_t growable = free_bytes - runtime->heap_growth_reserve;
    if (growable > runtime->heap_growth_ceiling - limit)
        growable = runtime->heap_growth_ceiling - limit;
    return remaining > SIZE_MAX - growable ? SIZE_MAX : remaining + growable;
}

void script_runtime_heap_growth(const ScriptRuntime *runtime,
                                ScriptHeapGrowth *growth)
{
    if (growth == NULL) return;
    memset(growth, 0, sizeof(*growth));
    if (runtime == NULL) return;
    growth->floor = runtime->heap_growth_enabled
        ? runtime->heap_growth_floor : runtime->base_memory_limit;
    growth->limit = script_runtime_current_limit(runtime);
    growth->peak_limit = runtime->heap_growth_peak_limit;
    growth->grown = runtime->heap_growth_bytes;
    growth->raises = runtime->heap_growth_raises;
    growth->pregrows = runtime->heap_growth_pregrows;
    growth->refusals = runtime->heap_growth_refusals;
    growth->returns = runtime->heap_growth_returns;
    growth->refused_bytes = runtime->heap_growth_refused_bytes;
    growth->refused_reason = runtime->heap_growth_refused_reason;
    growth->reentrant_checkpoints = runtime->reentrant_checkpoints_skipped;
    growth->collections = runtime->heap_collections;
    growth->threshold_collections = runtime->gc_threshold_collections;
    growth->max_advance_collections = runtime->gc_advance_collections_max;
    size_t open_advance = runtime->heap_collections
        - runtime->gc_advance_collections_begin;
    if (open_advance > growth->max_advance_collections)
        growth->max_advance_collections = open_advance;
    growth->max_checkpoint_collections =
        runtime->gc_checkpoint_collections_max;
    growth->pacing_starved = runtime->gc_pacing_starved;
    growth->pacing_backoffs = runtime->gc_pacing_backoffs;
    growth->collection_us = runtime->gc_total_ns / UINT64_C(1000);
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    if (runtime->runtime != NULL) {
        JSLazyFunctionStats stats;
        JS_GetLazyFunctionStats(runtime->runtime, &stats);
        growth->lazy_compile_failures = (size_t) stats.compile_failures;
    }
#endif
    growth->source_committed = runtime->bridge.script_quota_bytes;
    growth->source_limit = runtime->bridge.maximum_script_bytes;
    growth->source_raises = runtime->bridge.script_quota_pressure_raises;
}

bool script_runtime_grow_heap_limit(ScriptRuntime *runtime,
                                    size_t additional, size_t ceiling)
{
    if (runtime == NULL || runtime->runtime == NULL || additional == 0
        || runtime->base_memory_limit >= ceiling) return false;
    if (additional > ceiling - runtime->base_memory_limit)
        additional = ceiling - runtime->base_memory_limit;
    runtime->base_memory_limit += additional;
    size_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > SIZE_MAX - limit
            ? SIZE_MAX : limit + runtime->boot_window_bytes;
    }
    JS_SetMemoryLimit(runtime->runtime, limit);
    return true;
}

size_t script_runtime_boot_window_checks(const ScriptRuntime *runtime)
{
    return runtime == NULL ? 0 : runtime->boot_window_checks;
}

void script_runtime_lazy_function_counts(const ScriptRuntime *runtime,
                                         size_t *deferred, size_t *compiled)
{
    size_t deferred_count = 0, compiled_count = 0;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    if (runtime != NULL && runtime->runtime != NULL) {
        JSLazyFunctionStats stats;
        JS_GetLazyFunctionStats(runtime->runtime, &stats);
        deferred_count = (size_t) (stats.deferred + stats.restored);
        compiled_count = (size_t) stats.compiled;
    }
#else
    (void) runtime;
#endif
    if (deferred != NULL) *deferred = deferred_count;
    if (compiled != NULL) *compiled = compiled_count;
}

size_t script_runtime_heap_rejections(const ScriptRuntime *runtime)
{
    if (runtime == NULL) return 0;
    /* Rejections inside a compile that was abandoned as a memory refusal
       left the realm intact; they are not exhaustion. */
    size_t count = budget_quickjs_pool_rejection_count(runtime->quickjs_pool);
    return count > runtime->compile_heap_rejections_absorbed
        ? count - runtime->compile_heap_rejections_absorbed : 0;
}

bool script_runtime_last_slice_interrupted(const ScriptRuntime *runtime)
{
    return runtime != NULL && runtime->result.interrupted;
}

size_t script_runtime_inline_execution_reserve(
    const ScriptRuntime *runtime, size_t source_length)
{
    if (runtime == NULL || runtime->runtime == NULL) return 0;
    const uint64_t minimum = SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES;
    const uint64_t maximum = 2u * 1024u * 1024u;
    const uint64_t qualified_minimum = 4u * 1024u * 1024u;
    uint64_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > UINT64_MAX - limit
            ? UINT64_MAX : limit + runtime->boot_window_bytes;
    }
    if (limit < qualified_minimum) return 0;
    /* A fixed half-heap reserve made a small late inline script disappear
       whenever platform/test-library state happened to occupy just over half
       the heap. Scale the prospective expansion with source size while
       retaining a 512 KiB floor for DOM work and a 2 MiB ceiling for large
       bootstraps. This remains conservative on PSP without making execution
       depend on incidental pre-script heap occupancy. */
    uint64_t expansion = source_length > UINT64_MAX / 4u
        ? UINT64_MAX : (uint64_t) source_length * 4u;
    uint64_t reserve = minimum;
    if (expansion > reserve) reserve = expansion;
    if (reserve > maximum) reserve = maximum;
    if (reserve > limit / 2u) reserve = limit / 2u;
    return reserve > SIZE_MAX ? SIZE_MAX : (size_t) reserve;
}

void script_runtime_bound_allocator_cache(ScriptRuntime *runtime,
                                          size_t small_class_limit,
                                          size_t large_class_limit)
{
    if (runtime == NULL) return;
    budget_quickjs_pool_set_cache_limits(
        runtime->quickjs_pool, small_class_limit, large_class_limit);
}

bool script_runtime_commit_same_document(ScriptRuntime *runtime,
                                         const char *url,
                                         const char *old_url,
                                         ScriptResult *result)
{
    if (runtime == NULL || url == NULL || old_url == NULL) return false;
    if (!bridge_set_document_url(&runtime->bridge, url)) return false;
    JSValue arguments[2] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, old_url)
    };
    bool committed = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_COMMIT_SAME_DOCUMENT, 2,
                        arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    /* Native history is authoritative and has already committed. A throwing
       hashchange listener may fail the advisory callback, but rolling only
       the bridge URL back would split location/baseURI from native history. */
    if (!committed && runtime->result.error[0] == '\0') {
        js_rt_record_exception(runtime->context, &runtime->result);
    }
    js_rt_runtime_update_result(runtime, result);
    return committed;
}

bool script_runtime_restore_same_document(ScriptRuntime *runtime,
                                          const char *url,
                                          const char *old_url,
                                          ScriptResult *result)
{
    if (runtime == NULL || url == NULL || old_url == NULL) return false;
    if (!bridge_set_document_url(&runtime->bridge, url)) return false;
    JSValue arguments[2] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, old_url)
    };
    bool restored = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_RESTORE_SAME_DOCUMENT, 2,
                        arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    /* popstate/hashchange delivery is likewise advisory after native history
       restoration. Keep the bridge at the restored URL even when a handler
       throws so all URL and base views remain coherent. */
    if (!restored && runtime->result.error[0] == '\0')
        js_rt_record_exception(runtime->context, &runtime->result);
    js_rt_runtime_update_result(runtime, result);
    return restored;
}

static bool script_runtime_section_state_call(
    ScriptRuntime *runtime, ScriptHostCallback callback, size_t section,
    bool completes_mutations, size_t *count, ScriptResult *result)
{
    if (runtime == NULL || section > INT64_MAX)
        return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue argument = JS_NewInt64(runtime->context, (int64_t) section);
    JSValue returned = JS_UNDEFINED;
    bool ok = !JS_IsException(argument)
        && runtime_call_host(runtime, callback, 1, &argument, &returned);
    int64_t converted = 0;
    if (ok && JS_ToInt64(runtime->context, &converted, returned) < 0) {
        js_rt_record_exception(runtime->context, &runtime->result);
        ok = false;
    }
    JS_FreeValue(runtime->context, returned);
    JS_FreeValue(runtime->context, argument);
    /* Restore applies ordinary DOM setters and can enqueue mutation-observer
       jobs. Complete the same microtask/derived-document boundary used by
       author-script and event entry points so aggregate metadata and lazy
       caches cannot remain stale after a section swap. Save is read-only and
       deliberately avoids this PSP-expensive DOM traversal. */
    if (ok && completes_mutations) ok = js_rt_runtime_run_jobs(runtime);
    if (ok && completes_mutations) ok = js_rt_runtime_refresh(runtime);
    if (count != NULL) *count = ok && converted > 0
        ? (size_t) converted : 0;
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_save_section_state(ScriptRuntime *runtime,
                                       size_t section, size_t *count,
                                       ScriptResult *result)
{
    return script_runtime_section_state_call(
        runtime, SCRIPT_HOST_SAVE_SECTION_STATE, section, false, count, result);
}

bool script_runtime_restore_section_state(ScriptRuntime *runtime,
                                          size_t section, size_t *count,
                                          ScriptResult *result)
{
    return script_runtime_section_state_call(
        runtime, SCRIPT_HOST_RESTORE_SECTION_STATE, section, true, count, result);
}

bool script_runtime_set_section_identity(ScriptRuntime *runtime,
                                         size_t section_identity)
{
    if (runtime == NULL
        || (runtime->document_scope == SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME
            && section_identity != 0)) return false;
    runtime->bridge.section_identity = section_identity;
    return true;
}

bool script_runtime_set_current_script_source(
    ScriptRuntime *runtime, size_t section_identity, size_t element_ordinal)
{
    if (runtime == NULL) return false;
    int written = snprintf(
        runtime->bridge.current_script_source_key,
        sizeof(runtime->bridge.current_script_source_key),
        "s:%zu:%zu:script", section_identity, element_ordinal);
    if (written <= 0
        || (size_t) written
           >= sizeof(runtime->bridge.current_script_source_key)) return false;
    runtime->bridge.current_script_source_section = section_identity;
    runtime->bridge.current_script_source_active = true;
    return true;
}

void script_runtime_clear_current_script_source(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    runtime->bridge.current_script_source_active = false;
    runtime->bridge.current_script_source_key[0] = '\0';
}

bool script_runtime_register_script_source_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node,
    size_t section_identity, size_t element_ordinal)
{
    if (runtime == NULL || node == NULL || section_identity > INT64_MAX
        || runtime->bridge.script_source_node_count
           >= SCRIPT_SOURCE_NODE_LIMIT) return false;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->script_source_node_count
        == bridge->script_source_node_capacity) {
        size_t capacity = bridge->script_source_node_capacity == 0
            ? 16 : bridge->script_source_node_capacity * 2;
        if (capacity > SCRIPT_SOURCE_NODE_LIMIT)
            capacity = SCRIPT_SOURCE_NODE_LIMIT;
        ScriptSourceNode *resized = budget_realloc_category(
            bridge->budget, BUDGET_CATEGORY_JAVASCRIPT,
            bridge->script_source_nodes, capacity * sizeof(*resized));
        if (resized == NULL) return false;
        bridge->script_source_nodes = resized;
        bridge->script_source_node_capacity = capacity;
    }
    ScriptSourceNode *source =
        &bridge->script_source_nodes[bridge->script_source_node_count];
    int written = snprintf(source->key, sizeof(source->key),
                           "s:%zu:%zu:script", section_identity,
                           element_ordinal);
    if (written <= 0 || (size_t) written >= sizeof(source->key)) return false;
    source->node = node;
    source->owner_document_identity = js_rt_node_owner_identity(node);
    source->section = section_identity;
    bridge->script_source_node_count++;
    return true;
}

void script_runtime_clear_script_source_nodes(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    budget_free(runtime->bridge.budget,
                runtime->bridge.script_source_nodes);
    runtime->bridge.script_source_nodes = NULL;
    runtime->bridge.script_source_node_count = 0;
    runtime->bridge.script_source_node_capacity = 0;
}

static void capture_cached_error_source(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->session == NULL
        || runtime->result.error_source_context[0] != '\0') return;
    const char *open = strchr(runtime->result.error, '(');
    const char *close = open == NULL ? NULL : strchr(open, ')');
    if (close == NULL) return;
    const char *column_colon = close;
    while (column_colon > open && column_colon[-1] != ':') column_colon--;
    if (column_colon == open) return;
    const char *line_colon = column_colon - 1;
    while (line_colon > open && line_colon[-1] != ':') line_colon--;
    if (line_colon == open) return;
    size_t url_length = (size_t) ((line_colon - 1) - (open + 1));
    if (url_length == 0 || url_length >= 2048) return;
    char url[2048];
    memcpy(url, open + 1, url_length);
    url[url_length] = '\0';
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        runtime->session, url);
    if (entry != NULL) {
        js_rt_capture_error_source_context((const char *) entry->data,
                                     entry->length, url,
                                     &runtime->result);
    }
}
