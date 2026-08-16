/* ES-module pipeline: MIME admission, module graph registration, specifier
   normalization, the QuickJS module loader, and external classic/module
   evaluation entry points.  Split from js_runtime.c; shares the runtime
   internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

bool script_module_mime_type_allowed(const char *content_type)
{
    static const char *const javascript_mime_types[] = {
        "application/ecmascript", "application/javascript",
        "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "text/jscript",
        "text/livescript", "text/x-ecmascript", "text/x-javascript"
    };
    if (content_type == NULL) return false;
    while (isspace((unsigned char) *content_type)) content_type++;
    size_t length = strcspn(content_type, ";");
    while (length != 0
           && isspace((unsigned char) content_type[length - 1])) length--;
    if (length == 0) return false;
    for (size_t i = 0;
         i < sizeof(javascript_mime_types)
                 / sizeof(javascript_mime_types[0]); i++) {
        if (strlen(javascript_mime_types[i]) == length
            && strncasecmp(content_type, javascript_mime_types[i], length)
                   == 0) return true;
    }
    return false;
}

bool script_module_revalidated_mime_allowed(
    const char *cached_content_type, const char *response_content_type)
{
    return script_module_mime_type_allowed(cached_content_type)
        && (response_content_type == NULL
            || response_content_type[0] == '\0'
            || script_module_mime_type_allowed(response_content_type));
}

bool js_rt_module_set_import_meta(JSContext *context, JSValueConst module,
                            const char *response_url, bool is_main)
{
    if (JS_VALUE_GET_TAG(module) != JS_TAG_MODULE) return false;
    JSModuleDef *definition = JS_VALUE_GET_PTR(module);
    if (response_url == NULL || response_url[0] == '\0') return false;
    JSValue meta = JS_GetImportMeta(context, definition);
    bool ok = !JS_IsException(meta)
              && JS_DefinePropertyValueStr(
                     context, meta, "url",
                     JS_NewString(context, response_url),
                     JS_PROP_C_W_E) >= 0
              && JS_DefinePropertyValueStr(
                     context, meta, "main", JS_NewBool(context, is_main),
                     JS_PROP_C_W_E) >= 0;
    JS_FreeValue(context, meta);
    return ok;
}

const char *js_rt_runtime_module_base_lookup(const ScriptRuntime *runtime,
                                       const char *request_url)
{
    if (runtime == NULL || request_url == NULL) return NULL;
    for (size_t i = 0; i < runtime->module_base_count; i++) {
        if (strcmp(runtime->module_bases[i].request_url, request_url) == 0) {
            return runtime->module_bases[i].response_url;
        }
    }
    return NULL;
}

static size_t runtime_module_index_lookup(const ScriptRuntime *runtime,
                                          const char *request_url)
{
    if (runtime == NULL || request_url == NULL) return SIZE_MAX;
    for (size_t i = 0; i < runtime->module_base_count; i++) {
        if (strcmp(runtime->module_bases[i].request_url, request_url) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

ScriptModuleMapStatus script_runtime_module_map_status(
    const ScriptRuntime *runtime, const char *request_url)
{
    size_t index = runtime_module_index_lookup(runtime, request_url);
    if (index == SIZE_MAX) return SCRIPT_MODULE_MAP_MISSING;
    uint8_t state = runtime->module_bases[index].root_state;
    return state <= SCRIPT_MODULE_MAP_FAILED
        ? (ScriptModuleMapStatus) state : SCRIPT_MODULE_MAP_FAILED;
}

bool js_rt_runtime_module_root_state_set(
    ScriptRuntime *runtime, const char *request_url,
    ScriptModuleMapStatus state)
{
    size_t index = runtime_module_index_lookup(runtime, request_url);
    if (index == SIZE_MAX || state == SCRIPT_MODULE_MAP_MISSING) return false;
    runtime->module_bases[index].root_state = (uint8_t) state;
    return true;
}

uint8_t js_rt_runtime_module_referrer_policy_code(const char *policy)
{
    if (policy == NULL) return UINT8_MAX;
    if (policy[0] == '\0') return 0;
    static const char *known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(policy, known[i]) == 0) return (uint8_t) (i + 1u);
    }
    return UINT8_MAX;
}

const char *js_rt_runtime_module_referrer_policy_text(uint8_t code)
{
    static const char *known[] = {
        "", "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    return code < sizeof(known) / sizeof(known[0]) ? known[code] : NULL;
}

static bool runtime_module_referrer_policy_valid(const char *policy)
{
    return js_rt_runtime_module_referrer_policy_code(policy) != UINT8_MAX;
}

static bool runtime_module_edge_register(
    ScriptRuntime *runtime, const char *request_url,
    size_t parent_index, const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials, size_t *registered_index)
{
    if (registered_index != NULL) *registered_index = SIZE_MAX;
    uint8_t policy_code = js_rt_runtime_module_referrer_policy_code(
        effective_referrer_policy);
    if (runtime == NULL || request_url == NULL || request_url[0] == '\0'
        || (parent_index != SIZE_MAX
            && parent_index >= runtime->module_base_count)
        || (parent_index != SIZE_MAX && parent_index >= UINT16_MAX)
        || policy_code == UINT8_MAX
        || (credentials != TILEFINCH_CREDENTIALS_SAME_ORIGIN
            && credentials != TILEFINCH_CREDENTIALS_INCLUDE)) return false;
    size_t existing = runtime_module_index_lookup(runtime, request_url);
    if (existing != SIZE_MAX) {
        if (registered_index != NULL) *registered_index = existing;
        return true;
    }
    if (runtime->module_base_count >= SCRIPT_REALM_MAXIMUM_SCRIPTS) {
        return false;
    }
    if (runtime->module_base_count == runtime->module_base_capacity) {
        size_t next = runtime->module_base_capacity == 0
            ? 8 : runtime->module_base_capacity * 2;
        if (next > SCRIPT_REALM_MAXIMUM_SCRIPTS) {
            next = SCRIPT_REALM_MAXIMUM_SCRIPTS;
        }
        ScriptModuleBaseEntry *grown = budget_realloc(
            runtime->budget, runtime->module_bases,
            next * sizeof(*grown));
        if (grown == NULL) return false;
        runtime->module_bases = grown;
        runtime->module_base_capacity = next;
    }
    size_t request_length = strlen(request_url);
    char *request_copy = budget_malloc(runtime->budget, request_length + 1);
    if (request_copy == NULL) return false;
    memcpy(request_copy, request_url, request_length + 1);
    size_t index = runtime->module_base_count++;
    runtime->module_bases[index] =
        (ScriptModuleBaseEntry) {
            .request_url = request_copy,
            .credentials = credentials,
            .parent_index = parent_index == SIZE_MAX
                ? UINT16_MAX : (uint16_t) parent_index,
            .effective_referrer_policy = policy_code
        };
    if (registered_index != NULL) *registered_index = index;
    return true;
}

static bool runtime_module_response_register(
    ScriptRuntime *runtime, size_t index, const char *response_url,
    const char *response_referrer_policy)
{
    if (runtime == NULL || index >= runtime->module_base_count
        || response_url == NULL || response_url[0] == '\0'
        || response_referrer_policy == NULL
        || strlen(response_referrer_policy)
               >= BROWSER_MODULE_REFERRER_POLICY_LIMIT) return false;
    if (!runtime_module_referrer_policy_valid(response_referrer_policy)) {
        return false;
    }
    ScriptModuleBaseEntry *entry = &runtime->module_bases[index];
    if (entry->response_url != NULL) {
        return strcmp(entry->response_url, response_url) == 0;
    }
    size_t response_length = strlen(response_url);
    char *copy = budget_malloc(runtime->budget, response_length + 1);
    if (copy == NULL) return false;
    memcpy(copy, response_url, response_length + 1);
    entry->response_url = copy;
    if (response_referrer_policy[0] != '\0') {
        entry->effective_referrer_policy =
            js_rt_runtime_module_referrer_policy_code(response_referrer_policy);
    }
    return true;
}

bool js_rt_runtime_module_root_register(
ScriptRuntime *runtime, const char *request_url, const char *response_url,
const char *effective_referrer_policy,
TilefinchCredentialsMode credentials)
{
    size_t index = SIZE_MAX;
    return runtime_module_edge_register(
               runtime, request_url, SIZE_MAX,
               effective_referrer_policy == NULL
                   ? "" : effective_referrer_policy,
               credentials, &index)
        && runtime_module_response_register(runtime, index, response_url, "");
}

TilefinchCredentialsMode js_rt_module_credentials_for_node(
lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "crossorigin", &length);
    static const char use_credentials[] = "use-credentials";
    return value != NULL && length == sizeof(use_credentials) - 1
        && strncasecmp(value, use_credentials, length) == 0
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
}

void js_rt_module_referrer_policy_for_node(
lxb_dom_node_t *node, const char *fallback,
char output[BROWSER_MODULE_REFERRER_POLICY_LIMIT])
{
    const char *selected = fallback == NULL ? "" : fallback;
    char normalized[BROWSER_MODULE_REFERRER_POLICY_LIMIT] = {0};
    size_t length = 0;
    const char *attribute = document_attribute(
        node, "referrerpolicy", &length);
    if (attribute != NULL) {
        if (length != 0 && length < sizeof(normalized)) {
            for (size_t i = 0; i < length; i++) {
                normalized[i] = (char) tolower(
                    (unsigned char) attribute[i]);
            }
            normalized[length] = '\0';
            if (runtime_module_referrer_policy_valid(normalized)) {
                selected = normalized;
            }
        }
    }
    if (!runtime_module_referrer_policy_valid(selected)) selected = "";
    snprintf(output, BROWSER_MODULE_REFERRER_POLICY_LIMIT, "%s", selected);
}

static char *runtime_module_normalize(JSContext *context,
                                      const char *base_name,
                                      const char *module_name,
                                      void *opaque)
{
    ScriptRuntime *runtime = opaque;
    if (runtime == NULL || module_name == NULL) return NULL;
    size_t parent_index = base_name == NULL || base_name[0] == '<'
        ? SIZE_MAX : runtime_module_index_lookup(runtime, base_name);
    TilefinchCredentialsMode credentials =
        script_runtime_module_credentials(runtime);
    char base[TILEFINCH_URL_SERIALIZED_LIMIT];
    char policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    const char *selected_base = NULL;
    const char *selected_policy = runtime->bridge.referrer_policy;
    if (parent_index != SIZE_MAX) {
        const ScriptModuleBaseEntry *parent =
            &runtime->module_bases[parent_index];
        selected_base = parent->response_url;
        selected_policy = js_rt_runtime_module_referrer_policy_text(
            parent->effective_referrer_policy);
        credentials = parent->credentials;
    } else if (base_name != NULL && base_name[0] != '<') {
        selected_base = base_name;
    } else {
        selected_base = js_rt_bridge_calculated_base_url(&runtime->bridge);
    }
    if (selected_base == NULL) selected_base = runtime->document_url;
    if (selected_policy == NULL) selected_policy = "";
    if (strlen(selected_base) >= sizeof(base)
        || strlen(selected_policy) >= sizeof(policy)) return NULL;
    snprintf(base, sizeof(base), "%s", selected_base);
    snprintf(policy, sizeof(policy), "%s", selected_policy);
    char resolved[2048];
    if (!fetch_resolve_url(base, module_name, resolved, sizeof(resolved))) {
        return js_strdup(context, module_name);
    }
    if (!runtime_module_edge_register(
            runtime, resolved, parent_index, policy, credentials, NULL)) {
        return NULL;
    }
    return js_strdup(context, resolved);
}

static JSModuleDef *runtime_module_loader(JSContext *context,
                                          const char *module_name,
                                          void *opaque)
{
    ScriptRuntime *runtime = opaque;
    if (runtime->module_load == NULL) {
        JS_ThrowReferenceError(context, "module loading is disabled");
        return NULL;
    }
    size_t module_index = runtime_module_index_lookup(runtime, module_name);
    if (module_index == SIZE_MAX) {
        JS_ThrowReferenceError(context,
                               "module metadata is missing for '%s'",
                               module_name);
        return NULL;
    }
    uint16_t stored_parent_index =
        runtime->module_bases[module_index].parent_index;
    size_t parent_index = stored_parent_index;
    if (stored_parent_index == UINT16_MAX
        || parent_index >= runtime->module_base_count
        || runtime->module_bases[parent_index].response_url == NULL) {
        JS_ThrowReferenceError(context,
                               "module referrer is missing for '%s'",
                               module_name);
        return NULL;
    }
    char request_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char referrer_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    const ScriptModuleBaseEntry *entry = &runtime->module_bases[module_index];
    const ScriptModuleBaseEntry *parent = &runtime->module_bases[parent_index];
    const char *entry_policy = js_rt_runtime_module_referrer_policy_text(
        entry->effective_referrer_policy);
    if (strlen(entry->request_url) >= sizeof(request_url)
        || strlen(parent->response_url) >= sizeof(referrer_url)
        || entry_policy == NULL
        || strlen(entry_policy) >= sizeof(referrer_policy)) {
        JS_ThrowRangeError(context, "module request metadata is too large");
        return NULL;
    }
    snprintf(request_url, sizeof(request_url), "%s", entry->request_url);
    snprintf(referrer_url, sizeof(referrer_url), "%s",
             parent->response_url);
    snprintf(referrer_policy, sizeof(referrer_policy), "%s",
             entry_policy);
    TilefinchCredentialsMode credentials = entry->credentials;
    ScriptModuleLoadRequest request = {
        .request_url = request_url,
        .referrer_url = referrer_url,
        .referrer_policy = referrer_policy,
        .credentials = credentials
    };
    runtime->active_module_credentials = credentials;
    if (getenv("TILEFINCH_TRACE_MODULE_ORDER") != NULL) {
        fprintf(stderr, "tilefinch: module-load %s <- %s\n", request_url,
                referrer_url);
    }
    ScriptModuleLoadResult loaded = {0};
    if (!runtime->module_load(runtime->module_opaque, &request, &loaded)
        || loaded.source == NULL || loaded.response_url == NULL
        || loaded.response_url[0] == '\0'
        || memchr(loaded.response_referrer_policy, '\0',
                  sizeof(loaded.response_referrer_policy)) == NULL) {
        if (runtime->module_release != NULL
            && (loaded.source != NULL || loaded.response_url != NULL)) {
            runtime->module_release(runtime->module_opaque, &loaded);
        }
        JS_ThrowReferenceError(context, "could not load module '%s'",
                               module_name);
        return NULL;
    }
    bool registered = runtime_module_response_register(
        runtime, module_index, loaded.response_url,
        loaded.response_referrer_policy);
    if (!registered) {
        if (runtime->module_release != NULL) {
            runtime->module_release(runtime->module_opaque, &loaded);
        }
        JS_ThrowRangeError(context, "module metadata limit exceeded");
        return NULL;
    }
    bool admitted = false;
    uint64_t compile_started_ns = js_rt_monotonic_time_ns();
    JSValue compiled = js_rt_compile_source_type(
        context, loaded.source, loaded.source_length, module_name,
        JS_EVAL_TYPE_MODULE,
        SCRIPT_COMPILE_SOURCE_MODULE, &runtime->result, &admitted);
    runtime->result.module_compile_us +=
        (js_rt_monotonic_time_ns() - compile_started_ns) / 1000u;
    runtime->result.module_compile_count++;
    if (!admitted) {
        if (runtime->module_release != NULL) {
            runtime->module_release(runtime->module_opaque, &loaded);
        }
        JS_ThrowRangeError(context, "%s", runtime->result.error);
        return NULL;
    }
    if (JS_IsException(compiled)) {
        if (runtime->module_release != NULL) {
            runtime->module_release(runtime->module_opaque, &loaded);
        }
        JS_FreeValue(context, compiled);
        return NULL;
    }
    const char *response_url = js_rt_runtime_module_base_lookup(
        runtime, module_name);
    bool meta_set = response_url != NULL
        && js_rt_module_set_import_meta(context, compiled, response_url, false);
    if (runtime->module_release != NULL) {
        runtime->module_release(runtime->module_opaque, &loaded);
    }
    if (!meta_set) {
        JS_FreeValue(context, compiled);
        return NULL;
    }
    JSModuleDef *module = JS_VALUE_GET_PTR(compiled);
    JS_FreeValue(context, compiled);
    return module;
}

void js_rt_runtime_module_metadata_clear(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    for (size_t i = 0; i < runtime->module_base_count; i++) {
        budget_free(runtime->budget, runtime->module_bases[i].response_url);
        budget_free(runtime->budget, runtime->module_bases[i].request_url);
    }
    budget_free(runtime->budget, runtime->module_bases);
    runtime->module_bases = NULL;
    runtime->module_base_count = 0;
    runtime->module_base_capacity = 0;
}

void script_runtime_set_module_loader(
    ScriptRuntime *runtime, ScriptModuleLoadCallback load,
    ScriptModuleFreeCallback release, void *opaque)
{
    script_runtime_set_module_loader_owned(runtime, load, release, opaque,
                                           NULL);
}

void script_runtime_set_module_loader_owned(
    ScriptRuntime *runtime, ScriptModuleLoadCallback load,
    ScriptModuleFreeCallback release, void *opaque,
    ScriptModuleOpaqueDestroyCallback destroy_opaque)
{
    if (runtime == NULL) return;
    if (runtime->module_opaque_destroy != NULL
        && runtime->module_opaque != opaque) {
        runtime->module_opaque_destroy(runtime->module_opaque);
    }
    runtime->module_load = load;
    runtime->module_release = release;
    runtime->module_opaque = opaque;
    runtime->module_opaque_destroy = destroy_opaque;
    JS_SetModuleLoaderFunc(runtime->runtime,
                           load == NULL ? NULL : runtime_module_normalize,
                           load == NULL ? NULL : runtime_module_loader,
                           load == NULL ? NULL : runtime);
}

static bool script_runtime_evaluate_external_typed_at(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *module_referrer_policy,
    TilefinchCredentialsMode module_credentials, bool module,
    bool dispatch_completion, const char *classic_cache_url,
    ScriptResult *result)
{
    if (runtime == NULL || script_node == NULL || source == NULL) return false;
    if (!script_runtime_refresh_named_properties(runtime)) {
        js_rt_runtime_update_result(runtime, result);
        return false;
    }
    const char *previous_rejection_source =
        runtime->promise_rejection_state.active_source;
    runtime->promise_rejection_state.active_source =
        request_url == NULL ? "<external-script>" : request_url;
    int64_t script_handle = js_rt_bridge_register_node(
        &runtime->bridge, script_node);
    if (script_handle == 0) {
        runtime->promise_rejection_state.active_source =
            previous_rejection_source;
        return false;
    }
    js_rt_runtime_arm_watchdog(runtime);
    if (module && getenv("TILEFINCH_TRACE_STARTUP_FAILURE") != NULL) {
        static const char expose_startup_failure[] =
            "if(globalThis.__webMobileStartupRecovery)"
            "globalThis.__webMobileStartupRecovery.recover=()=>false;";
        if (!js_rt_evaluate_source(
                runtime->context, expose_startup_failure,
                sizeof(expose_startup_failure) - 1,
                "<startup-failure-diagnostic>", &runtime->result)) {
            runtime->result.success = false;
            js_rt_runtime_update_result(runtime, result);
            runtime->promise_rejection_state.active_source =
                previous_rejection_source;
            return false;
        }
    }
    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    bool current_script_active = js_rt_current_script_scope_begin(
        runtime->context, script_node, module, &previous_current_script,
        &runtime->result);
    bool evaluated = current_script_active;
    const char *evaluated_source = source;
    size_t evaluated_length = source_length;
    char *instrumented = NULL;
    JSValue trace_global = JS_GetGlobalObject(runtime->context);
    JSValue page_trace = JS_GetPropertyStr(runtime->context, trace_global,
                                           "__tilefinchPageTrace");
    bool page_diagnostic = JS_ToBool(runtime->context, page_trace) == 1;
    JS_FreeValue(runtime->context, page_trace);
    JS_FreeValue(runtime->context, trace_global);
    static const char trace_prefix[] = "with(__tilefinchGlobalProxy){";
    static const char trace_suffix[] = "\n}";
    /* A Module is always parsed in strict mode, where a WithStatement is a
       syntax error.  The capability tracer's `with` wrapper is therefore
       valid only for classic scripts.  Module globals are still observed by
       the proxied host objects installed by page_capability_trace_setup();
       compiling the author's module source unchanged preserves module
       grammar and strict-mode semantics. */
    if (evaluated && page_diagnostic && !module) {
        evaluated_length = sizeof(trace_prefix) - 1 + source_length
                           + sizeof(trace_suffix) - 1;
        instrumented = budget_malloc(runtime->budget, evaluated_length + 1);
        if (instrumented == NULL) evaluated = false;
        else {
            memcpy(instrumented, trace_prefix, sizeof(trace_prefix) - 1);
            memcpy(instrumented + sizeof(trace_prefix) - 1, source,
                   source_length);
            memcpy(instrumented + sizeof(trace_prefix) - 1 + source_length,
                   trace_suffix, sizeof(trace_suffix));
            instrumented[evaluated_length] = '\0';
            evaluated_source = instrumented;
        }
    }
    if (evaluated) {
        const char *name =
            request_url == NULL ? "<external-script>" : request_url;
        if (!module && !page_diagnostic && dispatch_completion
            && classic_cache_url != NULL && runtime->session != NULL) {
            BrowserSharedBody *cached =
                browser_session_classic_script_bytecode_acquire(
                    runtime->session, classic_cache_url,
                    (const unsigned char *) evaluated_source,
                    evaluated_length);
            JSValue compiled = JS_UNDEFINED;
            bool restored = false;
            bool cache_admitted = true;
            if (cached != NULL) {
                cache_admitted = js_rt_admit_cached_compile_source(
                        runtime->context, evaluated_length, name,
                        SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result);
                if (cache_admitted) {
                    compiled = JS_ReadObject(
                        runtime->context, cached->data, cached->length,
                        JS_READ_OBJ_BYTECODE);
                    restored = !JS_IsException(compiled);
                }
                if (restored) {
                    runtime->result.external_script_bytecode_cache_hits++;
                    js_rt_saturating_add_size(
                        &runtime->result.external_script_bytecode_cache_bytes,
                        cached->length);
                } else if (cache_admitted) {
                    if (JS_IsException(compiled)) {
                        JSValue exception =
                            JS_GetException(runtime->context);
                        JS_FreeValue(runtime->context, exception);
                    } else {
                        JS_FreeValue(runtime->context, compiled);
                    }
                    compiled = JS_UNDEFINED;
                    runtime->result
                        .external_script_bytecode_cache_restore_failures++;
                    browser_session_classic_script_bytecode_invalidate(
                        runtime->session, classic_cache_url,
                        (const unsigned char *) evaluated_source,
                        evaluated_length);
                }
                browser_shared_body_release(cached);
            }
            if (!cache_admitted) {
                evaluated = false;
            } else if (!restored) {
                bool admitted = false;
                runtime->result.external_script_bytecode_cache_misses++;
                compiled = js_rt_compile_source_type(
                    runtime->context, evaluated_source, evaluated_length,
                    name, JS_EVAL_TYPE_GLOBAL,
                    SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result,
                    &admitted);
                if (!admitted) {
                    evaluated = false;
                } else if (!JS_IsException(compiled)) {
                    size_t bytecode_length = 0;
                    bool may_fit =
                        browser_session_classic_script_bytecode_may_fit(
                            runtime->session, classic_cache_url,
                            (const unsigned char *) evaluated_source,
                            evaluated_length,
                            evaluated_length);
                    uint8_t *bytecode = may_fit
                        ? JS_WriteObject(
                            runtime->context, &bytecode_length, compiled,
                            JS_WRITE_OBJ_BYTECODE)
                        : NULL;
                    if (!may_fit) {
                        runtime->result
                            .external_script_bytecode_cache_admission_skips++;
                    }
                    bool bytecode_stored = bytecode != NULL
                        && bytecode_length != 0
                        && browser_session_classic_script_bytecode_put(
                            runtime->session, classic_cache_url,
                            (const unsigned char *) evaluated_source,
                            evaluated_length, bytecode, bytecode_length);
                    if (bytecode_stored) {
                        runtime->result
                            .external_script_bytecode_cache_stores++;
                    }
                    if (may_fit && bytecode == NULL) {
                        JSValue exception =
                            JS_GetException(runtime->context);
                        JS_FreeValue(runtime->context, exception);
                    }
                    js_free(runtime->context, bytecode);
                }
            }
            if (evaluated) {
                evaluated = js_rt_evaluate_compiled_source(
                    runtime->context, compiled, name, evaluated_length,
                    &runtime->result);
            }
        } else {
            evaluated = js_rt_evaluate_source_type_at(
                runtime->context, evaluated_source, evaluated_length, name,
                module && response_url != NULL ? response_url : request_url,
                module ? module_referrer_policy : NULL,
                module ? module_credentials
                       : TILEFINCH_CREDENTIALS_SAME_ORIGIN,
                module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL,
                module ? SCRIPT_COMPILE_SOURCE_MODULE
                       : SCRIPT_COMPILE_SOURCE_EXTERNAL,
                &runtime->result);
        }
        if (!evaluated) {
            js_rt_capture_error_source_context(source, source_length, request_url,
                                         &runtime->result);
        }
    }
    if (current_script_active
        && !js_rt_current_script_scope_end(
               runtime->context, &previous_current_script,
               &runtime->result)) evaluated = false;
    js_lazy_webpack_recover_failure(runtime);
    /*
     * A segmented ResourceLoader response is still one classic-script job.
     * Intermediate registrations must not expose a microtask checkpoint or
     * refreshed host state before every later registration has run.
     */
    if (evaluated && dispatch_completion) {
        evaluated = js_rt_runtime_run_jobs(runtime);
    }
    if (evaluated && dispatch_completion) {
        evaluated = js_rt_runtime_refresh(runtime);
    }
    budget_free(runtime->budget, instrumented);
    if (!evaluated) {
        js_rt_capture_error_source_context(source, source_length, request_url,
                                     &runtime->result);
        /* Compilation/evaluation failure is an author-script error, not a
           failed document load.  In a bounded realm the failed evaluation
           may leave unreachable parser objects and wrapper cycles at the
           heap ceiling; collect them before allocating the script's error
           event or the later DOMContentLoaded task. */
        (void) script_runtime_collect_and_trim(runtime);
        runtime->result.external_scripts_failed++;
        runtime->result.success = true;
        size_t slot = 0;
        if (js_rt_bridge_node_slot_for_handle(
                &runtime->bridge, script_handle, &slot)) {
            (void) script_runtime_dispatch_node(
                runtime, runtime->bridge.nodes[slot], "error", NULL);
        }
        js_rt_runtime_update_result(runtime, result);
        runtime->promise_rejection_state.active_source =
            previous_rejection_source;
        return false;
    }
    runtime->result.external_script_bytes += source_length;
    if (!dispatch_completion) {
        runtime->result.success = true;
        js_rt_runtime_update_result(runtime, result);
        runtime->promise_rejection_state.active_source =
            previous_rejection_source;
        return true;
    }
    runtime->result.external_scripts_loaded++;
    size_t slot = 0;
    bool live = js_rt_bridge_node_slot_for_handle(
        &runtime->bridge, script_handle, &slot);
    bool dispatched = !live || script_runtime_dispatch_node(
        runtime, runtime->bridge.nodes[slot], "load", result);
    runtime->result.success = dispatched;
    js_rt_runtime_update_result(runtime, result);
    runtime->promise_rejection_state.active_source = previous_rejection_source;
    return dispatched;
}

