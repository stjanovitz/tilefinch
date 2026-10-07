/* Diagnostic (TILEFINCH_JS_TRAP_ALLOC_SIZE): print the JS interpreter
   stack when the allocator trap fires, so giant allocations can be
   attributed to page code.  Reenters the VM from inside an allocation;
   host-only, one-shot, best-effort. */
#ifdef TILEFINCH_NATIVE_TIER_LAB
static uint64_t js_rt_lab_thread_cpu_ns(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec value;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return (uint64_t)value.tv_sec * UINT64_C(1000000000)
            + (uint64_t)value.tv_nsec;
#endif
    return 0;
}

static uint64_t js_rt_lab_realtime_us(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000)
        + (uint64_t)value.tv_nsec / UINT64_C(1000);
}
#endif

#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
static void js_rt_dump_interpreter_stack(void *opaque)
{
    ScriptRuntime *runtime = opaque;
    if (runtime == NULL || runtime->context == NULL) return;
    JSContext *ctx = runtime->context;
    JS_ThrowInternalError(ctx, "tilefinch alloc trap");
    JSValue error = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
    const char *text = JS_ToCString(ctx, stack);
    fprintf(stderr, "js-alloc-trap-js-stack:\n%s\n",
            text == NULL ? "<unavailable>" : text);
    if (text != NULL) JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, error);
}
#endif

void js_rt_runtime_arm_watchdog(ScriptRuntime *runtime)
{
    /* Every entry into JavaScript starts here. Computed styles retained
       across entries are keyed on the host style generation instead
       (document_style_generation()), which the parser and native DOM
       writes between entries advance. */
    /* References dropped by the previous entry may be collectable now: let
       its first handle-table exhaustion collect again. */
    runtime->bridge.node_reclaim_gc_skip = 0;
    watchdog_finish_slice(&runtime->watchdog);
    runtime->watchdog.started_ms = monotonic_time_ms();
    runtime->watchdog.deadline_ms = runtime->timeout_ms
            > UINT64_MAX - runtime->watchdog.started_ms
        ? UINT64_MAX
        : runtime->watchdog.started_ms + runtime->timeout_ms;
    if (runtime->execution_deadline_cap_ms != 0u
        && runtime->watchdog.deadline_ms
               > runtime->execution_deadline_cap_ms) {
        runtime->watchdog.deadline_ms =
            runtime->execution_deadline_cap_ms;
    }
#if defined(CONFIG_EXECUTION_PROFILE)
    runtime->watchdog.next_profile_ms = runtime->watchdog.started_ms + 1000u;
#endif
    runtime->watchdog.polls = 0;
    runtime->watchdog.interrupted = false;
    runtime->watchdog.slice_started_ns = js_rt_monotonic_time_ns();
    runtime->watchdog.slice_work_units = 0;
    script_profile_enter(runtime);
}

void script_runtime_limit_execution_for_us(ScriptRuntime *runtime,
                                           uint64_t remaining_us)
{
    if (runtime == NULL) return;
    /* Round the absolute deadline up, not the start down. A whole-ms start
       (up to 0.999 ms early) let the watchdog stop the script before the
       caller's microsecond deadline, so the parser stage accounted just
       under its limit and left its time breaker closed. */
    uint64_t now_us = js_rt_monotonic_time_ns() / UINT64_C(1000);
    /* Zero is the public sentinel for "no cap", so an exhausted allowance
       is represented by the current nonzero monotonic instant. */
    if (remaining_us > UINT64_MAX - UINT64_C(999) - now_us) {
        runtime->execution_deadline_cap_ms = UINT64_MAX;
    } else {
        runtime->execution_deadline_cap_ms =
            (now_us + remaining_us + UINT64_C(999)) / UINT64_C(1000);
        if (runtime->execution_deadline_cap_ms == 0u) {
            runtime->execution_deadline_cap_ms = 1u;
        }
    }
}

void script_runtime_clear_execution_limit(ScriptRuntime *runtime)
{
    if (runtime != NULL) runtime->execution_deadline_cap_ms = 0u;
}

static bool runtime_job_exception_is_fatal(const ScriptRuntime *runtime,
                                           size_t failures_before,
                                           size_t heap_rejections_before)
{
    if (runtime == NULL) return true;
    /* Error text is author-controlled and result.interrupted describes the
       previous completed slice until js_rt_runtime_update_result() runs.  Only a
       failure observed while executing this job can make it fatal. A QuickJS
       pool refusal is separate from the outer Budget counter. Pair it with the
       propagated exception here: bounded allocation fallbacks that return
       successfully remain nonfatal, while an uncaught inner-heap exhaustion
       cannot be misreported as an ordinary author exception. */
    return runtime->watchdog.interrupted
        || runtime->budget->failure_count != failures_before
        || script_runtime_heap_rejections(runtime) != heap_rejections_before;
}

