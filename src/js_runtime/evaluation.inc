static bool install_named_function(JSContext *context, JSValue global,
                                   const char *property_name,
                                   const char *function_name,
                                   JSCFunction *function, int arguments)
{
    JSValue value = JS_NewCFunction(context, function, function_name,
                                    arguments);
    return !JS_IsException(value)
           && JS_SetPropertyStr(context, global, property_name, value) >= 0;
}

static uint64_t wall_time_ms(void)
{
    return tilefinch_platform_wall_time_ns() / UINT64_C(1000000);
}

uint64_t js_rt_monotonic_time_ns(void)
{
    return tilefinch_platform_monotonic_time_ns();
}

static uint64_t monotonic_time_ms(void)
{
    return js_rt_monotonic_time_ns() / UINT64_C(1000000);
}

static void watchdog_add_work(Watchdog *watchdog, size_t units)
{
    if (watchdog->result != NULL) {
        if (units > SIZE_MAX - watchdog->result->script_work_units) {
            watchdog->result->script_work_units = SIZE_MAX;
        } else {
            watchdog->result->script_work_units += units;
        }
    }
    if (units > SIZE_MAX - watchdog->slice_work_units) {
        watchdog->slice_work_units = SIZE_MAX;
    } else {
        watchdog->slice_work_units += units;
    }
}

static void watchdog_finish_slice(Watchdog *watchdog)
{
    if (watchdog->slice_started_ns == 0) return;
    uint64_t finished = js_rt_monotonic_time_ns();
    uint64_t elapsed_us = finished >= watchdog->slice_started_ns
        ? (finished - watchdog->slice_started_ns) / UINT64_C(1000) : 0;
    if (watchdog->result != NULL
        && elapsed_us > watchdog->result->script_max_slice_us) {
        watchdog->result->script_max_slice_us = elapsed_us;
        watchdog->result->script_max_slice_work_units =
            watchdog->slice_work_units;
    }
    /* A finished slice is idle until its caller explicitly resumes script
       work.  Leaving this timestamp armed would charge layout, paint, or
       event-loop idle time to the next JavaScript checkpoint. */
    watchdog->slice_started_ns = 0;
    watchdog->slice_work_units = 0;
}

static bool watchdog_cooperate(Watchdog *watchdog)
{
    watchdog_finish_slice(watchdog);
    if (watchdog->result != NULL) {
        if (watchdog->result->script_cooperative_yields != SIZE_MAX) {
            watchdog->result->script_cooperative_yields++;
        }
    }
    bool keep_going = tilefinch_platform_cooperate(
        "script", watchdog->result == NULL
                    ? watchdog->polls
                    : watchdog->result->script_work_units);
    if (keep_going) watchdog->slice_started_ns = js_rt_monotonic_time_ns();
    if (!keep_going) watchdog->interrupted = true;
    return keep_going;
}

static int interrupt_handler(JSRuntime *runtime, void *opaque)
{
#if !defined(CONFIG_EXECUTION_PROFILE)
    (void) runtime;
#endif
    Watchdog *watchdog = opaque;
    uint64_t now = monotonic_time_ms();
    watchdog->polls++;
    watchdog_add_work(watchdog, 1);
    if (watchdog->slice_work_units >= 4
        && !watchdog_cooperate(watchdog)) return 1;
    /* Watchdog cadence is intentionally not page-visible. Deterministic
       replay time advances only at explicit Date/performance observations
       and event-loop elapsed_ms boundaries, which other engines can mirror. */
#if defined(CONFIG_EXECUTION_PROFILE)
    JS_RecordExecutionProfilePoll(runtime);
    if (now >= watchdog->next_profile_ms && now < watchdog->deadline_ms) {
        JS_DumpExecutionProfileSnapshot(runtime, now - watchdog->started_ms);
        do {
            watchdog->next_profile_ms += 1000u;
        } while (watchdog->next_profile_ms <= now);
    }
#endif
    if (now >= watchdog->deadline_ms) {
        watchdog->interrupted = true;
        return 1;
    }
    return 0;
}

bool js_rt_runtime_callback_checkpoint(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    Watchdog *watchdog = &runtime->watchdog;
    watchdog->polls++;
    watchdog_add_work(watchdog, 1);
    /* Unlike the VM's amortised interrupt counter, an asynchronous callback
       is already a browser task boundary.  Cooperating on every such entry is
       bounded by the host's callback budget and gives Circle/input a chance
       before any author callback starts. */
    if (!watchdog_cooperate(watchdog)) return false;
    if (monotonic_time_ms() >= watchdog->deadline_ms) {
        watchdog->interrupted = true;
        return false;
    }
    return true;
}

static bool node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

