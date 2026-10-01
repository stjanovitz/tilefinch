static uint64_t clock_saturating_add(uint64_t left, uint64_t right)
{
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

bool script_runtime_deterministic_replay_diagnostics(
    const ScriptRuntime *runtime,
    ScriptDeterministicReplayDiagnostics *diagnostics)
{
    if (runtime == NULL || diagnostics == NULL
        || !runtime->bridge.deterministic_clock
        || !runtime->bridge.deterministic_entropy) {
        if (diagnostics != NULL) {
            *diagnostics = (ScriptDeterministicReplayDiagnostics) {0};
        }
        return false;
    }
    const uint64_t *sources = runtime->bridge.clock_source_counts;
    uint64_t wall_observations = 0;
    for (size_t source = SCRIPT_CLOCK_SOURCE_DATE_NOW;
         source <= SCRIPT_CLOCK_SOURCE_DATE_CONSTRUCTOR; source++) {
        wall_observations = clock_saturating_add(
            wall_observations, sources[source]);
    }
    uint64_t monotonic_observations = 0;
    for (size_t source = SCRIPT_CLOCK_SOURCE_PERFORMANCE_NOW;
         source <= SCRIPT_CLOCK_SOURCE_IDLE_DEADLINE_TIME_REMAINING;
         source++) {
        monotonic_observations = clock_saturating_add(
            monotonic_observations, sources[source]);
    }
    uint64_t monotonic_samples = 0;
    for (size_t source = SCRIPT_CLOCK_SOURCE_ANIMATION_FRAME;
         source <= SCRIPT_CLOCK_SOURCE_IDLE_CALLBACK_START; source++) {
        monotonic_samples = clock_saturating_add(
            monotonic_samples, sources[source]);
    }
    *diagnostics = (ScriptDeterministicReplayDiagnostics) {
        .seed = runtime->bridge.replay_seed,
        .clock_origin_ms = runtime->bridge.clock_origin_ms,
        .host_elapsed_ms = runtime->bridge.clock_host_elapsed_ms,
        .wall_elapsed_ms = runtime->bridge.clock_wall_elapsed_ms,
        .monotonic_elapsed_ms =
            runtime->bridge.clock_monotonic_elapsed_ms,
        .wall_observations = wall_observations,
        .monotonic_observations = monotonic_observations,
        .monotonic_samples = monotonic_samples,
        .sources = {
            .date_now = sources[SCRIPT_CLOCK_SOURCE_DATE_NOW],
            .date_function = sources[SCRIPT_CLOCK_SOURCE_DATE_FUNCTION],
            .date_constructor =
                sources[SCRIPT_CLOCK_SOURCE_DATE_CONSTRUCTOR],
            .performance_now = sources[SCRIPT_CLOCK_SOURCE_PERFORMANCE_NOW],
            .performance_mark =
                sources[SCRIPT_CLOCK_SOURCE_PERFORMANCE_MARK],
            .performance_measure =
                sources[SCRIPT_CLOCK_SOURCE_PERFORMANCE_MEASURE],
            .animation_timeline =
                sources[SCRIPT_CLOCK_SOURCE_ANIMATION_TIMELINE],
            .idle_deadline_time_remaining = sources[
                SCRIPT_CLOCK_SOURCE_IDLE_DEADLINE_TIME_REMAINING],
            .animation_frame =
                sources[SCRIPT_CLOCK_SOURCE_ANIMATION_FRAME],
            .event_timestamp =
                sources[SCRIPT_CLOCK_SOURCE_EVENT_TIMESTAMP],
            .intersection_observer =
                sources[SCRIPT_CLOCK_SOURCE_INTERSECTION_OBSERVER],
            .idle_callback_start =
                sources[SCRIPT_CLOCK_SOURCE_IDLE_CALLBACK_START]
        }
    };
    return true;
}

/* The scheduler's clock as the pump would read it now. */
static double runtime_timer_now_ms(ScriptRuntime *runtime)
{
    const double *state = runtime->host_state;
    double now = state[SCRIPT_HOST_STATE_NOW];
    if (state[SCRIPT_HOST_STATE_SAMPLED_CLOCK] != 0) {
        double sample = js_rt_scheduler_sample(&runtime->bridge);
        if (sample >= now) now = sample;
    }
    return now;
}

/* Whether the pump would find a due timer, from the wheel size, head and
   clock scheduler.js publishes in host_state: the timers are sorted by due
   time, so an idle head means an idle wheel whatever the visibility. The
   sampled clock is read exactly as __tilefinchSchedulerNow reports it, just
   before the pump would have read it: a deadline falling in that gap runs
   on the next turn, like one falling just after the pump. */
static bool runtime_timer_due(ScriptRuntime *runtime)
{
    const double *state = runtime->host_state;
    if (state[SCRIPT_HOST_STATE_TIMERS] == 0) return false;
    return state[SCRIPT_HOST_STATE_EARLIEST_DUE]
        <= runtime_timer_now_ms(runtime);
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
static bool runtime_timer_is_frame_callback(double kind)
{
    return kind >= SCRIPT_HOST_TIMER_ANIMATION_FRAME
        && kind <= SCRIPT_HOST_TIMER_RENDER_FIXUP;
}
#endif

/* Fetch responses this realm delivers whose results are complete. */
static size_t runtime_responses_ready(const ScriptRuntime *runtime)
{
    const DomBridge *bridge = &runtime->bridge;
    size_t ready = 0;
    for (size_t i = 0; i < bridge->async_fetch_count; i++) {
        if (fetch_scheduler_request_complete(
                bridge->fetch_scheduler, bridge->async_fetches[i].id))
            ready++;
    }
    return ready;
}

/* The earliest task's deadline minus the scheduler's now, in ms: a due
   frame callback (requestAnimationFrame and the observer frames) waits
   for the next rendering opportunity, a due task does not, and one at the
   head must not hide the other. */
static double runtime_task_due_in_ms(ScriptRuntime *runtime)
{
    const double *state = runtime->host_state;
    if (state[SCRIPT_HOST_STATE_TIMERS] == 0) return 1e300;
    return state[SCRIPT_HOST_STATE_EARLIEST_TASK_DUE]
        - runtime_timer_now_ms(runtime);
}

bool script_runtime_task_runnable(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL) return false;
    if (js_rt_runtime_checkpoint_pending(runtime)
        || js_rt_dynamic_classic_continuation_pending(runtime))
        return true;
    if (runtime_task_due_in_ms(runtime) <= 0) return true;
    return runtime_responses_ready(runtime) != 0;
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
bool script_runtime_runnable_state(ScriptRuntime *runtime,
                                   ScriptRunnableState *state)
{
    if (state == NULL) return false;
    *state = (ScriptRunnableState) {0};
    if (runtime == NULL || runtime->runtime == NULL) return false;
    const double *host = runtime->host_state;
    double timers = host[SCRIPT_HOST_STATE_TIMERS];
    state->timers = timers > 0 ? (size_t) timers : 0u;
    if (state->timers != 0) {
        double due_in_ms = host[SCRIPT_HOST_STATE_EARLIEST_DUE]
            - runtime_timer_now_ms(runtime);
        if (due_in_ms > -1e9 && due_in_ms < 1e9) {
            state->timer_known = true;
            state->timer_due_in_us = (int64_t) (due_in_ms * 1000.0);
            state->timer_frame_callback = runtime_timer_is_frame_callback(
                host[SCRIPT_HOST_STATE_EARLIEST_KIND]);
        }
    }
    double task_due_in_ms = runtime_task_due_in_ms(runtime);
    if (task_due_in_ms > -1e9 && task_due_in_ms < 1e9) {
        state->task_known = true;
        state->task_due_in_us = (int64_t) (task_due_in_ms * 1000.0);
    }
    state->jobs_pending = js_rt_runtime_checkpoint_pending(runtime);
    state->cleanup_pending = JS_IsCleanupJobPending(runtime->runtime);
    state->continuation_pending =
        js_rt_dynamic_classic_continuation_pending(runtime);
    state->responses_ready = runtime_responses_ready(runtime);
    state->requests_in_flight =
        fetch_scheduler_pending(runtime->bridge.fetch_scheduler);
    return true;
}
#endif

static bool runtime_pump_timers(ScriptRuntime *runtime,
                                unsigned elapsed_ms,
                                size_t callback_budget,
                                size_t *callbacks_out)
{
    if (callbacks_out != NULL) *callbacks_out = 0;
    JSValue arguments[2] = {
        JS_NewUint32(runtime->context, elapsed_ms),
        JS_NewInt64(runtime->context, (int64_t) callback_budget)
    };
    JSValue returned = JS_UNDEFINED;
    /* A zero-budget pump only advances the scheduler's clock: bookkeeping.
       A budgeted pump runs an author task. */
    unsigned ledger = work_ledger_enter(callback_budget == 0
        ? WORK_LEDGER_OTHER : WORK_LEDGER_SCRIPT);
    unsigned split = callback_budget == 0
        ? SCRIPT_SPLIT_KIND_COUNT + 1u : script_split_enter(SCRIPT_SPLIT_JS);
    bool ok = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_PUMP_TIMERS, 2, arguments,
                        &returned);
    script_split_leave(split);
    work_ledger_leave(ledger);
    int64_t callbacks = 0;
    if (ok && (JS_ToInt64(runtime->context, &callbacks, returned) < 0
               || callbacks < 0
               || (uint64_t) callbacks > callback_budget)) {
        js_rt_record_exception(runtime->context, &runtime->result);
        ok = false;
    }
    JS_FreeValue(runtime->context, returned);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok && callbacks_out != NULL) *callbacks_out = (size_t) callbacks;
    return ok;
}

