void script_runtime_destroy(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    Budget *budget = runtime->budget;
    /* Defense in depth only: lookup admission also compares the unforgeable
       realm epoch, so correctness never depends on teardown running. */
    js_webgl_realm_epoch_release(&runtime->bridge);
    if (runtime->bridge.stylesheet != NULL)
        ((Stylesheet *) runtime->bridge.stylesheet)->fullscreen_node = NULL;
    runtime->bridge.fullscreen_node_handle = 0;
    tilefinch_game_audio_destroy(runtime->game_audio);
    runtime->game_audio = NULL;
    if (tilefinch_trace_tasks()) {
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
    js_rt_websockets_destroy(&runtime->bridge);
    js_rt_multiplayer_destroy(&runtime->bridge);
    for (size_t i = 0; i < runtime->bridge.async_fetch_count; i++) {
        budget_free(runtime->bridge.budget,
                    runtime->bridge.async_fetches[i].integrity);
        runtime->bridge.async_fetches[i].integrity = NULL;
    }
    runtime->bridge.async_fetch_count = 0;
    fetch_scheduler_destroy(runtime->bridge.fetch_scheduler);
    runtime->bridge.fetch_scheduler = NULL;
    fetch_scheduler_domain_release(
        runtime->bridge.fetch_scheduler_domain);
    runtime->bridge.fetch_scheduler_domain = NULL;
    script_runtime_clear_script_source_nodes(runtime);
    js_rt_runtime_module_metadata_clear(runtime);
#if defined(CONFIG_EXECUTION_PROFILE)
    if (runtime->runtime != NULL &&
        tilefinch_dump_js_profile() &&
        !runtime->result.interrupted) {
        JS_DumpExecutionProfile(runtime->runtime);
    }
#endif
    if (runtime->runtime != NULL &&
        tilefinch_dump_js_memory()) {
        /* Validation-only per-realm census.  The ordinary interactive report
           sees the top-level realm after child frames have already retired;
           reporting at the shared destruction boundary makes the transient
           frame/worker representation visible without changing page code. */
        script_runtime_report_memory(runtime, stderr);
    }
    if (runtime->context != NULL) {
        while (runtime->checkpoint_continuation_count != 0u) {
            size_t slot = runtime->checkpoint_continuation_head;
            JS_FreeValue(runtime->context,
                         runtime->checkpoint_continuations[slot]);
            runtime->checkpoint_continuations[slot] = JS_UNDEFINED;
            runtime->checkpoint_continuation_head = (uint8_t) (
                (slot + 1u) % SCRIPT_CHECKPOINT_CONTINUATION_LIMIT);
            runtime->checkpoint_continuation_count--;
        }
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
        JS_FreeValue(runtime->context,
                     runtime->page_visibility_host_apply);
        JS_FreeValue(runtime->context, runtime->fullscreen_host_exit);
        JS_FreeValue(runtime->context, runtime->game_audio_host_suspend);
        JS_FreeValue(runtime->context, runtime->game_audio_host_complete);
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
        runtime->page_visibility_host_apply = JS_UNDEFINED;
        runtime->fullscreen_host_exit = JS_UNDEFINED;
        runtime->game_audio_host_suspend = JS_UNDEFINED;
        runtime->game_audio_host_complete = JS_UNDEFINED;
        runtime->dispatch_input_handle = JS_UNDEFINED;
        runtime->dispatch_activation_handle = JS_UNDEFINED;
        runtime->dispatch_handle = JS_UNDEFINED;
        runtime->dispatch_at = JS_UNDEFINED;
        runtime->bridge.trusted_stable_script_wrap = JS_UNDEFINED;
        runtime->bridge.trusted_retire_native_node_state = JS_UNDEFINED;
        runtime->bridge.trusted_node_wrap = JS_UNDEFINED;
        runtime->bridge.current_script = JS_NULL;
        for (size_t slot = 0; slot < SCRIPT_WORKER_REALM_LIMIT; slot++) {
            if (runtime->worker_realms[slot].context == NULL) continue;
            JS_FreeValue(runtime->worker_realms[slot].context,
                         runtime->worker_realms[slot].global);
            JS_FreeContext(runtime->worker_realms[slot].context);
            runtime->worker_realms[slot].context = NULL;
            runtime->worker_realms[slot].global = JS_UNDEFINED;
        }
        for (size_t slot = 0; slot < SCRIPT_FRAME_REALM_LIMIT; slot++) {
            if (runtime->frame_realms[slot].context == NULL) continue;
            JS_FreeValue(runtime->frame_realms[slot].context,
                         runtime->frame_realms[slot].global);
            JS_FreeContext(runtime->frame_realms[slot].context);
        }
        JS_FreeContext(runtime->context);
    }
    if (runtime->runtime != NULL) JS_FreeRuntime(runtime->runtime);
    /* Instance finalizers ran above; the idle WebAssembly runtime and its
       pool are released at this boundary, not after each operation. */
    js_wasm_release_idle_runtime();
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