void js_rt_record_exception(JSContext *context, ScriptResult *result)
{
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    if (message != NULL) {
        snprintf(result->error, sizeof(result->error), "%s", message);
        JS_FreeCString(context, message);
    } else {
        snprintf(result->error, sizeof(result->error), "JavaScript exception");
    }
    JSValue stack_value = JS_GetPropertyStr(context, exception, "stack");
    const char *stack = JS_IsUndefined(stack_value)
                        ? NULL : JS_ToCString(context, stack_value);
    if (stack != NULL && stack[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 2 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\n%s", stack);
        }
        JS_FreeCString(context, stack);
    }
    JS_FreeValue(context, stack_value);
    JSValue global = JS_GetGlobalObject(context);
    JSValue selector_value = JS_GetPropertyStr(
        context, global, "__tilefinchLastMissingSelector");
    const char *selector = JS_IsString(selector_value)
                           ? JS_ToCString(context, selector_value) : NULL;
    if (selector != NULL && selector[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nlast empty selector: %s", selector);
        }
        JS_FreeCString(context, selector);
    }
    JS_FreeValue(context, selector_value);
    JSValue base64_value = JS_GetPropertyStr(
        context, global, "__tilefinchBase64Error");
    const char *base64 = JS_IsString(base64_value)
                         ? JS_ToCString(context, base64_value) : NULL;
    if (base64 != NULL && base64[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nbase64 diagnostic: %s", base64);
        }
        JS_FreeCString(context, base64);
    }
    JS_FreeValue(context, base64_value);
    JSValue frame_eval_value = JS_GetPropertyStr(
        context, global, "__tilefinchFrameEvalTelemetry");
    const char *frame_eval = JS_IsString(frame_eval_value)
                             ? JS_ToCString(context, frame_eval_value) : NULL;
    if (frame_eval != NULL && frame_eval[0] != '\0') {
        snprintf(result->frame_eval_telemetry,
                 sizeof(result->frame_eval_telemetry), "%s", frame_eval);
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nframe eval: %s", frame_eval);
        }
        JS_FreeCString(context, frame_eval);
    }
    JS_FreeValue(context, frame_eval_value);
    JS_FreeValue(context, global);
    JS_FreeValue(context, exception);
}

static void promise_rejection_tracker(JSContext *context,
                                      JSValueConst promise,
                                      JSValueConst reason,
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
                                      int is_handled,
#else
                                      bool is_handled,
#endif
                                      void *opaque)
{
    if (opaque == NULL) return;
    PromiseRejectionState *state = opaque;
    ScriptResult *result = state->result;
    if (result == NULL) return;
    void *identity = JS_VALUE_GET_PTR(promise);
    if (is_handled) {
        js_rt_saturating_add_size(&result->promise_rejections_handled, 1);
        for (size_t i = 0; i < state->count; i++) {
            if (state->promises[i] != identity) continue;
            state->promises[i] = state->promises[state->count - 1];
            state->count--;
            break;
        }
        result->promise_rejections = state->count;
        if (state->count == 0) {
            result->last_promise_rejection[0] = '\0';
            state->last_reported = NULL;
        } else if (state->last_reported == identity) {
            snprintf(result->last_promise_rejection,
                     sizeof(result->last_promise_rejection),
                     "unhandled promise rejection remains");
            state->last_reported = NULL;
        }
        return;
    }
    for (size_t i = 0; i < state->count; i++) {
        if (state->promises[i] == identity) return;
    }
    js_rt_saturating_add_size(&result->promise_rejections_created, 1);
    if (JS_IsUndefined(reason)) {
        js_rt_saturating_add_size(&result->promise_rejections_undefined, 1);
    }
    if (state->count < sizeof(state->promises) / sizeof(state->promises[0])) {
        state->promises[state->count++] = identity;
    }
    result->promise_rejections = state->count;
    state->last_reported = identity;
    const char *message = JS_ToCString(context, reason);
    snprintf(result->last_promise_rejection,
             sizeof(result->last_promise_rejection), "%s",
             message == NULL ? "unhandled promise rejection" : message);
    if (message != NULL) JS_FreeCString(context, message);
    JSValue stack_value = JS_GetPropertyStr(context, reason, "stack");
    const char *stack = JS_IsUndefined(stack_value)
                        ? NULL : JS_ToCString(context, stack_value);
    if (stack != NULL && stack[0] != '\0') {
        size_t used = strlen(result->last_promise_rejection);
        if (used + 2 < sizeof(result->last_promise_rejection)) {
            snprintf(result->last_promise_rejection + used,
                     sizeof(result->last_promise_rejection) - used,
                     "\n%s", stack);
        }
        JS_FreeCString(context, stack);
    }
    if (getenv("TILEFINCH_TRACE_PROMISE_REJECTIONS") != NULL) {
        fprintf(stderr, "promise-rejection source=\"%s\" reason=\"%s\" "
                "created=%zu handled=%zu live=%zu undefined=%zu\n",
                state->active_source == NULL ? "<event-loop>"
                                             : state->active_source,
                result->last_promise_rejection,
                result->promise_rejections_created,
                result->promise_rejections_handled,
                result->promise_rejections,
                result->promise_rejections_undefined);
    }
    JS_FreeValue(context, stack_value);
}