bool script_runtime_scheduler_snapshot(
    ScriptRuntime *runtime, ScriptSchedulerSnapshot *snapshot)
{
    if (runtime == NULL || snapshot == NULL) return false;
    *snapshot = (ScriptSchedulerSnapshot) {0};
    JSValue values = JS_UNDEFINED;
    if (!runtime_call_host(runtime, SCRIPT_HOST_SCHEDULER_SNAPSHOT,
                           0, NULL, &values)
        || !JS_IsArray(runtime->context, values)) {
        JS_FreeValue(runtime->context, values);
        return false;
    }
    double numeric[11] = {0};
    bool ok = true;
    for (uint32_t i = 0; i < 11; i++) {
        JSValue value = JS_GetPropertyUint32(runtime->context, values, i);
        if (JS_ToFloat64(runtime->context, &numeric[i], value) < 0) ok = false;
        JS_FreeValue(runtime->context, value);
        if (!ok) break;
    }
    JS_FreeValue(runtime->context, values);
    if (!ok || numeric[1] < 0 || numeric[1] > 1
        || numeric[2] < 0 || numeric[3] < 0) return false;
    snapshot->now_ms = numeric[0];
    snapshot->sampled_clock = numeric[1] != 0;
    snapshot->timers = (size_t) numeric[2];
    snapshot->due_timers = (size_t) numeric[3];
    snapshot->oldest_overdue_ms = numeric[4];
    snapshot->nearest_future_ms = numeric[5];
    snapshot->last_callback_ms = numeric[6];
    snapshot->active_cancel_attempts = (size_t) numeric[7];
    snapshot->active_cancel_hits = (size_t) numeric[8];
    snapshot->timeout_callbacks = (size_t) numeric[9];
    snapshot->interval_callbacks = (size_t) numeric[10];
    snapshot->checkpoint_continuations =
        runtime->checkpoint_continuation_count;
    snapshot->quickjs_jobs_pending = JS_IsJobPending(runtime->runtime);
    snapshot->classic_script_continuation =
        js_rt_dynamic_classic_continuation_pending(runtime);
    return true;
}

/* Every check costs a full collection, so space them out; the window only
   needs to return once, shortly after the hydration transient dies. A check
   that can't return it doubles the interval: a large settled heap (1.8 s to
   collect on the PSP) otherwise froze the page every 256 advances. */
#define BOOT_WINDOW_CHECK_INTERVAL 256u
#define BOOT_WINDOW_MAX_CHECK_INTERVAL 16384u
#define BOOT_WINDOW_MAX_DEFER_ADVANCES 4096u