static void runtime_record_author_job_exception(ScriptRuntime *runtime)
{
    js_rt_saturating_add_size(&runtime->result.uncaught_callback_errors, 1);
    snprintf(runtime->result.last_uncaught_callback_error,
             sizeof(runtime->result.last_uncaught_callback_error),
             "microtask: %s", runtime->result.error[0] == '\0'
                 ? "JavaScript exception" : runtime->result.error);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void runtime_log_reentrant_checkpoint(ScriptRuntime *runtime)
{
    static unsigned logged;
    if (logged >= 6u) return;
    logged++;
    char message[400];
    int length = snprintf(message, sizeof(message),
                          "tilefinch-reentrant-checkpoint: skipped=%zu stack=",
                          runtime->reentrant_checkpoints_skipped);
    for (int level = 0; level < 16 && length > 0
         && (size_t) length < sizeof(message) - 2u; level++) {
        JSAtom atom = JS_GetScriptOrModuleName(runtime->context, level);
        if (atom == JS_ATOM_NULL) continue;
        const char *name = JS_AtomToCString(runtime->context, atom);
        JS_FreeAtom(runtime->context, atom);
        if (name == NULL) break;
        const char *base = strrchr(name, '/');
        length += snprintf(message + length, sizeof(message) - (size_t) length,
                           "%s<", base == NULL ? name : base + 1);
        JS_FreeCString(runtime->context, name);
    }
    tilefinch_platform_log_message(message);
}
#endif

#ifdef TILEFINCH_NATIVE_TIER_LAB
#include "tilefinch/native_tier_lab.h"
#endif
static bool runtime_run_jobs_checkpoint(ScriptRuntime *runtime);

bool js_rt_runtime_run_jobs(ScriptRuntime *runtime)
{
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    js_rt_validation_cpu_enter(runtime, SCRIPT_RUNTIME_CPU_JOBS);
#endif
    unsigned ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
    unsigned split = script_split_enter(SCRIPT_SPLIT_JS);
    bool ok = runtime_run_jobs_checkpoint(runtime);
    script_split_leave(split);
    work_ledger_leave(ledger);
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    js_rt_validation_cpu_leave(runtime, SCRIPT_RUNTIME_CPU_JOBS);
#endif
    return ok;
}

static bool runtime_run_jobs_checkpoint(ScriptRuntime *runtime)
{
    /* HTML performs a microtask checkpoint only once the JavaScript
       execution context stack is empty. A host path reached from script
       (a synchronous layout callback, a nested dispatch) leaves queued jobs
       for the outer checkpoint: running them here executed chatgpt.com's
       deferred signal writes inside its own render. */
    if (JS_IsStackActive(runtime->runtime)) {
        js_rt_saturating_add_size(&runtime->reentrant_checkpoints_skipped, 1);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!runtime->checkpoint_trace_suppressed)
            runtime_log_reentrant_checkpoint(runtime);
#endif
        return true;
    }
    /*
     * A full browser can usually drain a microtask checkpoint before the
     * next task.  On the PSP, a long promise chain can otherwise monopolize
     * the only thread long enough that its own timeout never gets a turn.
     * Bound each checkpoint; the next runtime tick continues the queue.
     */
    size_t work = 0;
    const bool trace_steps =
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        !runtime->checkpoint_trace_suppressed;
#else
        tilefinch_trace_runtime_steps();
#endif
    uint64_t checkpoint_begin_us = trace_steps
        ? tilefinch_platform_monotonic_time_us() : 0;
#ifdef TILEFINCH_SCRIPT_SPLIT
    /* Where the checkpoint's and its longest job's time went
       (script_split.h), for the record below. */
    ScriptSplitTotals split_begin = {0}, split_longest = {0};
    bool split_on = trace_steps && script_split_enabled();
    if (split_on) script_split_snapshot(&split_begin);
#endif
    size_t gc_collections_before = runtime->heap_collections;
    size_t promise_jobs = 0, continuations = 0;
    uint64_t promise_job_us = 0, promise_job_max_us = 0;
    uint64_t continuation_us = 0, continuation_max_us = 0;
    uint64_t recovery_us = 0;
    while (work < 64) {
        if (!JS_IsJobPending(runtime->runtime)) {
            if (runtime->checkpoint_continuation_count == 0u) break;
            size_t slot = runtime->checkpoint_continuation_head;
            JSValue callback = runtime->checkpoint_continuations[slot];
            runtime->checkpoint_continuations[slot] = JS_UNDEFINED;
            runtime->checkpoint_continuation_head = (uint8_t) (
                (slot + 1u) % SCRIPT_CHECKPOINT_CONTINUATION_LIMIT);
            runtime->checkpoint_continuation_count--;
            size_t failures_before = runtime->budget->failure_count;
            size_t heap_rejections_before =
                script_runtime_heap_rejections(runtime);
            BudgetAllocationOwner previous_owner =
                document_allocation_owner_enter(runtime->document);
            uint64_t continuation_started_ns = trace_steps
                ? js_rt_monotonic_time_ns() : 0;
            script_profile_enter(runtime);
            if (runtime->profile != NULL) runtime->profile->phase = 2;
            JSValue value = JS_Call(
                runtime->context, callback, JS_UNDEFINED, 0, NULL);
            script_profile_leave(runtime);
            if (runtime->profile != NULL) runtime->profile->phase = 0;
            if (trace_steps) {
                uint64_t elapsed_us =
                    (js_rt_monotonic_time_ns() - continuation_started_ns)
                    / UINT64_C(1000);
                continuations++;
                continuation_us += elapsed_us;
                if (elapsed_us > continuation_max_us)
                    continuation_max_us = elapsed_us;
            }
            JS_FreeValue(runtime->context, callback);
            document_allocation_owner_leave(
                runtime->document, previous_owner);
            if (JS_IsException(value)) {
                JS_FreeValue(runtime->context, value);
                js_rt_record_exception(runtime->context, &runtime->result);
                bool fatal = runtime_job_exception_is_fatal(
                    runtime, failures_before, heap_rejections_before);
                uint64_t recovery_started_ns = trace_steps
                    ? js_rt_monotonic_time_ns() : 0;
                js_lazy_webpack_recover_failure(runtime);
                if (trace_steps) {
                    recovery_us +=
                        (js_rt_monotonic_time_ns() - recovery_started_ns)
                        / UINT64_C(1000);
                }
                if (fatal) return false;
                /* Continuations are the remainder of a browser-dispatched
                   callback checkpoint.  An ordinary author exception has the
                   same containment boundary as a rejected promise job: report
                   it, then keep the realm usable for later tasks. */
                runtime_record_author_job_exception(runtime);
                work++;
                continue;
            }
            JS_FreeValue(runtime->context, value);
            uint64_t recovery_started_ns = trace_steps
                ? js_rt_monotonic_time_ns() : 0;
            js_lazy_webpack_recover_failure(runtime);
            if (trace_steps) {
                recovery_us +=
                    (js_rt_monotonic_time_ns() - recovery_started_ns)
                    / UINT64_C(1000);
            }
            work++;
            continue;
        }
        /* Short promise jobs may never hit the VM instruction interrupt.
           Keep their bounded drain cooperative too, without a new task or
           resetting the existing execution deadline. */
        if (!js_rt_runtime_native_checkpoint(runtime)) return false;
        JSContext *job_context = NULL;
        uint64_t started_ns = js_rt_monotonic_time_ns();
#ifdef TILEFINCH_NATIVE_TIER_LAB
        uint64_t tier_epoch_before = 0, tier_calls_before = 0,
                 tier_completed_before = 0;
        uint64_t tier_lazy_before_ns = 0;
        uint64_t tier_cpu_before_ns = trace_steps
            ? js_rt_lab_thread_cpu_ns() : 0;
        if (trace_steps) tilefinch_native_tier_job_begin();
        if (trace_steps)
            tilefinch_native_tier_snapshot(runtime->runtime,
                &tier_epoch_before, &tier_calls_before, &tier_completed_before);
        if (trace_steps)
            tier_lazy_before_ns = tilefinch_native_tier_lazy_compile_ns();
#endif
        size_t polls_before = runtime->watchdog.polls;
        size_t failures_before = runtime->budget->failure_count;
        size_t heap_rejections_before =
            script_runtime_heap_rejections(runtime);
        BudgetAllocationOwner previous_owner =
            document_allocation_owner_enter(runtime->document);
#ifdef TILEFINCH_SCRIPT_SPLIT
        ScriptSplitTotals split_job_before = {0};
        if (split_on) script_split_snapshot(&split_job_before);
#endif
        script_profile_enter(runtime);
        ScriptProfileJobSnapshot job_profile = script_profile_job_snapshot(runtime);
        if (runtime->profile != NULL) {
            runtime->profile->phase = 1;
            runtime->profile->job_begin_ns = started_ns;
        }
#ifdef TILEFINCH_NATIVE_TIER_LAB
        uint64_t execute_started_ns = trace_steps
            ? js_rt_monotonic_time_ns() : 0;
        uint64_t execute_started_cpu_ns = trace_steps
            ? js_rt_lab_thread_cpu_ns() : 0;
#endif
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
        uint32_t census_job = script_census_job_begin(started_ns / UINT64_C(1000));
#endif
        int executed = JS_ExecutePendingJob(runtime->runtime, &job_context);
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
        uint64_t census_end_us = census_job
            ? js_rt_monotonic_time_ns() / UINT64_C(1000) : 0;
#endif
#ifdef TILEFINCH_NATIVE_TIER_LAB
        uint64_t execute_elapsed_ns = trace_steps
            ? js_rt_monotonic_time_ns() - execute_started_ns : 0;
        uint64_t execute_cpu_ns = trace_steps && execute_started_cpu_ns != 0
            ? js_rt_lab_thread_cpu_ns() - execute_started_cpu_ns : 0;
#endif
        script_profile_leave(runtime);
        script_profile_job_finish(runtime, job_profile, started_ns);
        if (runtime->profile != NULL) runtime->profile->phase = 0;
        document_allocation_owner_leave(runtime->document, previous_owner);
        js_rt_runtime_record_host_callback(
            runtime, "<promise-job>", started_ns, polls_before);
        if (trace_steps) {
            uint64_t elapsed_us =
                (js_rt_monotonic_time_ns() - started_ns) / UINT64_C(1000);
#ifdef TILEFINCH_NATIVE_TIER_LAB
            if (elapsed_us >= 5000u) {
                uint64_t epoch_after = 0, calls_after = 0,
                         completed_after = 0;
                tilefinch_native_tier_snapshot(runtime->runtime,
                    &epoch_after, &calls_after, &completed_after);
                bool same_owner = tier_epoch_before == 0 ||
                                  tier_epoch_before == epoch_after;
                fprintf(stderr,
                    "tilefinch-native-tier-job wall-us=%llu cpu-us=%llu native-calls=%llu "
                    "native-instructions=%llu lazy-compile-us=%llu reset=%u begin-us=%llu "
                    "execute-wall-us=%llu execute-cpu-us=%llu realtime-end-us=%llu\n",
                    (unsigned long long)elapsed_us,
                    (unsigned long long)(tier_cpu_before_ns == 0 ? 0 :
                        (js_rt_lab_thread_cpu_ns() - tier_cpu_before_ns)
                            / UINT64_C(1000)),
                    (unsigned long long)(same_owner ? calls_after - tier_calls_before : 0),
                    (unsigned long long)(same_owner ? completed_after - tier_completed_before : 0),
                    (unsigned long long)((tilefinch_native_tier_lazy_compile_ns()
                        - tier_lazy_before_ns) / UINT64_C(1000)),
                    same_owner ? 0u : 1u,
                    (unsigned long long)(started_ns / UINT64_C(1000)),
                    (unsigned long long)(execute_elapsed_ns / UINT64_C(1000)),
                    (unsigned long long)(execute_cpu_ns / UINT64_C(1000)),
                    (unsigned long long)js_rt_lab_realtime_us());
                tilefinch_native_tier_job_report(runtime->runtime, elapsed_us);
            }
#endif
            promise_jobs++;
            promise_job_us += elapsed_us;
#ifdef TILEFINCH_SCRIPT_SPLIT
            if (split_on && elapsed_us > promise_job_max_us) {
                ScriptSplitTotals split_job_after;
                script_split_snapshot(&split_job_after);
                script_split_difference(&split_job_after, &split_job_before,
                                        &split_longest);
            }
#endif
            if (elapsed_us > promise_job_max_us)
                promise_job_max_us = elapsed_us;
        }
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
        if (census_job) {
            /* Emit after the exact job timers close. Diagnostic output is
               host bookkeeping, not execution of the measured callback. */
            unsigned census_split = script_split_enter(SCRIPT_SPLIT_HOST);
            script_census_job_end(census_job, census_end_us);
            script_split_leave(census_split);
        }
#endif
        if (executed < 0) {
            js_rt_record_exception(job_context == NULL ? runtime->context
                                                 : job_context,
                             &runtime->result);
            bool fatal = runtime_job_exception_is_fatal(
                runtime, failures_before, heap_rejections_before);
            uint64_t recovery_started_ns = trace_steps
                ? js_rt_monotonic_time_ns() : 0;
            js_lazy_webpack_recover_failure(runtime);
            if (trace_steps) {
                recovery_us +=
                    (js_rt_monotonic_time_ns() - recovery_started_ns)
                    / UINT64_C(1000);
            }
            if (fatal) return false;
            /* A browser reports an exception thrown by an author microtask,
               then continues the checkpoint. It does not turn an optional
               resource event into a failed navigation. */
            runtime_record_author_job_exception(runtime);
        }
        work++;
    }
    if (runtime->heap_collections - gc_collections_before
            > runtime->gc_checkpoint_collections_max)
        runtime->gc_checkpoint_collections_max =
            runtime->heap_collections - gc_collections_before;
    /* Task boundary: a WebAssembly runtime left idle by this task's
       operations is released here (see wasm_runtime_leave). */
    uint64_t wasm_release_started_ns = trace_steps
        ? js_rt_monotonic_time_ns() : 0;
    js_wasm_release_idle_runtime();
    if (trace_steps && (promise_jobs != 0u || continuations != 0u)) {
        uint64_t wasm_release_us =
            (js_rt_monotonic_time_ns() - wasm_release_started_ns)
            / UINT64_C(1000);
        uint64_t checkpoint_end_us = tilefinch_platform_monotonic_time_us();
        /* Keep short device checkpoints silent; wall endpoints let the
           analyzer assign costly ones to their containing event or advance.
           Sum/max distinguish many cheap jobs from one indivisible job. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (checkpoint_end_us - checkpoint_begin_us >= 1000u)
#endif
        {
            char message[320 + 2u * (SCRIPT_SPLIT_FORMAT_BYTES + 16u)];
            int written = snprintf(message, 320,
                "runtime-checkpoint promise=%zu/%llu/%llu "
                "continuation=%zu/%llu/%llu recovery=%llu wasm-release=%llu "
                "begin-us=%llu end-us=%llu pending=%d",
                promise_jobs, (unsigned long long) promise_job_us,
                (unsigned long long) promise_job_max_us,
                continuations, (unsigned long long) continuation_us,
                (unsigned long long) continuation_max_us,
                (unsigned long long) recovery_us,
                (unsigned long long) wasm_release_us,
                (unsigned long long) checkpoint_begin_us,
                (unsigned long long) checkpoint_end_us,
                JS_IsJobPending(runtime->runtime)
                    || runtime->checkpoint_continuation_count != 0u);
#ifdef TILEFINCH_SCRIPT_SPLIT
            if (split_on && written > 0 && written < 320) {
                ScriptSplitTotals split_end, split_all;
                char all[SCRIPT_SPLIT_FORMAT_BYTES];
                char longest[SCRIPT_SPLIT_FORMAT_BYTES];
                script_split_snapshot(&split_end);
                script_split_difference(&split_end, &split_begin, &split_all);
                (void) script_split_format(&split_all, all, sizeof(all));
                (void) script_split_format(&split_longest, longest,
                                           sizeof(longest));
                snprintf(message + written, sizeof(message) - (size_t) written,
                         " split={%s} longest-split={%s}", all, longest);
            }
#else
            (void) written;
#endif
            tilefinch_platform_log_message(message);
        }
    }
    return true;
}

bool js_rt_runtime_checkpoint_pending(const ScriptRuntime *runtime)
{
    return runtime != NULL
        && (JS_IsJobPending(runtime->runtime)
            || runtime->checkpoint_continuation_count != 0u);
}

bool script_runtime_has_pending_startup_scripts(ScriptRuntime *runtime)
{
    return runtime != NULL
        && (runtime->bridge.dynamic_script_count != 0u
            || js_rt_runtime_checkpoint_pending(runtime)
            || js_rt_dynamic_classic_continuation_pending(runtime));
}

bool script_runtime_message_dispatch_ready(ScriptRuntime *runtime)
{
    return runtime != NULL
        && !js_rt_runtime_checkpoint_pending(runtime)
        && !js_rt_dynamic_classic_continuation_pending(runtime);
}

bool script_runtime_has_pending_author_work(ScriptRuntime *runtime)
{
    /* Do not use result.pending_tasks here. That diagnostic also counts an
       open EventSource, WebSocket, or multiplayer channel, none of which has
       a finite quiescent state. A timer is deliberately conservative: it may
       be the page's bounded delayed hydration root. */
    return runtime != NULL
        && (runtime->pending_timer_tasks != 0u
            || runtime->pending_network_tasks != 0u
            || runtime->bridge.async_fetch_count != 0u
            || script_runtime_has_pending_startup_scripts(runtime)
            || runtime->page_visibility_queue_count != 0u);
}


static uint64_t callback_observation_threshold(const ScriptRuntime *runtime)
{
    return runtime == NULL
           || runtime->bridge.execution_policy.slow_callback_threshold_us == 0
        ? SCRIPT_DEFAULT_SLOW_DEPENDENCY_US
        : runtime->bridge.execution_policy.slow_callback_threshold_us;
}

static const char *const runtime_host_callback_names[
    SCRIPT_HOST_CALLBACK_COUNT] = {
    "__tilefinchRefreshNamedProperties",
    "__tilefinchParserMutationCheckpoint",
    "__tilefinchReceiveMessage",
    "__tilefinchSetFrameWindowState",
    "__tilefinchIntersectionRecheck",
    "__tilefinchMediaRecheck",
    "__tilefinchRecordResourceTiming",
    "__tilefinchRecordNavigationTiming",
    "__tilefinchDeliverNetwork",
    "__tilefinchDetachNetwork",
    "__tilefinchPumpTimers",
    "__tilefinchSchedulerSnapshot",
    "__tilefinchRebindDocument",
    "__tilefinchCommitSameDocument",
    "__tilefinchRestoreSameDocument",
    "__tilefinchSaveSectionState",
    "__tilefinchRestoreSectionState",
    "__tilefinchUpdateGamepad"
};

static bool runtime_capture_host_callbacks(ScriptRuntime *runtime,
                                           JSValueConst global)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    for (size_t i = 0; i < SCRIPT_HOST_CALLBACK_COUNT; i++) {
        JSValue function = JS_GetPropertyStr(
            runtime->context, global, runtime_host_callback_names[i]);
        if (!JS_IsFunction(runtime->context, function)) {
            JS_FreeValue(runtime->context, function);
            return false;
        }
        runtime->host_callbacks[i] = function;
    }
    runtime->host_global = JS_DupValue(runtime->context, global);
    return true;
}