void js_rt_saturating_add_size(size_t *value, size_t amount)
{
    if (value == NULL) return;
    *value = amount > SIZE_MAX - *value ? SIZE_MAX : *value + amount;
}

void js_rt_saturating_add_u64(uint64_t *value, uint64_t amount)
{
    if (value == NULL) return;
    *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount;
}

static uint64_t compile_observation_threshold(const DomBridge *bridge)
{
    return bridge == NULL
           || bridge->execution_policy.slow_compile_threshold_us == 0
        ? SCRIPT_DEFAULT_SLOW_DEPENDENCY_US
        : bridge->execution_policy.slow_compile_threshold_us;
}

typedef struct {
    int64_t malloc_size;
    int64_t retained_size;
    int64_t function_count;
    int64_t code_size;
    int64_t object_count;
} ScriptResidencySample;

static const char *script_residency_trace_mode(void)
{
    const char *mode = getenv("TILEFINCH_TRACE_SCRIPT_RESIDENCY");
    return mode == NULL || mode[0] == '\0' ? NULL : mode;
}

static ScriptResidencySample script_residency_sample(JSRuntime *runtime)
{
    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    if (runtime != NULL) JS_ComputeMemoryUsage(runtime, &usage);
    return (ScriptResidencySample) {
        .malloc_size = usage.malloc_size,
        .retained_size = usage.memory_used_size,
        .function_count = usage.js_func_count,
        .code_size = usage.js_func_code_size,
        .object_count = usage.obj_count
    };
}

static int64_t script_residency_delta(int64_t after, int64_t before)
{
    if (after >= before) return after - before;
    return -(before - after);
}

static void script_residency_label(char output[256], const char *name)
{
    const char *source = name == NULL ? "<script>" : name;
    size_t written = 0;
    for (; *source != '\0' && written + 1 < 256; source++) {
        unsigned char character = (unsigned char) *source;
        output[written++] = character < 32 || character == 127
            || character == '"' || character == '\\' ? '_' : (char) character;
    }
    output[written] = '\0';
}

static void script_residency_trace_compile(
    JSRuntime *runtime, const char *name, size_t source_bytes,
    ScriptCompileSourceKind source_kind, ScriptResidencySample before,
    bool admitted, bool compiled)
{
    if (script_residency_trace_mode() == NULL) return;
    ScriptResidencySample after = script_residency_sample(runtime);
    char label[256];
    script_residency_label(label, name);
    fprintf(stderr,
            "script-residency phase=compile name=\"%s\" kind=%u "
            "source-bytes=%zu admitted=%s compiled=%s malloc=%lld "
            "malloc-delta=%lld retained=%lld retained-delta=%lld "
            "functions=%lld function-delta=%lld code=%lld code-delta=%lld "
            "objects=%lld object-delta=%lld\n",
            label, (unsigned) source_kind, source_bytes,
            admitted ? "yes" : "no", compiled ? "yes" : "no",
            (long long) after.malloc_size,
            (long long) script_residency_delta(
                after.malloc_size, before.malloc_size),
            (long long) after.retained_size,
            (long long) script_residency_delta(
                after.retained_size, before.retained_size),
            (long long) after.function_count,
            (long long) script_residency_delta(
                after.function_count, before.function_count),
            (long long) after.code_size,
            (long long) script_residency_delta(after.code_size,
                                                before.code_size),
            (long long) after.object_count,
            (long long) script_residency_delta(
                after.object_count, before.object_count));
}