static void runtime_boot_window_step(ScriptRuntime *runtime)
{
    if (runtime == NULL || !runtime->boot_window_active
        || runtime->runtime == NULL) {
        return;
    }
    runtime->boot_window_advances++;
    if (runtime->boot_window_next_check == 0)
        runtime->boot_window_next_check = BOOT_WINDOW_CHECK_INTERVAL;
    if (runtime->boot_window_advances < runtime->boot_window_next_check)
        return;
    runtime->boot_window_next_check =
        runtime->boot_window_advances + BOOT_WINDOW_CHECK_INTERVAL;
    /* A temporarily small live heap is not proof that startup has settled.
       Delayed hydration commonly waits on a timer or network response and
       can need the donated space in its next callback. Returning the window
       in that gap makes a bounded, otherwise-admitted callback fail (and has
       exposed allocator bugs in dependencies which assume ordinary OOM is
       not injected midway through their own response decoder). Keep the
       window while finite author startup work remains, with a hard lifetime
       so an ambient timer cannot permanently turn the boot allowance into a
       larger steady-state heap. */
    if (runtime->boot_window_advances < BOOT_WINDOW_MAX_DEFER_ADVANCES
        && script_runtime_has_pending_author_work(runtime)) {
        return;
    }
    bool dump = tilefinch_dump_js_memory();
    uint64_t gc_started_ns = dump ? js_rt_monotonic_time_ns() : 0;
    JS_RunGC(runtime->runtime);
    uint64_t gc_ns = dump ? js_rt_monotonic_time_ns() - gc_started_ns : 0;
    runtime->boot_window_checks++;
    uint64_t backoff = BOOT_WINDOW_CHECK_INTERVAL;
    for (size_t failed = 1; failed < runtime->boot_window_checks
         && backoff < BOOT_WINDOW_MAX_CHECK_INTERVAL; failed++)
        backoff *= 2u;
    runtime->boot_window_next_check = runtime->boot_window_advances + backoff;
    /* O(1): the allocator shim maintains the malloc_size a census would
       report. The census walk added 40-60% to this collection's pause on
       the host, more on the PSP (soft-float per string and shape). */
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    if (live > runtime->boot_window_peak) runtime->boot_window_peak = live;
    if (dump) {
        JSMemoryUsage usage;
        uint64_t census_started_ns = js_rt_monotonic_time_ns();
        js_rt_heap_census(runtime, &usage);
        uint64_t census_ns = js_rt_monotonic_time_ns() - census_started_ns;
        fprintf(stderr, "tilefinch: boot-window census advance=%llu live=%zu "
                "gc-us=%llu census-us=%llu\n",
                (unsigned long long) runtime->boot_window_advances, live,
                (unsigned long long) (gc_ns / 1000u),
                (unsigned long long) (census_ns / 1000u));
        JS_DumpMemoryUsage(stderr, &usage, runtime->runtime);
    }
    /* Return the window only when the live graph fits the base with the
       same headroom reserve the GC pacing keeps below a limit, so the
       shrunken limit does not immediately re-trigger pressure. */
    size_t reserve = runtime->base_memory_limit / 8u;
    if (reserve > 4u * 1024u * 1024u) reserve = 4u * 1024u * 1024u;
    if (live + reserve <= runtime->base_memory_limit) {
        JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
        runtime->boot_window_active = false;
        runtime->boot_window_returned_advance = runtime->boot_window_advances;
        size_t trimmed = budget_quickjs_pool_trim(runtime->quickjs_pool, 8);
        fprintf(stderr,
                "tilefinch: boot-window returned advance=%llu live=%zu "
                "base=%zu window=%zu trimmed=%zu\n",
                (unsigned long long) runtime->boot_window_advances,
                live, runtime->base_memory_limit,
                runtime->boot_window_bytes, trimmed);
    }
}

/* The limit that lets automatic collection keep its `growth_pct` spacing:
   QuickJS re-arms at half the remaining headroom near the limit, so twice
   the pacing headroom, rounded to growth steps. */
static size_t runtime_heap_pacing_limit(size_t live, long growth_pct)
{
    if (growth_pct <= 100) return live;
    uint64_t headroom = (uint64_t) live * (uint64_t) (growth_pct - 100) / 50u;
    uint64_t want = (uint64_t) live + headroom;
    want += (SCRIPT_HEAP_GROWTH_STEP - want % SCRIPT_HEAP_GROWTH_STEP)
        % SCRIPT_HEAP_GROWTH_STEP;
    return want > SIZE_MAX ? SIZE_MAX : (size_t) want;
}

static long runtime_gc_growth_pct_value = -1;

/* Return memory-pressure growth the live graph no longer uses, so the rest
   of the page (and the next page) regains that Budget. Same cadence and
   pending-work deferral as the boot window, which it follows. */
