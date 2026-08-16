static void js_rt_runtime_finish_slice_state(ScriptRuntime *runtime)
{
    watchdog_finish_slice(&runtime->watchdog);
    runtime->result.interrupted = runtime->watchdog.interrupted;
#if defined(CONFIG_EXECUTION_PROFILE)
    if (runtime->result.interrupted)
        JS_DumpExecutionProfile(runtime->runtime);
#endif
    runtime->result.watchdog_polls = runtime->watchdog.polls;
    uint64_t watchdog_now = monotonic_time_ms();
    runtime->result.watchdog_elapsed_ms =
        watchdog_now < runtime->watchdog.started_ms ? 0ul
        : (unsigned long) (watchdog_now - runtime->watchdog.started_ms);
}

void js_rt_runtime_update_result(ScriptRuntime *runtime,
                                 ScriptResult *result)
{
    js_rt_runtime_finish_slice_state(runtime);
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue summary = JS_GetPropertyStr(runtime->context, global,
                                        "pocSummary");
    if (!JS_IsUndefined(summary)) {
        const char *text = JS_ToCString(runtime->context, summary);
        if (text != NULL) {
            snprintf(runtime->result.summary,
                     sizeof(runtime->result.summary), "%s", text);
            JS_FreeCString(runtime->context, text);
        }
    }
    JS_FreeValue(runtime->context, summary);
    JSValue submitted = JS_GetPropertyStr(runtime->context, global,
                                          "__tilefinchSubmitted");
    runtime->result.form_submission_requested =
        JS_ToBool(runtime->context, submitted) > 0;
    JS_FreeValue(runtime->context, submitted);
    JSValue xhr_calls = JS_GetPropertyStr(runtime->context, global,
                                          "__tilefinchXHRSendCalls");
    int64_t xhr_call_count = 0;
    if (JS_ToInt64(runtime->context, &xhr_call_count, xhr_calls) == 0
        && xhr_call_count > 0) {
        runtime->result.xhr_send_calls = (size_t) xhr_call_count;
    }
    JS_FreeValue(runtime->context, xhr_calls);
    JSValue xhr_error = JS_GetPropertyStr(runtime->context, global,
                                          "__tilefinchXHRLastError");
    if (!JS_IsUndefined(xhr_error)) {
        const char *text = JS_ToCString(runtime->context, xhr_error);
        if (text != NULL) {
            snprintf(runtime->result.last_xhr_error,
                     sizeof(runtime->result.last_xhr_error), "%s", text);
            JS_FreeCString(runtime->context, text);
        }
    }
    JS_FreeValue(runtime->context, xhr_error);
    JSValue xhr_responses = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRResponseCount");
    int64_t xhr_response_count = 0;
    if (JS_ToInt64(runtime->context, &xhr_response_count, xhr_responses) == 0
        && xhr_response_count > 0) {
        runtime->result.xhr_response_count = (size_t) xhr_response_count;
    }
    JS_FreeValue(runtime->context, xhr_responses);
    JSValue xhr_status = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRLastStatus");
    int64_t xhr_status_code = 0;
    if (JS_ToInt64(runtime->context, &xhr_status_code, xhr_status) == 0) {
        runtime->result.last_xhr_status = (long) xhr_status_code;
    }
    JS_FreeValue(runtime->context, xhr_status);
    JSValue xhr_text_length = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRLastTextLength");
    int64_t xhr_text_units = 0;
    if (JS_ToInt64(runtime->context, &xhr_text_units, xhr_text_length) == 0
        && xhr_text_units >= 0) {
        runtime->result.last_xhr_response_text_units =
            (size_t) xhr_text_units;
    }
    JS_FreeValue(runtime->context, xhr_text_length);
    JSValue xhr_byte_length = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRLastByteLength");
    int64_t xhr_bytes = 0;
    if (JS_ToInt64(runtime->context, &xhr_bytes, xhr_byte_length) == 0
        && xhr_bytes >= 0) {
        runtime->result.last_xhr_response_bytes = (size_t) xhr_bytes;
    }
    JS_FreeValue(runtime->context, xhr_byte_length);
    JSValue xhr_type = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRLastResponseType");
    const char *xhr_type_text = JS_ToCString(runtime->context, xhr_type);
    if (xhr_type_text != NULL) {
        snprintf(runtime->result.last_xhr_response_type,
                 sizeof(runtime->result.last_xhr_response_type), "%s",
                 xhr_type_text);
        JS_FreeCString(runtime->context, xhr_type_text);
    }
    JS_FreeValue(runtime->context, xhr_type);
    JSValue xhr_states = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchXHRLastStates");
    const char *xhr_states_text = JS_ToCString(runtime->context, xhr_states);
    if (xhr_states_text != NULL) {
        snprintf(runtime->result.last_xhr_ready_states,
                 sizeof(runtime->result.last_xhr_ready_states), "%s",
                 xhr_states_text);
        JS_FreeCString(runtime->context, xhr_states_text);
    }
    JS_FreeValue(runtime->context, xhr_states);
    JSValue reject_stack = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchLastRejectStack");
    if (!JS_IsUndefined(reject_stack)) {
        const char *text = JS_ToCString(runtime->context, reject_stack);
        if (text != NULL) {
            snprintf(runtime->result.last_reject_stack,
                     sizeof(runtime->result.last_reject_stack), "%s", text);
            JS_FreeCString(runtime->context, text);
        }
    }
    JS_FreeValue(runtime->context, reject_stack);
    JSValue uncaught = JS_GetPropertyStr(runtime->context, global,
                                         "__tilefinchUncaughtErrors");
    JSValue uncaught_length = JS_GetPropertyStr(runtime->context, uncaught,
                                                "length");
    int64_t uncaught_count = 0;
    if (JS_ToInt64(runtime->context, &uncaught_count, uncaught_length) == 0
        && uncaught_count > 0) {
        runtime->result.uncaught_callback_errors = (size_t) uncaught_count;
        JSValue last = JS_GetPropertyUint32(
            runtime->context, uncaught, (uint32_t) (uncaught_count - 1));
        const char *text = JS_ToCString(runtime->context, last);
        if (text != NULL) {
            snprintf(runtime->result.last_uncaught_callback_error,
                     sizeof(runtime->result.last_uncaught_callback_error),
                     "%s", text);
            if (runtime->result.error[0] == '\0') {
                snprintf(runtime->result.error,
                         sizeof(runtime->result.error), "%s", text);
            }
            JS_FreeCString(runtime->context, text);
        }
        JS_FreeValue(runtime->context, last);
    }
    JS_FreeValue(runtime->context, uncaught_length);
    JS_FreeValue(runtime->context, uncaught);
    JSValue uncaught_task = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchLastUncaughtTask");
    if (!JS_IsUndefined(uncaught_task)) {
        const char *text = JS_ToCString(runtime->context, uncaught_task);
        if (text != NULL) {
            snprintf(runtime->result.last_uncaught_callback_task,
                     sizeof(runtime->result.last_uncaught_callback_task),
                     "%s", text);
            JS_FreeCString(runtime->context, text);
        }
    }
    JS_FreeValue(runtime->context, uncaught_task);
    JSValue network_queue = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchNetworkQueueStats");
#define READ_NETWORK_QUEUE(field, property)                                  \
    do {                                                                     \
        JSValue value = JS_GetPropertyStr(runtime->context, network_queue,   \
                                          property);                         \
        int64_t converted = 0;                                               \
        if (JS_ToInt64(runtime->context, &converted, value) == 0             \
            && converted >= 0) runtime->result.field = (size_t) converted;  \
        JS_FreeValue(runtime->context, value);                               \
    } while (0)
    if (JS_IsObject(network_queue)) {
        READ_NETWORK_QUEUE(async_network_logical_admitted, "admitted");
        READ_NETWORK_QUEUE(async_network_logical_completed, "completed");
        READ_NETWORK_QUEUE(async_network_logical_rejected, "rejected");
        READ_NETWORK_QUEUE(async_network_logical_cancelled, "cancelled");
        READ_NETWORK_QUEUE(async_network_logical_timed_out, "timedOut");
        READ_NETWORK_QUEUE(async_network_logical_peak, "peakCount");
        READ_NETWORK_QUEUE(async_network_logical_peak_bytes, "peakBytes");
        READ_NETWORK_QUEUE(async_network_active_native, "active");
        READ_NETWORK_QUEUE(async_network_pending_logical, "waiting");
    }
#undef READ_NETWORK_QUEUE
    JS_FreeValue(runtime->context, network_queue);
    JSValue indexed_db = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchIndexedDBStats");
#define READ_INDEXED_DB(field, property)                                     \
    do {                                                                     \
        JSValue value = JS_GetPropertyStr(runtime->context, indexed_db,      \
                                          property);                         \
        int64_t converted = 0;                                               \
        if (JS_ToInt64(runtime->context, &converted, value) == 0             \
            && converted >= 0) runtime->result.field = (size_t) converted;  \
        JS_FreeValue(runtime->context, value);                               \
    } while (0)
    if (JS_IsObject(indexed_db)) {
        READ_INDEXED_DB(indexed_db_opens, "opens");
        READ_INDEXED_DB(indexed_db_deletes, "deletes");
        READ_INDEXED_DB(indexed_db_transactions, "transactions");
        READ_INDEXED_DB(indexed_db_requests, "requests");
        READ_INDEXED_DB(indexed_db_records, "records");
        READ_INDEXED_DB(indexed_db_bytes, "bytes");
        READ_INDEXED_DB(indexed_db_peak_bytes, "peakBytes");
        READ_INDEXED_DB(indexed_db_quota_errors, "quotaErrors");
    }
#undef READ_INDEXED_DB
    JS_FreeValue(runtime->context, indexed_db);
    JSValue clipboard = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchClipboardStats");
    if (JS_IsObject(clipboard)) {
        JSValue writes = JS_GetPropertyStr(runtime->context, clipboard,
                                           "writes");
        int64_t converted = 0;
        if (JS_ToInt64(runtime->context, &converted, writes) == 0
            && converted >= 0) {
            runtime->result.clipboard_writes = (size_t) converted;
        }
        JS_FreeValue(runtime->context, writes);
        JSValue copied = JS_GetPropertyStr(runtime->context, clipboard,
                                           "text");
        const char *copied_text = JS_ToCString(runtime->context, copied);
        if (copied_text != NULL) {
            snprintf(runtime->result.last_clipboard_text,
                     sizeof(runtime->result.last_clipboard_text), "%s",
                     copied_text);
            JS_FreeCString(runtime->context, copied_text);
        }
        JS_FreeValue(runtime->context, copied);
    }
    JS_FreeValue(runtime->context, clipboard);
    JSValue retention = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchRetentionStats");
#define READ_RETENTION(field, property)                                      \
    do {                                                                     \
        JSValue value = JS_GetPropertyStr(runtime->context, retention,       \
                                          property);                         \
        int64_t converted = 0;                                               \
        if (JS_ToInt64(runtime->context, &converted, value) == 0             \
            && converted >= 0) runtime->result.field = (size_t) converted;  \
        JS_FreeValue(runtime->context, value);                               \
    } while (0)
    if (JS_IsObject(retention)) {
        READ_RETENTION(retention_wrapper_evictions, "wrapperEvictions");
        READ_RETENTION(retention_listener_drops, "listenerDrops");
        READ_RETENTION(retention_handler_drops, "handlerDrops");
        READ_RETENTION(retention_observer_drops, "observerDrops");
        READ_RETENTION(retention_record_drops, "recordDrops");
        READ_RETENTION(retention_dirty_drops, "dirtyDrops");
        READ_RETENTION(retention_state_evictions, "stateEvictions");
        READ_RETENTION(retention_control_drops, "controlDrops");
    }
#undef READ_RETENTION
    JS_FreeValue(runtime->context, retention);
    JSValue roots = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchRootCensus");
#define READ_ROOT(field, property)                                           \
    do {                                                                     \
        JSValue value = JS_GetPropertyStr(runtime->context, roots, property);\
        int64_t converted = 0;                                               \
        if (JS_ToInt64(runtime->context, &converted, value) == 0             \
            && converted >= 0) runtime->result.field = (size_t) converted;  \
        JS_FreeValue(runtime->context, value);                               \
    } while (0)
    if (JS_IsObject(roots)) {
        READ_ROOT(root_timers, "timers");
        READ_ROOT(root_node_wrappers, "nodeWrappers");
        READ_ROOT(root_stable_wrappers, "stableWrappers");
        READ_ROOT(root_stable_listener_targets, "stableListenerTargets");
        READ_ROOT(root_stable_handler_targets, "stableHandlerTargets");
        READ_ROOT(root_mutation_observers, "mutationObservers");
        READ_ROOT(root_pending_network, "pendingNetwork");
        READ_ROOT(root_frame_windows, "frameWindows");
    }
#undef READ_ROOT
    JS_FreeValue(runtime->context, roots);
    JS_FreeValue(runtime->context, global);
    if (result != NULL) *result = runtime->result;
}