static void script_residency_trace_settled(
    JSRuntime *runtime, const char *name, size_t source_bytes, bool success)
{
    const char *mode = script_residency_trace_mode();
    if (mode == NULL || runtime == NULL) return;
    ScriptResidencySample before = script_residency_sample(runtime);
    bool collected = strcmp(mode, "gc") == 0;
    if (collected) {
        JS_RunGC(runtime);
        JS_RunGC(runtime);
    }
    ScriptResidencySample after = collected
        ? script_residency_sample(runtime) : before;
    char label[256];
    script_residency_label(label, name);
    fprintf(stderr,
            "script-residency phase=settled name=\"%s\" source-bytes=%zu "
            "success=%s collected=%s malloc=%lld reclaimed-malloc=%lld "
            "retained=%lld reclaimed-retained=%lld functions=%lld "
            "reclaimed-functions=%lld code=%lld reclaimed-code=%lld "
            "objects=%lld reclaimed-objects=%lld\n",
            label, source_bytes, success ? "yes" : "no",
            collected ? "yes" : "no", (long long) after.malloc_size,
            (long long) script_residency_delta(before.malloc_size,
                                                after.malloc_size),
            (long long) after.retained_size,
            (long long) script_residency_delta(before.retained_size,
                                                after.retained_size),
            (long long) after.function_count,
            (long long) script_residency_delta(before.function_count,
                                                after.function_count),
            (long long) after.code_size,
            (long long) script_residency_delta(before.code_size,
                                                after.code_size),
            (long long) after.object_count,
            (long long) script_residency_delta(before.object_count,
                                                after.object_count));
}

void js_rt_trace_script_quota_rejection(DomBridge *bridge,
                                        const char *reason)
{
    if (script_residency_trace_mode() == NULL || bridge == NULL
        || bridge->host == NULL || bridge->host->runtime == NULL) return;
    ScriptResidencySample sample = script_residency_sample(
        bridge->host->runtime);
    fprintf(stderr,
            "script-residency phase=quota-reject reason=%s count=%zu/%zu "
            "source-bytes=%zu/%zu reserved-bytes=%zu malloc=%lld "
            "retained=%lld functions=%lld code=%lld objects=%lld\n",
            reason == NULL ? "unknown" : reason,
            bridge->script_quota_count, bridge->maximum_scripts,
            bridge->script_quota_bytes, bridge->maximum_script_bytes,
            bridge->script_quota_reserved_bytes,
            (long long) sample.malloc_size,
            (long long) sample.retained_size,
            (long long) sample.function_count,
            (long long) sample.code_size,
            (long long) sample.object_count);
}

static bool js_rt_compile_source_admitted(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result,
    bool count_compile_attempt)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t maximum = bridge == NULL ? 0
        : bridge->execution_policy.maximum_host_compile_source_bytes;
    if (result != NULL) {
        if (count_compile_attempt) {
            js_rt_saturating_add_size(&result->host_compile_attempts, 1);
        }
        result->host_compile_source_limit_bytes = maximum;
        result->last_compile_source_kind = source_kind;
        result->last_compile_source_bytes = length;
        snprintf(result->last_compile_source_name,
                 sizeof(result->last_compile_source_name), "%s",
                 name == NULL ? "<script>" : name);
    }
    if (maximum != 0 && length > maximum) {
        if (result != NULL) {
            result->last_compile_admission =
                SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT;
            js_rt_saturating_add_size(&result->host_compile_rejections, 1);
            js_rt_saturating_add_size(
                &result->host_compile_rejected_source_bytes, length);
            snprintf(result->error, sizeof(result->error),
                     "JavaScript source rejected before compile: "
                     "%s has %zu bytes; host compile limit is %zu bytes",
                     name == NULL ? "<script>" : name, length, maximum);
        }
        return false;
    }
    uint64_t projected_limit_us = bridge == NULL ? 0
        : bridge->execution_policy.maximum_host_compile_projected_us;
    size_t modeled_rate = bridge == NULL ? 0
        : bridge->execution_policy.modeled_compile_bytes_per_ms;
    if (projected_limit_us != 0 && modeled_rate != 0) {
        uint64_t projected_us =
            (uint64_t) length > UINT64_MAX / UINT64_C(1000)
                ? UINT64_MAX
                : (uint64_t) length * UINT64_C(1000) / (uint64_t) modeled_rate;
        if (projected_us > projected_limit_us) {
            if (result != NULL) {
                result->last_compile_admission =
                    SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME;
                js_rt_saturating_add_size(&result->host_compile_rejections, 1);
                js_rt_saturating_add_size(
                    &result->host_compile_projected_rejections, 1);
                js_rt_saturating_add_size(
                    &result->host_compile_projected_rejected_bytes, length);
                snprintf(result->error, sizeof(result->error),
                         "JavaScript source rejected before compile: "
                         "%s has %zu bytes; modeled compile time %llu us "
                         "exceeds the %llu us budget at %zu bytes/ms",
                         name == NULL ? "<script>" : name, length,
                         (unsigned long long) projected_us,
                         (unsigned long long) projected_limit_us,
                         modeled_rate);
            }
            return false;
        }
    }
    if (result != NULL) {
        result->last_compile_admission = SCRIPT_COMPILE_ADMISSION_ACCEPTED;
        if (count_compile_attempt) {
            js_rt_saturating_add_size(
                &result->host_compile_source_bytes, length);
        }
    }
    return true;
}

