/* Attribute event entry separately from its checkpoint and DOM refresh.
   These are nested in input/action wall time, never additional totals. */
static bool runtime_trace_dispatch_enabled(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    return true;
#else
    return tilefinch_trace_runtime_steps();
#endif
}

static void runtime_trace_dispatch(const char *event, unsigned phase,
    uint64_t begin, uint64_t call, uint64_t jobs, uint64_t end)
{
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    char message[256];
    snprintf(message, sizeof(message),
        "tilefinch-dispatch-timing: event=%s phase=%u begin-us=%llu end-us=%llu "
        "handler=%llu jobs=%llu refresh=%llu",
        event, phase, (unsigned long long) begin, (unsigned long long) end,
        (unsigned long long) (call - begin),
        (unsigned long long) (jobs - call),
        (unsigned long long) (end - jobs));
    tilefinch_platform_log_message(message);
#else
    (void) event; (void) phase; (void) begin; (void) call; (void) jobs; (void) end;
#endif
}

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
        runtime, runtime->host_callbacks[SCRIPT_HOST_DISPATCH_AT],
        "__tilefinchDispatchAt", 2,
        arguments, &returned);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
    return ok;
}

/* A page that loads a tag manager or an A/B testing client often waits on
   it: an anti-flicker snippet keeps the page hidden until the client says
   it is done, and a link handler navigates from a tag-manager callback.
   When the content blocker refused such a script, answer what the loaded
   script would have (content_blocker_refused_script_shims), before its
   error event: once per realm and kind, except that a Google tag is
   answered once per data layer (a second container may name its own with
   `l=`; one already answered keeps its marked push). Page functions it
   calls run guarded; callbacks run as ordinary page timers. Private,
   first-use code. */
static const char refused_script_answer[] =
    "(function(shims,url){try{var w=globalThis,d=w.document;"
    "if(shims&1){var m=/[?&]l=([A-Za-z_$][\\w$]{0,63})(?:[&#]|$)/.exec(url),"
    "name=m?m[1]:'dataLayer',layer=w[name];"
    "if(layer===undefined)layer=w[name]=[];"
    "if(Array.isArray(layer)&&!(layer.push&&layer.push.__tilefinchAnswer)){var hide=layer.hide;"
    "if(hide&&typeof hide.end==='function')try{hide.end()}catch(e){}"
    "var answer=function(item){try{if(item===null||typeof item!=='object')return;"
    "var callback=item.eventCallback;"
    "if(typeof callback!=='function'&&item[0]==='event'&&item[2]!==null"
    "&&typeof item[2]==='object')callback=item[2].event_callback;"
    "if(typeof callback==='function')setTimeout(callback,0)}catch(e){}};"
    "for(var i=0;i<layer.length;i++)answer(layer[i]);var push=layer.push;"
    "layer.push=function(){for(var j=0;j<arguments.length;j++)answer(arguments[j]);"
    "return push.apply(this,arguments)};"
    "Object.defineProperty(layer.push,'__tilefinchAnswer',{value:true})}}"
    "if(shims&2){var style=d.getElementById('at-body-style');if(style)style.remove()}"
    "if(shims&4){var vwo=w._vwo_code;if(vwo&&typeof vwo.finish==='function')vwo.finish()}"
    "}catch(e){}})";

static void js_rt_answer_refused_script(ScriptRuntime *runtime,
                                        lxb_dom_node_t *node)
{
    DomBridge *bridge = &runtime->bridge;
    if (bridge->session == NULL || bridge->session->content_blocker == NULL
        || bridge->document_url == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->ns != LXB_NS_HTML || node->local_name != LXB_TAG_SCRIPT)
        return;
    size_t length = 0;
    const char *src = document_attribute(node, "src", &length);
    const char *base = js_rt_bridge_calculated_base_url(bridge);
    char reference[TILEFINCH_URL_SERIALIZED_LIMIT];
    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (src == NULL || length == 0 || length >= sizeof(reference)
        || base == NULL) return;
    memcpy(reference, src, length);
    reference[length] = '\0';
    if (!tilefinch_url_resolve(base, reference, resolved, sizeof(resolved)))
        return;
    unsigned shims = content_blocker_refused_script_shims(
        bridge->session->content_blocker, resolved, bridge->document_url)
        & ~(runtime->refused_script_shims & ~CONTENT_BLOCKER_SHIM_GOOGLE_TAG);
    if (shims == 0) return;
    runtime->refused_script_shims |= shims;
    JSValue function = JS_Eval(
        runtime->context, refused_script_answer,
        sizeof(refused_script_answer) - 1u, "<refused-script-answer>",
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TRUSTED);
    if (JS_IsException(function)) {
        JSValue exception = JS_GetException(runtime->context);
        JS_FreeValue(runtime->context, exception);
        return;
    }
    JSValue arguments[2] = {
        JS_NewInt32(runtime->context, (int32_t) shims),
        JS_NewString(runtime->context, resolved)
    };
    if (JS_IsException(arguments[1])) {
        JSValue exception = JS_GetException(runtime->context);
        JS_FreeValue(runtime->context, exception);
    } else {
        (void) runtime_call_captured(runtime, function,
                                     "refused script answer", 2, arguments,
                                     NULL);
        JS_FreeValue(runtime->context, arguments[1]);
    }
    JS_FreeValue(runtime->context, function);
}

bool script_runtime_dispatch_node(ScriptRuntime *runtime,
                                  lxb_dom_node_t *node,
                                  const char *event_type,
                                  ScriptResult *result)
{
    bool trace_failure = false;
#ifndef TILEFINCH_NO_TRACE
    trace_failure =
        tilefinch_trace_script_failures();
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
    if (strcmp(event_type, "error") == 0)
        js_rt_answer_refused_script(runtime, node);
    JSValue arguments[2] = {
        JS_NewInt64(runtime->context, handle),
        JS_NewString(runtime->context, event_type)
    };
    JSValue returned = JS_UNDEFINED;
    bool trace_dispatch = runtime_trace_dispatch_enabled();
    uint64_t dispatch_begin = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
    bool ok = runtime_call_captured(
        runtime, runtime->host_callbacks[SCRIPT_HOST_DISPATCH_HANDLE],
        "__tilefinchDispatchHandle", 2,
        arguments, &returned);
    uint64_t dispatch_call = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
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
    uint64_t dispatch_jobs = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
    if (ok && !js_rt_runtime_refresh(runtime)) {
        ok = false;
        failure = "refresh";
    }
    if (trace_dispatch) runtime_trace_dispatch(event_type, 0,
        dispatch_begin, dispatch_call, dispatch_jobs,
        tilefinch_platform_monotonic_time_us());
    if (!ok && trace_failure) {
        fprintf(stderr, "tilefinch-dispatch-node: failure=%s handle=%lld "
                "event=%s error=\"%s\"\n",
                failure == NULL ? "unknown" : failure,
                (long long) handle, event_type, runtime->result.error);
    }
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
    return ok;
}

/* X on a highlighted element: the Enter press around its keyboard-style
   click (phase 6 of __tilefinchDispatchActivationHandle). */
bool script_runtime_dispatch_activation_node(ScriptRuntime *runtime,
                                             lxb_dom_node_t *node,
                                             ScriptResult *result)
{
    return script_runtime_dispatch_pointer_node(
        runtime, node, 6, 0, 0, 0, 0, 0, result);
}

/* Popunder guard input: a new gesture forgets the previous one's window
   request and hyperlink, then records the HTTP(S) destination of the
   hyperlink (if any) that the activation targets, before any listener can
   change it. */
static void runtime_note_activation_target(DomBridge *bridge,
                                           lxb_dom_node_t *node,
                                           bool gesture_start)
{
    if (gesture_start) {
        bridge->new_window_expires_ms = 0;
        bridge->new_window_site = 0;
        budget_free(bridge->budget, bridge->activation_link_url);
        bridge->activation_link_url = NULL;
        bridge->activation_link_default = false;
    }
    if (bridge->activation_link_url != NULL) return;
    /* Only an HTML hyperlink found within the bounded walk is the pressed
       link; exhausting the walk finds none. */
    lxb_dom_node_t *link = NULL;
    lxb_dom_node_t *at = node;
    for (size_t depth = 0; at != NULL && depth < 64;
         depth++, at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && at->ns == LXB_NS_HTML
            && (at->local_name == LXB_TAG_A
                || at->local_name == LXB_TAG_AREA)
            && lxb_dom_element_has_attribute(lxb_dom_interface_element(at),
                   (const lxb_char_t *) "href", 4)) {
            link = at;
            break;
        }
    }
    if (link == NULL || lxb_dom_element_has_attribute(
            lxb_dom_interface_element(link),
            (const lxb_char_t *) "download", 8)) return;
    size_t length = 0;
    const char *href = document_attribute(link, "href", &length);
    char reference[TILEFINCH_URL_SERIALIZED_LIMIT];
    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
    const char *base = js_rt_bridge_calculated_base_url(bridge);
    if (length >= sizeof(reference) || base == NULL) return;
    if (href != NULL) memcpy(reference, href, length);
    reference[length] = '\0';
    if (!tilefinch_url_resolve(base, reference, resolved, sizeof(resolved))
        || (strncmp(resolved, "https://", 8) != 0
            && strncmp(resolved, "http://", 7) != 0)) return;
    bridge->activation_link_url = js_rt_dynamic_copy_text(
        bridge->budget, resolved);
}

