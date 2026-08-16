/* Common server-rendered pages place large JSON bootstrap objects in classic
   inline scripts. Compiling those objects as JavaScript creates bytecode that
   is executed once and immediately discarded. Keep the shortcut deliberately
   narrower than JavaScript object-literal syntax so an uncertain program
   always reaches the established compiler. */
#define SCRIPT_INLINE_DATA_ASSIGNMENT_LIMIT 8u
#define SCRIPT_INLINE_DATA_PROPERTY_LIMIT 63u
#define SCRIPT_INLINE_DATA_DEPTH_LIMIT 32u
#define SCRIPT_INLINE_DATA_SOURCE_LIMIT (256u * 1024u)

typedef struct {
    char property[SCRIPT_INLINE_DATA_PROPERTY_LIMIT + 1u];
    size_t value_begin;
    size_t value_end;
} ScriptInlineDataAssignment;

static size_t script_inline_data_skip_space(
    const char *source, size_t length, size_t at)
{
    while (at < length && isspace((unsigned char) source[at])) at++;
    return at;
}

static bool script_inline_data_identifier_start(char value)
{
    unsigned char byte = (unsigned char) value;
    return value == '_' || value == '$' || isalpha(byte);
}

static bool script_inline_data_identifier_continue(char value)
{
    unsigned char byte = (unsigned char) value;
    return value == '_' || value == '$' || isalnum(byte);
}

static bool script_inline_data_scan(
    const char *source, size_t length, ScriptInlineDataAssignment *items,
    size_t *item_count)
{
    if (source == NULL || length == 0
        || length > SCRIPT_INLINE_DATA_SOURCE_LIMIT || items == NULL
        || item_count == NULL) return false;
    size_t count = 0;
    size_t at = script_inline_data_skip_space(source, length, 0);
    while (at < length) {
        while (at < length && source[at] == ';') {
            at = script_inline_data_skip_space(source, length, at + 1u);
        }
        if (at == length) break;
        if (count >= SCRIPT_INLINE_DATA_ASSIGNMENT_LIMIT) return false;
        static const char window_prefix[] = "window.";
        static const char global_prefix[] = "globalThis.";
        size_t prefix_length = 0;
        if (sizeof(window_prefix) - 1u <= length - at
            && memcmp(source + at, window_prefix,
                      sizeof(window_prefix) - 1u) == 0) {
            prefix_length = sizeof(window_prefix) - 1u;
        } else if (sizeof(global_prefix) - 1u <= length - at
                   && memcmp(source + at, global_prefix,
                             sizeof(global_prefix) - 1u) == 0) {
            prefix_length = sizeof(global_prefix) - 1u;
        } else {
            return false;
        }
        at += prefix_length;
        if (at == length || !script_inline_data_identifier_start(source[at])) {
            return false;
        }
        size_t name_begin = at++;
        while (at < length
               && script_inline_data_identifier_continue(source[at])) at++;
        size_t name_length = at - name_begin;
        if (name_length == 0
            || name_length > SCRIPT_INLINE_DATA_PROPERTY_LIMIT) return false;
        memcpy(items[count].property, source + name_begin, name_length);
        items[count].property[name_length] = '\0';
        at = script_inline_data_skip_space(source, length, at);
        if (at == length || source[at] != '=') return false;
        at = script_inline_data_skip_space(source, length, at + 1u);
        if (at == length || (source[at] != '{' && source[at] != '[')) {
            return false;
        }
        items[count].value_begin = at;
        char stack[SCRIPT_INLINE_DATA_DEPTH_LIMIT];
        size_t depth = 0;
        bool string = false;
        for (; at < length; at++) {
            char value = source[at];
            if (string) {
                if (value == '\\') {
                    if (++at >= length) return false;
                } else if (value == '"') {
                    string = false;
                } else if ((unsigned char) value < 0x20u) {
                    return false;
                }
                continue;
            }
            if (value == '"') {
                string = true;
            } else if (value == '{' || value == '[') {
                if (depth == SCRIPT_INLINE_DATA_DEPTH_LIMIT) return false;
                stack[depth++] = value;
            } else if (value == '}' || value == ']') {
                if (depth == 0) return false;
                char open = stack[--depth];
                if ((value == '}' && open != '{')
                    || (value == ']' && open != '[')) return false;
                if (depth == 0) {
                    items[count].value_end = ++at;
                    break;
                }
            }
        }
        if (string || depth != 0 || items[count].value_end == 0) return false;
        count++;
        at = script_inline_data_skip_space(source, length, at);
        if (at < length && source[at] == ';') {
            at = script_inline_data_skip_space(source, length, at + 1u);
        } else if (at < length) {
            return false;
        }
    }
    if (count == 0) return false;
    *item_count = count;
    return true;
}

static void script_inline_data_clear_exception(JSContext *context)
{
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
}