bool js_rt_admit_cached_compile_source(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result)
{
    if (js_rt_compile_source_admitted(
            context, length, name, source_kind, result, false)) return true;
    JSRuntime *runtime = JS_GetRuntime(context);
    ScriptResidencySample before = {0};
    if (script_residency_trace_mode() != NULL) {
        before = script_residency_sample(runtime);
    }
    script_residency_trace_compile(
        runtime, name, length, source_kind, before, false, false);
    return false;
}

bool js_rt_evaluate_compiled_source(
    JSContext *context, JSValue compiled, const char *name,
    size_t source_length, ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (JS_IsException(compiled)) return false;
    if (bridge != NULL && bridge->host != NULL
        && !js_rt_runtime_script_checkpoint(bridge->host, 1)) {
        JS_FreeValue(context, compiled);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, source_length, false);
        return false;
    }
    BudgetAllocationOwner previous_owner =
        bridge_document_owner_enter(bridge);
    compiled = JS_EvalFunction(context, compiled);
    bridge_document_owner_leave(bridge, previous_owner);
    if (JS_IsException(compiled)) {
        JS_FreeValue(context, compiled);
        js_rt_record_exception(context, result);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, source_length, false);
        return false;
    }
    JS_FreeValue(context, compiled);
    script_residency_trace_settled(
        JS_GetRuntime(context), name, source_length, true);
    return true;
}

JSValue js_rt_compile_source_type(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   int evaluation_type,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result, bool *admitted)
{
    if (admitted != NULL) *admitted = false;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSRuntime *js_runtime = JS_GetRuntime(context);
    ScriptResidencySample residency_before = {0};
    if (script_residency_trace_mode() != NULL) {
        residency_before = script_residency_sample(js_runtime);
    }
    if (!js_rt_compile_source_admitted(
            context, length, name, source_kind, result, true)) {
        script_residency_trace_compile(
            js_runtime, name, length, source_kind, residency_before,
            false, false);
        return JS_EXCEPTION;
    }
    if (admitted != NULL) *admitted = true;
    uint64_t compile_started = js_rt_monotonic_time_ns();
    JSValue value = JS_Eval(
        context, source, length, name == NULL ? "<script>" : name,
        evaluation_type | JS_EVAL_FLAG_COMPILE_ONLY);
    uint64_t compile_finished = js_rt_monotonic_time_ns();
    uint64_t compile_us = compile_finished >= compile_started
        ? (compile_finished - compile_started) / UINT64_C(1000) : 0;
    /* The parser polls the watchdog's interrupt handler, so an admitted
       compile can now end early through the normal parse-error unwind.
       Record the truthful classification; the elapsed slice still counts
       below because the aborted parse genuinely ran. */
    if (JS_IsException(value) && result != NULL && bridge != NULL
        && bridge->host != NULL && bridge->host->watchdog.interrupted) {
        result->last_compile_admission =
            SCRIPT_COMPILE_ADMISSION_ABORTED_WATCHDOG;
        js_rt_saturating_add_size(&result->host_compile_watchdog_aborts, 1);
        js_rt_saturating_add_size(
            &result->host_compile_watchdog_aborted_bytes, length);
    }
    if (result != NULL) {
        js_rt_saturating_add_u64(&result->host_compile_total_us, compile_us);
        if (compile_us > result->max_nonpreemptible_compile_us) {
            result->max_nonpreemptible_compile_us = compile_us;
            result->max_nonpreemptible_compile_bytes = length;
            result->max_nonpreemptible_compile_source_kind = source_kind;
            snprintf(result->max_nonpreemptible_compile_source_name,
                     sizeof(result->max_nonpreemptible_compile_source_name),
                     "%s", name == NULL ? "<script>" : name);
        }
        if (compile_us > compile_observation_threshold(bridge)) {
            js_rt_saturating_add_size(&result->nonpreemptible_compile_count, 1);
        }
    }
    script_residency_trace_compile(
        js_runtime, name, length, source_kind, residency_before,
        true, !JS_IsException(value));
    return value;
}