static void runtime_heap_growth_step(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || !runtime->heap_growth_enabled || runtime->boot_window_active
        || runtime->base_memory_limit <= runtime->heap_growth_floor) {
        return;
    }
    runtime->heap_growth_advances++;
    if (runtime->heap_growth_next_check == 0)
        runtime->heap_growth_next_check =
            runtime->heap_growth_advances + BOOT_WINDOW_CHECK_INTERVAL;
    if (runtime->heap_growth_advances < runtime->heap_growth_next_check)
        return;
    runtime->heap_growth_next_check =
        runtime->heap_growth_advances + BOOT_WINDOW_CHECK_INTERVAL;
    if (script_runtime_has_pending_author_work(runtime)) return;
    JS_RunGC(runtime->runtime);
    /* Assume this check returns nothing; a return resets the backoff. A
       settled realm whose growth is still in use (or kept as collection
       headroom) then collects at 256, 512, ... up to the boot window's
       longest interval instead of every 256 advances forever. */
    runtime->heap_growth_fruitless_checks++;
    uint64_t backoff = BOOT_WINDOW_CHECK_INTERVAL;
    for (size_t failed = 1; failed < runtime->heap_growth_fruitless_checks
         && backoff < BOOT_WINDOW_MAX_CHECK_INTERVAL; failed++)
        backoff *= 2u;
    runtime->heap_growth_next_check = runtime->heap_growth_advances + backoff;
    size_t live = budget_quickjs_pool_js_malloc_current(
        runtime->quickjs_pool);
    size_t reserve = live / 8u;
    if (reserve < 2u * SCRIPT_HEAP_GROWTH_STEP)
        reserve = 2u * SCRIPT_HEAP_GROWTH_STEP;
    if (reserve > 4u * 1024u * 1024u) reserve = 4u * 1024u * 1024u;
    size_t target = live > SIZE_MAX - reserve ? SIZE_MAX : live + reserve;
    target += (SCRIPT_HEAP_GROWTH_STEP - target % SCRIPT_HEAP_GROWTH_STEP)
        % SCRIPT_HEAP_GROWTH_STEP;
    /* Keep the collection headroom pre-growth would restore next advance;
       returning it only brings the collection storm back. Under pressure
       pre-growth stops, and this returns the rest. */
    size_t pacing = runtime_heap_pacing_limit(
        live, runtime_gc_growth_pct_value < 0 ? 125
                                              : runtime_gc_growth_pct_value);
    if (pacing > target && runtime->budget != NULL
        && budget_remaining(runtime->budget) > runtime->heap_growth_reserve)
        target = pacing;
    if (target < runtime->heap_growth_floor)
        target = runtime->heap_growth_floor;
    if (target >= runtime->base_memory_limit) return;
    runtime->base_memory_limit = target;
    runtime->heap_growth_returns++;
    runtime->heap_growth_fruitless_checks = 0;
    runtime->heap_growth_next_check =
        runtime->heap_growth_advances + BOOT_WINDOW_CHECK_INTERVAL;
    JS_SetMemoryLimit(runtime->runtime, script_runtime_current_limit(runtime));
    (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 8);
}

/* Deterministic replay must not let collection timing depend on
   allocation volume: reselect v5 memoizes through WeakRef, so a GC that
   fires at a malloc threshold reroutes React renders and any
   heap-behavior change (allocator, VM object layout) breaks frame
   determinism.  Reachability at a fixed logical point is semantics-only,
   so pin collections to the advance schedule instead and keep the
   threshold trigger out of the way.  Opt-in pacing experiments
   (TILEFINCH_JS_GC_SLACK_KB) override this. */
#define DETERMINISTIC_GC_ADVANCE_INTERVAL 16u

static void runtime_apply_deterministic_gc(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || !runtime->bridge.deterministic_clock) {
        return;
    }
    static long slack_kb = -1;
    if (slack_kb < 0) {
        const char *value = getenv("TILEFINCH_JS_GC_SLACK_KB");
        slack_kb = value == NULL ? 0 : atol(value);
        if (slack_kb < 0) slack_kb = 0;
        const char *growth = getenv("TILEFINCH_JS_GC_GROWTH_PCT");
        if (growth != NULL && atol(growth) > 0) slack_kb = -2;
    }
    if (slack_kb != 0) return;
    JS_SetGCThreshold(runtime->runtime, (size_t) -1);
    runtime->deterministic_gc_advances++;
    if (runtime->deterministic_gc_advances
            % DETERMINISTIC_GC_ADVANCE_INTERVAL == 0) {
        JS_RunGC(runtime->runtime);
    }
}

