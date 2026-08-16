bool script_runtime_dispatch(ScriptRuntime *runtime, const char *selector,
                             const char *event_type, ScriptResult *result)
{
    if (runtime == NULL || event_type == NULL) return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[2] = {
        JS_NewString(runtime->context, selector == NULL ? "" : selector),
        JS_NewString(runtime->context, event_type)
    };
    JSValue returned = JS_UNDEFINED;
    bool ok = runtime_call_captured(
        runtime, runtime->dispatch_at, "__tilefinchDispatchAt", 2,
        arguments, &returned);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_dispatch_node(ScriptRuntime *runtime,
                                  lxb_dom_node_t *node,
                                  const char *event_type,
                                  ScriptResult *result)
{
    bool trace_failure = false;
#ifndef TILEFINCH_NO_TRACE
    trace_failure =
        getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL;
#endif
    if (runtime == NULL || node == NULL || event_type == NULL) {
        if (trace_failure) {
            fprintf(stderr,
                    "tilefinch-dispatch-node: failure=arguments\n");
        }
        return false;
    }
    int64_t handle = js_rt_bridge_register_node(&runtime->bridge, node);
    if (handle == 0) {
        if (trace_failure) {
            fprintf(stderr, "tilefinch-dispatch-node: failure=handle\n");
        }
        return false;
    }
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[2] = {
        JS_NewInt64(runtime->context, handle),
        JS_NewString(runtime->context, event_type)
    };
    JSValue returned = JS_UNDEFINED;
    bool ok = runtime_call_captured(
        runtime, runtime->dispatch_handle, "__tilefinchDispatchHandle", 2,
        arguments, &returned);
    const char *failure = ok ? NULL : "call";
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok && !js_rt_runtime_run_jobs(runtime)) {
        ok = false;
        failure = "jobs";
    }
    if (ok && !js_rt_runtime_refresh(runtime)) {
        ok = false;
        failure = "refresh";
    }
    if (!ok && trace_failure) {
        fprintf(stderr, "tilefinch-dispatch-node: failure=%s handle=%lld "
                "event=%s error=\"%s\"\n",
                failure == NULL ? "unknown" : failure,
                (long long) handle, event_type, runtime->result.error);
    }
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_dispatch_activation_node(ScriptRuntime *runtime,
                                             lxb_dom_node_t *node,
                                             ScriptResult *result)
{
    return script_runtime_dispatch_pointer_node(
        runtime, node, 0, 0, 0, 0, 0, 0, result);
}

bool script_runtime_dispatch_pointer_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons, ScriptResult *result)
{
    if (runtime == NULL || node == NULL) return false;
    int64_t handle = js_rt_bridge_register_node(&runtime->bridge, node);
    if (handle == 0) return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[7] = {
        JS_NewInt64(runtime->context, handle),
        JS_NewInt32(runtime->context, (int32_t) phase),
        JS_NewInt32(runtime->context, client_x),
        JS_NewInt32(runtime->context, client_y),
        JS_NewInt32(runtime->context, offset_x),
        JS_NewInt32(runtime->context, offset_y),
        JS_NewInt32(runtime->context, (int32_t) buttons)
    };
    JSValue returned = JS_UNDEFINED;
    bool ok = runtime_call_captured(
        runtime, runtime->dispatch_activation_handle,
        "__tilefinchDispatchActivationHandle", 7, arguments, &returned);
    for (size_t i = 0; i < 7; i++)
        JS_FreeValue(runtime->context, arguments[i]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_dispatch_input_node(ScriptRuntime *runtime,
                                        lxb_dom_node_t *node,
                                        const char *event_type,
                                        const char *data,
                                        const char *input_type,
                                        const char *current_value,
                                        ScriptResult *result)
{
    if (runtime == NULL || node == NULL || event_type == NULL
        || input_type == NULL) return false;
    int64_t handle = js_rt_bridge_register_node(&runtime->bridge, node);
    if (handle == 0) return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[5] = {
        JS_NewInt64(runtime->context, handle),
        JS_NewString(runtime->context, event_type),
        data == NULL ? JS_NULL : JS_NewString(runtime->context, data),
        JS_NewString(runtime->context, input_type),
        current_value == NULL
            ? JS_NULL : JS_NewString(runtime->context, current_value)
    };
    JSValue returned = JS_UNDEFINED;
    bool ok = runtime_call_captured(
        runtime, runtime->dispatch_input_handle,
        "__tilefinchDispatchInputHandle", 5, arguments, &returned);
    for (size_t i = 0; i < 5; i++) JS_FreeValue(runtime->context, arguments[i]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_dispatch_submit_node(ScriptRuntime *runtime,
                                         lxb_dom_node_t *form,
                                         lxb_dom_node_t *submitter,
                                         ScriptResult *result)
{
    if (runtime == NULL || form == NULL) return false;
    int64_t form_handle = js_rt_bridge_register_node(&runtime->bridge, form);
    int64_t submitter_handle = submitter == NULL ? 0
        : js_rt_bridge_register_node(&runtime->bridge, submitter);
    if (form_handle == 0 || (submitter != NULL && submitter_handle == 0)) {
        return false;
    }
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[2] = {
        JS_NewInt64(runtime->context, form_handle),
        JS_NewInt64(runtime->context, submitter_handle)
    };
    JSValue returned = JS_UNDEFINED;
    bool ok = runtime_call_captured(
        runtime, runtime->dispatch_submit_handle,
        "__tilefinchDispatchSubmitHandle", 2, arguments, &returned);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_consume_relayout(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    bool needed = runtime->relayout_dirty;
    runtime->relayout_dirty = false;
    return needed;
}

bool script_runtime_consume_mutations(ScriptRuntime *runtime,
                                      ScriptMutationJournal *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (runtime == NULL || runtime->bridge.mutations.count == 0) {
        return false;
    }
    if (result != NULL) *result = runtime->bridge.mutations;
    memset(&runtime->bridge.mutations, 0,
           sizeof(runtime->bridge.mutations));
    return true;
}

bool script_runtime_consume_navigation(ScriptRuntime *runtime, char *url,
                                       size_t url_size, bool *replace)
{
    if (runtime == NULL || url == NULL || url_size == 0
        || !runtime->bridge.navigation_requested) return false;
    snprintf(url, url_size, "%s", runtime->bridge.navigation_url);
    if (replace != NULL) *replace = runtime->bridge.navigation_replace;
    runtime->bridge.navigation_requested = false;
    runtime->bridge.navigation_replace = false;
    runtime->bridge.navigation_url[0] = '\0';
    return true;
}

bool script_runtime_consume_media_request(ScriptRuntime *runtime,
                                          ScriptMediaRequest *request)
{
    if (runtime == NULL || request == NULL
        || !runtime->bridge.media_requested) return false;
    memset(request, 0, sizeof(*request));
    request->command = runtime->bridge.media_command;
    request->node_handle = runtime->bridge.media_node_handle;
    request->value = runtime->bridge.media_value;
    if (runtime->bridge.media_source != NULL) {
        snprintf(request->source, sizeof(request->source), "%s",
                 runtime->bridge.media_source);
    }
    budget_free(runtime->bridge.budget, runtime->bridge.media_source);
    runtime->bridge.media_source = NULL;
    runtime->bridge.media_requested = false;
    runtime->bridge.media_node_handle = 0;
    runtime->bridge.media_value = 0;
    return true;
}

static const char media_update_bootstrap[] =
    "(function(handle,nativeState,currentTime,"
    "duration){const node=globalThis.__tilefinchActiveMediaNode,state="
    "globalThis.__tilefinchActiveMediaState;"
    "if(!(node instanceof HTMLMediaElement)||node.__handle!==handle||!state)"
    "return false;const previousPaused=state.paused,"
    "previousReady=state.readyState;state.currentTime=Number(currentTime)||0;"
    "state.duration=Number.isFinite(Number(duration))?Number(duration):NaN;"
    "state.seeking=false;if(nativeState===0){state.networkState="
    "HTMLMediaElement.NETWORK_LOADING}else if(nativeState===4){"
    "state.networkState=HTMLMediaElement.NETWORK_NO_SOURCE;state.readyState="
    "HTMLMediaElement.HAVE_NOTHING;state.paused=true;"
    "node.dispatchEvent(new Event('error'));delete globalThis."
    "__tilefinchActiveMediaNode;delete globalThis.__tilefinchActiveMediaState;"
    "return true}else{"
    "state.networkState=HTMLMediaElement.NETWORK_IDLE;state.readyState="
    "HTMLMediaElement.HAVE_CURRENT_DATA}if(previousReady==="
    "HTMLMediaElement.HAVE_NOTHING&&state.readyState>="
    "HTMLMediaElement.HAVE_METADATA){node.dispatchEvent(new Event("
    "'loadedmetadata'));node.dispatchEvent(new Event('loadeddata'));"
    "node.dispatchEvent(new Event('canplay'))}if(nativeState===2){"
    "state.paused=false;state.ended=false;if(previousPaused)"
    "node.dispatchEvent(new Event('play'));node.dispatchEvent(new Event("
    "'playing'));node.dispatchEvent(new Event('timeupdate'))}else if("
    "nativeState===3){state.paused=true;state.ended=true;"
    "node.dispatchEvent(new Event('timeupdate'));node.dispatchEvent(new Event("
    "'ended'))}else{state.paused=true;state.ended=false;if(!previousPaused)"
    "node.dispatchEvent(new Event('pause'))}node.dispatchEvent(new Event("
    "'seeked'));if(nativeState===3){delete globalThis."
    "__tilefinchActiveMediaNode;delete globalThis.__tilefinchActiveMediaState;}"
    "return true})";

bool script_runtime_update_media_state(
    ScriptRuntime *runtime, int64_t node_handle, ScriptMediaState state,
    double current_time, double duration)
{
    if (runtime == NULL || runtime->context == NULL || node_handle <= 0)
        return false;
    size_t slot = 0;
    if (!js_rt_bridge_node_slot_for_handle(
            &runtime->bridge, node_handle, &slot)) return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSContext *context = runtime->context;
    if (!JS_IsFunction(context, runtime->media_update)) {
        JSValue installed = JS_Eval(
            context, media_update_bootstrap,
            sizeof(media_update_bootstrap) - 1u,
            "<browser-media-update>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(installed)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
            JS_FreeValue(context, installed);
            return false;
        }
        if (!JS_IsFunction(context, installed)) {
            JS_FreeValue(context, installed);
            return false;
        }
        runtime->media_update = installed;
    }
    JSValue arguments[4] = {
        JS_NewInt64(context, node_handle),
        JS_NewInt32(context, (int32_t) state),
        JS_NewFloat64(context, current_time),
        JS_NewFloat64(context, duration)
    };
    bool ok = runtime_call_captured(
        runtime, runtime->media_update, "media state dispatch",
        4, arguments, NULL);
    for (size_t i = 0; i < 4; i++) JS_FreeValue(context, arguments[i]);
    if (state == SCRIPT_MEDIA_STATE_ENDED
        || state == SCRIPT_MEDIA_STATE_ERROR) {
        bridge_release_native_node_pin(
            &runtime->bridge, node_handle);
    }
    return ok;
}

bool script_runtime_consume_scroll(ScriptRuntime *runtime, int *scroll_y)
{
    if (runtime == NULL || scroll_y == NULL
        || !runtime->bridge.scroll_requested) return false;
    *scroll_y = runtime->bridge.scroll_y;
    if (getenv("TILEFINCH_TRACE_SCROLL") != NULL) {
        fprintf(stderr, "javascript-scroll requested-y=%d\n", *scroll_y);
    }
    runtime->bridge.scroll_requested = false;
    return true;
}

void script_runtime_set_layout(ScriptRuntime *runtime, LayoutDocument *layout,
                               int viewport_height)
{
    if (runtime == NULL) return;
    runtime->bridge.layout = layout;
    runtime->bridge.viewport_height = viewport_height > 0
                                      ? viewport_height : 272;
    if (layout != NULL) {
        for (size_t i = 0; i < runtime->bridge.scroll_intent_count; i++) {
            ElementScrollIntent *intent =
                &runtime->bridge.scroll_intents[i];
            (void) layout_scroll_node(layout, intent->node,
                                      intent->scroll_x, intent->scroll_y);
        }
    }
}

void script_runtime_set_images(ScriptRuntime *runtime,
                               const ImageResources *images)
{
    if (runtime == NULL) return;
    runtime->bridge.images = images;
}

void script_runtime_set_stylesheet(ScriptRuntime *runtime,
                                   const Stylesheet *stylesheet)
{
    if (runtime == NULL) return;
    runtime->bridge.stylesheet = stylesheet;
}

void script_runtime_set_synchronous_layout_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque)
{
    if (runtime == NULL) return;
    runtime->bridge.synchronous_layout = callback;
    runtime->bridge.synchronous_layout_opaque =
        callback == NULL ? NULL : opaque;
}

bool script_runtime_set_page_scroll(ScriptRuntime *runtime, int scroll_y)
{
    if (runtime == NULL) return false;
    int next = scroll_y < 0 ? 0 : scroll_y;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue arguments[2] = {
        JS_NewInt32(runtime->context, next),
        JS_NewBool(runtime->context, true)
    };
    bool ok = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_captured(runtime, runtime->page_scroll_apply,
                                 "page scroll update", 2, arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->bridge.page_scroll_y = next;
    runtime->result.success = ok;
    return ok;
}