bool script_runtime_dispatch_pointer_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons, ScriptResult *result)
{
    if (runtime == NULL || (node == NULL && phase != 1u)) return false;
    runtime->result.event_dispatch_entered = false;
    if (result != NULL) result->event_dispatch_entered = false;
    int64_t handle = node == NULL
        ? 0 : js_rt_bridge_register_node(&runtime->bridge, node);
    if (node != NULL && handle == 0) return false;
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
    /* Activation-triggering input (HTML): the press of a click, a pointer
       or mouse down, an Enter keydown (6, 7). Not keyup (8). */
    bool trusted_activation = phase == 0u || phase == 2u || phase == 5u
        || phase == 6u || phase == 7u;
    if (trusted_activation) {
        js_rt_bridge_notify_user_activation(&runtime->bridge);
        runtime_note_activation_target(&runtime->bridge, node, phase != 5u);
    }
    runtime->result.event_dispatch_entered = true;
    if (result != NULL) result->event_dispatch_entered = true;
    js_rt_saturating_add_size(
        &runtime->result.event_dispatches_entered, 1u);
    bool trace_dispatch = runtime_trace_dispatch_enabled();
    uint64_t pointer_begin_us = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
    bool ok = runtime_call_captured(
        runtime,
        runtime->host_callbacks[SCRIPT_HOST_DISPATCH_ACTIVATION_HANDLE],
        "__tilefinchDispatchActivationHandle", 7, arguments, &returned);
    for (size_t i = 0; i < 7; i++)
        JS_FreeValue(runtime->context, arguments[i]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    uint64_t pointer_call_us = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    uint64_t pointer_jobs_us = trace_dispatch
        ? tilefinch_platform_monotonic_time_us() : 0;
    if (ok) ok = js_rt_runtime_refresh(runtime);
    if (trace_dispatch)
        runtime_trace_dispatch("pointer", phase, pointer_begin_us,
            pointer_call_us, pointer_jobs_us,
            tilefinch_platform_monotonic_time_us());
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
    return ok;
}

void script_runtime_notify_user_activation(ScriptRuntime *runtime)
{
    if (runtime != NULL)
        js_rt_bridge_notify_user_activation(&runtime->bridge);
}

static bool runtime_clear_page_fullscreen(ScriptRuntime *runtime,
                                          bool dispatch_event)
{
    if (runtime == NULL) return false;
    bool had_fullscreen = runtime->bridge.fullscreen_node_handle != 0;
    runtime->bridge.fullscreen_node_handle = 0;
    if (runtime->bridge.stylesheet != NULL) {
        ((Stylesheet *) runtime->bridge.stylesheet)->fullscreen_node = NULL;
    }
    if (had_fullscreen) runtime->relayout_dirty = true;
    if (!had_fullscreen || !dispatch_event || runtime->context == NULL
        || !JS_IsFunction(
               runtime->context,
               runtime->host_callbacks[SCRIPT_HOST_EXIT_FULLSCREEN])) {
        return had_fullscreen;
    }
    JSValue returned = JS_Call(
        runtime->context,
        runtime->host_callbacks[SCRIPT_HOST_EXIT_FULLSCREEN],
        JS_UNDEFINED, 0, NULL);
    bool ok = !JS_IsException(returned);
    if (!ok) {
        JSValue exception = JS_GetException(runtime->context);
        JS_FreeValue(runtime->context, exception);
    }
    JS_FreeValue(runtime->context, returned);
    return ok;
}

bool script_runtime_page_fullscreen_active(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->bridge.fullscreen_node_handle == 0)
        return false;
    size_t slot = 0;
    if (!js_rt_bridge_node_slot_for_handle(
            &runtime->bridge, runtime->bridge.fullscreen_node_handle, &slot)
        || !bridge_node_is_connected(runtime->bridge.nodes[slot])) {
        (void) runtime_clear_page_fullscreen(runtime, true);
        return false;
    }
    return true;
}

bool script_runtime_take_page_controls_notice(ScriptRuntime *runtime)
{
    if (runtime == NULL) return true;
    bool quiet = runtime->bridge.page_controls_notice_once
        && runtime->bridge.page_controls_notice_shown;
    runtime->bridge.page_controls_notice_once = false;
    runtime->bridge.page_controls_notice_shown = true;
    return !quiet;
}

bool script_runtime_exit_page_fullscreen(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    if (runtime->bridge.fullscreen_node_handle == 0) return true;
    return runtime_clear_page_fullscreen(runtime, true);
}

bool script_runtime_set_page_visibility(ScriptRuntime *runtime, bool visible)
{
    if (runtime == NULL) return false;
    if (runtime->page_visibility_desired == visible) return true;
    runtime->page_visibility_desired = visible;
    uint8_t count = runtime->page_visibility_queue_count;
    if (count < 2u) {
        uint8_t at = (uint8_t) (
            (runtime->page_visibility_queue_head + count) % 2u);
        runtime->page_visibility_queue[at] = visible;
        runtime->page_visibility_queue_count++;
    } else {
        /* Only suspend -> resume can produce two undrained edges in normal
           operation. If a platform toggles again, preserve the oldest edge
           and coalesce the tail to the final observable state. */
        uint8_t head = runtime->page_visibility_queue_head;
        if (runtime->page_visibility_queue[head] == visible) {
            runtime->page_visibility_queue_count = 1u;
        } else {
            uint8_t tail = (uint8_t) ((head + 1u) % 2u);
            runtime->page_visibility_queue[tail] = visible;
        }
    }
    return true;
}

void script_runtime_suspend_game_audio(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    tilefinch_game_audio_suspend(runtime->game_audio);
    if (runtime->context == NULL || runtime->game_audio == NULL
        || !JS_IsFunction(runtime->context,
                          runtime->game_audio_host_suspend)) return;
    {
        JSValue returned = JS_Call(
            runtime->context, runtime->game_audio_host_suspend,
            JS_UNDEFINED, 0, NULL);
        if (JS_IsException(returned)) {
            JSValue exception = JS_GetException(runtime->context);
            JS_FreeValue(runtime->context, exception);
        }
        JS_FreeValue(runtime->context, returned);
    }
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
        runtime,
        runtime->host_callbacks[SCRIPT_HOST_DISPATCH_INPUT_HANDLE],
        "__tilefinchDispatchInputHandle", 5, arguments, &returned);
    for (size_t i = 0; i < 5; i++) JS_FreeValue(runtime->context, arguments[i]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    if (ok) ok = js_rt_runtime_refresh(runtime);
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
    return ok;
}

bool script_runtime_dispatch_submit_node(ScriptRuntime *runtime,
                                         lxb_dom_node_t *form,
                                         lxb_dom_node_t *submitter,
                                         ScriptResult *result)
{
    if (runtime == NULL || form == NULL) return false;
    runtime->result.event_dispatch_entered = false;
    if (result != NULL) result->event_dispatch_entered = false;
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
    runtime->result.event_dispatch_entered = true;
    if (result != NULL) result->event_dispatch_entered = true;
    js_rt_saturating_add_size(
        &runtime->result.event_dispatches_entered, 1u);
    bool trace_dispatch = runtime_trace_dispatch_enabled();
    uint64_t begin = trace_dispatch ? tilefinch_platform_monotonic_time_us() : 0;
    bool ok = runtime_call_captured(
        runtime,
        runtime->host_callbacks[SCRIPT_HOST_DISPATCH_SUBMIT_HANDLE],
        "__tilefinchDispatchSubmitHandle", 2, arguments, &returned);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->result.last_event_cancelled =
        JS_ToBool(runtime->context, returned) <= 0;
    JS_FreeValue(runtime->context, returned);
    uint64_t call = trace_dispatch ? tilefinch_platform_monotonic_time_us() : 0;
    if (ok) ok = js_rt_runtime_run_jobs(runtime);
    uint64_t jobs = trace_dispatch ? tilefinch_platform_monotonic_time_us() : 0;
    if (ok) ok = js_rt_runtime_refresh(runtime);
    if (trace_dispatch)
        runtime_trace_dispatch("submit", 0, begin, call, jobs,
            tilefinch_platform_monotonic_time_us());
    runtime->result.success = ok;
    js_rt_runtime_update_frame_result(runtime, result);
    return ok;
}

bool script_runtime_consume_relayout(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    bool needed = runtime->relayout_dirty;
    runtime->relayout_dirty = false;
    return needed;
}

bool script_runtime_take_refresh_meta_mutation(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    bool mutated = runtime->bridge.refresh_meta_mutated;
    runtime->bridge.refresh_meta_mutated = false;
    return mutated;
}

bool script_runtime_has_pending_mutations(const ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    const ScriptMutationJournal *journal = runtime->bridge.mutations;
    return runtime->relayout_dirty || runtime->bridge.mutations_unrecorded
        || (journal != NULL && (journal->count != 0 || journal->overflowed));
}

const ScriptMutationJournal *script_runtime_pending_journal(
    const ScriptRuntime *runtime)
{
    return runtime == NULL || runtime->bridge.mutations_unrecorded ? NULL
        : runtime->bridge.mutations;
}

bool script_runtime_has_pending_resource_mutations(const ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    const ScriptMutationJournal *journal = runtime->bridge.mutations;
    return runtime->bridge.mutations_unrecorded
        || (journal != NULL
            && (journal->resource_rebuild_required
                || journal->image_resource_scan_required
                || journal->conservative_resource_scan));
}

