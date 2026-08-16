/* Diagnostic (TILEFINCH_JS_TRAP_ALLOC_SIZE): print the JS interpreter
   stack when the allocator trap fires, so giant allocations can be
   attributed to page code.  Reenters the VM from inside an allocation;
   host-only, one-shot, best-effort. */
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

void js_rt_runtime_arm_watchdog(ScriptRuntime *runtime)
{
    watchdog_finish_slice(&runtime->watchdog);
    runtime->watchdog.started_ms = monotonic_time_ms();
    runtime->watchdog.deadline_ms = runtime->watchdog.started_ms
                                    + runtime->timeout_ms;
#if defined(CONFIG_EXECUTION_PROFILE)
    runtime->watchdog.next_profile_ms = runtime->watchdog.started_ms + 1000u;
#endif
    runtime->watchdog.polls = 0;
    runtime->watchdog.interrupted = false;
    runtime->watchdog.slice_started_ns = js_rt_monotonic_time_ns();
    runtime->watchdog.slice_work_units = 0;
}

static bool runtime_job_exception_is_fatal(const ScriptRuntime *runtime,
                                           size_t failures_before)
{
    if (runtime == NULL) return true;
    /* Error text is author-controlled and result.interrupted describes the
       previous completed slice until js_rt_runtime_update_result() runs.  Only a
       failure observed while executing this job can make it fatal. */
    return runtime->watchdog.interrupted
        || runtime->budget->failure_count != failures_before;
}

static void runtime_record_author_job_exception(ScriptRuntime *runtime)
{
    js_rt_saturating_add_size(&runtime->result.uncaught_callback_errors, 1);
    snprintf(runtime->result.last_uncaught_callback_error,
             sizeof(runtime->result.last_uncaught_callback_error),
             "microtask: %s", runtime->result.error[0] == '\0'
                 ? "JavaScript exception" : runtime->result.error);
}

