static bool dynamic_attribute_present(lxb_dom_node_t *node,
                                      const char *name)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name));
}

static bool dynamic_script_type(lxb_dom_node_t *node, bool *module)
{
    if (module != NULL) *module = false;
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    if (type != NULL && type_length == 6
        && strncasecmp(type, "module", 6) == 0) {
        if (module != NULL) *module = true;
        return true;
    }
    return type == NULL || type_length == 0
        || (type_length == 15
            && strncasecmp(type, "text/javascript", 15) == 0)
        || (type_length == 22
            && strncasecmp(type, "application/javascript", 22) == 0);
}

static bool script_node_source_length(lxb_dom_node_t *node,
                                      size_t *source_length)
{
    if (source_length == NULL) return false;
    *source_length = 0;
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > SIZE_MAX - *source_length) return false;
        *source_length += length;
    }
    return true;
}

static char *script_node_source_copy(DomBridge *bridge,
                                     lxb_dom_node_t *node,
                                     size_t source_length)
{
    if (bridge == NULL || source_length == SIZE_MAX) return NULL;
    char *copy = budget_malloc(bridge->budget, source_length + 1);
    if (copy == NULL) return NULL;
    size_t copied = 0;
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > source_length - copied) {
            budget_free(bridge->budget, copy);
            return NULL;
        }
        memcpy(copy + copied, source, length);
        copied += length;
    }
    if (copied != source_length) {
        budget_free(bridge->budget, copy);
        return NULL;
    }
    copy[copied] = '\0';
    return copy;
}

static ScriptDynamicTask *dynamic_free_task(DomBridge *bridge)
{
    if (bridge == NULL
        || bridge->dynamic_script_count >= SCRIPT_DYNAMIC_TASK_LIMIT) {
        return NULL;
    }
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        if (!bridge->dynamic_scripts[i].active) {
            return &bridge->dynamic_scripts[i];
        }
    }
    return NULL;
}

static void dynamic_record_quota_rejection(DomBridge *bridge)
{
    if (bridge != NULL && bridge->result != NULL) {
        js_rt_saturating_add_size(
            &bridge->result->dynamic_scripts_quota_rejected, 1);
    }
}