bool script_runtime_consume_mutations(ScriptRuntime *runtime,
                                      ScriptMutationJournal *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (runtime == NULL) return false;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->mutations_unrecorded) {
        /* The Budget refused the journal: nothing since the last consume
           was recorded, so report what a full journal that lost its
           overflow roots reports, plus every resource cause. */
        bridge->mutations_unrecorded = false;
        if (result != NULL) {
            result->overflowed = true;
            result->overflow_roots_lost = true;
            result->retirement_unobserved = true;
            result->conservative_resource_scan = true;
            result->resource_rebuild_required = true;
            result->stylesheet_rebuild_required = true;
            result->image_rebuild_required = true;
            result->image_resource_scan_required = true;
            result->image_resource_refresh_required = true;
            result->image_reveal_possible = true;
            result->relational_selector_sensitive = true;
        }
        return true;
    }
    if (bridge->mutations == NULL || bridge->mutations->count == 0)
        return false;
    if (result != NULL) *result = *bridge->mutations;
    memset(bridge->mutations, 0, sizeof(*bridge->mutations));
    return true;
}

bool script_runtime_consume_navigation(ScriptRuntime *runtime, char *url,
                                       size_t url_size, bool *replace,
                                       bool *user_activated)
{
    if (runtime == NULL || url == NULL || url_size == 0
        || !runtime->bridge.navigation_requested) return false;
    JS_RT_XHR_COUNT(runtime, navigation_consumed);
    snprintf(url, url_size, "%s", runtime->bridge.navigation_url);
    if (replace != NULL) *replace = runtime->bridge.navigation_replace;
    if (user_activated != NULL)
        *user_activated = runtime->bridge.navigation_user_activated;
    runtime->bridge.navigation_requested = false;
    runtime->bridge.navigation_replace = false;
    runtime->bridge.navigation_user_activated = false;
    runtime->bridge.navigation_follows_link = false;
    runtime->bridge.navigation_url[0] = '\0';
    return true;
}

void script_runtime_note_link_default(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    bridge->activation_link_default = true;
    if (!bridge->navigation_follows_link) return;
    bridge->navigation_requested = false;
    bridge->navigation_replace = false;
    bridge->navigation_user_activated = false;
    bridge->navigation_follows_link = false;
    bridge->navigation_url[0] = '\0';
}

bool script_runtime_has_pending_navigation(const ScriptRuntime *runtime)
{
    return runtime != NULL && runtime->bridge.navigation_requested;
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
    request->audio_only = runtime->bridge.media_audio_only;
    request->mode = runtime->bridge.media_mode;
    request->credentials = runtime->bridge.media_credentials;
    if (runtime->bridge.media_source != NULL) {
        snprintf(request->source, sizeof(request->source), "%s",
                 runtime->bridge.media_source);
    }
    memcpy(request->title, runtime->bridge.media_title,
           sizeof(request->title));
    runtime->bridge.media_title[0] = '\0';
    budget_free(runtime->bridge.budget, runtime->bridge.media_source);
    runtime->bridge.media_source = NULL;
    runtime->bridge.media_requested = false;
    runtime->bridge.media_audio_only = false;
    runtime->bridge.media_node_handle = 0;
    runtime->bridge.media_value = 0;
    return true;
}

bool script_runtime_has_pending_media_request(const ScriptRuntime *runtime)
{
    return runtime != NULL && runtime->bridge.media_requested;
}

static const char media_update_bootstrap[] =
    "(function(stateFor,wrap){return function(handle,nativeState,currentTime,"
    "duration){const node=wrap(handle),state=stateFor(node);"
    "if(!(node instanceof HTMLMediaElement)||!state)"
    "return false;const previousPaused=state.paused,"
    "previousReady=state.readyState,previousSeeking=state.seeking,"
    "previousEnded=state.ended,previousNative=state.nativeState,"
    "previousNetwork=state.networkState;"
    "state.nativeState=nativeState;"
    "state.currentTime=Number(currentTime)||0;"
    "state.duration=Number.isFinite(Number(duration))?Number(duration):NaN;"
    "if(nativeState===0){state.networkState=HTMLMediaElement.NETWORK_LOADING;"
    "if(!previousPaused&&previousNetwork!==HTMLMediaElement.NETWORK_LOADING)"
    "node.dispatchEvent(new Event("
    "'waiting'));return true}state.seeking=false;if(nativeState===4){"
    "state.networkState=HTMLMediaElement.NETWORK_NO_SOURCE;state.readyState="
    "HTMLMediaElement.HAVE_NOTHING;state.paused=true;"
    "if(previousNative!==4)node.dispatchEvent(new Event('error'));"
    "return true}else{"
    "state.networkState=HTMLMediaElement.NETWORK_IDLE;state.readyState="
    "HTMLMediaElement.HAVE_CURRENT_DATA}if(previousReady==="
    "HTMLMediaElement.HAVE_NOTHING&&state.readyState>="
    "HTMLMediaElement.HAVE_METADATA){node.dispatchEvent(new Event("
    "'loadedmetadata'));node.dispatchEvent(new Event('loadeddata'));"
    "node.dispatchEvent(new Event('canplay'))}if(nativeState===2){"
    "state.paused=false;state.ended=false;if(previousPaused)"
    "node.dispatchEvent(new Event('play'));if(previousPaused||previousReady==="
    "HTMLMediaElement.HAVE_NOTHING||previousNative===0)"
    "node.dispatchEvent(new Event('playing'));"
    "node.dispatchEvent(new Event('timeupdate'))}else if("
    "nativeState===3){state.paused=true;state.ended=true;"
    "if(!previousEnded){node.dispatchEvent(new Event('timeupdate'));"
    "node.dispatchEvent(new Event('ended'))}}else{state.paused=true;"
    "state.ended=false;if(!previousPaused)"
    "node.dispatchEvent(new Event('pause'))}if(previousSeeking)"
    "node.dispatchEvent(new Event('seeked'));"
    "return true}})";

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
        JSValue factory = JS_Eval(
            context, media_update_bootstrap,
            sizeof(media_update_bootstrap) - 1u,
            "<browser-media-update>",
            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TRUSTED);
        if (JS_IsException(factory)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
            JS_FreeValue(context, factory);
            return false;
        }
        JSValue factory_arguments[2] = {
            JS_DupValue(context,
                        runtime->host_callbacks[SCRIPT_HOST_MEDIA_STATE_FOR]),
            JS_DupValue(context,
                        runtime->host_callbacks[SCRIPT_HOST_WRAP_NODE])
        };
        JSValue installed = JS_UNDEFINED;
        bool installed_ok = runtime_call_captured(
            runtime, factory, "media state installer", 2,
            factory_arguments, &installed);
        JS_FreeValue(context, factory_arguments[1]);
        JS_FreeValue(context, factory_arguments[0]);
        JS_FreeValue(context, factory);
        if (!installed_ok || !JS_IsFunction(context, installed)) {
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
    if (tilefinch_trace_scroll()) {
        fprintf(stderr, "javascript-scroll requested-y=%d\n", *scroll_y);
    }
    runtime->bridge.scroll_requested = false;
    return true;
}

static void geometry_inputs_capture(ScriptRuntime *runtime)
{
    const DomBridge *bridge = &runtime->bridge;
    runtime->geometry_content_generation = bridge->document == NULL ? 0
        : bridge->document->content_generation;
    runtime->geometry_style_generation = document_style_generation();
    runtime->geometry_sheet_generation = bridge->stylesheet == NULL ? 0
        : bridge->stylesheet->build_generation;
    runtime->geometry_container_generation = bridge->stylesheet == NULL ? 0
        : bridge->stylesheet->container_state_generation;
    runtime->geometry_font_generation = font_generation();
    runtime->geometry_serial++;
}

/* The incumbent's input generations, while a scratch snapshot's replace
   them: put back with the incumbent (script_runtime_keep_published_layout)
   they say again what that layout was built from. */
static void geometry_inputs_save(ScriptRuntime *runtime)
{
    runtime->geometry_incumbent_inputs[0] = runtime->geometry_content_generation;
    runtime->geometry_incumbent_inputs[1] = runtime->geometry_style_generation;
    runtime->geometry_incumbent_inputs[2] = runtime->geometry_sheet_generation;
    runtime->geometry_incumbent_inputs[3] = runtime->geometry_container_generation;
    runtime->geometry_incumbent_inputs[4] = runtime->geometry_font_generation;
}

static void geometry_inputs_restore(ScriptRuntime *runtime)
{
    runtime->geometry_content_generation = runtime->geometry_incumbent_inputs[0];
    runtime->geometry_style_generation = runtime->geometry_incumbent_inputs[1];
    runtime->geometry_sheet_generation = runtime->geometry_incumbent_inputs[2];
    runtime->geometry_container_generation = runtime->geometry_incumbent_inputs[3];
    runtime->geometry_font_generation = runtime->geometry_incumbent_inputs[4];
    runtime->geometry_serial++;
}