bool js_rt_evaluate_source_type_at(JSContext *context, const char *source,
                             size_t length, const char *name,
                             const char *module_base_url,
                             const char *module_referrer_policy,
                             TilefinchCredentialsMode module_credentials,
                             int evaluation_type,
                             ScriptCompileSourceKind source_kind,
                             ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    bool module = (evaluation_type & JS_EVAL_TYPE_MASK)
        == JS_EVAL_TYPE_MODULE;
    const char *base = module_base_url == NULL ? name : module_base_url;
    if (module) {
        if (runtime == NULL
            || !js_rt_runtime_module_root_register(
                runtime, name, base, module_referrer_policy,
                module_credentials)) {
            return false;
        }
        /* `module_base_url` may be borrowed HTTP-cache metadata. Import
           normalization during compile can evict that cache entry, so use
           the runtime-owned copy registered above for import.meta as well. */
        base = js_rt_runtime_module_base_lookup(runtime, name);
        if (base == NULL) return false;
        if (!js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_LOADING)) return false;
    }
    bool admitted = false;
    JSValue value = js_rt_compile_source_type(
        context, source, length, name, evaluation_type, source_kind,
        result, &admitted);
    if (!admitted) {
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    if (!JS_IsException(value) && bridge != NULL && bridge->host != NULL
        && !js_rt_runtime_script_checkpoint(bridge->host, 1)) {
        JS_FreeValue(context, value);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, length, false);
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    if (module) {
        if (!JS_IsException(value)
            && !js_rt_module_set_import_meta(context, value, base, true)) {
            JS_FreeValue(context, value);
            value = JS_EXCEPTION;
        }
    }
    BudgetAllocationOwner previous_owner =
        bridge_document_owner_enter(bridge);
    if (!JS_IsException(value)) value = JS_EvalFunction(context, value);
    bridge_document_owner_leave(bridge, previous_owner);
    if (JS_IsException(value)) {
        JS_FreeValue(context, value);
        js_rt_record_exception(context, result);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, length, false);
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    JS_FreeValue(context, value);
    script_residency_trace_settled(
        JS_GetRuntime(context), name, length, true);
    if (module) {
        (void) js_rt_runtime_module_root_state_set(
            runtime, name, SCRIPT_MODULE_MAP_EVALUATED);
    }
    return true;
}

static bool evaluate_source_type(JSContext *context, const char *source,
                                 size_t length, const char *name,
                                 int evaluation_type,
                                 ScriptCompileSourceKind source_kind,
                                 ScriptResult *result)
{
    return js_rt_evaluate_source_type_at(context, source, length, name, name,
                                   NULL, TILEFINCH_CREDENTIALS_SAME_ORIGIN,
                                   evaluation_type, source_kind, result);
}

bool js_rt_evaluate_source(JSContext *context, const char *source,
                     size_t length, const char *name,
                     ScriptResult *result)
{
    return evaluate_source_type(context, source, length, name,
                                JS_EVAL_TYPE_GLOBAL,
                                SCRIPT_COMPILE_SOURCE_INTERNAL, result);
}

void js_rt_capture_error_source_context(const char *source, size_t length,
                                  const char *source_url,
                                  ScriptResult *result)
{
    if (source == NULL || source_url == NULL || source_url[0] == '\0'
        || result == NULL || result->error[0] == '\0') return;
    snprintf(result->error_source_context,
             sizeof(result->error_source_context),
             "capture-pending url=%s length=%zu", source_url, length);
    const char *position = strstr(result->error, source_url);
    if (position != NULL) {
        position += strlen(source_url);
        if (*position++ != ':') position = NULL;
    }
    if (position == NULL) {
        const char *open = strchr(result->error, '(');
        const char *close = open == NULL ? NULL : strchr(open, ')');
        if (close == NULL) return;
        const char *column_colon = close;
        while (column_colon > open && column_colon[-1] != ':') {
            column_colon--;
        }
        if (column_colon == open) return;
        const char *line_colon = column_colon - 1;
        while (line_colon > open && line_colon[-1] != ':') line_colon--;
        if (line_colon == open) return;
        position = line_colon;
    }
    char *end = NULL;
    unsigned long line = strtoul(position, &end, 10);
    if (end == position || *end++ != ':' || line == 0) return;
    unsigned long column = strtoul(end, &end, 10);
    if (column == 0) return;
    size_t offset = 0;
    for (unsigned long current = 1; current < line && offset < length;
         current++) {
        const char *newline = memchr(source + offset, '\n', length - offset);
        if (newline == NULL) return;
        offset = (size_t) (newline - source) + 1;
    }
    if (column - 1 > length - offset) return;
    offset += (size_t) column - 1;
    size_t start = offset > 900 ? offset - 900 : 0;
    size_t finish = offset + 900 < length ? offset + 900 : length;
    size_t written = 0;
    for (size_t at = start;
         at < finish && written + 1 < sizeof(result->error_source_context);
         at++) {
        unsigned char character = (unsigned char) source[at];
        result->error_source_context[written++] =
            character < 32 || character == 127 ? ' ' : (char) character;
    }
    result->error_source_context[written] = '\0';
}