void script_runtime_telemetry(const ScriptRuntime *runtime,
                              ScriptRuntimeTelemetry *telemetry)
{
    if (telemetry == NULL) return;
    memset(telemetry, 0, sizeof(*telemetry));
    if (runtime == NULL) return;
    telemetry->host_compile_total_us =
        runtime->result.host_compile_total_us;
    telemetry->host_callback_total_us =
        runtime->result.host_callback_total_us;
    telemetry->scripts_evaluated = runtime->result.scripts_evaluated;
    telemetry->dom_mutations = runtime->result.dom_mutations;
}

bool script_runtime_take_form_submission(
    ScriptRuntime *runtime, lxb_dom_node_t **form,
    lxb_dom_node_t **submitter)
{
    if (form != NULL) *form = NULL;
    if (submitter != NULL) *submitter = NULL;
    if (runtime == NULL || form == NULL || submitter == NULL) return false;
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue queued = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchSubmitted");
    bool pending = JS_ToBool(runtime->context, queued) > 0;
    JS_FreeValue(runtime->context, queued);
    if (!pending) {
        JS_FreeValue(runtime->context, global);
        return false;
    }
    JSValue form_value = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchSubmittedFormHandle");
    JSValue submitter_value = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchSubmittedSubmitterHandle");
    int64_t form_handle = 0, submitter_handle = 0;
    bool converted = JS_ToInt64(
        runtime->context, &form_handle, form_value) == 0
        && JS_ToInt64(
               runtime->context, &submitter_handle, submitter_value) == 0;
    JS_FreeValue(runtime->context, form_value);
    JS_FreeValue(runtime->context, submitter_value);
    (void) JS_SetPropertyStr(runtime->context, global, "__tilefinchSubmitted",
                             JS_FALSE);
    (void) JS_SetPropertyStr(
        runtime->context, global, "__tilefinchSubmittedFormHandle",
        JS_NewInt32(runtime->context, 0));
    (void) JS_SetPropertyStr(
        runtime->context, global, "__tilefinchSubmittedSubmitterHandle",
        JS_NewInt32(runtime->context, 0));
    JS_FreeValue(runtime->context, global);
    runtime->result.form_submission_requested = false;
    if (!converted || form_handle <= 0) return false;
    JSValue form_handle_value = JS_NewInt64(
        runtime->context, form_handle);
    *form = js_rt_bridge_node_arg(
        runtime->context, &runtime->bridge, form_handle_value);
    JS_FreeValue(runtime->context, form_handle_value);
    if (submitter_handle > 0) {
        JSValue submitter_handle_value = JS_NewInt64(
            runtime->context, submitter_handle);
        *submitter = js_rt_bridge_node_arg(
            runtime->context, &runtime->bridge, submitter_handle_value);
        JS_FreeValue(runtime->context, submitter_handle_value);
    }
    return *form != NULL;
}