void script_runtime_set_layout(ScriptRuntime *runtime, LayoutDocument *layout,
                               int viewport_height)
{
    if (runtime == NULL) return;
    if (runtime->geometry_layout != NULL) {
        layout_destroy(runtime->geometry_layout);
        budget_free(runtime->budget, runtime->geometry_layout);
        runtime->geometry_layout = NULL;
        runtime->geometry_incumbent = NULL;
    }
    runtime->bridge.layout = layout;
    runtime->bridge.layout_region_end = 0;
    runtime->geometry_incumbent = NULL;
    runtime->geometry_paint_stale = false;
    runtime->geometry_refused = false;
    geometry_inputs_capture(runtime);
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

static bool geometry_environment_current(const ScriptRuntime *runtime,
    const ViewportContext *viewport, const LayoutDocument *layout)
{
    const DomBridge *bridge = &runtime->bridge;
    return layout != NULL && bridge->layout == layout
        && bridge->document != NULL && bridge->stylesheet != NULL
        && layout->budget != NULL
        && runtime->geometry_style_generation == document_style_generation()
        && runtime->geometry_sheet_generation == bridge->stylesheet->build_generation
        && runtime->geometry_container_generation == bridge->stylesheet->container_state_generation
        && runtime->geometry_font_generation == font_generation()
        && layout->fonts == bridge->fonts
        && layout->web_fonts == stylesheet_web_font_set(bridge->stylesheet)
        && layout->viewport.css_width == viewport->css_width
        && layout->viewport.css_height == viewport->css_height
        && layout->viewport.device_width == viewport->device_width
        && layout->viewport.device_height == viewport->device_height
        && layout->viewport.scale_numerator == viewport->scale_numerator
        && layout->viewport.scale_denominator == viewport->scale_denominator;
}

static bool geometry_layout_current(const ScriptRuntime *runtime,
                                     const ViewportContext *viewport)
{
    const LayoutDocument *layout = runtime->geometry_layout != NULL
        ? runtime->geometry_layout : runtime->geometry_incumbent;
    return geometry_environment_current(runtime, viewport, layout)
        && runtime->geometry_content_generation
            == runtime->bridge.document->content_generation;
}

void script_runtime_set_layout_region(ScriptRuntime *runtime, int region_end)
{
    if (runtime != NULL)
        runtime->bridge.layout_region_end = region_end > 0 ? region_end : 0;
}

#ifdef TILEFINCH_LAYOUT_DIFF
/* Study instrumentation: why script's forced geometry could or could not
   be adopted by the relayout that follows it. */
const char *script_runtime_geometry_adoption_state(
    const ScriptRuntime *runtime, const ViewportContext *viewport)
{
    if (runtime == NULL) return "no-runtime";
    if (runtime->geometry_layout == NULL) return "no-geometry";
    if (runtime->geometry_paint_stale) return "paint-stale";
    const LayoutDocument *layout = runtime->geometry_layout;
    if (!geometry_environment_current(runtime, viewport, layout)) {
        const DomBridge *bridge = &runtime->bridge;
        if (bridge->layout != layout) return "environment-changed:incumbent";
        if (bridge->document == NULL || bridge->stylesheet == NULL || layout->budget == NULL)
            return "environment-changed:missing";
        if (runtime->geometry_style_generation != document_style_generation())
            return "environment-changed:style";
        if (runtime->geometry_sheet_generation != bridge->stylesheet->build_generation)
            return "environment-changed:sheet";
        if (runtime->geometry_container_generation
            != bridge->stylesheet->container_state_generation)
            return "environment-changed:container";
        if (runtime->geometry_font_generation != font_generation()
            || layout->fonts != bridge->fonts
            || layout->web_fonts != stylesheet_web_font_set(bridge->stylesheet))
            return "environment-changed:font";
        return "environment-changed:viewport";
    }
    if (runtime->geometry_content_generation
        != runtime->bridge.document->content_generation)
        return "dom-changed-after-read";
    return "current";
}
#endif

bool script_runtime_take_geometry_layout(ScriptRuntime *runtime,
                                         const ViewportContext *viewport,
                                         LayoutDocument *layout)
{
    if (runtime == NULL || viewport == NULL || layout == NULL
        || layout->budget != NULL || runtime->geometry_layout == NULL
        || runtime->geometry_paint_stale
        || !geometry_layout_current(runtime, viewport))
        return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_GEOMETRY_LAYOUT_ADOPTION") != NULL) return false;
#endif
    *layout = *runtime->geometry_layout;
    budget_free(runtime->budget, runtime->geometry_layout);
    runtime->geometry_layout = NULL;
    runtime->bridge.layout = runtime->geometry_incumbent;
    runtime->geometry_incumbent = NULL;
    return true;
}

/* Room that must come free before a refused geometry build is worth trying
   again with unchanged inputs: the step its last refused request was. */
#define GEOMETRY_REFUSAL_RETRY_ROOM ((size_t) 64u * 1024u)

static void geometry_refusal_record(ScriptRuntime *runtime,
                                    const ViewportContext *viewport)
{
    const DomBridge *bridge = &runtime->bridge;
    runtime->geometry_refused = true;
    runtime->geometry_refused_content_generation =
        bridge->document->content_generation;
    runtime->geometry_refused_style_generation = document_style_generation();
    runtime->geometry_refused_sheet_generation =
        bridge->stylesheet->build_generation;
    runtime->geometry_refused_font_generation = font_generation();
    runtime->geometry_refused_container_generation =
        bridge->stylesheet->container_state_generation;
    runtime->geometry_refused_css_width = viewport->css_width;
    runtime->geometry_refused_css_height = viewport->css_height;
    runtime->geometry_refused_room = budget_remaining(runtime->budget);
}

/* Whether the same build, on the same inputs, would only be refused again.
   A page reading a box per element in a loop would otherwise pay a partial
   layout per read, each ending at the same refused allocation. */
static bool geometry_refusal_stands(const ScriptRuntime *runtime,
                                    const ViewportContext *viewport)
{
    const DomBridge *bridge = &runtime->bridge;
    return runtime->geometry_refused
        && runtime->geometry_refused_content_generation
            == bridge->document->content_generation
        && runtime->geometry_refused_style_generation
            == document_style_generation()
        && runtime->geometry_refused_sheet_generation
            == bridge->stylesheet->build_generation
        && runtime->geometry_refused_font_generation == font_generation()
        && runtime->geometry_refused_container_generation
            == bridge->stylesheet->container_state_generation
        && runtime->geometry_refused_css_width == viewport->css_width
        && runtime->geometry_refused_css_height == viewport->css_height
        && budget_remaining(runtime->budget)
            < runtime->geometry_refused_room + GEOMETRY_REFUSAL_RETRY_ROOM;
}

/* The load lifecycle's layout allowance (STYLE_INVALIDATION.md, "Reads
   during page load"): a first build whose estimated work is at most
   LOADING_LAYOUT_WORK_FLOOR, then builds that together do
   LOADING_LAYOUT_SHARE_PERCENT of the first one's work, or the floor on a
   small page. LOADING_LAYOUT_UNITS_PER_NODE is what laying out a node of
   ordinary text content counts (15-19 measured on text pages; less where
   a subtree is hidden). */
enum {
    LOADING_LAYOUT_SHARE_PERCENT = 50, LOADING_LAYOUT_WORK_FLOOR = 16384,
    LOADING_LAYOUT_UNITS_PER_NODE = 16
};

/* A build's work, counted rather than timed so that a replay spends the
   allowance alike every run: the selector candidates its style resolution
   tested plus its layout work units (nodes and text laid out). On the
   host each costs about 0.1 us, so the sum follows a build's time. */
static uint64_t geometry_build_work(const LayoutDocument *layout)
{
    return layout->performance.style_rule_candidates
        + layout->layout_work_units;
}

/* The live document's nodes and elements, counted until the nodes alone
   would put the estimate past the floor (then it is refused whatever the
   rest holds). The document's own totals are the parser's, refreshed only
   between script turns, so a tree a script built before its read would
   pass for the empty shell it replaced. The count is kept until the tree
   changes (tree_version), so a style write and read in a loop walks it
   once. */
static void loading_dom_count(DomBridge *bridge, size_t *nodes,
                              size_t *elements)
{
    enum { CAP = LOADING_LAYOUT_WORK_FLOOR / LOADING_LAYOUT_UNITS_PER_NODE + 1 };
    if (!bridge->loading_count_valid
        || bridge->loading_count_version != bridge->tree_version) {
        size_t counted = 0, counted_elements = 0;
        lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
        for (lxb_dom_node_t *node = root; node != NULL && counted < CAP;) {
            counted++;
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) counted_elements++;
            if (node->first_child != NULL) {
                node = node->first_child;
                continue;
            }
            while (node != root && node->next == NULL) node = node->parent;
            node = node == root ? NULL : node->next;
        }
        bridge->loading_count_nodes = counted;
        bridge->loading_count_elements = counted_elements;
        bridge->loading_count_version = bridge->tree_version;
        bridge->loading_count_valid = true;
    }
    *nodes = bridge->loading_count_nodes;
    *elements = bridge->loading_count_elements;
}

/* A first build's work, estimated before it runs: every element tests the
   rules no tag, id or class selects (stylesheet_rules_tested_per_element,
   which builds the rule index as the build would), and every node is laid
   out. It leaves out what the keys add and how much text a node holds, so
   it is a lower bound of what the build counts. */
static uint64_t geometry_build_estimate(DomBridge *bridge)
{
    size_t nodes = 0, elements = 0;
    loading_dom_count(bridge, &nodes, &elements);
    uint64_t per_element = stylesheet_rules_tested_per_element(
        (Stylesheet *) bridge->stylesheet);
    return (uint64_t) elements * per_element
        + (uint64_t) nodes * LOADING_LAYOUT_UNITS_PER_NODE;
}