static void runtime_apply_gc_slack(ScriptRuntime *runtime)
{
    /* Boot-window pressure pacing: cap QuickJS's GC-threshold growth to
       live + a bounded slack, trading collection CPU for a heap ceiling
       close to the live graph.  Opt-in while under experiment.

       Adaptive mode (TILEFINCH_JS_GC_GROWTH_PCT, with
       TILEFINCH_JS_GC_MIN_SLACK_KB): flat-tight pacing pays the full
       collection cost across the whole boot, but the ceiling only
       matters near the peak.  A growth-based threshold
       (live * growth%) collects logarithmically while far from the
       limit and converges to live + min_slack only inside the danger
       zone, aiming for the tight-pacing ceiling at a fraction of its
       CPU cost. */
    static long slack_kb = -1;
    static long growth_pct = -1;
    static long min_slack_kb = -1;
    if (slack_kb < 0) {
        const char *value = getenv("TILEFINCH_JS_GC_SLACK_KB");
        slack_kb = value == NULL ? 0 : atol(value);
        if (slack_kb < 0) slack_kb = 0;
        value = getenv("TILEFINCH_JS_GC_GROWTH_PCT");
        /* QuickJS's unconstrained threshold growth can reach a realm's hard
           ceiling before collecting dead cyclic browser state.  A 125% live
           heap threshold remains logarithmic while there is room, then the
           reserve clamp below leaves ordinary event construction headroom.
           Tests and deterministic replay may still override this explicitly. */
        growth_pct = value == NULL ? 125 : atol(value);
        if (growth_pct < 0) growth_pct = 0;
        runtime_gc_growth_pct_value = growth_pct;
        value = getenv("TILEFINCH_JS_GC_MIN_SLACK_KB");
        min_slack_kb = value == NULL ? 512 : atol(value);
        if (min_slack_kb < 64) min_slack_kb = 64;
    }
    if ((slack_kb == 0 && growth_pct == 0)
        || runtime == NULL || runtime->runtime == NULL) {
        return;
    }
    /* O(1): the allocator shim maintains the enforced malloc_size; a
       JS_ComputeMemoryUsage heap walk here costs more than the
       collections it paces. */
    uint64_t live = budget_quickjs_pool_js_malloc_current(
        runtime->quickjs_pool);
    /* A growable realm paces from the graph the last collection left, not
       from the current size: that includes garbage, and re-arming 25% above
       it every advance would let garbage grow with the limit. A fixed limit
       already bounds that, so fixed realms keep pacing from the current
       size. */
    uint64_t paced = runtime->heap_growth_enabled
            && runtime->heap_live_after_gc != 0
            && runtime->heap_live_after_gc < live
        ? runtime->heap_live_after_gc : live;
    if (growth_pct > 0)
        script_runtime_heap_pregrow(
            runtime, runtime_heap_pacing_limit((size_t) paced, growth_pct));
    uint64_t effective_limit = runtime->base_memory_limit
        + (runtime->boot_window_active ? runtime->boot_window_bytes : 0);
    uint64_t threshold;
    if (growth_pct > 0) {
        uint64_t growth = (uint64_t) growth_pct;
        uint64_t grown;
        /* The PSP realm and its configured growth percentage fit this path.
           Avoid a software 64-bit division on every scripted loop turn while
           retaining saturating wide arithmetic for host overrides. */
        if (paced <= UINT32_MAX && growth <= UINT32_MAX
            && (growth == 0 || paced <= UINT32_MAX / growth)) {
            grown = (uint32_t) paced * (uint32_t) growth / 100u;
        } else {
            uint64_t product = 0;
#if defined(__GNUC__) || defined(__clang__)
            grown = __builtin_mul_overflow(paced, growth, &product)
                ? UINT64_MAX : product / 100u;
#else
            grown = growth != 0 && paced > UINT64_MAX / growth
                ? UINT64_MAX : paced * growth / 100u;
#endif
        }
        uint64_t floor = paced + (uint64_t) min_slack_kb * 1024u;
        threshold = grown > floor ? grown : floor;
    } else {
        threshold = live + (uint64_t) slack_kb * 1024u;
    }
    bool clamped = false;
    if (effective_limit > 0 && threshold >= effective_limit) {
        clamped = true;
        static long reserve_kb = -1;
        if (reserve_kb < 0) {
            const char *value = getenv("TILEFINCH_JS_GC_RESERVE_KB");
            reserve_kb = value == NULL ? 256 : atol(value);
            if (reserve_kb < 64) reserve_kb = 64;
        }
        uint64_t limit = effective_limit;
        uint64_t reserve = limit / 8u;
        if (reserve > (uint64_t) reserve_kb * 1024u) {
            reserve = (uint64_t) reserve_kb * 1024u;
        }
        threshold = limit > reserve ? limit - reserve : limit;
    }
    /* Once live state occupies the desired reserve, reapplying a threshold
       below live makes the first tiny object allocation collect on EVERY
       advance, even when the preceding collection reclaimed nothing. Allow
       half the remaining headroom to be allocated before the next automatic
       collection. This never raises the allocator's hard limit, and the
       exhausted-heap case still collects before object creation. In a
       growable realm an unclamped threshold at or below live means garbage
       has passed its pacing share since the last collection: collect at the
       next allocation. */
    if ((clamped || !runtime->heap_growth_enabled)
        && threshold <= live && live < effective_limit) {
        uint64_t headroom = effective_limit - live;
        threshold = live + headroom / 2u + headroom % 2u;
    }
    JS_SetGCThreshold(runtime->runtime,
                      threshold > SIZE_MAX ? SIZE_MAX : (size_t) threshold);
}

static bool runtime_dispatch_page_visibility_edge(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->page_visibility_queue_count == 0)
        return true;
    uint8_t head = runtime->page_visibility_queue_head;
    bool visible = runtime->page_visibility_queue[head];
    runtime->page_visibility_queue_head = (uint8_t) ((head + 1u) % 2u);
    runtime->page_visibility_queue_count--;
    JSValue argument = JS_NewBool(runtime->context, visible);
    bool ok = !JS_IsException(argument)
        && runtime_call_captured(
               runtime, runtime->page_visibility_host_apply,
               "page visibility", 1, &argument, NULL);
    JS_FreeValue(runtime->context, argument);
    return ok;
}

static bool runtime_dispatch_game_audio_completion(ScriptRuntime *runtime,
                                                    bool *dispatched)
{
    if (dispatched != NULL) *dispatched = false;
    if (runtime == NULL || runtime->game_audio == NULL) return true;
    if (!JS_IsFunction(runtime->context,
                       runtime->game_audio_host_complete)) return false;
    uint32_t voice_handle = 0;
    if (!tilefinch_game_audio_take_completion(
            runtime->game_audio, &voice_handle)) return true;
    JSValue argument = JS_NewUint32(runtime->context, voice_handle);
    bool ok = !JS_IsException(argument)
        && runtime_call_captured(
               runtime, runtime->game_audio_host_complete,
               "game audio completion", 1, &argument, NULL);
    JS_FreeValue(runtime->context, argument);
    if (dispatched != NULL) *dispatched = true;
    return ok;
}

static bool runtime_task_slice_available(const ScriptRuntime *runtime,
                                         uint64_t started_ns,
                                         size_t remaining, size_t budget)
{
    uint64_t limit = runtime->bridge.execution_policy.maximum_advance_time_us;
    /* Make at least one task's progress even when preparation itself is
       expensive. Subsequent tasks wait for another host turn, leaving input
       and presentation an opportunity without changing task/job ordering. */
    if (limit == 0 || remaining == budget) return true;
    uint64_t now = js_rt_monotonic_time_ns();
    return now < started_ns || (now - started_ns) / UINT64_C(1000) < limit;
}

/* Low-priority JavaScript-engine tasks: never spend the promise-job quota on
   wrapper cleanup, and never run it inside a continued task. Each cleanup
   is one task from the caller's budget followed by its own checkpoint.
   Memory-pressure handle reclamation remains synchronous and independent. */