ScriptInlineDataEvaluation script_runtime_evaluate_inline_data(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    size_t *source_bytes)
{
    if (source_bytes != NULL) *source_bytes = 0;
    if (runtime == NULL || script_node == NULL) {
        return SCRIPT_INLINE_DATA_FAILED;
    }
    const char *source = NULL;
    size_t source_length = 0;
    for (lxb_dom_node_t *child = script_node->first_child;
         child != NULL; child = child->next) {
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        if (text == NULL || length == 0) continue;
        /* Requiring one raw-text span keeps rejection allocation-free. */
        if (source != NULL) return SCRIPT_INLINE_DATA_FALLBACK;
        source = text;
        source_length = length;
    }
    ScriptInlineDataAssignment items[SCRIPT_INLINE_DATA_ASSIGNMENT_LIMIT];
    memset(items, 0, sizeof(items));
    size_t item_count = 0;
    if (!script_inline_data_scan(
            source, source_length, items, &item_count)) {
        return SCRIPT_INLINE_DATA_FALLBACK;
    }
    if (source_length == SIZE_MAX) return SCRIPT_INLINE_DATA_FAILED;
    char *copy = budget_malloc(runtime->budget, source_length + 1u);
    if (copy == NULL) return SCRIPT_INLINE_DATA_FAILED;
    memcpy(copy, source, source_length);
    copy[source_length] = '\0';

    js_rt_runtime_arm_watchdog(runtime);
    if (!script_runtime_refresh_named_properties(runtime)) {
        budget_free(runtime->budget, copy);
        js_rt_runtime_finish_slice_state(runtime);
        return SCRIPT_INLINE_DATA_FAILED;
    }
    JSValue values[SCRIPT_INLINE_DATA_ASSIGNMENT_LIMIT];
    for (size_t i = 0; i < item_count; i++) values[i] = JS_UNDEFINED;
    for (size_t i = 0; i < item_count; i++) {
        size_t begin = items[i].value_begin;
        size_t end = items[i].value_end;
        char saved = copy[end];
        copy[end] = '\0';
        values[i] = JS_ParseJSON(
            runtime->context, copy + begin, end - begin,
            "<inline-data>");
        copy[end] = saved;
        if (JS_IsException(values[i])) {
            script_inline_data_clear_exception(runtime->context);
            for (size_t j = 0; j < i; j++) {
                JS_FreeValue(runtime->context, values[j]);
            }
            budget_free(runtime->budget, copy);
            js_rt_runtime_finish_slice_state(runtime);
            return SCRIPT_INLINE_DATA_FALLBACK;
        }
    }

    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    bool evaluated = js_rt_current_script_scope_begin(
        runtime->context, script_node, false, &previous_current_script,
        &runtime->result);
    JSValue global = evaluated
        ? JS_GetGlobalObject(runtime->context) : JS_UNDEFINED;
    if (evaluated && JS_IsException(global)) evaluated = false;
    for (size_t i = 0; i < item_count; i++) {
        if (!evaluated) {
            JS_FreeValue(runtime->context, values[i]);
            continue;
        }
        int set = JS_SetPropertyStr(
            runtime->context, global, items[i].property, values[i]);
        values[i] = JS_UNDEFINED;
        if (set < 0) evaluated = false;
    }
    JS_FreeValue(runtime->context, global);
    if (!js_rt_current_script_scope_end(
            runtime->context, &previous_current_script,
            &runtime->result)) evaluated = false;
    budget_free(runtime->budget, copy);
    js_lazy_webpack_recover_failure(runtime);
    if (evaluated) evaluated = js_rt_runtime_run_jobs(runtime);
    if (evaluated) evaluated = js_rt_runtime_refresh(runtime);
    if (evaluated) runtime->result.scripts_evaluated++;
    runtime->result.success = evaluated;
    js_rt_runtime_finish_slice_state(runtime);
    if (source_bytes != NULL) *source_bytes = source_length;
    return evaluated ? SCRIPT_INLINE_DATA_APPLIED
                     : SCRIPT_INLINE_DATA_FAILED;
}

