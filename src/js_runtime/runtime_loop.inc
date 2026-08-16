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
    bool ok = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_PUMP_TIMERS, 2, arguments,
                        &returned);
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

/* Every check costs a full collection, so space them out; the window only
   needs to return once, shortly after the hydration transient dies. */
#define BOOT_WINDOW_CHECK_INTERVAL 256u

static void runtime_boot_window_step(ScriptRuntime *runtime)
{
    if (runtime == NULL || !runtime->boot_window_active
        || runtime->runtime == NULL) {
        return;
    }
    runtime->boot_window_advances++;
    if (runtime->boot_window_advances % BOOT_WINDOW_CHECK_INTERVAL != 0) {
        return;
    }
    JS_RunGC(runtime->runtime);
    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    JS_ComputeMemoryUsage(runtime->runtime, &usage);
    if (usage.malloc_size < 0) return;
    if ((size_t) usage.malloc_size > runtime->boot_window_peak) {
        runtime->boot_window_peak = (size_t) usage.malloc_size;
    }
    if (getenv("TILEFINCH_DUMP_JS_MEMORY") != NULL) {
        fprintf(stderr, "tilefinch: boot-window census advance=%llu live=%lld\n",
                (unsigned long long) runtime->boot_window_advances,
                (long long) usage.malloc_size);
        JS_DumpMemoryUsage(stderr, &usage, runtime->runtime);
    }
    /* Return the window only when the live graph fits the base with the
       same headroom reserve the GC pacing keeps below a limit, so the
       shrunken limit does not immediately re-trigger pressure. */
    size_t reserve = runtime->base_memory_limit / 8u;
    if (reserve > 4u * 1024u * 1024u) reserve = 4u * 1024u * 1024u;
    if ((size_t) usage.malloc_size + reserve <= runtime->base_memory_limit) {
        JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
        runtime->boot_window_active = false;
        runtime->boot_window_returned_advance = runtime->boot_window_advances;
        size_t trimmed = budget_quickjs_pool_trim(runtime->quickjs_pool, 8);
        fprintf(stderr,
                "tilefinch: boot-window returned advance=%llu live=%lld "
                "base=%zu window=%zu trimmed=%zu\n",
                (unsigned long long) runtime->boot_window_advances,
                (long long) usage.malloc_size, runtime->base_memory_limit,
                runtime->boot_window_bytes, trimmed);
    }
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
        growth_pct = value == NULL ? 0 : atol(value);
        if (growth_pct < 0) growth_pct = 0;
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
    uint64_t effective_limit = runtime->base_memory_limit
        + (runtime->boot_window_active ? runtime->boot_window_bytes : 0);
    uint64_t threshold;
    if (growth_pct > 0) {
        uint64_t growth = (uint64_t) growth_pct;
        uint64_t grown;
        /* The PSP realm and its configured growth percentage fit this path.
           Avoid a software 64-bit division on every scripted loop turn while
           retaining saturating wide arithmetic for host overrides. */
        if (live <= UINT32_MAX && growth <= UINT32_MAX
            && (growth == 0 || live <= UINT32_MAX / growth)) {
            grown = (uint32_t) live * (uint32_t) growth / 100u;
        } else {
            uint64_t product = 0;
#if defined(__GNUC__) || defined(__clang__)
            grown = __builtin_mul_overflow(live, growth, &product)
                ? UINT64_MAX : product / 100u;
#else
            grown = growth != 0 && live > UINT64_MAX / growth
                ? UINT64_MAX : live * growth / 100u;
#endif
        }
        uint64_t floor = live + (uint64_t) min_slack_kb * 1024u;
        threshold = grown > floor ? grown : floor;
    } else {
        threshold = live + (uint64_t) slack_kb * 1024u;
    }
    if (effective_limit > 0 && threshold >= effective_limit) {
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
    JS_SetGCThreshold(runtime->runtime,
                      threshold > SIZE_MAX ? SIZE_MAX : (size_t) threshold);
}

bool script_runtime_advance(ScriptRuntime *runtime, unsigned elapsed_ms,
                            size_t callback_budget, ScriptResult *result)
{
    if (runtime == NULL || callback_budget > 1024) return false;
    runtime_apply_gc_slack(runtime);
    runtime_apply_deterministic_gc(runtime);
    runtime_boot_window_step(runtime);
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
    bool trace_steps = getenv("TILEFINCH_TRACE_RUNTIME_STEPS") != NULL;
    uint64_t network_elapsed_ns = 0;
    uint64_t timer_elapsed_ns = 0;
    uint64_t microtask_elapsed_ns = 0;

    /* Advance the timer wheel once even when this turn's callback budget is
       zero. No author callback runs at this preparation boundary. */
    uint64_t phase_started = js_rt_monotonic_time_ns();
    size_t prepared_callbacks = 0;
    bool ok = runtime_pump_timers(runtime, elapsed_ms, 0,
                                  &prepared_callbacks);
    timer_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    if (!ok || prepared_callbacks != 0) {
        runtime->result.success = false;
        js_rt_runtime_update_result(runtime, result);
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
        js_rt_runtime_update_result(runtime, result);
        return false;
    }

    bool classic_continuation =
        js_rt_dynamic_classic_continuation_pending(runtime);
    /* Native input may update the viewport many times between event-loop
       turns. The captured bridge emits at most one window/visualViewport
       pair here, after the final position is already observable. A continued
       classic script must remain the only author task until it completes. */
    if (!classic_continuation
        && !runtime_call_captured(runtime, runtime->page_scroll_flush,
                                  "page scroll dispatch", 0, NULL, NULL)) {
        runtime->result.success = false;
        js_rt_runtime_update_result(runtime, result);
        return false;
    }

    /* A task is never followed by another task until its bounded QuickJS job
       checkpoint has run. If the 64-job checkpoint remains non-empty, defer
       all later tasks to the next host turn instead of reordering them. */
    if (!classic_continuation) {
        phase_started = js_rt_monotonic_time_ns();
        ok = js_rt_runtime_run_jobs(runtime);
        microtask_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    }
    size_t remaining = callback_budget;
    size_t callbacks = 0;
    while (ok && remaining != 0
           && (classic_continuation
               || !JS_IsJobPending(runtime->runtime))) {
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
                ok = js_rt_runtime_run_jobs(runtime);
                microtask_elapsed_ns +=
                    js_rt_monotonic_time_ns() - phase_started;
            }
            continue;
        }

        if (classic_continuation) break;
        size_t timer_callbacks = 0;
        phase_started = js_rt_monotonic_time_ns();
        ok = runtime_pump_timers(runtime, 0, 1, &timer_callbacks);
        timer_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
        if (!ok) break;
        if (timer_callbacks == 0) break;
        callbacks += timer_callbacks;
        remaining -= timer_callbacks;
        phase_started = js_rt_monotonic_time_ns();
        ok = js_rt_runtime_run_jobs(runtime);
        microtask_elapsed_ns += js_rt_monotonic_time_ns() - phase_started;
    }
    if (trace_steps) {
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
            if (trace_steps) {
                fprintf(stderr, "runtime-step phase=dom-refresh us=%llu\n",
                        (unsigned long long)
                          ((js_rt_monotonic_time_ns() - phase_started)
                           / UINT64_C(1000)));
            }
        }
    }
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}