static bool document_current_script_get(JSContext *context,
                                        ScriptCurrentScriptScope *scope,
                                        ScriptResult *result)
{
    (void) result;
    if (scope == NULL) return false;
    scope->value = JS_UNDEFINED;
    scope->owner_document_identity = 0;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    scope->value = JS_DupValue(context, bridge->current_script);
    scope->owner_document_identity =
        bridge->current_script_owner_document_identity;
    return true;
}

/* Consumes value, matching JS_SetPropertyStr's ownership contract. */
static bool document_current_script_set(JSContext *context, JSValue value,
                                        ScriptResult *result)
{
    (void) result;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) {
        JS_FreeValue(context, value);
        return false;
    }
    JS_FreeValue(context, bridge->current_script);
    bridge->current_script = value;
    bridge->current_script_owner_document_identity = 0;
    return true;
}

static bool set_current_script(JSContext *context, lxb_dom_node_t *node,
                               ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    bool source_override = bridge != NULL
        && bridge->current_script_source_active;
    const char *source_key = source_override
        ? bridge->current_script_source_key : NULL;
    size_t source_section = source_override
        ? bridge->current_script_source_section : 0;
    if (!source_override && bridge != NULL) {
        for (size_t i = 0; i < bridge->script_source_node_count; i++) {
            ScriptSourceNode *source = &bridge->script_source_nodes[i];
            if (source->node == node) {
                source_override = true;
                source_key = source->key;
                source_section = source->section;
                break;
            }
        }
    }
    int64_t handle = source_override ? 0 : js_rt_bridge_register_node(bridge, node);
    JSValue wrap = source_override
        ? bridge->trusted_stable_script_wrap : bridge->trusted_node_wrap;
    if (!JS_IsFunction(context, wrap)) return false;
    JSValue arguments[2];
    int argument_count = 1;
    if (source_override) {
        arguments[0] = JS_NewString(context, source_key);
        arguments[1] = JS_NewInt64(
            context, (int64_t) source_section);
        argument_count = 2;
    } else arguments[0] = JS_NewInt64(context, handle);
    JSValue current = JS_Call(
        context, wrap, JS_UNDEFINED, argument_count, arguments);
    for (int index = 0; index < argument_count; index++) {
        JS_FreeValue(context, arguments[index]);
    }
    bool current_exception = JS_IsException(current);
    bool current_set = !current_exception
        && document_current_script_set(context, current, result);
    if (current_exception || !current_set) {
        if (current_exception) JS_FreeValue(context, current);
        if (current_exception) js_rt_record_exception(context, result);
        return false;
    }
    bridge->current_script_owner_document_identity = js_rt_node_owner_identity(node);
    return true;
}

bool js_rt_current_script_scope_begin(JSContext *context,
                                lxb_dom_node_t *node, bool module,
                                ScriptCurrentScriptScope *previous,
                                ScriptResult *result)
{
    if (!document_current_script_get(context, previous, result)) return false;
    bool set = module
        ? document_current_script_set(context, JS_NULL, result)
        : set_current_script(context, node, result);
    if (!set) {
        (void) document_current_script_set(
            context, JS_DupValue(context, previous->value), result);
        DomBridge *bridge = JS_GetContextOpaque(context);
        if (bridge != NULL) {
            bridge->current_script_owner_document_identity =
                previous->owner_document_identity;
        }
        JS_FreeValue(context, previous->value);
        previous->value = JS_UNDEFINED;
        previous->owner_document_identity = 0;
    }
    return set;
}

bool js_rt_current_script_scope_end(JSContext *context,
                              ScriptCurrentScriptScope *previous,
                              ScriptResult *result)
{
    if (previous == NULL) return false;
    bool restored = document_current_script_set(
        context, JS_DupValue(context, previous->value), result);
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (restored && bridge != NULL) {
        bridge->current_script_owner_document_identity =
            previous->owner_document_identity;
    }
    JS_FreeValue(context, previous->value);
    previous->value = JS_UNDEFINED;
    previous->owner_document_identity = 0;
    return restored;
}