bool script_runtime_evaluate_external_typed(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool module, ScriptResult *result)
{
    char module_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT] = {0};
    if (runtime != NULL && module) {
        runtime->active_module_credentials =
            js_rt_module_credentials_for_node(script_node);
        js_rt_module_referrer_policy_for_node(
            script_node, runtime->bridge.referrer_policy,
            module_referrer_policy);
    }
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, source_url, source_url,
        runtime == NULL ? NULL
                        : (module ? module_referrer_policy
                                  : runtime->bridge.referrer_policy),
        runtime == NULL ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                        : runtime->active_module_credentials,
        module, true, module ? NULL : source_url, result);
}

bool script_runtime_evaluate_external_classic_cached(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, ScriptResult *result)
{
    const char *name = response_url == NULL ? request_url : response_url;
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, name, name,
        runtime == NULL ? NULL : runtime->bridge.referrer_policy,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, false, true, request_url, result);
}

bool js_rt_evaluate_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool final_segment)
{
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, source_url, source_url,
        runtime == NULL ? NULL : runtime->bridge.referrer_policy,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, false, final_segment, NULL, NULL);
}

bool js_rt_preflight_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url)
{
    if (runtime == NULL || script_node == NULL || source == NULL) return false;
    const char *name =
        source_url == NULL ? "<external-script-preflight>" : source_url;
    const char *previous_source =
        runtime->promise_rejection_state.active_source;
    runtime->promise_rejection_state.active_source = name;
    js_rt_runtime_arm_watchdog(runtime);
    bool admitted = false;
    JSValue compiled = js_rt_compile_source_type(
        runtime->context, source, source_length, name, JS_EVAL_TYPE_GLOBAL,
        SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result, &admitted);
    bool ok = admitted && !JS_IsException(compiled);
    if (ok) {
        JS_FreeValue(runtime->context, compiled);
        runtime->promise_rejection_state.active_source = previous_source;
        return true;
    }
    if (JS_IsException(compiled)) {
        js_rt_record_exception(runtime->context, &runtime->result);
    } else {
        JS_FreeValue(runtime->context, compiled);
    }
    js_rt_capture_error_source_context(
        source, source_length, name, &runtime->result);
    if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
        fprintf(stderr,
                "dynamic-script-segment-preflight-failure url=\"%s\" "
                "bytes=%zu error=\"%s\" context=\"%s\"\n",
                name, source_length, runtime->result.error,
                runtime->result.error_source_context);
    }
    runtime->result.external_scripts_failed++;
    runtime->result.success = true;
    (void) script_runtime_collect_and_trim(runtime);
    (void) script_runtime_dispatch_node(
        runtime, script_node, "error", NULL);
    runtime->promise_rejection_state.active_source = previous_source;
    return false;
}