static size_t runtime_run_cleanup_tasks(ScriptRuntime *runtime, size_t limit,
                                        size_t *remaining,
                                        uint64_t task_slice_started_ns,
                                        size_t callback_budget, bool *ok)
{
    size_t ran_tasks = 0;
    while (*ok && ran_tasks < limit && *remaining != 0
           && !js_rt_runtime_checkpoint_pending(runtime)
           && JS_IsCleanupJobPending(runtime->runtime)
           && runtime_task_slice_available(runtime, task_slice_started_ns,
                                           *remaining, callback_budget)) {
        if (!js_rt_runtime_native_checkpoint(runtime)) { *ok = false; break; }
        size_t failures = runtime->budget->failure_count;
        size_t rejections = script_runtime_heap_rejections(runtime);
        JSContext *context = NULL;
        BudgetAllocationOwner owner = document_allocation_owner_enter(
            runtime->document);
        uint64_t started = js_rt_monotonic_time_ns();
        size_t polls = runtime->watchdog.polls;
        unsigned ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
        unsigned split = script_split_enter(SCRIPT_SPLIT_JS);
        script_profile_enter(runtime);
        int ran = JS_ExecutePendingCleanupJob(runtime->runtime, &context);
        script_profile_leave(runtime);
        script_split_leave(split);
        work_ledger_leave(ledger);
        document_allocation_owner_leave(runtime->document, owner);
        js_rt_runtime_record_host_callback(runtime, "<finalizer-task>", started, polls);
        if (ran == 0) break;
        ran_tasks++;
        (*remaining)--;
        if (ran < 0) {
            js_rt_record_exception(context != NULL ? context : runtime->context,
                                    &runtime->result);
            js_lazy_webpack_recover_failure(runtime);
            if (runtime_job_exception_is_fatal(runtime, failures, rejections)) {
                *ok = false;
                break;
            }
            runtime_record_author_job_exception(runtime);
        }
        *ok = js_rt_runtime_run_jobs(runtime);
    }
    return ran_tasks;
}