static bool evaluate_script_node(JSContext *context, lxb_dom_node_t *node,
                                 ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    size_t source_length = 0;
    (void) document_attribute(node, "src", &source_length);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "src", 3)) {
        result->external_scripts_skipped++;
        return true;
    }
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    if (type != NULL && type_length == 6
        && memcmp(type, "module", 6) == 0) {
        /* Module loading is deliberately outside this bounded host. */
        result->external_scripts_skipped++;
        return true;
    }
    if (type != NULL && type_length != 0
        && !(type_length == 15 && memcmp(type, "text/javascript", 15) == 0)
        && !(type_length == 22 && memcmp(type, "application/javascript", 22) == 0)) {
        return true;
    }
    source_length = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > SIZE_MAX - source_length) return false;
        source_length += length;
    }
    if (source_length != 0) {
        size_t execution_reserve =
            script_runtime_inline_execution_reserve(
                bridge->host, source_length);
        size_t required = source_length > SIZE_MAX
                          - execution_reserve
            ? SIZE_MAX
            : source_length + execution_reserve;
        size_t remaining = script_runtime_heap_remaining(bridge->host);
        if (remaining < required) {
            size_t before = budget_remaining(bridge->budget);
            (void) script_runtime_collect_and_trim(bridge->host);
            size_t after = budget_remaining(bridge->budget);
            remaining = script_runtime_heap_remaining(bridge->host);
            size_t reclaimed = after > before ? after - before : 0;
            if (remaining < required) {
                budget_record_pressure(
                    bridge->budget, BUDGET_PRESSURE_JAVASCRIPT,
                    source_length, reclaimed);
                if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
                    fprintf(stderr,
                            "inline-script-admission bytes=%zu "
                            "heap-remaining=%zu reserve=%zu admitted=no\n",
                            source_length, remaining, execution_reserve);
                }
                return true;
            }
            budget_record_pressure(
                bridge->budget, BUDGET_PRESSURE_JAVASCRIPT, 0, reclaimed);
        }
    }
    ScriptQuotaReservation quota = {0};
    ScriptQuotaReserveResult quota_status = js_rt_bridge_script_quota_reserve(
        bridge, SCRIPT_QUOTA_NEW_EXECUTABLE, source_length, &quota);
    if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
        || !js_rt_bridge_script_quota_commit(bridge, &quota, source_length)) {
        js_rt_bridge_script_quota_abort(bridge, &quota);
        snprintf(result->error, sizeof(result->error),
                 "initial inline script exceeded executable quota");
        return false;
    }
    char *source_copy = budget_malloc(bridge->budget, source_length + 1);
    if (source_copy == NULL) return false;
    size_t copied = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        memcpy(source_copy + copied, source, length);
        copied += length;
    }
    source_copy[copied] = '\0';
    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    if (!js_rt_current_script_scope_begin(
            context, node, false, &previous_current_script, result)) {
        budget_free(bridge->budget, source_copy);
        return false;
    }
    bool evaluated = source_length == 0 || evaluate_source_type(
        context, source_copy, source_length, "<inline-script>",
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_INLINE, result);
    if (!js_rt_current_script_scope_end(
            context, &previous_current_script, result)) evaluated = false;
    budget_free(bridge->budget, source_copy);
    if (evaluated) result->scripts_evaluated++;
    return evaluated;
}

static bool evaluate_scripts(JSContext *context, lxb_dom_node_t *node,
                             ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    ScriptElementSnapshot *scripts = budget_calloc(
        bridge->budget, SCRIPT_DYNAMIC_NODE_LIMIT, sizeof(*scripts));
    if (scripts == NULL) return false;
    size_t script_count = 0;
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (!node_name_is(at, "script")) continue;
        if (script_count >= SCRIPT_DYNAMIC_NODE_LIMIT) break;
        ScriptElementState *state = js_rt_script_element_state_register(
            bridge, at, true);
        if (state == NULL) continue;
        state->programmatic = false;
        state->force_async = false;
        state->already_started = true;
        int64_t handle = js_rt_bridge_register_node(bridge, at);
        if (handle != 0) {
            scripts[script_count++] = (ScriptElementSnapshot) {
                .node_handle = handle, .identity = state->identity
            };
        }
    }
    bool complete = true;
    for (size_t index = 0; index < script_count; index++) {
        size_t slot = 0;
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, scripts[index].node_handle, &slot)) continue;
        lxb_dom_node_t *node = bridge->nodes[slot];
        ScriptElementState *state = js_rt_script_element_state_find(
            bridge, node);
        if (state == NULL || state->identity != scripts[index].identity) {
            continue;
        }
        if (!evaluate_script_node(context, node, result)) {
            result->scripts_failed++;
            /* An uncaught author exception terminates only that script. Keep
               loading the document, but retain genuinely fatal engine and
               budget failures as pipeline failures. */
            if (strstr(result->error, "interrupted") != NULL
                || strstr(result->error, "out of memory") != NULL
                || strstr(result->error, "memory limit") != NULL
                || strstr(result->error, "allocation") != NULL) {
                complete = false;
                break;
            }
        }
    }
    budget_free(bridge->budget, scripts);
    return complete;
}

FetchScheduler *script_runtime_fetch_scheduler(ScriptRuntime *runtime)
{
    if (runtime == NULL) return NULL;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->fetch_scheduler == NULL) {
        bridge->fetch_scheduler = bridge_fetch_scheduler_create(
            bridge, bridge->maximum_script_bytes);
    }
    return bridge->fetch_scheduler;
}