/* Whether a build may start while the page loads: only for a read of the
   page's own (its handler's, or browser code that handler called), never
   for the browser's own work, and only while the work fits. A first
   build is admitted by its estimate; a later one without the reuse cache
   starts cold, as costly as the first, and must fit what is left; one
   with it (often a fraction, unknown until it ran) may start while any
   is left. */
static bool loading_build_admitted(ScriptRuntime *runtime, bool cold)
{
    DomBridge *bridge = &runtime->bridge;
    if (!JS_IsAuthorCodeRunning(runtime->context)) return false;
    if (bridge->loading_layout_first == 0)
        return geometry_build_estimate(bridge) <= LOADING_LAYOUT_WORK_FLOOR;
    return bridge->loading_layout_allowance
        >= (cold ? bridge->loading_layout_first : 1u);
}

/* Script reads the incumbent again (the layoutless estimate when there is
   none), as it was before the snapshot. */
static void geometry_snapshot_release(ScriptRuntime *runtime)
{
    if (runtime->geometry_layout == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    layout_destroy(runtime->geometry_layout);
    budget_free(runtime->budget, runtime->geometry_layout);
    runtime->geometry_layout = NULL;
    bridge->layout = runtime->geometry_incumbent;
    runtime->geometry_incumbent = NULL;
    runtime->geometry_paint_stale = false;
    geometry_inputs_restore(runtime);
    /* Element scrolls set meanwhile moved only the snapshot's boxes. */
    for (size_t i = 0; bridge->layout != NULL
                       && i < bridge->scroll_intent_count; i++) {
        ElementScrollIntent *intent = &bridge->scroll_intents[i];
        (void) layout_scroll_node(bridge->layout, intent->node,
                                  intent->scroll_x, intent->scroll_y);
    }
}

#ifdef TILEFINCH_LAYOUT_DIFF
/* TILEFINCH_VERIFY_GEOMETRY_PROOF (host and validation builds): a read that
   a proof answers without a build builds anyway, into a temporary laid out
   from the same cache, and compares every box and command with the layout
   the read answers from (layout_validation_compare). Command differences
   on paint the read never adopts (`paint_stale`) are reported apart. */
static void geometry_proof_verify(ScriptRuntime *runtime,
                                  const ViewportContext *viewport,
                                  LayoutReuseCache *reuse,
                                  LayoutDocument *reading, const char *kind,
                                  bool paint_stale)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("TILEFINCH_VERIFY_GEOMETRY_PROOF") != NULL;
    if (!enabled || reading == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    /* The build below moves the cache's coverage to itself: hand it back
       to the layout read when the two are equal, never otherwise. */
    bool covered = layout_reuse_proof_covers(reuse, reading);
    LayoutDocument *fresh = budget_calloc_category(
        runtime->budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*fresh));
    bool built = false;
    if (fresh != NULL) {
        layout_validation_set_cause("verify");
        built = layout_build_context_reuse(
            fresh, runtime->budget, bridge->document, bridge->stylesheet,
            bridge->fonts, bridge->images, viewport, reuse);
    }
    char detail[160] = "";
    const char *verdict = "refused";
    if (built) {
        layout_transfer_scroll_state(reading, fresh);
        for (size_t i = 0; i < bridge->scroll_intent_count; i++) {
            ElementScrollIntent *intent = &bridge->scroll_intents[i];
            (void) layout_scroll_node(fresh, intent->node,
                                      intent->scroll_x, intent->scroll_y);
        }
        LayoutValidationCompare same = layout_validation_compare(
            reading, fresh, detail, sizeof(detail));
        verdict = same == LAYOUT_VALIDATION_SAME ? "match"
            : same == LAYOUT_VALIDATION_BOXES_DIFFER ? "mismatch"
            : paint_stale ? "match-stale-paint" : "mismatch-commands";
        /* The cache now vouches for the fresh build; it equals the one read. */
        if (covered && same != LAYOUT_VALIDATION_BOXES_DIFFER
            && fresh->reuse_serial != 0)
            reading->reuse_serial = fresh->reuse_serial;
    }
    char message[320];
    snprintf(message, sizeof(message),
             "tilefinch-geometry-proof-verify: kind=%s result=%s boxes=%zu %s",
             kind, verdict, reading->node_box_count, detail);
    tilefinch_platform_log_message(message);
    if (fresh != NULL) {
        layout_destroy(fresh);
        budget_free(runtime->budget, fresh);
    }
}
#endif

const LayoutDocument *script_runtime_geometry_reading(
    const ScriptRuntime *runtime)
{
    return runtime == NULL ? NULL : runtime->bridge.layout;
}

bool script_runtime_flush_geometry_layout_reuse(
    ScriptRuntime *runtime, const ViewportContext *viewport,
    ScriptGeometryReuseCallback prepare, ScriptGeometryReuseRelease release,
    void *opaque)
{
    if (runtime == NULL || viewport == NULL) return false;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->document == NULL || bridge->stylesheet == NULL) return false;
    if (geometry_layout_current(runtime, viewport)) {
#ifndef TILEFINCH_NO_TRACE
        if (bridge->result != NULL) bridge->result->geometry_snapshot_hits++;
#endif
        return true;
    }
    if (geometry_refusal_stands(runtime, viewport)) return false;
    /* Loading listeners can update and then measure the document, but
       their reads must not cost a page layouts it cannot afford before it
       can be published, nor may the browser's own work during load. A
       read no build is admitted for, and that cannot prove the snapshot
       still holds, drops it: script then reads the estimate, which follows
       its writes, never a superseded snapshot. */
    bool loading = bridge->synchronous_layout_loading;
    bool provable = prepare != NULL
        && geometry_environment_current(runtime, viewport, bridge->layout)
        && bridge->layout->content_limit_y == 0;
    if (loading && !provable && !loading_build_admitted(runtime, false)) {
        geometry_snapshot_release(runtime);
        return false;
    }
#ifndef TILEFINCH_NO_TRACE
    bool trace_geometry = tilefinch_trace_runtime_steps();
    uint64_t trace_begin = trace_geometry
        ? tilefinch_platform_monotonic_time_us() : 0;
#endif
    bool unchanged = false;
    LayoutReuseCache *reuse = prepare == NULL ? NULL
        : prepare(opaque, !bridge->mutations_unrecorded
                && bridge->mutations != NULL
                && (bridge->mutations->count != 0
                    || bridge->mutations->overflowed)
            ? bridge->mutations : NULL,
            provable ? &unchanged : NULL);
    /* A truncated layout is never borrowed as script's geometry (see
       js_rt_bridge_layout): it is completed into the scratch below. */
    if (unchanged && bridge->layout != NULL && bridge->layout->budget != NULL
        && bridge->layout->content_limit_y == 0) {
#ifdef TILEFINCH_LAYOUT_DIFF
        geometry_proof_verify(runtime, viewport, reuse, bridge->layout,
                              reuse == NULL ? "focus" : "proof",
                              runtime->geometry_paint_stale);
#endif
        /* Borrow only: never install this old paint as a fresh publication.
           The existing input generations cache the proof across subsequent
           reads in this turn; any new DOM/style/font/viewport input breaks it. */
        if (runtime->geometry_layout == NULL)
            runtime->geometry_incumbent = bridge->layout;
        geometry_inputs_capture(runtime);
        runtime->geometry_paint_stale = true;
#ifndef TILEFINCH_NO_TRACE
        if (bridge->result != NULL) bridge->result->geometry_snapshot_hits++;
#endif
        return true;
    }
    if (loading && !loading_build_admitted(runtime, reuse == NULL)) {
        geometry_snapshot_release(runtime);
        return false;
    }
    /* A build is where a read's memory peaks, and it usually follows a
       mutation, which already emptied the geometry records: release their
       table for it (the walk after the build stores afresh). */
    js_rt_bridge_computed_style_release_records(bridge);
    if (runtime->geometry_layout == NULL) {
        runtime->geometry_layout = budget_calloc_category(
            runtime->budget, BUDGET_CATEGORY_LAYOUT, 1,
            sizeof(*runtime->geometry_layout));
        if (runtime->geometry_layout == NULL) return false;
        runtime->geometry_incumbent = bridge->layout;
        geometry_inputs_save(runtime);
    } else {
        /* The previous snapshot is no longer authoritative. Release it
           before rebuilding rather than admitting two scratch layouts. */
        bridge->layout = runtime->geometry_incumbent;
        layout_destroy(runtime->geometry_layout);
        memset(runtime->geometry_layout, 0, sizeof(*runtime->geometry_layout));
    }
#ifndef TILEFINCH_NO_TRACE
    uint64_t trace_prepared = trace_geometry
        ? tilefinch_platform_monotonic_time_us() : 0;
#endif
    size_t failures = runtime->budget->failure_count;
    size_t injections = runtime->budget->injected_failure_count;
#ifdef TILEFINCH_LAYOUT_DIFF
    layout_validation_set_cause("geometry");
    {
        char note[48];
        snprintf(note, sizeof(note), "mutations=%zu%s",
                 bridge->mutations == NULL ? (size_t) 0
                     : bridge->mutations->count,
                 bridge->mutations != NULL && bridge->mutations->overflowed
                     ? "+overflow" : "");
        layout_validation_note(note);
    }