void js_rt_runtime_record_host_callback(ScriptRuntime *runtime,
                                        const char *name,
                                        uint64_t started_ns,
                                        size_t polls_before)
{
    if (runtime == NULL) return;
    uint64_t finished_ns = js_rt_monotonic_time_ns();
    uint64_t elapsed_us = finished_ns >= started_ns
        ? (finished_ns - started_ns) / UINT64_C(1000) : 0;
    js_rt_saturating_add_size(&runtime->result.host_callback_calls, 1);
    js_rt_saturating_add_u64(&runtime->result.host_callback_total_us, elapsed_us);
    if (runtime->watchdog.polls != polls_before) {
        js_rt_saturating_add_size(
            &runtime->result.host_callback_calls_with_interrupt_polls, 1);
        return;
    }
    if (elapsed_us > runtime->result.max_nonpreemptible_callback_us) {
        runtime->result.max_nonpreemptible_callback_us = elapsed_us;
        snprintf(runtime->result.max_nonpreemptible_callback_name,
                 sizeof(runtime->result.max_nonpreemptible_callback_name),
                 "%s", name == NULL ? "<callback>" : name);
    }
    if (elapsed_us > callback_observation_threshold(runtime)) {
        js_rt_saturating_add_size(
            &runtime->result.nonpreemptible_callback_count, 1);
    }
}

static bool runtime_call_host(ScriptRuntime *runtime,
                              ScriptHostCallback callback,
                              int argc, JSValueConst *argv,
                              JSValue *returned)
{
    if (runtime == NULL || callback < 0
        || callback >= SCRIPT_HOST_CALLBACK_COUNT) return false;
    const char *name = runtime_host_callback_names[callback];
    BudgetAllocationOwner previous_owner =
        document_allocation_owner_enter(runtime->document);
    JSValueConst function = runtime->host_callbacks[callback];
    JSValue value;
    if (JS_IsFunction(runtime->context, function)) {
        uint64_t started_ns = js_rt_monotonic_time_ns();
        size_t polls_before = runtime->watchdog.polls;
        /* The timer pump's caller knows whether it runs a task; a
           scheduler snapshot is bookkeeping. Everything else runs script. */
        unsigned ledger = callback == SCRIPT_HOST_PUMP_TIMERS
                || callback == SCRIPT_HOST_SCHEDULER_SNAPSHOT
            ? WORK_LEDGER_KIND_COUNT : work_ledger_enter(WORK_LEDGER_SCRIPT);
        unsigned split = ledger == WORK_LEDGER_KIND_COUNT
            ? SCRIPT_SPLIT_KIND_COUNT + 1u : script_split_enter(SCRIPT_SPLIT_JS);
        script_profile_enter(runtime);
        value = JS_Call(runtime->context, function, runtime->host_global,
                        argc, argv);
        script_profile_leave(runtime);
        script_split_leave(split);
        work_ledger_leave(ledger);
        js_rt_runtime_record_host_callback(runtime, name, started_ns,
                                     polls_before);
    } else {
        value = JS_ThrowTypeError(runtime->context, "%s is not callable",
                                  name);
    }
    if (JS_IsException(value)) {
        JS_FreeValue(runtime->context, value);
        js_rt_record_exception(runtime->context, &runtime->result);
        js_lazy_webpack_recover_failure(runtime);
        document_allocation_owner_leave(runtime->document, previous_owner);
        return false;
    }
    if (returned != NULL) *returned = value;
    else JS_FreeValue(runtime->context, value);
    js_lazy_webpack_recover_failure(runtime);
    document_allocation_owner_leave(runtime->document, previous_owner);
    return true;
}

static bool gamepad_state_equal(const TilefinchGamepadState *left,
                                const TilefinchGamepadState *right)
{
    if (left == NULL || right == NULL
        || left->connected != right->connected
        || left->buttons != right->buttons
        || left->timestamp_ms != right->timestamp_ms) return false;
    for (size_t i = 0; i < TILEFINCH_GAMEPAD_AXIS_COUNT; i++) {
        if (left->axes[i] != right->axes[i]) return false;
    }
    return true;
}

bool script_runtime_set_gamepad_state(
    ScriptRuntime *runtime, const TilefinchGamepadState *state)
{
    if (runtime == NULL || runtime->context == NULL || state == NULL)
        return false;
    if (runtime->gamepad_state_valid
        && gamepad_state_equal(&runtime->gamepad_state, state)) return true;

    JSValue arguments[5] = {
        JS_NewBool(runtime->context, state->connected),
        JS_NewUint32(runtime->context, state->buttons),
        JS_NewInt32(runtime->context, state->axes[0]),
        JS_NewInt32(runtime->context, state->axes[1]),
        JS_NewFloat64(runtime->context, (double) state->timestamp_ms)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_UPDATE_GAMEPAD,
                                5, arguments, NULL);
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); i++)
        JS_FreeValue(runtime->context, arguments[i]);
    if (!ok) return false;
    runtime->gamepad_state = *state;
    runtime->gamepad_state_valid = true;
    return true;
}

bool script_runtime_refresh_named_properties(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    return runtime_call_host(runtime, SCRIPT_HOST_REFRESH_NAMED_PROPERTIES,
                        0, NULL, NULL);
}

static bool runtime_parser_insertion_append(ScriptRuntime *runtime,
                                            JSValue batch, uint32_t *at,
                                            lxb_dom_node_t *const nodes[4])
{
    for (size_t k = 0; k < 4; k++) {
        int64_t handle = nodes[k] == NULL ? 0
            : js_rt_bridge_register_node(&runtime->bridge, nodes[k]);
        if (JS_SetPropertyUint32(runtime->context, batch, (*at)++,
                                 JS_NewInt64(runtime->context, handle)) < 0)
            return false;
    }
    return true;
}

/* Flatten the parser-insertion journal into [parent, node, previous, next]
   handle quadruples (0 for an absent or unregistrable node) and empty it.
   Each coalesced root {parent, first} follows as one quadruple per child of
   its run while the expansion stays within the journal limit; past that, as
   the parent reported as added (the document's element for the document),
   which a subtree scan still covers. */