bool js_rt_runtime_run_jobs(ScriptRuntime *runtime)
{
    /*
     * A full browser can usually drain a microtask checkpoint before the
     * next task.  On the PSP, a long promise chain can otherwise monopolize
     * the only thread long enough that its own timeout never gets a turn.
     * Bound each checkpoint; the next runtime tick continues the queue.
     */
    size_t jobs = 0;
    while (jobs < 64 && JS_IsJobPending(runtime->runtime)) {
        JSContext *job_context = NULL;
        uint64_t started_ns = js_rt_monotonic_time_ns();
        size_t polls_before = runtime->watchdog.polls;
        size_t failures_before = runtime->budget->failure_count;
        BudgetAllocationOwner previous_owner =
            document_allocation_owner_enter(runtime->document);
        int executed = JS_ExecutePendingJob(runtime->runtime, &job_context);
        document_allocation_owner_leave(runtime->document, previous_owner);
        js_rt_runtime_record_host_callback(
            runtime, "<promise-job>", started_ns, polls_before);
        if (executed < 0) {
            js_rt_record_exception(job_context == NULL ? runtime->context
                                                 : job_context,
                             &runtime->result);
            bool fatal = runtime_job_exception_is_fatal(
                runtime, failures_before);
            js_lazy_webpack_recover_failure(runtime);
            if (fatal) return false;
            /* A browser reports an exception thrown by an author microtask,
               then continues the checkpoint. It does not turn an optional
               resource event into a failed navigation. */
            runtime_record_author_job_exception(runtime);
        }
        jobs++;
    }
    return true;
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
    "__tilefinchPendingTimers",
    "__tilefinchPendingNetworkRequests",
    "__tilefinchDeliverNetwork",
    "__tilefinchPumpTimers",
    "__tilefinchRebindDocument",
    "__tilefinchCommitSameDocument",
    "__tilefinchRestoreSameDocument",
    "__tilefinchSaveSectionState",
    "__tilefinchRestoreSectionState"
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
        value = JS_Call(runtime->context, function, runtime->host_global,
                        argc, argv);
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

bool script_runtime_refresh_named_properties(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    return runtime_call_host(runtime, SCRIPT_HOST_REFRESH_NAMED_PROPERTIES,
                        0, NULL, NULL);
}

bool script_runtime_parser_mutation_checkpoint(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    return runtime_call_host(runtime, SCRIPT_HOST_PARSER_MUTATION_CHECKPOINT,
                        0, NULL, NULL)
           && js_rt_runtime_run_jobs(runtime);
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
    JSValue value = JS_Call(runtime->context, function, global, argc, argv);
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
    bool child_opaque_origin,
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
        "(()=>{globalThis.__tilefinchTaskRealm='child';"
        "const parentWindow={postMessage(data,targetOrigin='/'){"
        "if(data&&data.event==='reject')globalThis.__tilefinchLastRejectStack=String(new Error('Turnstile reject').stack||'');"
        "__tilefinchPostParentMessage(data,targetOrigin);}};globalThis.parent=parentWindow;"
        "globalThis.top=parentWindow;Object.defineProperty(globalThis,'opener',"
        "{value:null,writable:false,configurable:false});})();";
    static const char cross_origin_child_setup[] =
        "(()=>{globalThis.__tilefinchTaskRealm='child';"
        "const send=function postMessage(data,targetOrigin='/'){"
        "if(data&&data.event==='reject')globalThis.__tilefinchLastRejectStack=String(new Error('Turnstile reject').stack||'');"
        "__tilefinchPostParentMessage(data,targetOrigin);};const parentWindow={};"
        "Object.defineProperty(parentWindow,'postMessage',{enumerable:true,"
        "configurable:false,get(){return send},set(){throw new DOMException("
        "'Blocked cross-origin Window assignment','SecurityError')}});"
        "globalThis.parent=parentWindow;globalThis.top=parentWindow;"
        "Object.defineProperty(globalThis,'opener',{value:null,writable:false,"
        "configurable:false});})();";
    const char *child_setup = parent_same_origin
        ? same_origin_child_setup : cross_origin_child_setup;
    size_t child_setup_length = parent_same_origin
        ? sizeof(same_origin_child_setup) - 1
        : sizeof(cross_origin_child_setup) - 1;
    js_rt_runtime_arm_watchdog(runtime);
    bool ok = js_rt_evaluate_source(runtime->context, child_setup,
                              child_setup_length,
                              "<frame-messaging>", &runtime->result);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok && child_opaque_origin) {
        static const char opaque_setup[] =
            "(()=>{Object.defineProperty(location,'origin',{value:'null',"
            "writable:false,configurable:false});"
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
                                           ScriptResult *result)
{
    if (runtime == NULL || frame_handle <= 0) return false;
    JSValue arguments[5] = {
        JS_NewInt64(runtime->context, frame_handle),
        JS_NewBool(runtime->context, active),
        JS_NewBool(runtime->context, same_origin),
        JS_NewBool(runtime->context, opaque_origin),
        committed_url == NULL
            ? JS_NULL : JS_NewString(runtime->context, committed_url)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_SET_FRAME_WINDOW_STATE, 5,
                           arguments, NULL);
    for (size_t i = 0; i < 5; i++) JS_FreeValue(runtime->context, arguments[i]);
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
    JS_ComputeMemoryUsage(runtime->runtime, &usage);
    if (getenv("TILEFINCH_DUMP_JS_MEMORY") != NULL) {
        JS_DumpMemoryUsage(stderr, &usage, runtime->runtime);
    }
    if (getenv("TILEFINCH_DUMP_JS_POOL") != NULL) {
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
}

void script_runtime_report_roots(ScriptRuntime *runtime, FILE *output,
                                 const char *phase, const char *realm,
                                 long realm_handle)
{
    if (runtime == NULL || runtime->runtime == NULL || output == NULL) return;
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(runtime->runtime, &usage);
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
    if (!ok && getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
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

bool script_runtime_record_resource_timing(ScriptRuntime *runtime,
                                           const char *url,
                                           const char *initiator_type)
{
    if (runtime == NULL || url == NULL || initiator_type == NULL) return false;
    JSValue arguments[2] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, initiator_type)
    };
    bool ok = runtime_call_host(runtime, SCRIPT_HOST_RECORD_RESOURCE_TIMING, 2,
                           arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
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

lxb_dom_node_t *script_runtime_node_handle_resolve(
    const ScriptRuntime *runtime, long handle)
{
    size_t slot = 0;
    return runtime != NULL
        && js_rt_bridge_node_slot_for_handle(
               &runtime->bridge, (int64_t) handle, &slot)
            ? runtime->bridge.nodes[slot] : NULL;
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

bool js_rt_runtime_refresh(ScriptRuntime *runtime)
{
    if (runtime->result.dom_mutations != runtime->refreshed_mutations) {
        if (!document_refresh(runtime->document)) {
            snprintf(runtime->result.error, sizeof(runtime->result.error),
                     "could not refresh mutated DOM");
            return false;
        }
        runtime->refreshed_mutations = runtime->result.dom_mutations;
    }
    JSValue pending;
    if (!runtime_call_host(runtime, SCRIPT_HOST_PENDING_TIMERS, 0, NULL, &pending)) {
        return false;
    }
    int64_t pending_count = 0;
    if (JS_ToInt64(runtime->context, &pending_count, pending) < 0) {
        JS_FreeValue(runtime->context, pending);
        js_rt_record_exception(runtime->context, &runtime->result);
        return false;
    }
    JS_FreeValue(runtime->context, pending);
    runtime->result.pending_tasks = pending_count > 0
        ? (size_t) pending_count : 0;
    if (!runtime_call_host(runtime, SCRIPT_HOST_PENDING_NETWORK_REQUESTS, 0, NULL,
                      &pending)) {
        return false;
    }
    pending_count = 0;
    if (JS_ToInt64(runtime->context, &pending_count, pending) < 0) {
        JS_FreeValue(runtime->context, pending);
        js_rt_record_exception(runtime->context, &runtime->result);
        return false;
    }
    JS_FreeValue(runtime->context, pending);
    if (pending_count > 0) {
        runtime->result.pending_tasks += (size_t) pending_count;
    }
    runtime->result.pending_tasks += runtime->bridge.async_fetch_count;
    runtime->result.pending_tasks += runtime->bridge.event_source_count;
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
    return fetch_scheduler_cancel_all(
        runtime->bridge.fetch_scheduler, reason);
}

static bool runtime_pump_network(ScriptRuntime *runtime,
                                 size_t completion_budget,
                                 unsigned wait_ms,
                                 size_t *completed_out)
{
    if (completed_out != NULL) *completed_out = 0;
    DomBridge *bridge = &runtime->bridge;
    for (;;) {
        if (bridge->fetch_scheduler != NULL
            && fetch_scheduler_pending(bridge->fetch_scheduler) >= 4) break;
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
        if (success && !js_rt_script_response_origin_allowed(
                bridge, fetched, async_fetch.target_origin,
                TILEFINCH_DESTINATION_FETCH, async_fetch.mode,
                async_fetch.credentials)) {
            success = false;
            snprintf(fetched->error, sizeof(fetched->error), "%s",
                     "CORS or cross-origin redirect response blocked");
        }
        JSValue payload;
        if (success) {
            if (bridge->result != NULL) js_rt_record_network_response(
                bridge->result, fetched);
            js_rt_script_store_response_cookies(
                bridge, fetched, fetched->effective_url,
                async_fetch.mode, async_fetch.credentials,
                TILEFINCH_DESTINATION_FETCH);
            char visible_headers[FETCH_RESPONSE_HEADERS_LIMIT];
            size_t visible_headers_length = js_rt_script_visible_response_headers(
                bridge, fetched, async_fetch.credentials, visible_headers,
                sizeof(visible_headers));
            payload = JS_NewObject(runtime->context);
            if (!JS_IsException(payload)) {
                (void) JS_SetPropertyStr(runtime->context, payload, "status",
                    JS_NewInt64(runtime->context, fetched->status_code));
                (void) JS_SetPropertyStr(runtime->context, payload, "url",
                    JS_NewString(runtime->context, fetched->effective_url));
                (void) JS_SetPropertyStr(runtime->context, payload,
                    "contentType", JS_NewString(runtime->context,
                                                 fetched->content_type));
                (void) JS_SetPropertyStr(runtime->context, payload,
                    "headers", JS_NewStringLen(
                        runtime->context, visible_headers,
                        visible_headers_length));
                js_rt_script_set_response_body(runtime->context, payload, fetched);
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
        JSValue arguments[3] = {
            JS_NewInt64(runtime->context, (int64_t) id),
            JS_NewBool(runtime->context, success), payload
        };
        bool delivered = runtime_call_host(runtime, SCRIPT_HOST_DELIVER_NETWORK, 3,
                                      arguments, NULL);
        for (size_t value = 0; value < 3; value++) {
            JS_FreeValue(runtime->context, arguments[value]);
        }
        fetch_result_free(fetched);
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