bool script_runtime_evaluate_external_module(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, ScriptResult *result)
{
    return script_runtime_evaluate_external_module_credentials(
        runtime, script_node, source, source_length, request_url,
        response_url, TILEFINCH_CREDENTIALS_SAME_ORIGIN, result);
}

bool script_runtime_evaluate_external_module_credentials(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, TilefinchCredentialsMode credentials,
    ScriptResult *result)
{
    char effective_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT] = {0};
    if (runtime != NULL) {
        js_rt_module_referrer_policy_for_node(
            script_node, runtime->bridge.referrer_policy,
            effective_referrer_policy);
    }
    return script_runtime_evaluate_external_module_context(
        runtime, script_node, source, source_length, request_url,
        response_url, runtime == NULL ? NULL : effective_referrer_policy,
        credentials, result);
}

bool script_runtime_evaluate_external_module_context(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials, ScriptResult *result)
{
    if (runtime == NULL
        || !runtime_module_referrer_policy_valid(
               effective_referrer_policy)
        || (credentials != TILEFINCH_CREDENTIALS_SAME_ORIGIN
            && credentials != TILEFINCH_CREDENTIALS_INCLUDE)) return false;
    ScriptModuleMapStatus settled = script_runtime_module_map_status(
        runtime, request_url);
    if (settled == SCRIPT_MODULE_MAP_EVALUATED
        || settled == SCRIPT_MODULE_MAP_FAILED) {
        bool succeeded = settled == SCRIPT_MODULE_MAP_EVALUATED;
        if (succeeded) {
            js_rt_saturating_add_size(
                &runtime->result.external_scripts_loaded, 1);
        } else {
            js_rt_saturating_add_size(
                &runtime->result.external_scripts_failed, 1);
        }
        runtime->result.success = true;
        bool dispatched = script_runtime_dispatch_node(
            runtime, script_node, succeeded ? "load" : "error", result);
        js_rt_runtime_update_result(runtime, result);
        return succeeded && dispatched;
    }
    runtime->active_module_credentials = credentials;
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, request_url, response_url,
        effective_referrer_policy, credentials, true, true, NULL, result);
}

TilefinchCredentialsMode script_runtime_module_credentials(
    const ScriptRuntime *runtime)
{
    return runtime != NULL
        && runtime->active_module_credentials == TILEFINCH_CREDENTIALS_INCLUDE
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
}

bool script_runtime_evaluate_external(ScriptRuntime *runtime,
                                      lxb_dom_node_t *script_node,
                                      const char *source,
                                      size_t source_length,
                                      const char *source_url,
                                      ScriptResult *result)
{
    return script_runtime_evaluate_external_typed(
        runtime, script_node, source, source_length, source_url, false,
        result);
}