static JSValue runtime_parser_insertion_batch(ScriptRuntime *runtime,
                                              PocDocument *document)
{
    JSContext *context = runtime->context;
    DocumentParserInsertionJournal *journal = &document->parser_insertions;
    JSValue batch = JS_NewArray(context);
    if (JS_IsException(batch)) return batch;
    uint32_t at = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < journal->count; i++) {
        const DocumentParserInsertion *entry = &journal->entries[i];
        lxb_dom_node_t *const nodes[4] = {
            entry->parent, entry->node, entry->previous, entry->next
        };
        ok = runtime_parser_insertion_append(runtime, batch, &at, nodes);
    }
    size_t expansion = DOCUMENT_PARSER_INSERTION_LIMIT;
    for (size_t i = 0; ok && i < journal->root_count; i++) {
        lxb_dom_node_t *parent = journal->roots[i].parent;
        lxb_dom_node_t *first = journal->roots[i].first;
        size_t run = 0;
        for (lxb_dom_node_t *child = first;
             child != NULL && child->parent == parent && run <= expansion;
             child = child->next) run++;
        if (first != NULL && first->parent == parent && run <= expansion) {
            expansion -= run;
            for (lxb_dom_node_t *child = first; ok && run-- > 0;
                 child = child->next) {
                lxb_dom_node_t *const nodes[4] = {
                    parent, child, child->prev, child->next
                };
                ok = runtime_parser_insertion_append(
                    runtime, batch, &at, nodes);
            }
            continue;
        }
        lxb_dom_node_t *added = parent;
        if (parent != NULL && parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            added = lxb_dom_interface_node(
                lxb_dom_interface_document(parent)->element);
        }
        lxb_dom_node_t *const nodes[4] = { parent, added, NULL, NULL };
        ok = runtime_parser_insertion_append(runtime, batch, &at, nodes);
    }
    document_parser_insertions_clear(document);
    if (!ok || JS_SetPropertyStr(context, batch, "coalesced",
                                 JS_NewInt64(context,
                                             (int64_t) journal->coalesced))
                   < 0) {
        JS_FreeValue(context, batch);
        return JS_EXCEPTION;
    }
    return batch;
}

bool script_runtime_parser_mutation_checkpoint(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    /* With nothing journaled (no observer armed it, or the parser inserted
       nothing) the checkpoint is only the microtask checkpoint. */
    if (document_parser_insertions_pending(runtime->document)) {
        BudgetAllocationOwner previous_owner =
            document_allocation_owner_enter(runtime->document);
        JSValue batch = runtime_parser_insertion_batch(
            runtime, runtime->document);
        document_allocation_owner_leave(runtime->document, previous_owner);
        if (JS_IsException(batch)) {
            js_rt_record_exception(runtime->context, &runtime->result);
            return false;
        }
        bool delivered = runtime_call_host(
            runtime, SCRIPT_HOST_PARSER_MUTATION_CHECKPOINT, 1,
            (JSValueConst *) &batch, NULL);
        JS_FreeValue(runtime->context, batch);
        if (!delivered) return false;
    }
    return js_rt_runtime_run_jobs(runtime);
}

static bool runtime_call_captured(ScriptRuntime *runtime,
                                  JSValueConst function,
                                  const char *diagnostic_name,
                                  int argc, JSValueConst *argv,
                                  JSValue *returned)
{
    if (runtime == NULL || runtime->context == NULL
        || !JS_IsFunction(runtime->context, function)) return false;
    BudgetAllocationOwner previous_owner =
        document_allocation_owner_enter(runtime->document);
    JSValue global = JS_GetGlobalObject(runtime->context);
    uint64_t started_ns = js_rt_monotonic_time_ns();
    size_t polls_before = runtime->watchdog.polls;
    unsigned ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
    unsigned split = script_split_enter(SCRIPT_SPLIT_JS);
    script_profile_enter(runtime);
    JSValue value = JS_Call(runtime->context, function, global, argc, argv);
    script_profile_leave(runtime);
    script_split_leave(split);
    work_ledger_leave(ledger);
    js_rt_runtime_record_host_callback(runtime, diagnostic_name, started_ns,
                                 polls_before);
    JS_FreeValue(runtime->context, global);
    if (JS_IsException(value)) {
        JS_FreeValue(runtime->context, value);
        js_rt_record_exception(runtime->context, &runtime->result);
        js_lazy_webpack_recover_failure(runtime);
        document_allocation_owner_leave(runtime->document, previous_owner);
        return false;
    }
    if (returned != NULL) *returned = value;
    else JS_FreeValue(runtime->context, value);
    js_lazy_webpack_recover_failure(runtime);
    document_allocation_owner_leave(runtime->document, previous_owner);
    return true;
}

bool script_runtime_configure_messaging(
    ScriptRuntime *runtime, bool child_context, bool parent_same_origin,
    bool child_opaque_origin, bool top_is_parent,
    ScriptPostMessageCallback callback, void *opaque)
{
    if (runtime == NULL || callback == NULL) return false;
    if (!child_context) {
        /* Once a realm has exposed child WindowProxy globals, treating it as
           a top-level realm again would reopen top-level-only host bindings. */
        if (runtime->document_scope
            == SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME) return false;
        runtime->bridge.post_message = callback;
        runtime->bridge.post_message_opaque = opaque;
        return true;
    }

    /* The scope transition is deliberately irreversible, including when the
       fallible JavaScript setup below fails. Clear both host surfaces before
       child globals become observable; a failed setup must never leave the
       new callback opaque callable by partially-installed author objects. */
    runtime->bridge.post_message = NULL;
    runtime->bridge.post_message_opaque = NULL;
    runtime->bridge.opaque_origin = runtime->bridge.opaque_origin
                                    || child_opaque_origin;
    (void) script_runtime_rebind_remote_document(runtime, NULL);
    runtime->document_scope = SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME;
    static const char same_origin_child_setup[] =
        "(()=>{globalThis.__tilefinchTaskRealm='child';const topIsParent=!!globalThis.__tilefinchTopIsParent;delete globalThis.__tilefinchTopIsParent;"
        "const makeAncestor=(send,parentOf,topOf)=>{let windowProxy;const target={};Object.defineProperties(target,{"
        "postMessage:{enumerable:true,configurable:true,writable:true,value:send},"
        "window:{enumerable:true,configurable:true,get:()=>windowProxy},self:{enumerable:true,configurable:true,get:()=>windowProxy},"
        "frames:{enumerable:true,configurable:true,get:()=>windowProxy},parent:{enumerable:true,configurable:true,get:parentOf},"
        "top:{enumerable:true,configurable:true,get:topOf},closed:{enumerable:true,configurable:true,get:()=>false},"
        "length:{enumerable:true,configurable:true,get:()=>0},opener:{enumerable:true,configurable:true,get:()=>null},"
        "focus:{enumerable:true,configurable:true,writable:true,value:function focus(){}},"
        "blur:{enumerable:true,configurable:true,writable:true,value:function blur(){}},"
        "close:{enumerable:true,configurable:true,writable:true,value:function close(){}}});"
        "Object.defineProperty(target,Symbol.toStringTag,{configurable:true,value:'Window'});windowProxy=target;return target};"
        "const parentSend=function postMessage(data,targetOrigin='/'){"
        "if(data&&data.event==='reject')globalThis.__tilefinchLastRejectStack=String(new Error('Turnstile reject').stack||'');"
        "__tilefinchPostParentMessage(data,targetOrigin)};let parentWindow,topWindow;"
        "if(topIsParent){parentWindow=makeAncestor(parentSend,()=>parentWindow,()=>parentWindow);topWindow=parentWindow}"
        "else{topWindow=makeAncestor(function postMessage(data,targetOrigin='/'){__tilefinchPostTopMessage(data,targetOrigin)},()=>topWindow,()=>topWindow);"
        "parentWindow=makeAncestor(parentSend,()=>topWindow,()=>topWindow)}globalThis.parent=parentWindow;globalThis.top=topWindow;Object.defineProperty(globalThis,'opener',"
        "{value:null,writable:false,configurable:false});})();";
    static const char cross_origin_child_setup[] =
        "(()=>{globalThis.__tilefinchTaskRealm='child';const topIsParent=!!globalThis.__tilefinchTopIsParent;delete globalThis.__tilefinchTopIsParent;"
        "const makeAncestor=(send,parentOf,topOf)=>{let windowProxy;const target={};const denied=()=>{throw new DOMException("
        "'Blocked cross-origin Window access','SecurityError')};Object.defineProperties(target,{"
        "postMessage:{enumerable:true,configurable:false,get:()=>send,set:denied},"
        "window:{enumerable:true,configurable:false,get:()=>windowProxy},self:{enumerable:true,configurable:false,get:()=>windowProxy},"
        "frames:{enumerable:true,configurable:false,get:()=>windowProxy},parent:{enumerable:true,configurable:false,get:parentOf},"
        "top:{enumerable:true,configurable:false,get:topOf},closed:{enumerable:true,configurable:false,get:()=>false},"
        "length:{enumerable:true,configurable:false,get:()=>0},opener:{enumerable:true,configurable:false,get:()=>null},"
        "focus:{enumerable:true,configurable:false,get:()=>function focus(){},set:denied},"
        "blur:{enumerable:true,configurable:false,get:()=>function blur(){},set:denied},"
        "close:{enumerable:true,configurable:false,get:()=>function close(){},set:denied}});"
        "Object.defineProperty(target,Symbol.toStringTag,{configurable:false,value:'Window'});"
        "windowProxy=new Proxy(target,{get:(object,key,receiver)=>Reflect.has(object,key)?Reflect.get(object,key,receiver):denied(),"
        "set:(object,key,value,receiver)=>Reflect.has(object,key)?Reflect.set(object,key,value,receiver):denied()});return windowProxy};"
        "const send=function postMessage(data,targetOrigin='/'){"
        "if(data&&data.event==='reject')globalThis.__tilefinchLastRejectStack=String(new Error('Turnstile reject').stack||'');"
        "__tilefinchPostParentMessage(data,targetOrigin);};let parentWindow,topWindow;"
        "if(topIsParent){parentWindow=makeAncestor(send,()=>parentWindow,()=>parentWindow);topWindow=parentWindow}"
        "else{topWindow=makeAncestor(function postMessage(data,targetOrigin='/'){__tilefinchPostTopMessage(data,targetOrigin)},()=>topWindow,()=>topWindow);"
        "parentWindow=makeAncestor(send,()=>topWindow,()=>topWindow)}globalThis.parent=parentWindow;globalThis.top=topWindow;"
        "Object.defineProperty(globalThis,'opener',{value:null,writable:false,"
        "configurable:false});})();";
    const char *child_setup = parent_same_origin
        ? same_origin_child_setup : cross_origin_child_setup;
    size_t child_setup_length = parent_same_origin
        ? sizeof(same_origin_child_setup) - 1
        : sizeof(cross_origin_child_setup) - 1;
    JSValue global = JS_GetGlobalObject(runtime->context);
    bool top_state_ok = JS_SetPropertyStr(
        runtime->context, global, "__tilefinchTopIsParent",
        JS_NewBool(runtime->context, top_is_parent)) >= 0;
    JS_FreeValue(runtime->context, global);
    if (!top_state_ok) {
        js_rt_record_exception(runtime->context, &runtime->result);
        runtime->result.success = false;
        return false;
    }
    js_rt_runtime_arm_watchdog(runtime);
    bool ok = js_rt_evaluate_source(runtime->context, child_setup,
                              child_setup_length,
                              "<frame-messaging>", &runtime->result);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok && child_opaque_origin) {
        static const char opaque_setup[] =
            "(()=>{globalThis.__tilefinchMarkLocationOpaque?.();"
            "Object.defineProperty(globalThis,'origin',{value:'null',"
            "writable:false,configurable:false});"
            "const denyStorage=()=>{throw new DOMException('Opaque origin',"
            "'SecurityError')};"
            "Object.defineProperty(globalThis,'localStorage',{get:denyStorage,"
            "configurable:false});"
            "Object.defineProperty(globalThis,'sessionStorage',{get:denyStorage,"
            "configurable:false});"
            "try{Object.defineProperty(document,'domain',{get(){return ''},"
            "set(){throw new DOMException('Opaque origin','SecurityError')},"
            "configurable:false})}catch(_){}})();";
        ok = js_rt_evaluate_source(
            runtime->context, opaque_setup, sizeof(opaque_setup) - 1,
            "<frame-opaque-origin>", &runtime->result);
        if (ok) ok = js_rt_runtime_run_jobs(runtime);
    }
    if (ok) {
        runtime->bridge.post_message = callback;
        runtime->bridge.post_message_opaque = opaque;
    }
    runtime->result.success = ok;
    return ok;
}