static bool dynamic_claim_external_count(DomBridge *bridge)
{
    ScriptQuotaReservation reservation = {0};
    ScriptQuotaReserveResult status = js_rt_bridge_script_quota_reserve(
        bridge, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &reservation);
    bool claimed = status == SCRIPT_QUOTA_RESERVE_GRANTED
        && js_rt_bridge_script_quota_commit(bridge, &reservation, 0);
    if (!claimed) js_rt_bridge_script_quota_abort(bridge, &reservation);
    return claimed;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static char *dynamic_resolve_external_url(DomBridge *bridge,
                                          const char *reference,
                                          size_t reference_length)
{
    if (bridge == NULL || reference == NULL || reference_length == 0
        || reference_length >= TILEFINCH_URL_SERIALIZED_LIMIT) return NULL;
    char reference_copy[TILEFINCH_URL_SERIALIZED_LIMIT];
    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
    memcpy(reference_copy, reference, reference_length);
    reference_copy[reference_length] = '\0';
    const char *base = js_rt_bridge_calculated_base_url(bridge);
    if (base == NULL
        || !tilefinch_url_resolve(base, reference_copy,
                            resolved, sizeof(resolved))) return NULL;
    size_t resolved_length = strlen(resolved);
    char *copy = budget_malloc(bridge->budget, resolved_length + 1);
    if (copy != NULL) memcpy(copy, resolved, resolved_length + 1);
    return copy;
}

static char *dynamic_inline_module_url(ScriptRuntime *runtime)
{
    if (runtime == NULL) return NULL;
    char suffix[64];
    size_t sequence = ++runtime->inline_module_sequence;
    int written = snprintf(suffix, sizeof(suffix),
                           "#inline-module-%zu", sequence);
    if (written <= 0 || (size_t) written >= sizeof(suffix)) return NULL;
    const char *base = js_rt_bridge_calculated_base_url(&runtime->bridge);
    if (base == NULL) return NULL;
    size_t base_length = strlen(base);
    size_t suffix_length = (size_t) written;
    if (base_length > SIZE_MAX - suffix_length - 1) return NULL;
    char *url = budget_malloc(runtime->budget,
                              base_length + suffix_length + 1);
    if (url == NULL) return NULL;
    memcpy(url, base, base_length);
    memcpy(url + base_length, suffix, suffix_length + 1);
    return url;
}

static bool dynamic_task_activate(
    ScriptRuntime *runtime, ScriptDynamicTask *task,
    lxb_dom_node_t *node, bool module, bool ordered,
    ScriptDynamicState state, bool success, char *request_url)
{
    DomBridge *bridge = runtime == NULL ? NULL : &runtime->bridge;
    if (bridge == NULL || task == NULL || node == NULL) return false;
    int64_t handle = js_rt_bridge_register_node(bridge, node);
    size_t slot = 0;
    if (handle == 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)) return false;
    memset(task, 0, sizeof(*task));
    task->active = true;
    task->state = state;
    task->success = success;
    task->sequence = ++bridge->dynamic_script_sequence;
    if (task->sequence == 0) {
        task->sequence = ++bridge->dynamic_script_sequence;
    }
    task->node = node;
    task->owner_document_identity =
        bridge->node_owner_document_identities[slot];
    task->node_handle = handle;
    task->request_url = request_url;
    task->module = module;
    task->ordered = ordered;
    task->mode = module ? TILEFINCH_REQUEST_MODE_CORS
                        : TILEFINCH_REQUEST_MODE_NO_CORS;
    task->credentials = module ? js_rt_module_credentials_for_node(node)
                               : TILEFINCH_CREDENTIALS_INCLUDE;
    char referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    js_rt_module_referrer_policy_for_node(
        node, bridge->referrer_policy, referrer_policy);
    task->incoming_referrer_policy =
        js_rt_runtime_module_referrer_policy_code(referrer_policy);
    if (module) {
        task->effective_referrer_policy =
            task->incoming_referrer_policy;
    }
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
    bridge->dynamic_script_count++;
    if (bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->dynamic_scripts_queued, 1);
        if (bridge->dynamic_script_count
            > bridge->result->dynamic_scripts_peak_pending) {
            bridge->result->dynamic_scripts_peak_pending =
                bridge->dynamic_script_count;
        }
    }
    return true;
}

/* This path can be re-entered when an inline classic script inserts another
   inline classic script. It intentionally does not arm a new watchdog, drain
   jobs, refresh layout, or dispatch load: all of those belong to the outer
   JavaScript turn. */
static bool dynamic_evaluate_inline_classic_now(
    ScriptRuntime *runtime, lxb_dom_node_t *node,
    const char *source, size_t source_length)
{
    if (runtime == NULL || node == NULL || source == NULL) return false;
    ScriptCurrentScriptScope previous = {.value = JS_UNDEFINED};
    bool scoped = js_rt_current_script_scope_begin(
        runtime->context, node, false, &previous, &runtime->result);
    bool evaluated = scoped && evaluate_source_type(
        runtime->context, source, source_length, "<dynamic-inline-script>",
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_INLINE,
        &runtime->result);
    if (scoped && !js_rt_current_script_scope_end(
            runtime->context, &previous, &runtime->result)) {
        evaluated = false;
    }
    if (evaluated) {
        js_rt_saturating_add_size(&runtime->result.scripts_evaluated, 1);
    } else {
        js_rt_saturating_add_size(&runtime->result.scripts_failed, 1);
    }
    return evaluated;
}