#endif
    bool built = layout_build_context_reuse(
        runtime->geometry_layout, runtime->budget, bridge->document,
        bridge->stylesheet, bridge->fonts, bridge->images, viewport, reuse);
    bool refused = !built && !runtime->geometry_layout->performance.cancelled
        && runtime->budget->failure_count > failures
        && runtime->budget->injected_failure_count == injections;
    if (!built) {
        layout_destroy(runtime->geometry_layout);
        memset(runtime->geometry_layout, 0, sizeof(*runtime->geometry_layout));
    }
    /* An accelerator must not cost the page its geometry. The cache's own
       memory and growth can be what the build was refused for, and whether
       the pressure check before the build had already evicted it can come
       down to a few KiB. */
    if (refused && reuse != NULL && release != NULL) {
        release(opaque);
        built = layout_build_context_reuse(
            runtime->geometry_layout, runtime->budget, bridge->document,
            bridge->stylesheet, bridge->fonts, bridge->images, viewport, NULL);
        refused = !built && !runtime->geometry_layout->performance.cancelled
            && runtime->budget->injected_failure_count == injections;
        if (!built) {
            layout_destroy(runtime->geometry_layout);
            memset(runtime->geometry_layout, 0,
                   sizeof(*runtime->geometry_layout));
        }
    }
    if (!built) {
        if (refused) geometry_refusal_record(runtime, viewport);
        return false;
    }
    runtime->geometry_refused = false;
#ifndef TILEFINCH_NO_TRACE
    if (trace_geometry) {
        const LayoutPerformance *p = &runtime->geometry_layout->performance;
        fprintf(stderr, "geometry-snapshot prepare-us=%llu build-us=%llu "
            "flow-us=%llu style-us=%llu styles=%llu/%llu mutations=%zu "
            "overflow=%d resource=%d commands=%zu reuse=%d work=%llu loading=%d\n",
            (unsigned long long)(trace_prepared - trace_begin),
            (unsigned long long)(tilefinch_platform_monotonic_time_us() - trace_prepared),
            (unsigned long long)p->flow_us,
            (unsigned long long)p->style_resolve_us,
            (unsigned long long)p->style_cache_hits,
            (unsigned long long)p->style_cache_misses,
            bridge->mutations == NULL ? 0 : bridge->mutations->count,
            bridge->mutations != NULL && bridge->mutations->overflowed,
            bridge->mutations != NULL
                && bridge->mutations->resource_rebuild_required,
            runtime->geometry_layout->count, reuse != NULL,
            (unsigned long long) geometry_build_work(runtime->geometry_layout),
            bridge->synchronous_layout_loading);
    }
#endif
    if (bridge->synchronous_layout_loading) {
        uint64_t work = geometry_build_work(runtime->geometry_layout);
        if (bridge->loading_layout_first == 0) {
            uint64_t share = work / 100u * LOADING_LAYOUT_SHARE_PERCENT;
            bridge->loading_layout_first = work != 0 ? work : 1u;
            bridge->loading_layout_allowance =
                share > LOADING_LAYOUT_WORK_FLOOR
                    ? share : LOADING_LAYOUT_WORK_FLOOR;
        } else {
            bridge->loading_layout_allowance -=
                work < bridge->loading_layout_allowance
                    ? work : bridge->loading_layout_allowance;
        }
    }
    geometry_inputs_capture(runtime);
    runtime->geometry_paint_stale = false;
#ifndef TILEFINCH_NO_TRACE
    if (bridge->result != NULL) bridge->result->geometry_snapshot_builds++;
#endif
    layout_transfer_scroll_state(runtime->geometry_incumbent,
                                 runtime->geometry_layout);
    for (size_t i = 0; i < bridge->scroll_intent_count; i++) {
        ElementScrollIntent *intent = &bridge->scroll_intents[i];
        (void) layout_scroll_node(runtime->geometry_layout, intent->node,
                                  intent->scroll_x, intent->scroll_y);
    }
    bridge->layout = runtime->geometry_layout;
    return true;
}

void script_runtime_keep_published_layout(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->geometry_layout == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    /* A snapshot of this very DOM is the published layout's equal: the
       next relayout, or an idle completion, may still adopt it. */
    if (bridge->layout == runtime->geometry_layout && bridge->document != NULL
        && runtime->geometry_content_generation
            == bridge->document->content_generation) return;
    geometry_snapshot_release(runtime);
}

bool script_runtime_flush_geometry_layout(ScriptRuntime *runtime,
                                          const ViewportContext *viewport)
{
    return script_runtime_flush_geometry_layout_reuse(
        runtime, viewport, NULL, NULL, NULL);
}

bool js_rt_geometry_opacity_write_begin(DomBridge *bridge, lxb_dom_node_t *node)
{
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    return runtime != NULL && runtime->geometry_layout != NULL
        && bridge->stylesheet != NULL && bridge->shadow_root_count == 0
        && !bridge->mutations_unrecorded
        && (bridge->mutations == NULL
            || (!bridge->mutations->overflowed
                && !bridge->mutations->resource_rebuild_required
                && !bridge->mutations->conservative_resource_scan))
        && !stylesheet_has_container_queries(bridge->stylesheet)
        && !style_container_layout_state_present(bridge->stylesheet)
        && geometry_layout_current(runtime, &runtime->geometry_layout->viewport)
        && !stylesheet_selectors_reference_attribute_prefix(bridge->stylesheet, "style", 5)
        /* A pseudo-element can read attr(style) without a [style] selector.
           Keep such generated geometry on the conservative path. */
        && !style_node_pseudo_rules_may_affect_discovery(bridge->stylesheet, node);
}

void js_rt_geometry_opacity_write_end(DomBridge *bridge)
{
    ScriptRuntime *runtime = bridge->host;
    runtime->geometry_content_generation = bridge->document->content_generation;
    runtime->geometry_style_generation = document_style_generation();
    /* Box geometry is current, but the old commands must never be adopted
       as the new presentation. Owner mutation/repaint work stays pending. */
    runtime->geometry_paint_stale = true;
}

/* Lazy loaders write an <img>'s sources and data-* bookkeeping, then read
   its box to size it (lazysizes' data-autosize), image after image: each
   read rebuilt the whole page's geometry for writes nothing lays out
   (eurogamer: 65 of 79 builds). Such a write keeps the current geometry:

   - data-* (not the engine's data-tilefinch-* state, nor data-placeholder,
     which layout prints), and an HTML <img>'s src, srcset and sizes, which
     layout reads only through the page's image table: the owner's relayout
     loads the new source, and the box changes then, as a browser's does
     when the new image arrives;
   - no attr() reads the name, and either no selector tests it, or every
     selector that does tests it in its subject compound only (no
     combinator or pseudo-element after it, nothing nested, no custom
     property) and the element's own computed style is the same after the
     write as before (eurogamer's `img[data-autosize]` and `img[src*=..]`
     rules): then only that element's style could change, it did not, and
     nothing else in the cascade can see the write;
   - no shadow root (its rules are not in the page sheet's name list), and
     a journal that still audits every record.

   The DOM write and its journal record stand; only the geometry stays
   current, and its commands are never adopted as the new presentation. */