bool script_runtime_origin_is_opaque(const ScriptRuntime *runtime)
{
    return runtime != NULL && runtime->bridge.opaque_origin;
}

void script_runtime_relocate_messaging_opaque(ScriptRuntime *runtime,
                                              void *opaque)
{
    if (runtime == NULL || runtime->bridge.post_message == NULL) return;
    runtime->bridge.post_message_opaque = opaque;
}

bool script_runtime_dispatch_message(ScriptRuntime *runtime,
                                     const char *json,
                                     const char *source_origin,
                                     long source_frame_handle,
                                     ScriptResult *result)
{
    if (runtime == NULL || json == NULL || source_origin == NULL
        || strlen(json) > 65536 || strlen(source_origin) > 2048) return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[3] = {
        JS_NewString(runtime->context, json),
        JS_NewString(runtime->context, source_origin),
        JS_NewInt64(runtime->context, source_frame_handle)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_RECEIVE_MESSAGE, 3, arguments,
                           NULL);
    for (size_t i = 0; i < 3; i++) JS_FreeValue(runtime->context, arguments[i]);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_set_frame_window_state(ScriptRuntime *runtime,
                                           long frame_handle, bool active,
                                           bool same_origin,
                                           bool opaque_origin,
                                           const char *committed_url,
                                           bool document_committed,
                                           ScriptResult *result)
{
    if (runtime == NULL || frame_handle <= 0) return false;
    JSValue arguments[6] = {
        JS_NewInt64(runtime->context, frame_handle),
        JS_NewBool(runtime->context, active),
        JS_NewBool(runtime->context, same_origin),
        JS_NewBool(runtime->context, opaque_origin),
        committed_url == NULL
            ? JS_NULL : JS_NewString(runtime->context, committed_url),
        JS_NewBool(runtime->context, document_committed)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_SET_FRAME_WINDOW_STATE, 6,
                           arguments, NULL);
    for (size_t i = 0; i < 6; i++) JS_FreeValue(runtime->context, arguments[i]);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

void script_runtime_intersection_recheck(ScriptRuntime *runtime)
{
    /* Schedules the page-side IntersectionObserver recomputation
       microtask after a layout commit, mirroring browsers' post-layout
       observer cadence.  Cheap no-op when the page defines no hook. */
    if (runtime == NULL) return;
    (void) runtime_call_host(runtime, SCRIPT_HOST_INTERSECTION_RECHECK, 0, NULL,
                        NULL);
}

void script_runtime_media_recheck(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    (void) runtime_call_host(runtime, SCRIPT_HOST_MEDIA_RECHECK, 0, NULL, NULL);
}

void script_runtime_report_memory(ScriptRuntime *runtime, FILE *output)
{
    if (runtime == NULL || runtime->runtime == NULL || output == NULL) return;
    /* Force a full collection first so the report reads the retained
       floor, not whatever garbage the last slice left behind. */
    JS_RunGC(runtime->runtime);
    JSMemoryUsage usage;
    js_rt_heap_census(runtime, &usage);
    if (tilefinch_dump_js_memory()) {
        JS_DumpMemoryUsage(stderr, &usage, runtime->runtime);
        fprintf(stderr, "script-arenas count=%lld capacity=%lld used=%lld\n",
            (long long)usage.malloc_arena_count,
            (long long)usage.malloc_arena_capacity,
            (long long)usage.malloc_arena_used);
    }
    if (tilefinch_dump_js_pool()) {
        budget_quickjs_pool_report_classes(runtime->quickjs_pool, stderr);
    }
    fprintf(output, "script-js-malloc-peak bytes=%zu largest-request=%zu\n",
            budget_quickjs_pool_js_malloc_peak(runtime->quickjs_pool),
            budget_quickjs_pool_largest_request(runtime->quickjs_pool));
    /* Allocation-free records cover every realm in this process, including
       child frames whose runtime is destroyed before the top-level report.
       Printing them with the ordinary browser memory census makes a heap
       ceiling distinguishable from the shared page budget in one PSP log. */
    budget_quickjs_pool_report_rejects(output);
    if (runtime->boot_window_bytes != 0) {
        fprintf(output,
                "script-boot-window base=%zu window=%zu active=%d "
                "peak=%zu returned-advance=%llu\n",
                runtime->base_memory_limit, runtime->boot_window_bytes,
                runtime->boot_window_active ? 1 : 0,
                runtime->boot_window_peak,
                (unsigned long long) runtime->boot_window_returned_advance);
    }
    fprintf(output,
            "script-memory retained=%lld malloc=%lld allocs=%lld "
            "atoms=%lld/%lld strings=%lld/%lld objects=%lld/%lld "
            "props=%lld shapes=%lld/%lld funcs=%lld code=%lld "
            "pc2line=%lld arrays=%lld/%lld binaries=%lld/%lld\n",
            (long long) usage.memory_used_size,
            (long long) usage.malloc_size,
            (long long) usage.malloc_count,
            (long long) usage.atom_count, (long long) usage.atom_size,
            (long long) usage.str_count, (long long) usage.str_size,
            (long long) usage.obj_count, (long long) usage.obj_size,
            (long long) usage.prop_size,
            (long long) usage.shape_count, (long long) usage.shape_size,
            (long long) usage.js_func_count,
            (long long) usage.js_func_code_size,
            (long long) usage.js_func_pc2line_size,
            (long long) usage.fast_array_count,
            (long long) usage.fast_array_elements,
            (long long) usage.binary_object_count,
            (long long) usage.binary_object_size);
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    JSLazyFunctionStats lazy;
    JS_GetLazyFunctionStats(runtime->runtime, &lazy);
    fprintf(output,
            "script-lazy-functions deferred=%llu deferred-bytes=%llu "
            "restored=%llu compiled=%llu compiled-bytes=%llu failures=%llu "
            "preparsed=%llu preparsed-bytes=%llu preparse-fallbacks=%llu "
            "preparse-restarts=%llu\n",
            (unsigned long long) lazy.deferred,
            (unsigned long long) lazy.deferred_source_bytes,
            (unsigned long long) lazy.restored,
            (unsigned long long) lazy.compiled,
            (unsigned long long) lazy.compiled_source_bytes,
            (unsigned long long) lazy.compile_failures,
            (unsigned long long) lazy.preparsed,
            (unsigned long long) lazy.preparsed_source_bytes,
            (unsigned long long) lazy.preparse_fallbacks,
            (unsigned long long) lazy.preparse_restarts);
#endif
}

void script_runtime_report_roots(ScriptRuntime *runtime, FILE *output,
                                 const char *phase, const char *realm,
                                 long realm_handle)
{
    if (runtime == NULL || runtime->runtime == NULL || output == NULL) return;
    JSMemoryUsage usage;
    js_rt_heap_census(runtime, &usage);
    const ScriptResult *result = &runtime->result;
    fprintf(output,
            "script-roots phase=%s realm=%s handle=%ld retained=%lld "
            "timers=%zu node-wrappers=%zu stable-wrappers=%zu "
            "listener-targets=%zu handler-targets=%zu observers=%zu "
            "network=%zu frame-windows=%zu\n",
            phase == NULL ? "unknown" : phase,
            realm == NULL ? "unknown" : realm, realm_handle,
            (long long) usage.memory_used_size, result->root_timers,
            result->root_node_wrappers, result->root_stable_wrappers,
            result->root_stable_listener_targets,
            result->root_stable_handler_targets,
            result->root_mutation_observers, result->root_pending_network,
            result->root_frame_windows);
}

bool script_runtime_evaluate_diagnostic(ScriptRuntime *runtime,
                                        const char *source,
                                        const char *source_url,
                                        ScriptResult *result)
{
    if (runtime == NULL || source == NULL || source_url == NULL) return false;
    bool preserve_failure_telemetry = !runtime->result.success;
    bool previous_interrupted = runtime->result.interrupted;
    size_t previous_watchdog_polls = runtime->result.watchdog_polls;
    unsigned long previous_watchdog_elapsed_ms =
        runtime->result.watchdog_elapsed_ms;
    js_rt_runtime_arm_watchdog(runtime);
    /* Diagnostics are commonly used after a complete parser/script pass,
       when short-lived parser callbacks may still occupy a tight realm.
       Match the lifecycle boundary's pressure behavior before admitting a
       fresh diagnostic program. */
    if (script_runtime_heap_remaining(runtime) < 256u * 1024u)
        (void) script_runtime_collect_and_trim(runtime);
    bool ok = evaluate_source_type(
        runtime->context, source, strlen(source), source_url,
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_DIAGNOSTIC,
        &runtime->result);
    const char *failed_stage = ok ? NULL : "evaluation";
    js_lazy_webpack_recover_failure(runtime);
    if (ok && !js_rt_runtime_run_jobs(runtime)) {
        ok = false;
        failed_stage = "microtasks";
    }
    if (ok && !js_rt_runtime_refresh(runtime)) {
        ok = false;
        failed_stage = "document-refresh";
    }
    if (!ok && tilefinch_trace_script_failures()) {
        fprintf(stderr,
                "diagnostic-script-failure source=\"%s\" stage=%s "
                "error=\"%s\"\n",
                source_url, failed_stage == NULL ? "unknown" : failed_stage,
                runtime->result.error);
    }
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    if (preserve_failure_telemetry) {
        runtime->result.interrupted = previous_interrupted;
        runtime->result.watchdog_polls = previous_watchdog_polls;
        runtime->result.watchdog_elapsed_ms = previous_watchdog_elapsed_ms;
        if (result != NULL) {
            result->interrupted = previous_interrupted;
            result->watchdog_polls = previous_watchdog_polls;
            result->watchdog_elapsed_ms = previous_watchdog_elapsed_ms;
        }
    }
    return ok;
}

static void js_rt_runtime_finish_slice_state(ScriptRuntime *runtime);

bool script_runtime_evaluate_probe(ScriptRuntime *runtime,
                                   const char *source,
                                   const char *source_url,
                                   ScriptResult *result)
{
    if (runtime == NULL || source == NULL || source_url == NULL) return false;
    bool preserve_failure_telemetry = !runtime->result.success;
    bool previous_interrupted = runtime->result.interrupted;
    size_t previous_polls = runtime->result.watchdog_polls;
    unsigned long previous_elapsed = runtime->result.watchdog_elapsed_ms;
    Watchdog previous_watchdog = runtime->watchdog;
    js_rt_runtime_arm_watchdog(runtime);
    bool ok = evaluate_source_type(
        runtime->context, source, strlen(source), source_url,
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_DIAGNOSTIC,
        &runtime->result);
    /* ClipboardWrite stores its bounded result in this bootstrap record.
       Read only that channel: a full snapshot visits author-visible globals
       unrelated to the predicate, and can itself execute their getters. */
    if (ok) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSValue clipboard = JS_GetPropertyStr(
            runtime->context, global, "__tilefinchClipboardStats");
        JSValue writes = JS_UNDEFINED, text = JS_UNDEFINED;
        const char *copied = NULL;
        if (JS_IsException(clipboard)) ok = false;
        else if (JS_IsObject(clipboard)) {
            writes = JS_GetPropertyStr(runtime->context, clipboard, "writes");
            if (JS_IsException(writes)) ok = false;
            int64_t count = 0;
            if (ok && JS_ToInt64(runtime->context, &count, writes) < 0)
                ok = false;
            if (ok) {
                text = JS_GetPropertyStr(runtime->context, clipboard, "text");
                if (JS_IsException(text)) ok = false;
                else {
                    copied = JS_ToCString(runtime->context, text);
                    if (copied == NULL) ok = false;
                }
            }
            if (ok) {
                if (count >= 0 && (uint64_t) count <= SIZE_MAX)
                    runtime->result.clipboard_writes = (size_t) count;
                snprintf(runtime->result.last_clipboard_text,
                         sizeof(runtime->result.last_clipboard_text),
                         "%s", copied);
            }
        }
        if (copied != NULL) JS_FreeCString(runtime->context, copied);
        JS_FreeValue(runtime->context, text);
        JS_FreeValue(runtime->context, writes);
        JS_FreeValue(runtime->context, clipboard);
        JS_FreeValue(runtime->context, global);
        if (!ok) js_rt_record_exception(runtime->context, &runtime->result);
    }
    js_rt_runtime_finish_slice_state(runtime);
    runtime->result.success = ok;
    if (preserve_failure_telemetry) {
        runtime->result.interrupted = previous_interrupted;
        runtime->result.watchdog_polls = previous_polls;
        runtime->result.watchdog_elapsed_ms = previous_elapsed;
    }
    /* A probe owns a bounded entry, not the interrupted task's deadline or
       counters. arm_watchdog already finished any preceding active slice;
       resume its clock from here rather than charging the probe twice. */
    runtime->watchdog = previous_watchdog;
    if (runtime->watchdog.slice_started_ns != 0)
        runtime->watchdog.slice_started_ns = js_rt_monotonic_time_ns();
    if (result != NULL) *result = runtime->result;
    return ok;
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
bool script_runtime_call_boolean_probe(ScriptRuntime *runtime,
                                       const char *function_name,
                                       bool *matched)
{
    if (matched != NULL) *matched = false;
    if (runtime == NULL || function_name == NULL || matched == NULL)
        return false;
    size_t length = 0;
    while (length <= 96u && function_name[length] != '\0') length++;
    if (length == 0 || length > 96u) return false;
    bool preserve_failure_telemetry = !runtime->result.success;
    bool previous_interrupted = runtime->result.interrupted;
    size_t previous_polls = runtime->result.watchdog_polls;
    unsigned long previous_elapsed = runtime->result.watchdog_elapsed_ms;
    Watchdog previous_watchdog = runtime->watchdog;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue function = JS_GetPropertyStr(
        runtime->context, global, function_name);
    JSValue value = JS_UNDEFINED;
    bool ok = !JS_IsException(function);
    if (ok && !JS_IsFunction(runtime->context, function)) {
        JS_ThrowTypeError(runtime->context, "diagnostic predicate is not callable");
        ok = false;
    }
    if (ok) {
        value = JS_Call(runtime->context, function, global, 0, NULL);
        ok = !JS_IsException(value);
        if (ok) {
            int boolean = JS_ToBool(runtime->context, value);
            ok = boolean >= 0;
            if (ok) *matched = boolean != 0;
        }
    }
    JS_FreeValue(runtime->context, value);
    JS_FreeValue(runtime->context, function);
    JS_FreeValue(runtime->context, global);
    if (!ok) js_rt_record_exception(runtime->context, &runtime->result);
    js_rt_runtime_finish_slice_state(runtime);
    runtime->result.success = ok;
    if (preserve_failure_telemetry) {
        runtime->result.interrupted = previous_interrupted;
        runtime->result.watchdog_polls = previous_polls;
        runtime->result.watchdog_elapsed_ms = previous_elapsed;
    }
    runtime->watchdog = previous_watchdog;
    if (runtime->watchdog.slice_started_ns != 0)
        runtime->watchdog.slice_started_ns = js_rt_monotonic_time_ns();
    return ok;
}
#endif

bool script_runtime_record_resource_timing(ScriptRuntime *runtime,
                                           const char *url,
                                           const char *initiator_type)
{
    return script_runtime_record_resource_timing_details(
        runtime, url, initiator_type, NULL);
}

bool script_runtime_record_resource_timing_details(
    ScriptRuntime *runtime, const char *url, const char *initiator_type,
    const ScriptResourceTiming *timing)
{
    if (runtime == NULL || url == NULL || initiator_type == NULL) return false;
    const ScriptResourceTiming empty = {0};
    if (timing == NULL) timing = &empty;
    JSValue arguments[16] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, initiator_type),
        JS_NewUint32(runtime->context, timing->name_lookup_us),
        JS_NewUint32(runtime->context, timing->connect_us),
        JS_NewUint32(runtime->context, timing->appconnect_us),
        JS_NewUint32(runtime->context, timing->first_byte_us),
        JS_NewUint32(runtime->context, timing->total_us),
        JS_NewUint32(runtime->context, timing->encoded_body_bytes),
        JS_NewUint32(runtime->context, timing->decoded_body_bytes),
        JS_NewBool(runtime->context, timing->measured),
        JS_NewBool(runtime->context, timing->cache_hit),
        JS_NewBool(runtime->context, timing->cache_validated),
        JS_NewBool(runtime->context, timing->timing_allowed),
        JS_NewUint32(runtime->context, timing->next_hop_protocol),
        JS_NewUint32(runtime->context, timing->response_status),
        JS_NewString(runtime->context, timing->content_type == NULL
            ? "" : timing->content_type)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_RECORD_RESOURCE_TIMING, 16,
                           arguments, NULL);
    for (size_t i = 0; i < 16; i++) {
        JS_FreeValue(runtime->context, arguments[i]);
    }
    return ok;
}