bool script_runtime_evaluate_inline(ScriptRuntime *runtime,
                                    lxb_dom_node_t *script_node,
                                    bool module, ScriptResult *result)
{
    if (runtime == NULL || script_node == NULL) return false;
    js_rt_runtime_arm_watchdog(runtime);
    if (!script_runtime_refresh_named_properties(runtime)) {
        js_rt_runtime_update_result(runtime, result);
        return false;
    }
    size_t source_length = 0;
    for (lxb_dom_node_t *child = script_node->first_child;
         child != NULL; child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > SIZE_MAX - source_length) return false;
        source_length += length;
    }
    if (source_length == SIZE_MAX) return false;
    char *source_copy = NULL;
    if (source_length != 0) {
        source_copy = budget_malloc(runtime->budget, source_length + 1);
        if (source_copy == NULL) return false;
        size_t copied = 0;
        for (lxb_dom_node_t *child = script_node->first_child;
             child != NULL; child = child->next) {
            size_t length = 0;
            const char *source = document_text_data(child, &length);
            if (source == NULL || length == 0) continue;
            memcpy(source_copy + copied, source, length);
            copied += length;
        }
        source_copy[copied] = '\0';
    }
    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    bool current_script_active = js_rt_current_script_scope_begin(
        runtime->context, script_node, module, &previous_current_script,
        &runtime->result);
    bool evaluated = current_script_active;
    char module_name[2200];
    char module_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT] = {0};
    const char *name = "<inline-script>";
    const char *module_base = NULL;
    if (module && source_length != 0) {
        runtime->active_module_credentials =
            js_rt_module_credentials_for_node(script_node);
        js_rt_module_referrer_policy_for_node(
            script_node, runtime->bridge.referrer_policy,
            module_referrer_policy);
        module_base = js_rt_bridge_calculated_base_url(&runtime->bridge);
        if (module_base == NULL) evaluated = false;
        snprintf(module_name, sizeof(module_name), "%s#inline-module-%zu",
                 module_base == NULL ? runtime->document_url : module_base,
                 ++runtime->inline_module_sequence);
        name = module_name;
    }
    if (evaluated && source_length != 0) {
        evaluated = js_rt_evaluate_source_type_at(
            runtime->context, source_copy, source_length, name,
            module ? module_base : name,
            module ? module_referrer_policy : NULL,
            module ? runtime->active_module_credentials
                   : TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL,
            module ? SCRIPT_COMPILE_SOURCE_MODULE
                   : SCRIPT_COMPILE_SOURCE_INLINE,
            &runtime->result);
    }
    if (current_script_active
        && !js_rt_current_script_scope_end(
               runtime->context, &previous_current_script,
               &runtime->result)) evaluated = false;
    budget_free(runtime->budget, source_copy);
    js_lazy_webpack_recover_failure(runtime);
    if (evaluated) evaluated = js_rt_runtime_run_jobs(runtime);
    if (evaluated) evaluated = js_rt_runtime_refresh(runtime);
    if (evaluated) runtime->result.scripts_evaluated++;
    runtime->result.success = evaluated;
    /* Parser checkpoints do not consume the diagnostic snapshot. Avoid
       walking dozens of global telemetry properties after every inline
       script; the final loading boundary and explicit callers still request
       the complete ScriptResult. */
    if (result != NULL) js_rt_runtime_update_result(runtime, result);
    else js_rt_runtime_finish_slice_state(runtime);
    return evaluated;
}

bool script_runtime_finish_loading(ScriptRuntime *runtime,
                                   ScriptResult *result)
{
    if (runtime == NULL) return false;
    if (runtime->result.dom_content_loaded_dispatched) {
        js_rt_runtime_update_result(runtime, result);
        return true;
    }
    js_rt_runtime_arm_watchdog(runtime);
    /*
     * Parser-script temporaries are no longer observable at this boundary.
     * Reclaim them before lifecycle-event allocation when a bounded realm is
     * close to its ceiling; ordinary realms avoid the collection entirely.
     */
    if (script_runtime_heap_remaining(runtime) < 512u * 1024u)
        (void) script_runtime_collect_and_trim(runtime);
    /* The dispatcher is part of the trusted bootstrap and already compiled.
       Calling it directly avoids compiling a fresh wrapper at every document
       boundary.  That transient compilation could exhaust a tight 2 MiB
       realm even though the dispatcher and author script both fit. */
    bool evaluated = runtime_call_captured(
        runtime, runtime->dom_content_loaded_dispatch,
        "__tilefinchDispatchDOMContentLoaded", 0, NULL, NULL);
    js_lazy_webpack_recover_failure(runtime);
    if (evaluated) evaluated = js_rt_runtime_run_jobs(runtime);
    if (evaluated) evaluated = js_rt_runtime_refresh(runtime);
    if (!evaluated) capture_cached_error_source(runtime);
    runtime->result.dom_content_loaded_dispatched = evaluated;
    if (evaluated) {
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
        JS_FreeValue(runtime->context, global);
    }
    runtime->result.success = evaluated;
    js_rt_runtime_update_result(runtime, result);
    return evaluated;
}

void script_runtime_invalidate_document_base(ScriptRuntime *runtime)
{
    if (runtime != NULL) runtime->bridge.document_base_dirty = true;
}

bool script_runtime_copy_document_url(
    const ScriptRuntime *runtime, char *output, size_t output_size)
{
    if (runtime == NULL || output == NULL || output_size == 0
        || runtime->bridge.document_url == NULL
        || strlen(runtime->bridge.document_url) >= output_size) {
        if (output != NULL && output_size != 0) output[0] = '\0';
        return false;
    }
    snprintf(output, output_size, "%s", runtime->bridge.document_url);
    return true;
}

bool script_runtime_copy_top_level_url(
    const ScriptRuntime *runtime, char *output, size_t output_size)
{
    if (runtime == NULL || output == NULL || output_size == 0
        || runtime->bridge.top_level_url == NULL
        || strlen(runtime->bridge.top_level_url) >= output_size) {
        if (output != NULL && output_size != 0) output[0] = '\0';
        return false;
    }
    snprintf(output, output_size, "%s", runtime->bridge.top_level_url);
    return true;
}