bool js_rt_geometry_attribute_write_begin(DomBridge *bridge,
                                          lxb_dom_node_t *node,
                                          const char *name, size_t length,
                                          JsRtGeometryAttributeWrite *write)
{
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (write == NULL || runtime == NULL || node == NULL || name == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->ns != LXB_NS_HTML
        || bridge->stylesheet == NULL || bridge->document == NULL
        || bridge->shadow_root_count != 0 || bridge->mutations_unrecorded
        || (bridge->mutations != NULL
            && (bridge->mutations->overflowed
                || bridge->mutations->resource_rebuild_required
                || bridge->mutations->conservative_resource_scan)))
        return false;
    bool data = length > 5 && strncasecmp(name, "data-", 5) == 0
        && !(length >= 14 && strncasecmp(name, "data-tilefinch", 14) == 0)
        && !(length == 16 && strncasecmp(name, "data-placeholder", 16) == 0);
    bool image_source = node->local_name == LXB_TAG_IMG
        && ((length == 3 && strncasecmp(name, "src", 3) == 0)
            || (length == 6 && strncasecmp(name, "srcset", 6) == 0)
            || (length == 5 && strncasecmp(name, "sizes", 5) == 0));
    if (!data && !image_source) return false;
    const LayoutDocument *layout = runtime->geometry_layout != NULL
        ? runtime->geometry_layout : runtime->geometry_incumbent;
    if (layout == NULL || !geometry_layout_current(runtime, &layout->viewport)
        || stylesheet_declarations_read_attribute(
               bridge->stylesheet, name, length)) return false;
    write->node = node;
    write->compare = false;
    write->has_parent = false;
    if (!stylesheet_selectors_reference_attribute_prefix(
            bridge->stylesheet, name, length)) return true;
    /* This also covers the :has() rules that make the write relational
       (eurogamer's `noscript:has(img)+img[data-autosize]`): none tests the
       name inside :has() or ahead of a combinator. */
    if (!stylesheet_attribute_selectors_subject_only(
            bridge->stylesheet, name, length)) return false;
    /* The parent chain resolves root first, as layout inherits it. */
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    lxb_dom_node_t *at = node->parent;
    for (; at != NULL && at->type == LXB_DOM_NODE_TYPE_ELEMENT;
         at = at->parent) {
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = at;
    }
    if (at == NULL || at->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return false;
    for (size_t i = count; i-- > 0;) {
        ComputedStyle style = style_for_node(
            bridge->stylesheet, ancestors[i],
            write->has_parent ? &write->parent : NULL);
        write->parent = style;
        write->has_parent = true;
    }
    write->before = style_for_node(bridge->stylesheet, node,
                                   write->has_parent ? &write->parent : NULL);
    write->compare = true;
    return true;
}

void js_rt_geometry_attribute_write_end(DomBridge *bridge,
                                        const JsRtGeometryAttributeWrite *write)
{
    ScriptRuntime *runtime = bridge->host;
    /* The write's own record must still be auditable, and nothing it set
       off may have reported a style change (begin found the geometry's
       style generation current). */
    if (bridge->mutations_unrecorded
        || (bridge->mutations != NULL
            && (bridge->mutations->overflowed
                || bridge->mutations->resource_rebuild_required
                || bridge->mutations->conservative_resource_scan))
        || runtime->geometry_style_generation != document_style_generation())
        return;
    if (write->compare) {
        ComputedStyle after = style_for_node(
            bridge->stylesheet, write->node,
            write->has_parent ? &write->parent : NULL);
        if (!computed_style_same(bridge->stylesheet, &write->before, &after))
            return;
    }
    runtime->geometry_content_generation = bridge->document->content_generation;
    runtime->geometry_paint_stale = true;
}

/* jQuery measures an element without a box by writing position,
   visibility and display inline, reading its size and writing the old
   values back (jQuery.swap), element after element; carousels toggle
   classes on items inside display:none popovers between such reads. Each
   read rebuilt the whole page's geometry. A class or inline style write on
   an element under a display:none ancestor keeps the current geometry:

   - nothing under a display:none element has a box or counts a counter,
     whatever its own style, and a selector reaches outside the ancestor
     only through :has() (`relational`): a sibling or descendant combinator
     from inside stays inside, so the ancestor's own style, and every box,
     stay as they are;
   - the element is not itself rendered in the current geometry (no box or
     fragment: then no ancestor can be display:none), and every element
     from it up to the ancestor is HTML (SVG <use> draws referenced
     content wherever it sits);
   - no shadow root (slots render light children elsewhere), no container
     queries, a complete (not truncated) geometry, and a journal that still
     audits every record.

   The ancestor is found once per geometry (resolving the ancestors' styles
   root first) and remembered: kept writes cannot change its style. The
   DOM write and its journal record stand, and the geometry's commands are
   never adopted as the new presentation. */
bool js_rt_geometry_hidden_write_begin(DomBridge *bridge, lxb_dom_node_t *node,
                                       bool relational)
{
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (relational || runtime == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->ns != LXB_NS_HTML
        || node->local_name == LXB_TAG_IFRAME
        || node->local_name == LXB_TAG_FRAME
        || node->local_name == LXB_TAG_OBJECT
        || node->local_name == LXB_TAG_EMBED
        || bridge->stylesheet == NULL || bridge->document == NULL
        || bridge->shadow_root_count != 0 || bridge->mutations_unrecorded
        || (bridge->mutations != NULL
            && (bridge->mutations->overflowed
                || bridge->mutations->resource_rebuild_required
                || bridge->mutations->conservative_resource_scan)))
        return false;
    const LayoutDocument *layout = runtime->geometry_layout != NULL
        ? runtime->geometry_layout : runtime->geometry_incumbent;
    if (layout == NULL || layout->content_limit_y != 0
        || !geometry_layout_current(runtime, &layout->viewport)
        || stylesheet_has_container_queries(bridge->stylesheet)
        || layout_box_for_node(layout, node) != NULL
        || layout_inline_fragment_first(layout, node) != NULL)
        return false;
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    lxb_dom_node_t *at = node->parent;
    bool remembered = runtime->geometry_hidden_root != NULL
        && runtime->geometry_hidden_serial == runtime->geometry_serial;
    for (; at != NULL && at->type == LXB_DOM_NODE_TYPE_ELEMENT;
         at = at->parent) {
        if (at->ns != LXB_NS_HTML) return false;
        if (remembered && at == runtime->geometry_hidden_root) return true;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = at;
    }
    if (at == NULL || at->type != LXB_DOM_NODE_TYPE_DOCUMENT) return false;
    ComputedStyle parent = {0};
    bool has_parent = false;
    for (size_t i = count; i-- > 0;) {
        ComputedStyle style = style_for_node(
            bridge->stylesheet, ancestors[i], has_parent ? &parent : NULL);
        if (style.display == DISPLAY_NONE) {
            runtime->geometry_hidden_root = ancestors[i];
            runtime->geometry_hidden_serial = runtime->geometry_serial;
            return true;
        }
        parent = style;
        has_parent = true;
    }
    return false;
}

void js_rt_geometry_hidden_write_end(DomBridge *bridge)
{
    const JsRtGeometryAttributeWrite write = {0};
    js_rt_geometry_attribute_write_end(bridge, &write);
}

void script_runtime_notify_viewport_changed(ScriptRuntime *runtime)
{
    if (runtime != NULL) runtime->viewport_resize_phase = 1u;
}

void script_runtime_set_viewport(ScriptRuntime *runtime, int css_width,
                                 int css_height, int device_width,
                                 int device_height)
{
    if (runtime == NULL || runtime->context == NULL || css_width <= 0
        || css_height <= 0 || device_width <= 0 || device_height <= 0) return;
    /* The bootstrap copies the viewport globals into innerWidth/innerHeight
       once at realm creation. A realm created at a parser-blocking script
       before <meta name=viewport> was scanned therefore keeps the legacy
       layout width unless the resolved viewport is republished here. */
    JSValue global = JS_GetGlobalObject(runtime->context);
    (void) JS_SetPropertyStr(runtime->context, global,
                             "__tilefinchViewportWidth",
                             JS_NewInt32(runtime->context, css_width));
    (void) JS_SetPropertyStr(runtime->context, global,
                             "__tilefinchViewportHeight",
                             JS_NewInt32(runtime->context, css_height));
    (void) JS_SetPropertyStr(runtime->context, global,
                             "__tilefinchDeviceWidth",
                             JS_NewInt32(runtime->context, device_width));
    (void) JS_SetPropertyStr(runtime->context, global,
                             "__tilefinchDeviceHeight",
                             JS_NewInt32(runtime->context, device_height));
    (void) JS_SetPropertyStr(runtime->context, global,
                             "__tilefinchViewportScale",
                             JS_NewFloat64(runtime->context,
                                           (double) device_width
                                           / (double) css_width));
    (void) JS_SetPropertyStr(runtime->context, global, "innerWidth",
                             JS_NewInt32(runtime->context, css_width));
    (void) JS_SetPropertyStr(runtime->context, global, "innerHeight",
                             JS_NewInt32(runtime->context, css_height));
    /* Keep the captured object, not an author replacement of Window's
       visualViewport. No author event is delivered until commit. */
    if (JS_IsObject(runtime->host_visual_viewport)) {
        (void) JS_SetPropertyStr(runtime->context, runtime->host_visual_viewport,
                                 "width", JS_NewInt32(runtime->context, css_width));
        (void) JS_SetPropertyStr(runtime->context, runtime->host_visual_viewport,
                                 "height", JS_NewInt32(runtime->context, css_height));
        (void) JS_SetPropertyStr(runtime->context, runtime->host_visual_viewport,
                                 "scale", JS_NewFloat64(runtime->context,
                                     runtime->diagnostic_mobile_safari ? 1.0
                                         : (double) device_width / (double) css_width));
    }
    JS_FreeValue(runtime->context, global);
}

/* A record keeps an <img>'s load: its source, so republication does not fire
   `load` again, and its natural size, for script to read. It outlives the
   image's table entry, because viewport admission removes entries far from the
   reader and brings them back; to the page that image never stopped being
   loaded. It ends with the element's connection or a change of source. */
enum { SCRIPT_IMAGE_LOAD_LIMIT = 2u * IMAGE_TRACKED_NODE_LIMIT };

/* Room for one more record: grow within the limit, else drop the oldest
   delivered record of an image outside the table, or failing that the oldest
   delivered record (a later republication may then deliver it again). */
static bool runtime_image_load_reserve(ScriptRuntime *runtime)
{
    size_t count = runtime->image_load_state_count;
    size_t capacity = runtime->image_load_state_capacity;
    if (count < capacity) return true;
    if (capacity < SCRIPT_IMAGE_LOAD_LIMIT) {
        size_t grown = capacity == 0 ? 16u : capacity * 2u;
        if (grown > SCRIPT_IMAGE_LOAD_LIMIT) grown = SCRIPT_IMAGE_LOAD_LIMIT;
        ScriptImageLoadState *states = budget_realloc_category(
            runtime->budget, BUDGET_CATEGORY_JAVASCRIPT,
            runtime->image_load_states, grown * sizeof(*states));
        if (states != NULL) {
            runtime->image_load_states = states;
            runtime->image_load_state_capacity = grown;
            return true;
        }
    }
    size_t oldest = count;
    for (size_t i = 0; i < count; i++) {
        if (runtime->image_load_states[i].pending) continue;
        if (oldest == count) oldest = i;
        if (images_find_node(runtime->bridge.images,
                script_runtime_node_handle_resolve(
                    runtime, runtime->image_load_states[i].handle)) == NULL) {
            oldest = i;
            break;
        }
    }
    if (oldest == count) return false;
    runtime->image_load_state_count--;
    memmove(&runtime->image_load_states[oldest],
        &runtime->image_load_states[oldest + 1u],
        (runtime->image_load_state_count - oldest)
            * sizeof(*runtime->image_load_states));
    return true;
}

/* The record for `handle`, or the record count. Tables are republished in
   much the same order, so the search resumes after the previous match. */
static size_t runtime_image_load_find(const ScriptRuntime *runtime,
                                      long handle, size_t *hint)
{
    size_t count = runtime->image_load_state_count;
    if (*hint >= count) *hint = 0;
    for (size_t probe = 0; probe < count; probe++) {
        size_t at = *hint + probe < count ? *hint + probe : *hint + probe - count;
        if (runtime->image_load_states[at].handle != handle) continue;
        *hint = at + 1u < count ? at + 1u : 0u;
        return at;
    }
    return count;
}

bool js_rt_image_load_remembered(DomBridge *bridge, const lxb_dom_node_t *node,
                                 int *natural_width, int *natural_height)
{
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    long handle = runtime == NULL || node == NULL ? 0
        : (long) js_rt_bridge_find_node_handle(bridge, node);
    if (handle == 0) return false;
    size_t hint = 0;
    size_t at = runtime_image_load_find(runtime, handle, &hint);
    if (at == runtime->image_load_state_count) return false;
    const ScriptImageLoadState *state = &runtime->image_load_states[at];
    if (state->natural_width <= 0 || state->natural_height <= 0
        || state->source_hash
               != image_current_source_hash(bridge->stylesheet, node))
        return false;
    *natural_width = state->natural_width;
    *natural_height = state->natural_height;
    return true;
}

static void runtime_queue_image_loads(ScriptRuntime *runtime, ImageResources *images)
{
    for (size_t i = 0; i < runtime->image_load_state_count;) {
        ScriptImageLoadState *state = &runtime->image_load_states[i];
        lxb_dom_node_t *node = script_runtime_node_handle_resolve_connected(
            runtime, state->handle);
        if (node != NULL && (state->pending
            || images_find_node(runtime->bridge.images, node) != NULL
            || (state->delivered
                && state->source_hash == image_current_source_hash(
                       runtime->bridge.stylesheet, node)))) {
            i++;
        } else {
            if (state->pending) runtime->image_load_pending--;
            runtime->image_load_state_count--;
            memmove(state, state + 1, (runtime->image_load_state_count - i)
                * sizeof(*state));
        }
    }
    size_t hint = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *image = &images->items[i];
        if (image->is_canvas || image->is_mask || image->is_background
            || image->source_hash == 0 || !image_resource_available(image)
            || image->node == NULL || image->node->local_name != LXB_TAG_IMG)
            continue;
        /* Only an image that will be queued gets a handle: one without a
           handle has no record yet, and registration can reclaim slots. */
        long handle = (long) js_rt_bridge_find_node_handle(
            &runtime->bridge, image->node);
        size_t at = handle == 0 ? runtime->image_load_state_count
            : runtime_image_load_find(runtime, handle, &hint);
        if (at != runtime->image_load_state_count) {
            const ScriptImageLoadState *state = &runtime->image_load_states[at];
            if (!image->request_replaced && state->source_hash == image->source_hash
                && (state->pending || state->delivered)) continue;
        }
        /* A <noscript> fallback is the browser's: no load event for it. */
        if (!image_resource_source_current(runtime->bridge.stylesheet, image)
            || image_noscript_fallback_shown(runtime->bridge.stylesheet,
                                             image->node)) continue;
        images->items[i].request_replaced = false;
        if (at == runtime->image_load_state_count) {
            if (!runtime_image_load_reserve(runtime)) continue;
            if (handle == 0)
                handle = script_runtime_node_weak_handle(runtime, image->node);
            if (handle == 0) continue;
            js_rt_bridge_keep_node_handle(&runtime->bridge, handle);
            at = runtime->image_load_state_count++;
            runtime->image_load_states[at] = (ScriptImageLoadState) {.handle = handle};
        }
        ScriptImageLoadState *state = &runtime->image_load_states[at];
        if (!state->pending) runtime->image_load_pending++;
        ScriptImageLoadState queued = {.handle = handle,
            .source_hash = image->source_hash,
            .natural_width = image->source_width,
            .natural_height = image->source_height, .pending = true};
        /* A replacement completion is a newly queued task, not the old source's
           place in line. Preserve order when pruning or replacing records. */
        memmove(state, state + 1,
            (runtime->image_load_state_count - at - 1u) * sizeof(*state));
        runtime->image_load_states[runtime->image_load_state_count - 1u] = queued;
    }
    image_queue_page_failures(runtime, images);
}