bool script_runtime_record_navigation_timing(
    ScriptRuntime *runtime, const ScriptNavigationTiming *timing)
{
    if (runtime == NULL || timing == NULL) return false;
    JSValue arguments[15] = {
        JS_NewUint32(runtime->context, timing->name_lookup_us),
        JS_NewUint32(runtime->context, timing->connect_us),
        JS_NewUint32(runtime->context, timing->appconnect_us),
        JS_NewUint32(runtime->context, timing->first_byte_us),
        JS_NewUint32(runtime->context, timing->total_us),
        JS_NewFloat64(runtime->context, (double) timing->response_start_us),
        JS_NewFloat64(runtime->context, (double) timing->response_end_us),
        JS_NewUint32(runtime->context, timing->encoded_body_bytes),
        JS_NewUint32(runtime->context, timing->decoded_body_bytes),
        JS_NewBool(runtime->context, timing->measured),
        JS_NewBool(runtime->context, timing->response_complete),
        JS_NewBool(runtime->context, timing->encoded_body_bytes_measured),
        JS_NewUint32(runtime->context, timing->response_status),
        JS_NewString(runtime->context, timing->next_hop_protocol == NULL
            ? "" : timing->next_hop_protocol),
        JS_NewString(runtime->context, timing->content_type == NULL
            ? "" : timing->content_type)
    };
    bool ok = runtime_call_host(
        runtime, SCRIPT_HOST_RECORD_NAVIGATION_TIMING, 15, arguments, NULL);
    for (size_t i = 0; i < 15; i++) {
        JS_FreeValue(runtime->context, arguments[i]);
    }
    return ok;
}

long script_runtime_node_handle(ScriptRuntime *runtime,
                                lxb_dom_node_t *node)
{
    if (runtime == NULL) return 0;
    int64_t handle = js_rt_bridge_register_node(&runtime->bridge, node);
    size_t slot = 0;
    if (js_rt_bridge_node_slot_for_handle(&runtime->bridge, handle, &slot)) {
        /* Public native handles may outlive any particular JavaScript
           wrapper (frame routing is the current user), so finalizers must
           not recycle their slots until document invalidation. */
        runtime->bridge.node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
    }
    return (long) handle;
}