bool script_runtime_advance(ScriptRuntime *runtime, unsigned elapsed_ms,
                            size_t callback_budget, ScriptResult *result)
{
    if (runtime == NULL || callback_budget > 1024) return false;
    uint64_t task_slice_started_ns = js_rt_monotonic_time_ns();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t advance_started_ns = js_rt_monotonic_time_ns();
    uint64_t prelude_started_ns = advance_started_ns;
    uint64_t webgl_before_us = runtime->bridge.webgl_native_total_us;
    size_t webgl_before_frames = runtime->bridge.webgl_native_frames;
    BudgetQuickJSActivity allocator_before = {0};
    budget_quickjs_pool_activity(runtime->quickjs_pool, &allocator_before);
    runtime->timing_metrics.advance_timer_late_max_us = 0;
#endif
    runtime_apply_gc_slack(runtime);
    runtime_apply_deterministic_gc(runtime);
    runtime_boot_window_step(runtime);
    runtime_heap_growth_step(runtime);
    js_rt_runtime_arm_watchdog(runtime);
    if (runtime->bridge.deterministic_clock
        && elapsed_ms != 0) {
        runtime->bridge.clock_host_elapsed_ms =
            runtime->bridge.clock_host_elapsed_ms
                > UINT64_MAX - (uint64_t) elapsed_ms
            ? UINT64_MAX
            : runtime->bridge.clock_host_elapsed_ms
                  + (uint64_t) elapsed_ms;
        runtime->bridge.clock_wall_elapsed_ms =
            runtime->bridge.clock_wall_elapsed_ms
                > UINT64_MAX - (uint64_t) elapsed_ms
            ? UINT64_MAX
            : runtime->bridge.clock_wall_elapsed_ms
                  + (uint64_t) elapsed_ms;
        runtime->bridge.clock_monotonic_elapsed_ms =
            runtime->bridge.clock_monotonic_elapsed_ms
                > UINT64_MAX - (uint64_t) elapsed_ms
            ? UINT64_MAX
            : runtime->bridge.clock_monotonic_elapsed_ms
                  + (uint64_t) elapsed_ms;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    runtime->timing_metrics.prelude_us +=
        (js_rt_monotonic_time_ns() - prelude_started_ns) / UINT64_C(1000);
#endif
    bool trace_steps = tilefinch_trace_runtime_steps();
    uint64_t network_elapsed_ns = 0;
    uint64_t timer_elapsed_ns = 0;
    uint64_t microtask_elapsed_ns = 0;

    /* Advance the timer wheel once even when this turn's callback budget is
       zero. No author callback runs at this preparation boundary. */
    uint64_t phase_started = js_rt_monotonic_time_ns();
    size_t prepared_callbacks = 0;
    bool ok = runtime_pump_timers(runtime, elapsed_ms, 0,
                                  &prepared_callbacks);
    uint64_t timer_prepare_ns = js_rt_monotonic_time_ns() - phase_started;
    timer_elapsed_ns += timer_prepare_ns;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    runtime->timing_metrics.timer_prepare_us +=
        timer_prepare_ns / UINT64_C(1000);
#endif
    if (!ok || prepared_callbacks != 0) {
        runtime->result.success = false;
        js_rt_runtime_update_frame_result(runtime, result);
        return false;
    }

    /* Transport may make progress independently of the author-task budget,
       but promise settlement and dynamic-script execution remain gated. */
    phase_started = js_rt_monotonic_time_ns();
    size_t prepared_network_tasks = 0;
    ok = runtime_pump_network(runtime, 0, elapsed_ms,
                              &prepared_network_tasks);
    network_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    if (!ok || prepared_network_tasks != 0) {
        runtime->result.success = false;
        js_rt_runtime_update_frame_result(runtime, result);
        return false;
    }

    bool classic_continuation =
        js_rt_dynamic_classic_continuation_pending(runtime);
    /* Native input may update the viewport many times between event-loop
       turns. The captured bridge emits at most one window/visualViewport
       pair here, after the final position is already observable. A continued
       classic script must remain the only author task until it completes. */
    if (!classic_continuation
        && runtime->host_state[SCRIPT_HOST_STATE_SCROLL_PENDING] != 0
        && !runtime_call_captured(runtime, runtime->page_scroll_flush,
                                  "page scroll dispatch", 0, NULL, NULL)) {
        runtime->result.success = false;
        js_rt_runtime_update_frame_result(runtime, result);
        return false;
    }

    /* A task is never followed by another task until its bounded QuickJS job
       checkpoint has run. If the 64-job checkpoint remains non-empty, defer
       all later tasks to the next host turn instead of reordering them. */
    if (!classic_continuation) {
        phase_started = js_rt_monotonic_time_ns();
        if (runtime->profile != NULL) runtime->profile->micro_site = 1;
        ok = js_rt_runtime_run_jobs(runtime);
        if (runtime->profile != NULL) runtime->profile->micro_site = 0;
        microtask_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    }
    size_t remaining = callback_budget;
    if (ok && !classic_continuation
        && !js_rt_runtime_checkpoint_pending(runtime)
        && runtime->page_visibility_queue_count != 0
        && remaining != 0) {
        ok = runtime_dispatch_page_visibility_edge(runtime);
        remaining--;
        if (ok) ok = js_rt_runtime_run_jobs(runtime);
    }
    while (ok && !classic_continuation
           && !js_rt_runtime_checkpoint_pending(runtime) && remaining != 0
           && runtime_task_slice_available(runtime, task_slice_started_ns,
                                           remaining, callback_budget)) {
        bool dispatched = false;
        ok = runtime_dispatch_game_audio_completion(runtime, &dispatched);
        if (!ok || !dispatched) break;
        remaining--;
        ok = js_rt_runtime_run_jobs(runtime);
    }
    /* Cleanup normally takes only the slots due tasks leave. A page whose
       timers or deliveries are due on every turn would then starve it, and
       with it the release of every collected wrapper's handle slot, for as
       long as the page stays busy (two frame-rate intervals fill the PSP's
       two-task turn indefinitely). After a few consecutive starved turns,
       admit one cleanup ahead of this turn's tasks. */
    uint64_t cleanup_started_ns = trace_steps ? js_rt_monotonic_time_ns() : 0;
    size_t cleanups = 0;
    if (ok && !classic_continuation
        && runtime->cleanup_starved_advances
               >= SCRIPT_CLEANUP_STARVATION_ADVANCES) {
        cleanups = runtime_run_cleanup_tasks(
            runtime, 1, &remaining, task_slice_started_ns, callback_budget,
            &ok);
    }
    size_t callbacks = 0;
    while (ok && remaining != 0
           && runtime_task_slice_available(runtime, task_slice_started_ns,
                                           remaining, callback_budget)
           && (classic_continuation
               || !js_rt_runtime_checkpoint_pending(runtime))) {
        size_t network_tasks = 0;
        phase_started = js_rt_monotonic_time_ns();
        ok = runtime_pump_network(runtime, 1, 0, &network_tasks);
        network_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
        if (!ok) break;
        if (network_tasks != 0) {
            remaining--;
            classic_continuation =
                js_rt_dynamic_classic_continuation_pending(runtime);
            if (!classic_continuation) {
                phase_started = js_rt_monotonic_time_ns();
                if (runtime->profile != NULL) runtime->profile->micro_site = 2;
                ok = js_rt_runtime_run_jobs(runtime);
                if (runtime->profile != NULL) runtime->profile->micro_site = 0;
                microtask_elapsed_ns +=
                    js_rt_monotonic_time_ns() - phase_started;
            }
            continue;
        }

        if (classic_continuation || !runtime_timer_due(runtime)) break;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* The head of the sorted wheel is the task this pump runs (a
           hidden page may skip visual timers; then this is approximate). */
        double timer_late_ms = runtime_timer_now_ms(runtime)
            - runtime->host_state[SCRIPT_HOST_STATE_EARLIEST_DUE];
        bool frame_callback = runtime_timer_is_frame_callback(
            runtime->host_state[SCRIPT_HOST_STATE_EARLIEST_KIND]);
#endif
        size_t timer_callbacks = 0;
        phase_started = js_rt_monotonic_time_ns();
        ok = runtime_pump_timers(runtime, 0, 1, &timer_callbacks);
        uint64_t timer_turn_ns = js_rt_monotonic_time_ns() - phase_started;
        timer_elapsed_ns += timer_turn_ns;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (timer_callbacks != 0 && frame_callback) {
            runtime->timing_metrics.frame_callbacks++;
            if (timer_late_ms > 0)
                runtime->timing_metrics.frame_callback_late_us +=
                    (uint64_t) (timer_late_ms * 1000.0);
        } else if (timer_callbacks != 0 && timer_late_ms > 0) {
            uint64_t late_us = (uint64_t) (timer_late_ms * 1000.0);
            runtime->timing_metrics.timer_late_us += late_us;
            if (late_us > runtime->timing_metrics.advance_timer_late_max_us)
                runtime->timing_metrics.advance_timer_late_max_us = late_us;
            if (late_us >= UINT64_C(16667))
                runtime->timing_metrics.timer_late_frames++;
        }
        if (timer_callbacks != 0) {
            uint64_t timer_turn_us = timer_turn_ns / UINT64_C(1000);
            runtime->timing_metrics.timer_callbacks += timer_callbacks;
            runtime->timing_metrics.timer_callback_us += timer_turn_us;
            if (timer_turn_us
                    > runtime->timing_metrics.maximum_timer_callback_us) {
                runtime->timing_metrics.maximum_timer_callback_us =
                    timer_turn_us;
            }
        }
#endif
        if (!ok) break;
        if (timer_callbacks == 0) break;
        callbacks += timer_callbacks;
        remaining -= timer_callbacks;
        phase_started = js_rt_monotonic_time_ns();
        if (runtime->profile != NULL) runtime->profile->micro_site = 3;
        ok = js_rt_runtime_run_jobs(runtime);
        if (runtime->profile != NULL) runtime->profile->micro_site = 0;
        microtask_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    }
    if (ok && !classic_continuation && cleanups < 16) {
        cleanups += runtime_run_cleanup_tasks(
            runtime, 16 - cleanups, &remaining, task_slice_started_ns,
            callback_budget, &ok);
    }
    /* Only a turn whose tasks left cleanup no slot counts as starved; a
       continued classic script or an unfinished checkpoint legitimately
       holds every later task, cleanup included. */
    if (cleanups != 0 || !JS_IsCleanupJobPending(runtime->runtime)) {
        runtime->cleanup_starved_advances = 0;
    } else if (ok && !classic_continuation
               && !js_rt_runtime_checkpoint_pending(runtime)
               && runtime->cleanup_starved_advances < UINT8_MAX) {
        runtime->cleanup_starved_advances++;
    }
    if (trace_steps) {
        if (cleanups != 0)
            fprintf(stderr, "runtime-cleanup tasks=%zu us=%llu pending=%d\n",
                    cleanups, (unsigned long long)
                    ((js_rt_monotonic_time_ns()-cleanup_started_ns)/1000u),
                    JS_IsCleanupJobPending(runtime->runtime));
        fprintf(stderr, "runtime-step phase=network-pump us=%llu\n",
                (unsigned long long)
                  (network_elapsed_ns / UINT64_C(1000)));
        fprintf(stderr, "runtime-step phase=timer-pump us=%llu\n",
                (unsigned long long)
                  (timer_elapsed_ns / UINT64_C(1000)));
        fprintf(stderr, "runtime-step phase=microtasks us=%llu\n",
                (unsigned long long)
                  (microtask_elapsed_ns / UINT64_C(1000)));
    }
    classic_continuation =
        js_rt_dynamic_classic_continuation_pending(runtime);
    if (ok) {
        runtime->result.runtime_ticks++;
        if (callbacks != 0) {
            runtime->result.timer_callbacks_run += callbacks;
        }
        if (!classic_continuation) {
            phase_started = js_rt_monotonic_time_ns();
            ok = js_rt_runtime_refresh(runtime);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            runtime->timing_metrics.refresh_us +=
                (js_rt_monotonic_time_ns() - phase_started)
                    / UINT64_C(1000);
#endif
            if (trace_steps) {
                fprintf(stderr, "runtime-step phase=dom-refresh us=%llu\n",
                        (unsigned long long)
                          ((js_rt_monotonic_time_ns() - phase_started)
                           / UINT64_C(1000)));
            }
        }
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    runtime->timing_metrics.network_us +=
        network_elapsed_ns / UINT64_C(1000);
    runtime->timing_metrics.microtask_us +=
        microtask_elapsed_ns / UINT64_C(1000);
    uint64_t advance_us =
        (js_rt_monotonic_time_ns() - advance_started_ns) / UINT64_C(1000);
    runtime->timing_metrics.advances++;
    runtime->timing_metrics.total_us += advance_us;
    runtime->timing_metrics.webgl_native_us +=
        runtime->bridge.webgl_native_total_us - webgl_before_us;
    runtime->timing_metrics.webgl_native_frames +=
        runtime->bridge.webgl_native_frames - webgl_before_frames;
    BudgetQuickJSActivity allocator_after = {0};
    budget_quickjs_pool_activity(runtime->quickjs_pool, &allocator_after);
    runtime->timing_metrics.js_allocation_calls +=
        allocator_after.allocation_calls - allocator_before.allocation_calls;
    runtime->timing_metrics.js_free_calls +=
        allocator_after.free_calls - allocator_before.free_calls;
    runtime->timing_metrics.js_reallocation_calls +=
        allocator_after.reallocation_calls
            - allocator_before.reallocation_calls;
    runtime->timing_metrics.js_allocated_bytes +=
        allocator_after.allocated_bytes - allocator_before.allocated_bytes;
    runtime->timing_metrics.js_freed_bytes +=
        allocator_after.freed_bytes - allocator_before.freed_bytes;
    runtime->timing_metrics.js_reallocated_bytes +=
        allocator_after.reallocated_bytes
            - allocator_before.reallocated_bytes;
    runtime->timing_metrics.js_live_bytes = allocator_after.live_bytes;
    /* Exact js_malloc census: rises are heap-garbage production the pool
       counters above cannot see; a drop this large between two advances is
       a full collection.  Steady gameplay must hold rises near zero, or the
       accumulated garbage paces full-GC pauses onto presented frames. */
    {
        size_t js_heap_now = budget_quickjs_pool_js_malloc_current(
            runtime->quickjs_pool);
        ScriptRuntimeTimingMetrics *census = &runtime->timing_metrics;
        if (census->js_heap_samples == 0) {
            census->js_heap_low_bytes = js_heap_now;
            census->js_heap_high_bytes = js_heap_now;
        } else {
            size_t previous = census->js_heap_last_bytes;
            if (js_heap_now > previous) {
                census->js_heap_risen_bytes += js_heap_now - previous;
            } else if (previous - js_heap_now >= 32u * 1024u) {
                census->js_heap_gc_drops++;
                if (previous - js_heap_now > census->js_heap_gc_drop_max_bytes)
                    census->js_heap_gc_drop_max_bytes =
                        previous - js_heap_now;
            }
            if (js_heap_now < census->js_heap_low_bytes)
                census->js_heap_low_bytes = js_heap_now;
            if (js_heap_now > census->js_heap_high_bytes)
                census->js_heap_high_bytes = js_heap_now;
        }
        census->js_heap_last_bytes = js_heap_now;
        census->js_heap_samples++;
    }
    if (advance_us > runtime->timing_metrics.maximum_total_us)
        runtime->timing_metrics.maximum_total_us = advance_us;
#endif
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    runtime_report_heap_failure(runtime);
#endif
    return ok;
}

bool script_runtime_timing_metrics(
    const ScriptRuntime *runtime, ScriptRuntimeTimingMetrics *metrics)
{
    if (metrics == NULL) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (runtime == NULL) {
        memset(metrics, 0, sizeof(*metrics));
        return false;
    }
    *metrics = runtime->timing_metrics;
    return metrics->advances != 0;
#else
    (void) runtime;
    memset(metrics, 0, sizeof(*metrics));
    return false;
#endif
}

void script_runtime_timing_metrics_reset(ScriptRuntime *runtime)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (runtime == NULL) return;
    memset(&runtime->timing_metrics, 0, sizeof(runtime->timing_metrics));
    runtime->bridge.webgl_native_total_us = 0;
    runtime->bridge.webgl_native_frames = 0;
#else
    (void) runtime;
#endif
}

/* Validation probe: one timed full collection on the live realm, so a report
   can state what a garbage-paced GC pause costs on the measured processor
   instead of inferring it.  The collection itself is always semantically
   safe; shipping builds simply never ask. */
uint64_t script_runtime_gc_probe_us(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL) return 0;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    JS_RunGC(runtime->runtime);
    return (js_rt_monotonic_time_ns() - started_ns) / UINT64_C(1000);
}