void script_runtime_inherit_storage_scope(ScriptRuntime *child,
                                         const ScriptRuntime *parent)
{
    if (child != NULL && parent != NULL)
        child->bridge.session_storage_scope = parent->bridge.session_storage_scope;
}

void script_runtime_set_images(ScriptRuntime *runtime,
                               ImageResources *images)
{
    if (runtime == NULL) return;
    runtime->bridge.images = images;
    if (images == NULL) return;
    /* Loading publishes pixels only. Author callbacks run later through
       the ordinary task budget, never inside layout/resource adoption. */
    runtime_queue_image_loads(runtime, images);
    /* A canvas can be drawn by an inline script before final layout attaches
       the page resource table. Retry that bounded dirty publication now; a
       failed pressure admission remains a soft visual degradation. */
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue flush = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchFlushCanvasSurfaces");
    if (JS_IsFunction(runtime->context, flush)) {
        JSValue called = JS_Call(
            runtime->context, flush, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(called)) {
            JSValue exception = JS_GetException(runtime->context);
            JS_FreeValue(runtime->context, exception);
        }
        JS_FreeValue(runtime->context, called);
    }
    JS_FreeValue(runtime->context, flush);
    flush = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchFlushWebGLSurfaces");
    if (JS_IsFunction(runtime->context, flush)) {
        JSValue called = JS_Call(
            runtime->context, flush, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(called)) {
            JSValue exception = JS_GetException(runtime->context);
            JS_FreeValue(runtime->context, exception);
        }
        JS_FreeValue(runtime->context, called);
    }
    JS_FreeValue(runtime->context, flush);
    JS_FreeValue(runtime->context, global);
}

bool script_runtime_has_shadow_roots(const ScriptRuntime *runtime)
{
    return runtime != NULL && runtime->bridge.shadow_root_count != 0;
}

void script_runtime_set_stylesheet(ScriptRuntime *runtime,
                                   const Stylesheet *stylesheet)
{
    if (runtime == NULL) return;
    if (runtime->bridge.stylesheet != stylesheet)
        image_preload_cancel_job(runtime);
    runtime->bridge.stylesheet = stylesheet;
}

void script_runtime_drop_computed_styles_within(
    ScriptRuntime *runtime, lxb_dom_node_t *const *roots, size_t count)
{
    if (runtime != NULL)
        (void) js_rt_bridge_computed_style_drop_within(&runtime->bridge,
                                                       roots, count);
}

void script_runtime_set_synchronous_layout_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque)
{
    if (runtime == NULL) return;
    runtime->bridge.synchronous_layout = callback;
    runtime->bridge.synchronous_layout_opaque =
        callback == NULL ? NULL : opaque;
    runtime->bridge.synchronous_layout_images_only = false;
    runtime->bridge.synchronous_layout_geometry_only = false;
    runtime->bridge.sheet_settle = NULL;
    runtime->bridge.sheet_settle_opaque = NULL;
    runtime->bridge.sheet_settle_refused = false;
}

void script_runtime_set_sheet_settle_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque)
{
    if (runtime == NULL) return;
    runtime->bridge.sheet_settle = callback;
    runtime->bridge.sheet_settle_opaque = callback == NULL ? NULL : opaque;
    runtime->bridge.sheet_settle_refused = false;
}

void script_runtime_set_geometry_layout_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque)
{
    script_runtime_set_synchronous_layout_callback(runtime, callback, opaque);
    if (runtime != NULL)
        runtime->bridge.synchronous_layout_geometry_only = callback != NULL;
}

void script_runtime_set_image_sizing_layout_callback(
    ScriptRuntime *runtime, ScriptSynchronousLayoutCallback callback,
    void *opaque)
{
    script_runtime_set_synchronous_layout_callback(runtime, callback, opaque);
    if (runtime != NULL)
        runtime->bridge.synchronous_layout_images_only = callback != NULL;
}

void script_runtime_set_node_retirement_callback(
    ScriptRuntime *runtime, ScriptNodeRetirementCallback callback,
    void *opaque)
{
    if (runtime == NULL) return;
    runtime->bridge.node_retirement = callback;
    runtime->bridge.node_retirement_opaque =
        callback == NULL ? NULL : opaque;
}

void script_runtime_relocate_node_retirement_opaque(ScriptRuntime *runtime,
                                                    void *opaque)
{
    if (runtime == NULL || runtime->bridge.node_retirement == NULL) return;
    runtime->bridge.node_retirement_opaque = opaque;
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
        && runtime_call_captured(
               runtime,
               runtime->host_callbacks[SCRIPT_HOST_APPLY_PAGE_SCROLL],
               "page scroll update", 2, arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (ok) runtime->bridge.page_scroll_y = next;
    runtime->result.success = ok;
    return ok;
}