long script_runtime_node_weak_handle(ScriptRuntime *runtime,
                                     lxb_dom_node_t *node)
{
    return runtime == NULL
        ? 0 : (long) js_rt_bridge_register_node(&runtime->bridge, node);
}

void script_runtime_node_handle_release(ScriptRuntime *runtime, long handle)
{
    if (runtime != NULL && handle != 0)
        bridge_release_native_node_pin(&runtime->bridge, (int64_t) handle);
}

bool script_runtime_node_handle_capacity_available(
    const ScriptRuntime *runtime)
{
    if (runtime == NULL) return true;
    const DomBridge *bridge = &runtime->bridge;
    if (bridge->node_count < bridge->node_capacity_limit) return true;
    for (size_t slot = 0; slot < bridge->node_count; slot++) {
        if (bridge->nodes[slot] == NULL
            && bridge->node_generations[slot] != 0
            && bridge->node_generations[slot]
                   < DOM_BRIDGE_NODE_GENERATION_MAX) return true;
    }
    return false;
}

lxb_dom_node_t *script_runtime_node_handle_resolve(
    const ScriptRuntime *runtime, long handle)
{
    size_t slot = 0;
    return runtime != NULL
        && js_rt_bridge_node_slot_for_handle(
               &runtime->bridge, (int64_t) handle, &slot)
            ? runtime->bridge.nodes[slot] : NULL;
}

lxb_dom_node_t *script_runtime_node_handle_resolve_connected(
    const ScriptRuntime *runtime, long handle)
{
    lxb_dom_node_t *node = script_runtime_node_handle_resolve(runtime, handle);
    return node != NULL && bridge_node_is_connected(node) ? node : NULL;
}

bool script_runtime_dynamic_script_is_scheduled(
    const ScriptRuntime *runtime, const lxb_dom_node_t *node)
{
    if (runtime == NULL || node == NULL) return false;
    const DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        const ScriptElementState *state = &bridge->script_elements[i];
        if (state->node == node && state->already_started) return true;
    }
    return false;
}

void script_runtime_script_mark_started(ScriptRuntime *runtime,
                                        const lxb_dom_node_t *node)
{
    ScriptElementState *state = runtime == NULL ? NULL
        : js_rt_script_element_state_find(&runtime->bridge, node);
    if (state == NULL || state->already_started) return;
    lxb_dom_node_t *element = state->node;
    size_t source_length = 0;
    if (dynamic_attribute_present(element, "src")
        || !script_node_source_length(element, &source_length)
        || source_length != 0) state->already_started = true;
}

bool script_runtime_script_parser_inserted(
    const ScriptRuntime *runtime, const lxb_dom_node_t *node)
{
    if (runtime == NULL || node == NULL) return true;
    const DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        const ScriptElementState *state = &bridge->script_elements[i];
        if (state->node == node) return !state->programmatic;
    }
    return true;
}

ScriptQuotaReserveResult script_runtime_script_quota_reserve(
    ScriptRuntime *runtime, ScriptQuotaCountMode count_mode,
    size_t requested_max_bytes, ScriptQuotaReservation *reservation)
{
    return runtime == NULL ? SCRIPT_QUOTA_RESERVE_REJECTED
        : js_rt_bridge_script_quota_reserve(
              &runtime->bridge, count_mode, requested_max_bytes,
              reservation);
}

bool script_runtime_script_slot_available(const ScriptRuntime *runtime)
{
    return runtime != NULL
        && (runtime->bridge.maximum_scripts == 0
            || runtime->bridge.script_quota_count
                < runtime->bridge.maximum_scripts);
}

bool script_runtime_script_quota_commit(
    ScriptRuntime *runtime, ScriptQuotaReservation *reservation,
    size_t actual_source_bytes)
{
    return runtime != NULL && js_rt_bridge_script_quota_commit(
        &runtime->bridge, reservation, actual_source_bytes);
}

void script_runtime_script_quota_abort(
    ScriptRuntime *runtime, ScriptQuotaReservation *reservation)
{
    if (runtime != NULL) {
        js_rt_bridge_script_quota_abort(&runtime->bridge, reservation);
    }
}

#if !defined(__PSP__)
void script_runtime_test_refuse_next_document_refresh(void)
{
    tilefinch_test_faults()->refuse_next_document_refresh = true;
}

void script_runtime_test_mark_document_refresh_failed(
    ScriptRuntime *runtime)
{
    if (runtime != NULL)
        runtime->document_refresh_failed_after_mutation = true;
}
#endif

bool script_runtime_document_refresh_failed(const ScriptRuntime *runtime)
{
    return runtime != NULL
        && runtime->document_refresh_failed_after_mutation;
}

bool js_rt_runtime_refresh(ScriptRuntime *runtime)
{
    if (runtime->bridge.stats_mutations != runtime->refreshed_mutations) {
        bool refreshed = true;
#if !defined(__PSP__)
        if (tilefinch_test_faults()->refuse_next_document_refresh) {
            tilefinch_test_faults()->refuse_next_document_refresh = false;
            refreshed = false;
        } else
#endif
        {
            refreshed = document_refresh(runtime->document);
        }
        if (!refreshed) {
            runtime->document_refresh_failed_after_mutation = true;
            snprintf(runtime->result.error, sizeof(runtime->result.error),
                     "could not refresh mutated DOM");
            return false;
        }
        runtime->refreshed_mutations = runtime->bridge.stats_mutations;
    }
    /* Pending timers and waiting network requests, each capped at 65535,
       read from the bootstrap's native-readable state (no JS entry). */
    double timers = runtime->host_state[SCRIPT_HOST_STATE_TIMERS];
    runtime->pending_timer_tasks = timers >= 65535.0 ? 65535u
        : timers > 0 ? (size_t) timers : 0u;
    runtime->result.pending_tasks = runtime->pending_timer_tasks;
    int64_t waiting = 0;
    if (JS_IsObject(runtime->host_network_queue_stats)) {
        JSValue value = JS_GetPropertyStr(
            runtime->context, runtime->host_network_queue_stats, "waiting");
        if (JS_ToInt64(runtime->context, &waiting, value) < 0) waiting = 0;
        JS_FreeValue(runtime->context, value);
    }
    runtime->pending_network_tasks = waiting >= 65535 ? 65535u
        : waiting > 0 ? (size_t) waiting : 0u;
    runtime->result.pending_tasks += runtime->pending_network_tasks;
    runtime->result.pending_tasks += runtime->bridge.async_fetch_count;
    runtime->result.pending_tasks += runtime->bridge.event_source_count;
    runtime->result.pending_tasks += runtime->bridge.websocket_count;
    runtime->result.pending_tasks += runtime->bridge.multiplayer.active ? 1u : 0u;
    runtime->result.pending_tasks += runtime->bridge.dynamic_script_count;
    return true;
}

bool js_rt_network_error_is_timeout(const char *error)
{
    return error != NULL
        && (strstr(error, "timeout") != NULL
            || strstr(error, "Timeout") != NULL
            || strstr(error, "timed out") != NULL);
}

size_t script_runtime_cancel_network(
    ScriptRuntime *runtime, const char *reason)
{
    if (runtime == NULL) return 0;
    size_t cancelled = fetch_scheduler_cancel_all(
        runtime->bridge.fetch_scheduler, reason);
    return cancelled + js_rt_websockets_abort(&runtime->bridge)
        + js_rt_multiplayer_abort(&runtime->bridge);
}