static int dynamic_prepare_node(JSContext *context, lxb_dom_node_t *node)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    ScriptElementState *element = js_rt_script_element_state_find(bridge, node);
    if (runtime == NULL || element == NULL || !element->programmatic
        || !element->html || element->already_started
        || !bridge_node_is_connected(node)) return 0;

    bool module = false;
    if (!dynamic_script_type(node, &module)) return 0;
    if (!module && dynamic_attribute_present(node, "nomodule")) {
        /* Mark the element started before returning so reinsertion cannot
           turn a capability fallback into executable work. No quota, task,
           allocation, or fetch has been admitted at this point. */
        element->already_started = true;
        if (bridge->result != NULL) {
            js_rt_saturating_add_size(
                &bridge->result->dynamic_scripts_nomodule_skipped, 1);
        }
        return 1;
    }
    size_t source_length = 0;
    if (!script_node_source_length(node, &source_length)) return -1;
    size_t reference_length = 0;
    const char *reference = document_attribute(
        node, "src", &reference_length);
    bool has_source_attribute = dynamic_attribute_present(node, "src");
    if (!has_source_attribute && source_length == 0) return 0;

    bool ordered = !element->force_async
        && !dynamic_attribute_present(node, "async");
    element->already_started = true;

    if (has_source_attribute) {
        ScriptDynamicTask *task = dynamic_free_task(bridge);
        if (task == NULL) {
            dynamic_record_quota_rejection(bridge);
            return -1;
        }
        if (!dynamic_claim_external_count(bridge)) {
            /* Resource and policy failures are reported from a later runtime
               advance, just like fetch failures.  Dispatching error while
               appendChild is still running lets the surrounding author turn
               overwrite observable listener state and is not the script
               preparation task ordering browsers expose. */
            dynamic_record_quota_rejection(bridge);
            return dynamic_task_activate(
                runtime, task, node, module, ordered,
                SCRIPT_DYNAMIC_READY, false, NULL) ? 1 : -1;
        }
        char *url = dynamic_resolve_external_url(
            bridge, reference, reference_length);
        ScriptDynamicState state = url == NULL
            ? SCRIPT_DYNAMIC_READY : SCRIPT_DYNAMIC_QUEUED;
        bool activated = dynamic_task_activate(
            runtime, task, node, module, ordered, state, false, url);
        if (!activated) {
            budget_free(bridge->budget, url);
            dynamic_record_quota_rejection(bridge);
            return -1;
        }
        return 1;
    }

    ScriptDynamicTask *module_task = module
        ? dynamic_free_task(bridge) : NULL;
    if (module && module_task == NULL) {
        dynamic_record_quota_rejection(bridge);
        return -1;
    }
    ScriptQuotaReservation reservation = {0};
    ScriptQuotaReserveResult quota = js_rt_bridge_script_quota_reserve(
        bridge, SCRIPT_QUOTA_NEW_EXECUTABLE, source_length,
        &reservation);
    if (quota != SCRIPT_QUOTA_RESERVE_GRANTED) {
        js_rt_bridge_script_quota_abort(bridge, &reservation);
        dynamic_record_quota_rejection(bridge);
        return -1;
    }
    char *source = script_node_source_copy(
        bridge, node, source_length);
    if (source == NULL || !js_rt_bridge_script_quota_commit(
            bridge, &reservation, source_length)) {
        budget_free(bridge->budget, source);
        js_rt_bridge_script_quota_abort(bridge, &reservation);
        dynamic_record_quota_rejection(bridge);
        return -1;
    }

    if (!module) {
        size_t failures_before = bridge->budget->failure_count;
        bool evaluated = dynamic_evaluate_inline_classic_now(
            runtime, node, source, source_length);
        budget_free(bridge->budget, source);
        bool fatal = !evaluated
            && (runtime->watchdog.interrupted
                || bridge->budget->failure_count != failures_before);
        return fatal ? -1 : 1;
    }

    BrowserSharedBody *body = browser_shared_body_take(
        bridge->budget, (unsigned char *) source, source_length);
    if (body == NULL) {
        budget_free(bridge->budget, source);
        dynamic_record_quota_rejection(bridge);
        return -1;
    }
    char *module_url = dynamic_inline_module_url(runtime);
    const char *inline_base = js_rt_bridge_calculated_base_url(bridge);
    char *response_url = inline_base == NULL ? NULL
        : js_rt_dynamic_copy_text(bridge->budget, inline_base);
    if (module_url == NULL || response_url == NULL
        || !dynamic_task_activate(
            runtime, module_task, node, true, ordered,
            SCRIPT_DYNAMIC_READY, module_url != NULL, module_url)) {
        budget_free(bridge->budget, module_url);
        budget_free(bridge->budget, response_url);
        browser_shared_body_release(body);
        dynamic_record_quota_rejection(bridge);
        return -1;
    }
    module_task->response_url = response_url;
    module_task->source_body = body;
    module_task->source_length = source_length;
    bridge->dynamic_script_bytes += source_length;
    if (bridge->result != NULL) {
        js_rt_saturating_add_size(
            &bridge->result->dynamic_script_bytes, source_length);
    }
    return 1;
}

