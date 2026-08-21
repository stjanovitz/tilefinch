void script_runtime_destroy(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    Budget *budget = runtime->budget;
    if (getenv("TILEFINCH_TRACE_TASKS") != NULL) {
        /* Teardown must never invoke author-overridable JavaScript. Apart from
           reentrancy, a detached incumbent runtime may no longer point at its
           own live document. Retain bounded native counters for diagnostics. */
        fprintf(stderr,
                "javascript-timers teardown pending=%zu ticks=%zu callbacks=%zu\n",
                runtime->result.pending_tasks,
                runtime->result.runtime_ticks,
                runtime->result.timer_callbacks_run);
    }
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        js_rt_dynamic_task_clear(&runtime->bridge,
                           &runtime->bridge.dynamic_scripts[i],
                           true, true);
    }
    budget_free(runtime->bridge.budget, runtime->bridge.media_source);
    runtime->bridge.media_source = NULL;
    js_rt_event_sources_destroy(&runtime->bridge);
    fetch_scheduler_destroy(runtime->bridge.fetch_scheduler);
    runtime->bridge.fetch_scheduler = NULL;
    fetch_scheduler_domain_release(
        runtime->bridge.fetch_scheduler_domain);
    runtime->bridge.fetch_scheduler_domain = NULL;
    script_runtime_clear_script_source_nodes(runtime);
    js_rt_runtime_module_metadata_clear(runtime);
#if defined(CONFIG_EXECUTION_PROFILE)
    if (runtime->runtime != NULL &&
        getenv("TILEFINCH_DUMP_JS_PROFILE") != NULL &&
        !runtime->result.interrupted) {
        JS_DumpExecutionProfile(runtime->runtime);
    }
#endif
    if (runtime->context != NULL) {
        js_lazy_webpack_bundles_destroy(runtime);
        JS_FreeValue(runtime->context,
                     runtime->bridge.trusted_stable_script_wrap);
        JS_FreeValue(runtime->context,
                     runtime->bridge.trusted_retire_native_node_state);
        JS_FreeValue(runtime->context, runtime->bridge.trusted_node_wrap);
        JS_FreeValue(runtime->context, runtime->bridge.current_script);
        JS_FreeValue(runtime->context, runtime->function_to_string);
        for (size_t callback = 0; callback < SCRIPT_HOST_CALLBACK_COUNT;
             callback++) {
            JS_FreeValue(runtime->context,
                         runtime->host_callbacks[callback]);
            runtime->host_callbacks[callback] = JS_UNDEFINED;
        }
        JS_FreeValue(runtime->context, runtime->host_global);
        runtime->host_global = JS_UNDEFINED;
        JS_FreeValue(runtime->context, runtime->dispatch_submit_handle);
        JS_FreeValue(
            runtime->context, runtime->dom_content_loaded_dispatch);
        JS_FreeValue(runtime->context, runtime->media_state_for);
        JS_FreeValue(runtime->context, runtime->media_update);
        JS_FreeValue(runtime->context, runtime->dispatch_input_handle);
        JS_FreeValue(runtime->context,
                     runtime->dispatch_activation_handle);
        JS_FreeValue(runtime->context, runtime->dispatch_handle);
        JS_FreeValue(runtime->context, runtime->dispatch_at);
        JS_FreeValue(runtime->context, runtime->page_scroll_flush);
        JS_FreeValue(runtime->context, runtime->page_scroll_apply);
        runtime->page_scroll_flush = JS_UNDEFINED;
        runtime->page_scroll_apply = JS_UNDEFINED;
        runtime->function_to_string = JS_UNDEFINED;
        runtime->dispatch_submit_handle = JS_UNDEFINED;
        runtime->dom_content_loaded_dispatch = JS_UNDEFINED;
        runtime->media_state_for = JS_UNDEFINED;
        runtime->media_update = JS_UNDEFINED;
        runtime->dispatch_input_handle = JS_UNDEFINED;
        runtime->dispatch_activation_handle = JS_UNDEFINED;
        runtime->dispatch_handle = JS_UNDEFINED;
        runtime->dispatch_at = JS_UNDEFINED;
        runtime->bridge.trusted_stable_script_wrap = JS_UNDEFINED;
        runtime->bridge.trusted_retire_native_node_state = JS_UNDEFINED;
        runtime->bridge.trusted_node_wrap = JS_UNDEFINED;
        runtime->bridge.current_script = JS_NULL;
        JS_FreeContext(runtime->context);
    }
    if (runtime->runtime != NULL) JS_FreeRuntime(runtime->runtime);
    if (!budget_quickjs_pool_destroy(runtime->quickjs_pool)) {
        budget->failure_count++;
    }
    if (runtime->module_opaque_destroy != NULL) {
        runtime->module_opaque_destroy(runtime->module_opaque);
    }
    budget_free(budget, runtime);
}

bool scripts_run_document_at(PocDocument *document, Budget *budget,
                             size_t js_memory_limit, unsigned timeout_ms,
                             const char *document_url,
                             ScriptResult *result)
{
    ScriptRuntime *runtime = script_runtime_create(
        document, budget, js_memory_limit, timeout_ms, document_url, result);
    bool success = runtime != NULL;
    script_runtime_destroy(runtime);
    return success;
}

bool scripts_run_document_at_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    int css_viewport_width, int css_viewport_height,
    int device_width, int device_height, ScriptResult *result)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, css_viewport_width,
                               css_viewport_height, device_width,
                               device_height)) return false;
    return scripts_run_document_at_context(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &viewport, result);
}

bool scripts_run_document_at_context(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ViewportContext *viewport, ScriptResult *result)
{
    return scripts_run_document_at_context_with_policy(
        document, budget, js_memory_limit, timeout_ms, document_url,
        viewport, NULL, result);
}

bool scripts_run_document_at_context_with_policy(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ViewportContext *viewport,
    const ScriptExecutionPolicy *execution_policy, ScriptResult *result)
{
    if (viewport == NULL) return false;
    ScriptRuntimeOptions options = {.viewport = *viewport};
    if (execution_policy != NULL) {
        options.execution_policy = *execution_policy;
    }
    ScriptRuntime *runtime = script_runtime_create_configured(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &options, result);
    bool success = runtime != NULL;
    script_runtime_destroy(runtime);
    return success;
}

bool scripts_run_document(PocDocument *document, Budget *budget,
                          size_t js_memory_limit, unsigned timeout_ms,
                          ScriptResult *result)
{
    return scripts_run_document_at(document, budget, js_memory_limit,
                                   timeout_ms, "", result);
}