static bool runtime_pump_network(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 unsigned wait_ms,
                                 size_t *completed_out)
{
    if (completed_out != NULL) *completed_out = 0;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->document == NULL) {
        /* detach_document() drains normal work before clearing authority.
           This guard keeps a partial/OOM cleanup from turning a stale count
           into a document-budget dereference. */
        (void) script_runtime_cancel_network(runtime, "document detached");
        return true;
    }
    for (;;) {
        if (bridge->fetch_scheduler != NULL
            && fetch_scheduler_pending(bridge->fetch_scheduler)
                   >= SCRIPT_DYNAMIC_INFLIGHT_REQUESTS) break;
        ScriptDynamicTask *next = NULL;
        for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
            ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
            if (task->active && task->state == SCRIPT_DYNAMIC_QUEUED
                && (next == NULL || task->sequence < next->sequence)) {
                next = task;
            }
        }
        if (next == NULL) break;
        bool deferred = false;
        if (!js_rt_dynamic_start_task(runtime, next, &deferred)) return false;
        if (deferred) break;
    }
    /* The zero-budget preparation call is the single transport-progress
       boundary for this host turn.  Author-task delivery calls below may
       consume several already-completed results, but must not advance the
       transport again: doing so makes replay delays (and sufficiently fast
       live responses) overtake an async script which was already ready at
       the start of the turn. */
    if (completion_budget == 0
        && bridge->fetch_scheduler != NULL
        && fetch_scheduler_pending(bridge->fetch_scheduler) != 0) {
        (void) fetch_scheduler_pump(
            bridge->fetch_scheduler, 1,
            wait_ms > 4 ? 4 : wait_ms);
    }
    size_t author_tasks = 0;
    /*
     * A segmented classic script remains one author task. Once its first
     * registration has executed, finish its next slice before delivering
     * fetch promises or another dynamic script; otherwise page code could
     * observe a partially registered ResourceLoader response.
     */
    if (completion_budget != 0
        && js_rt_dynamic_classic_continuation_pending(runtime)) {
        if (!js_rt_dynamic_execute_ready(runtime, 1, &author_tasks)) {
            return false;
        }
        if (completed_out != NULL) *completed_out = author_tasks;
        return true;
    }
    if (!js_rt_websockets_deliver(
            runtime, completion_budget, &author_tasks)) {
        return false;
    }
    if (author_tasks != 0) {
        if (completed_out != NULL) *completed_out = author_tasks;
        return true;
    }
    if (!js_rt_multiplayer_deliver(
            runtime, completion_budget, &author_tasks)) {
        return false;
    }
    if (author_tasks != 0) {
        if (completed_out != NULL) *completed_out = author_tasks;
        return true;
    }
    if (!js_rt_event_sources_deliver(
            runtime, completion_budget, &author_tasks)) {
        return false;
    }
    if (author_tasks != 0) {
        if (completed_out != NULL) *completed_out = author_tasks;
        return true;
    }
    for (size_t i = 0;
         completion_budget != 0 && i < bridge->async_fetch_count;) {
        bool success = false;
        FetchResult *fetched = fetch_result_create(bridge->document->budget);
        if (fetched == NULL) return false;
        ScriptAsyncFetch async_fetch = bridge->async_fetches[i];
        uint64_t id = async_fetch.id;
        if (!fetch_scheduler_take(bridge->fetch_scheduler, id,
                                  &success, fetched)) {
            fetch_result_free(fetched);
            i++;
            continue;
        }
        if (success && !fetched->opaque_redirect
            && async_fetch.integrity_length != 0u) {
            TilefinchIntegrityResult integrity =
                tilefinch_resource_integrity_verify(
                    async_fetch.integrity, async_fetch.integrity_length,
                    (const uint8_t *) fetched->data, fetched->length);
            if (integrity == TILEFINCH_INTEGRITY_MISMATCH
                || integrity == TILEFINCH_INTEGRITY_INVALID) {
                success = false;
                snprintf(fetched->error, sizeof(fetched->error), "%s",
                         integrity == TILEFINCH_INTEGRITY_MISMATCH
                             ? "response integrity mismatch"
                             : "invalid response integrity metadata");
            }
        }
        if (success && !js_rt_script_response_origin_allowed(
                bridge, fetched, async_fetch.target_origin,
                async_fetch.destination, async_fetch.mode,
                async_fetch.credentials)) {
            success = false;
            snprintf(fetched->error, sizeof(fetched->error), "%s",
                     "CORS or cross-origin redirect response blocked");
        }
        const char *response_url = fetched->effective_url[0] != '\0'
            ? fetched->effective_url : async_fetch.target_origin;
        bool opaque_response = success && !fetched->opaque_redirect
            && async_fetch.destination == TILEFINCH_DESTINATION_FETCH
            && async_fetch.mode == TILEFINCH_REQUEST_MODE_NO_CORS
            && (bridge->opaque_origin || bridge->document_url == NULL
                || response_url == NULL
                || !tilefinch_url_same_origin(
                       bridge->document_url, response_url));
        JSValue payload;
        if (success) {
            /* Large textual responses are commonly followed by an immediate
               parser/compiler burst inside the fetch/XHR callback.  Enter
               that burst from the post-GC retained floor rather than from
               whatever cyclic event/DOM garbage the realm accumulated while
               transport was in flight.  The threshold keeps ordinary API
               responses off this comparatively expensive boundary; the
               native response remains authoritative until the JavaScript
               payload below has been created. */
            (void) js_rt_prepare_network_response_delivery(
                runtime, fetched->length);
            if (bridge->result != NULL) js_rt_record_network_response(
                bridge->result, fetched);
            js_rt_script_store_response_cookies(
                bridge, fetched, fetched->effective_url,
                async_fetch.mode, async_fetch.credentials,
                async_fetch.destination);
            char visible_headers[FETCH_RESPONSE_HEADERS_LIMIT];
            size_t visible_headers_length = js_rt_script_visible_response_headers(
                bridge, fetched, async_fetch.credentials, visible_headers,
                sizeof(visible_headers));
            payload = JS_NewObject(runtime->context);
            if (!JS_IsException(payload)) {
                bool filtered = fetched->opaque_redirect || opaque_response;
                (void) JS_SetPropertyStr(runtime->context, payload, "status",
                    JS_NewInt64(runtime->context,
                        filtered ? 0 : fetched->status_code));
                (void) JS_SetPropertyStr(runtime->context, payload, "url",
                    JS_NewString(runtime->context,
                        filtered ? "" : fetched->effective_url));
                (void) JS_SetPropertyStr(runtime->context, payload,
                    "contentType", JS_NewString(runtime->context,
                        filtered ? "" : fetched->content_type));
                (void) JS_SetPropertyStr(runtime->context, payload,
                    "headers", JS_NewStringLen(
                        runtime->context, visible_headers,
                        filtered ? 0u : visible_headers_length));
                (void) JS_SetPropertyStr(runtime->context, payload,
                    "redirected", JS_NewBool(runtime->context,
                                               fetched->redirected));
                (void) JS_SetPropertyStr(runtime->context, payload, "type",
                    JS_NewString(runtime->context,
                        fetched->opaque_redirect ? "opaqueredirect"
                            : opaque_response ? "opaque"
                            : async_fetch.mode == TILEFINCH_REQUEST_MODE_CORS
                                && (bridge->opaque_origin
                                    || !tilefinch_url_same_origin(
                                           bridge->document_url,
                                           fetched->effective_url))
                              ? "cors" : "basic"));
                if (!filtered) {
                    if (!js_rt_script_set_response_body(
                            runtime->context, payload, fetched,
                            async_fetch.prefer_text_response)) {
                        JS_FreeValue(runtime->context, payload);
                        payload = JS_EXCEPTION;
                    }
                }
            }
        } else {
            if (bridge->result != NULL) {
                bridge->result->network_failures++;
                if (js_rt_network_error_is_timeout(fetched->error)) {
                    bridge->result->async_network_timed_out++;
                }
            }
            payload = JS_NewString(runtime->context,
                                   fetched->error[0] == '\0'
                                   ? "network request failed"
                                   : fetched->error);
        }
        char timing_content_type[128] = {0};
        const char *timing_response_url = fetched->effective_url[0] != '\0'
            ? fetched->effective_url : NULL;
        bool timing_body_info_visible = success
            && timing_response_url != NULL
            && js_rt_resource_timing_body_info_visible(
                bridge, async_fetch.mode, timing_response_url,
                fetched->redirect_origin_tainted);
        if (timing_body_info_visible && fetched->content_type[0] != '\0') {
            (void) js_rt_resource_timing_minimize_content_type(
                fetched->content_type, timing_content_type,
                sizeof(timing_content_type));
        }
        uint32_t timing_encoded_bytes =
            fetched->transport_timing.encoded_body_bytes_measured
                ? fetched->transport_timing.encoded_body_bytes
                : fetched->length > UINT32_MAX
                    ? UINT32_MAX : (uint32_t) fetched->length;
        uint32_t timing_decoded_bytes = fetched->length > UINT32_MAX
            ? UINT32_MAX : (uint32_t) fetched->length;
        uint16_t timing_response_status = timing_body_info_visible
            && fetched->status_code > 0
            && fetched->status_code <= UINT16_MAX
            && fetched->status_code != 304
                ? (uint16_t) fetched->status_code : 0;
        JSValue arguments[18] = {
            JS_NewInt64(runtime->context, (int64_t) id),
            JS_NewBool(runtime->context, success), payload,
            JS_NewBool(runtime->context, true),
            JS_NewUint32(runtime->context,
                         fetched->transport_timing.name_lookup_us),
            JS_NewUint32(runtime->context,
                         fetched->transport_timing.connect_us),
            JS_NewUint32(runtime->context,
                         fetched->transport_timing.appconnect_us),
            JS_NewUint32(runtime->context,
                         fetched->transport_timing.first_byte_us),
            JS_NewUint32(runtime->context,
                         fetched->transport_timing.total_us),
            JS_NewUint32(runtime->context, timing_encoded_bytes),
            JS_NewUint32(runtime->context, timing_decoded_bytes),
            JS_NewBool(runtime->context,
                       fetched->transport_timing.measured),
            JS_NewBool(runtime->context, false),
            JS_NewBool(runtime->context, fetched->status_code == 304),
            JS_NewBool(runtime->context,
                       timing_response_url != NULL
                       && js_rt_script_resource_timing_allowed(
                              bridge, fetched, timing_response_url)),
            JS_NewUint32(runtime->context,
                         js_rt_script_resource_timing_protocol(
                             fetched->negotiated_http_version)),
            JS_NewUint32(runtime->context, timing_response_status),
            JS_NewString(runtime->context, timing_content_type)
        };
        /* Every response field the page can observe now lives in the bounded
           JavaScript payload.  Release the native transport copy before the
           author callback runs: large XHR bodies otherwise occupy both the
           native Budget and the QuickJS heap throughout parsing/dispatch,
           exactly when a constrained realm most needs callback headroom. */
        fetch_result_free(fetched);
        fetched = NULL;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char fetch_timeline[192];
        snprintf(fetch_timeline, sizeof(fetch_timeline),
               "tilefinch-fetch-timeline: stage=deliver-begin scheduler=%p id=%llu at-us=%llu",
               (void *) bridge->fetch_scheduler,
               (unsigned long long) id,
               (unsigned long long) tilefinch_platform_monotonic_time_us());
        tilefinch_platform_log_message(fetch_timeline);
#endif
        bool delivered = runtime_call_host(runtime, SCRIPT_HOST_DELIVER_NETWORK, 18,
                                      arguments, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        snprintf(fetch_timeline, sizeof(fetch_timeline),
               "tilefinch-fetch-timeline: stage=deliver-end scheduler=%p id=%llu at-us=%llu success=%d",
               (void *) bridge->fetch_scheduler,
               (unsigned long long) id,
               (unsigned long long) tilefinch_platform_monotonic_time_us(),
               delivered ? 1 : 0);
        tilefinch_platform_log_message(fetch_timeline);
#endif
        for (size_t value = 0; value < 18; value++) {
            JS_FreeValue(runtime->context, arguments[value]);
        }
        budget_free(bridge->budget, async_fetch.integrity);
        memmove(bridge->async_fetches + i,
                bridge->async_fetches + i + 1,
                (bridge->async_fetch_count - i - 1)
                    * sizeof(bridge->async_fetches[0]));
        bridge->async_fetch_count--;
        if (bridge->result != NULL) {
            bridge->result->async_network_completed++;
        }
        if (!delivered) return false;
        author_tasks = 1;
        break;
    }
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        if (!js_rt_dynamic_take_completion(
                runtime, &bridge->dynamic_scripts[i])) return false;
    }
    if (author_tasks == 0 && completion_budget != 0) {
        size_t dynamic_tasks = 0;
        if (!js_rt_dynamic_execute_ready(runtime, 1, &dynamic_tasks)) return false;
        author_tasks += dynamic_tasks;
    }
    if (completed_out != NULL) *completed_out = author_tasks;
    return true;
}