int js_rt_dynamic_prepare_subtree(JSContext *context,
                            lxb_dom_node_t *root)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || root == NULL) return 0;
    size_t capacity = bridge->script_element_count;
    if (capacity == 0) return 0;
    ScriptElementSnapshot *candidates = budget_calloc(
        bridge->budget, capacity, sizeof(*candidates));
    if (candidates == NULL) return -1;
    size_t count = 0;
    DomDocumentOrderTraversal traversal = {
        .next = root, .boundary = root
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && count < capacity;
         at = dom_document_order_next(&traversal)) {
        ScriptElementState *state = js_rt_script_element_state_find(bridge, at);
        if (state != NULL) {
            int64_t handle = js_rt_bridge_register_node(bridge, at);
            if (handle != 0) {
                candidates[count++] = (ScriptElementSnapshot) {
                    .node_handle = handle, .identity = state->identity
                };
            }
        }
    }
    int prepared = 0;
    for (size_t i = 0; i < count; i++) {
        /* A preceding inline script can replace the remaining subtree. The
           destructive mutation hooks purge native state before Lexbor frees
           it, so lookup is a safe liveness gate before dereferencing. */
        size_t slot = 0;
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, candidates[i].node_handle, &slot)) continue;
        lxb_dom_node_t *node = bridge->nodes[slot];
        ScriptElementState *state = js_rt_script_element_state_find(bridge, node);
        if (state == NULL || state->identity != candidates[i].identity) {
            continue;
        }
        if (!bridge_node_is_connected(node)) continue;
        int result = dynamic_prepare_node(context, node);
        if (result < 0) {
            budget_free(bridge->budget, candidates);
            return -1;
        }
        if (result > 0) prepared = 1;
    }
    budget_free(bridge->budget, candidates);
    return prepared;
}

bool js_rt_runtime_script_checkpoint(ScriptRuntime *runtime,
                                     size_t work_units)
{
    watchdog_add_work(&runtime->watchdog, work_units);
    if (watchdog_cooperate(&runtime->watchdog)) return true;
    snprintf(runtime->result.error, sizeof(runtime->result.error),
             "script execution cancelled at a cooperative boundary");
    runtime->result.interrupted = true;
    return false;
}

static bool runtime_evaluate_checkpointed(ScriptRuntime *runtime,
                                          const char *source,
                                          size_t length,
                                          const char *source_url)
{
    return js_rt_evaluate_source(runtime->context, source, length, source_url,
                           &runtime->result)
           && js_rt_runtime_script_checkpoint(runtime, 1);
}

static bool runtime_call_host(ScriptRuntime *runtime,
                              ScriptHostCallback callback,
                              int argc, JSValueConst *argv,
                              JSValue *returned);
