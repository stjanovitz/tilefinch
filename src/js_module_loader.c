/* ES-module pipeline: MIME admission, module graph registration, specifier
   normalization, the QuickJS module loader, and external classic/module
   evaluation entry points.  Split from js_runtime.c; shares the runtime
   internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

static bool script_ascii_whitespace(unsigned char value)
{
    return value == 0x09u || value == 0x0au || value == 0x0cu
        || value == 0x0du || value == 0x20u;
}

static bool script_javascript_mime_span_allowed(
    const char *content_type, size_t length)
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
    while (length != 0
           && script_ascii_whitespace((unsigned char) *content_type)) {
        content_type++;
        length--;
    }
    const char *semicolon = memchr(content_type, ';', length);
    if (semicolon != NULL) length = (size_t) (semicolon - content_type);
    while (length != 0
           && script_ascii_whitespace(
                  (unsigned char) content_type[length - 1])) length--;
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

bool script_module_mime_type_allowed(const char *content_type)
{
    return content_type != NULL
        && script_javascript_mime_span_allowed(
            content_type, strlen(content_type));
}

bool script_compile_classic_bytecode(
    Budget *budget, const char *source, size_t source_length,
    const char *source_url, size_t maximum_bytecode_length,
    unsigned char **bytecode, size_t *bytecode_length)
{
    if (bytecode != NULL) *bytecode = NULL;
    if (bytecode_length != NULL) *bytecode_length = 0;
    if (budget == NULL || source == NULL || source_length == 0
        || source_length == SIZE_MAX
        || source_url == NULL || source_url[0] == '\0'
        || maximum_bytecode_length == 0 || bytecode == NULL
        || bytecode_length == NULL) return false;

    /* Cache/pack sources are exact byte spans. QuickJS requires a readable
       trailing NUL even when JS_Eval is passed an explicit length. */
    char *terminated_source = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, source_length + 1u);
    if (terminated_source == NULL) return false;
    memcpy(terminated_source, source, source_length);
    terminated_source[source_length] = '\0';
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(budget);
    if (pool == NULL) {
        budget_free(budget, terminated_source);
        return false;
    }
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    JSContext *context = NULL;
    JSValue compiled = JS_UNDEFINED;
    uint8_t *serialized = NULL;
    size_t serialized_length = 0;
    bool okay = false;
    if (runtime != NULL) {
        /* Installation is infrequent, but an untrusted package still must not
           create a second unbounded JavaScript heap beside the live page. */
        JS_SetMemoryLimit(runtime, 8u * 1024u * 1024u);
        JS_SetMaxStackSize(runtime, 256u * 1024u);
        /* The authoritative source is already retained in the offline pack.
           Keeping another source copy inside bytecode wastes most of the
           accelerator budget while adding no fallback or diagnostic value.
           Large games also carry disproportionately large line/debug tables;
           restoring those tables delayed their first usable frame by seconds
           on PSP.  Strip them only for the large-game class. Small scripts
           keep line metadata, while every package still retains source for a
           compiler-ABI miss or ordinary source recompile. */
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        JS_SetStripInfo(runtime,
            source_length >= 128u * 1024u
                ? JS_STRIP_DEBUG : JS_STRIP_SOURCE);
#endif
        context = JS_NewContext(runtime);
    }
    if (context != NULL) {
        compiled = JS_Eval(
            context, terminated_source, source_length, source_url,
            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(compiled)) {
            int write_flags = JS_WRITE_OBJ_BYTECODE;
#if !defined(PSP_BROWSER_BELLARD_QUICKJS)
            write_flags |= source_length >= 128u * 1024u
                ? JS_WRITE_OBJ_STRIP_DEBUG : JS_WRITE_OBJ_STRIP_SOURCE;
#endif
            serialized = JS_WriteObject(
                context, &serialized_length, compiled, write_flags);
        }
        if (serialized != NULL && serialized_length != 0
            && serialized_length <= maximum_bytecode_length) {
            unsigned char *copy = budget_malloc_category(
                budget, BUDGET_CATEGORY_SESSION, serialized_length);
            if (copy != NULL) {
                memcpy(copy, serialized, serialized_length);
                *bytecode = copy;
                *bytecode_length = serialized_length;
                okay = true;
            }
        }
        if (JS_IsException(compiled)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
        } else {
            JS_FreeValue(context, compiled);
        }
        js_free(context, serialized);
        JS_FreeContext(context);
    }
    if (runtime != NULL) JS_FreeRuntime(runtime);
    if (!budget_quickjs_pool_destroy(pool)) {
        budget_free(budget, *bytecode);
        *bytecode = NULL;
        *bytecode_length = 0;
        okay = false;
    }
    budget_free(budget, terminated_source);
    return okay;
}

bool script_type_attribute_classify(
    const char *type, size_t length, bool *module)
{
    if (module != NULL) *module = false;
    if (type == NULL) return true;
    while (length != 0
           && script_ascii_whitespace((unsigned char) *type)) {
        type++;
        length--;
    }
    while (length != 0
           && script_ascii_whitespace(
                  (unsigned char) type[length - 1u])) length--;
    if (length == 0) return true;
    if (length == 6u && strncasecmp(type, "module", 6u) == 0) {
        if (module != NULL) *module = true;
        return true;
    }
    return script_javascript_mime_span_allowed(type, length);
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

bool script_runtime_module_loaded(const ScriptRuntime *runtime,
                                  const char *request_url)
{
    size_t index = runtime_module_index_lookup(runtime, request_url);
    return index != SIZE_MAX
        && runtime->module_bases[index].response_url != NULL;
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
    return tilefinch_referrer_policy_code(policy, strlen(policy), false);
}

const char *js_rt_runtime_module_referrer_policy_text(uint8_t code)
{
    return tilefinch_referrer_policy_name(code);
}

static bool runtime_module_referrer_policy_valid(const char *policy)
{
    return js_rt_runtime_module_referrer_policy_code(policy) != UINT8_MAX;
}

static bool runtime_module_edge_register(
    ScriptRuntime *runtime, const char *request_url,
    size_t parent_index, const char *classic_referrer_url,
    const char *effective_referrer_policy,
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
    char *referrer_copy = NULL;
    if (parent_index == SIZE_MAX && classic_referrer_url != NULL
        && classic_referrer_url[0] != '\0') {
        size_t referrer_length = strlen(classic_referrer_url);
        referrer_copy = budget_malloc(runtime->budget, referrer_length + 1);
        if (referrer_copy == NULL) {
            budget_free(runtime->budget, request_copy);
            return false;
        }
        memcpy(referrer_copy, classic_referrer_url, referrer_length + 1);
    }
    size_t index = runtime->module_base_count++;
    runtime->module_bases[index] =
        (ScriptModuleBaseEntry) {
            .request_url = request_copy,
            .classic_referrer_url = referrer_copy,
            .credentials = credentials,
            .parent_index = parent_index == SIZE_MAX
                ? UINT16_MAX : (uint16_t) parent_index,
            .effective_referrer_policy = policy_code,
            /* A static import inherits its parent's; import() from a
               classic script inherits that script's. */
            .csp_grant = parent_index == SIZE_MAX
                ? runtime->bridge.current_script_csp_grant
                : runtime->module_bases[parent_index].csp_grant
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

uint8_t script_runtime_module_csp_grant(const ScriptRuntime *runtime,
                                        const char *module_url)
{
    if (runtime == NULL || module_url == NULL) return 0;
    for (size_t i = 0; i < runtime->module_base_count; i++) {
        const ScriptModuleBaseEntry *entry = &runtime->module_bases[i];
        if (strcmp(entry->request_url, module_url) == 0
            || (entry->response_url != NULL
                && strcmp(entry->response_url, module_url) == 0))
            return entry->csp_grant;
    }
    return 0;
}

void js_rt_runtime_module_csp_grant_set(ScriptRuntime *runtime,
                                        const char *request_url,
                                        uint8_t grant)
{
    size_t index = runtime == NULL || request_url == NULL ? SIZE_MAX
        : runtime_module_index_lookup(runtime, request_url);
    if (index != SIZE_MAX
        && runtime->module_bases[index].parent_index == UINT16_MAX)
        runtime->module_bases[index].csp_grant = grant;
}

bool js_rt_runtime_module_root_register(
ScriptRuntime *runtime, const char *request_url, const char *response_url,
const char *effective_referrer_policy,
TilefinchCredentialsMode credentials)
{
    size_t index = SIZE_MAX;
    return runtime_module_edge_register(
               runtime, request_url, SIZE_MAX, NULL,
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

/*
 * Source retention for page modules.
 *
 * QuickJS keeps each function's source text so Function.prototype.toString()
 * can return it; with shared source spans that is close to one copy of every
 * compiled module in the page heap (dropping it saves about 1 MB of
 * chatgpt.com's 11 MB realm, and 1.5 MB of its first-load peak because the
 * parser no longer copies each nested function's text). JS_STRIP_SOURCE
 * drops only that text. It keeps the line/column table and file name, so
 * Error.stack, our uncaught-error reports and the client-fatal-error beacons
 * some sites send still carry file:line:col. JS_STRIP_DEBUG would also drop
 * those and is not used for page code.
 *
 * What a page loses: toString() of a function compiled from a stripped
 * module returns QuickJS's NativeFunction form, "function name() {\n
 * [native code]\n}" (as the specification allows when no source text is
 * retained). Code that parses its own functions' text - dependency-injection
 * argument parsing, building a Worker from fn.toString(), self-integrity
 * checks - would see that form instead. Those patterns belong to classic
 * scripts and eval/Function bodies far more than to bundled ES modules, and
 * none of those are stripped. On chatgpt.com's startup (the site that set
 * this policy) no function compiled from a module had toString() called on it
 * at all; the one module that reads its own functions' text falls back
 * cleanly when it finds no location marker. Modules smaller than the
 * threshold keep their source: they hold under a tenth of a typical graph's
 * bytes, and a small helper module is where a stringified worker body is
 * most likely to live.
 *
 * A small module's bytecode therefore carries its text, as every classic
 * script's does. That is why a no-store module response, like a classic
 * one, is compiled and run but its bytecode never kept: retaining it would
 * retain the response. Cacheable responses keep their text in bytecode just
 * as the HTTP cache may keep the response itself.
 */
#define SCRIPT_MODULE_STRIP_SOURCE_MINIMUM_BYTES (8u * 1024u)

static int script_module_strip_flags(size_t source_length)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    return source_length >= SCRIPT_MODULE_STRIP_SOURCE_MINIMUM_BYTES
        ? JS_STRIP_SOURCE : 0;
#else
    (void) source_length;
    return 0;
#endif
}

/*
 * Module bytecode cache (BrowserScriptBytecodeTable in session.h).
 *
 * chatgpt.com loads ~112 modules and reloads itself once during startup, so
 * the whole graph used to be compiled twice; a revisit compiled it again.
 * After a module compiles, its record is serialized with JS_WriteObject and
 * kept in the session, keyed by the module name QuickJS bakes into the
 * record, the response URL, the top-level site that fetched it, the strip
 * policy and the exact source. A later load of the same bytes restores it
 * with JS_ReadObject and then resolves its imports (JS_ResolveModule) the
 * way JS_Eval does after a compile, so loading, linking, import.meta, dynamic
 * import and error unwinding take the same path either way. The cache sits
 * after fetching: every CSP, SRI, CORS, MIME and quota check has already
 * admitted the bytes, and the compile-admission size policy is applied to a
 * hit as to a compile. A record that fails to restore is dropped and the
 * source compiled instead.
 */
static bool module_bytecode_key_init(
    ScriptRuntime *runtime, BrowserScriptBytecodeKey *key,
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT], const char *source,
    size_t source_length, const char *module_name, const char *response_url,
    int strip_flags)
{
    memset(key, 0, sizeof(*key));
    /* An opaque origin has no network partition to key by. */
    if (runtime->session == NULL || response_url == NULL
        || response_url[0] == '\0'
        || runtime->session->maximum_module_bytecode_bytes == 0
        || script_runtime_origin_is_opaque(runtime)
        || runtime->bridge.top_level_url == NULL
        || !tilefinch_url_site_key(runtime->bridge.top_level_url, partition,
                                   TILEFINCH_ORIGIN_SERIALIZED_LIMIT)) {
        return false;
    }
    if (runtime->module_bytecode_generation == 0) {
        runtime->module_bytecode_generation =
            browser_session_module_bytecode_generation(runtime->session);
    }
    *key = (BrowserScriptBytecodeKey) {
        .module_name = module_name,
        .response_url = response_url,
        .partition_key = partition,
        .source = (const unsigned char *) source,
        .source_length = source_length,
        .compile_flags = (uint32_t) strip_flags
    };
    return true;
}

static void module_bytecode_clear_exception(JSContext *context)
{
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
}

/* The RAM table, then the persistent tier: on a RAM miss the pack for this
   key's group (if the tier's index names one) is read and verified, at
   most once per load, and its records copied into RAM, where the lookup is
   repeated. The synchronous disk work is timed, hit or not. */
static BrowserSharedBody *script_bytecode_lookup(
    ScriptRuntime *runtime, BrowserScriptBytecodeKind kind,
    BrowserScriptBytecodeKey *key, ScriptResult *result, bool *from_disk)
{
    *from_disk = false;
    BrowserSharedBody *cached = browser_session_script_bytecode_acquire(
        runtime->session, kind, key, runtime->module_bytecode_generation);
    if (cached != NULL
        || !browser_session_script_disk_enabled(runtime->session))
        return cached;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    if (browser_session_script_disk_promote(
            runtime->session, kind, key,
            runtime->module_bytecode_generation)) {
        cached = browser_session_script_bytecode_acquire(
            runtime->session, kind, key,
            runtime->module_bytecode_generation);
    }
    result->script_bytecode_disk_load_us +=
        (js_rt_monotonic_time_ns() - started_ns) / 1000u;
    *from_disk = cached != NULL;
    return cached;
}

/* Serialize a freshly compiled module into the cache when there is room.
   Serialization is an optional accelerator beside a compiled module: it is
   skipped rather than allowed to consume the realm's remaining headroom, and
   a refusal never affects the module. */
static void module_bytecode_store(ScriptRuntime *runtime, JSContext *context,
                                  BrowserScriptBytecodeKey *key,
                                  JSValueConst compiled,
                                  ScriptResult *result)
{
    size_t source_length = key->source_length;
    size_t heap_reserve = script_admission_work_reserve(
        source_length, SCRIPT_ADMISSION_STORE_HEAP_MULTIPLIER, SIZE_MAX);
    /* The copy lands in the page Budget, which keeps the presentation
       reserve script admission keeps. */
    const size_t budget_reserve = SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES;
    size_t budget_left = budget_remaining(runtime->session->budget);
    size_t minimum_bytecode = source_length / 4u + 1u;
    /* The persistent tier, when on, writes from the RAM table at idle. */
    if (script_runtime_heap_available(runtime) < heap_reserve
        || budget_left < budget_reserve
        || minimum_bytecode > budget_left - budget_reserve
        || !browser_session_module_bytecode_may_fit(
               runtime->session, key, minimum_bytecode,
               runtime->module_bytecode_generation)) {
        js_rt_saturating_add_size(
            &result->module_bytecode_cache_admission_skips, 1);
        return;
    }
    size_t length = 0;
    uint8_t *bytecode = JS_WriteObject(context, &length, compiled,
                                       JS_WRITE_OBJ_BYTECODE);
    if (bytecode == NULL) {
        module_bytecode_clear_exception(context);
        js_rt_saturating_add_size(
            &result->module_bytecode_cache_admission_skips, 1);
        return;
    }
    budget_left = budget_remaining(runtime->session->budget);
    bool stored = length != 0 && budget_left >= budget_reserve
        && length <= budget_left - budget_reserve
        && browser_session_module_bytecode_put(
               runtime->session, key, runtime->module_bytecode_generation,
               bytecode, length);
    js_free(context, bytecode);
    if (stored) {
        js_rt_saturating_add_size(&result->module_bytecode_cache_stores, 1);
        js_rt_saturating_add_size(
            &result->module_bytecode_cache_stored_bytes, length);
    } else {
        js_rt_saturating_add_size(
            &result->module_bytecode_cache_admission_skips, 1);
    }
}

/* Restore a cached record. Returns true when it was restored (its import
   resolution may still have failed: then *module is JS_EXCEPTION with the
   exception pending, exactly as a compile whose imports fail). Returns false
   when there was nothing usable to restore. */
static bool module_bytecode_restore(ScriptRuntime *runtime,
                                    JSContext *context,
                                    BrowserScriptBytecodeKey *key,
                                    ScriptResult *result, bool *admitted,
                                    JSValue *module)
{
    bool from_disk = false;
    BrowserSharedBody *cached = script_bytecode_lookup(
        runtime, BROWSER_SCRIPT_BYTECODE_MODULE, key, result, &from_disk);
    if (cached == NULL) return false;
    if (!js_rt_admit_cached_compile_source(
            context, key->source_length, key->module_name,
            SCRIPT_COMPILE_SOURCE_MODULE, result)) {
        /* The same refusal a compile of these bytes would get. */
        browser_shared_body_release(cached);
        *module = JS_EXCEPTION;
        return true;
    }
    *admitted = true;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    JSValue restored = JS_ReadObject(context, cached->data, cached->length,
                                     JS_READ_OBJ_BYTECODE);
    uint64_t read_ns = js_rt_monotonic_time_ns();
    /* Deserialization, successful or not. */
    result->module_bytecode_restore_us += (read_ns - started_ns) / 1000u;
    size_t restored_bytes = cached->length;
    bool restored_module = !JS_IsException(restored)
        && JS_VALUE_GET_TAG(restored) == JS_TAG_MODULE;
    if (from_disk && restored_module)
        js_rt_saturating_add_size(&result->script_bytecode_disk_hits, 1);
    browser_shared_body_release(cached);
    if (JS_IsException(restored)
        || JS_VALUE_GET_TAG(restored) != JS_TAG_MODULE) {
        if (JS_IsException(restored)) {
            module_bytecode_clear_exception(context);
        } else {
            JS_FreeValue(context, restored);
        }
        *admitted = false;
        js_rt_saturating_add_size(
            &result->module_bytecode_cache_restore_failures, 1);
        /* Neither tier keeps what does not restore. */
        browser_session_module_bytecode_invalidate(runtime->session, key);
        browser_session_script_disk_discard(
            runtime->session, BROWSER_SCRIPT_BYTECODE_MODULE, key);
        return false;
    }
    js_rt_saturating_add_size(&result->module_bytecode_cache_hits, 1);
    js_rt_saturating_add_size(&result->module_bytecode_cache_bytes,
                              restored_bytes);
    js_rt_heavy_note_restored(runtime, key->source_length, from_disk);
    /* JS_Eval resolves a compiled module's imports before returning it;
       do the same here, loading each import through the ordinary loader. */
    if (JS_ResolveModule(context, restored) < 0) {
        JS_FreeValue(context, restored);
        *module = JS_EXCEPTION;
        return true;
    }
    *module = restored;
    return true;
}

JSValue js_rt_module_compile_external(
    ScriptRuntime *runtime, JSContext *context, const char *source,
    size_t source_length, const char *module_name, const char *response_url,
    bool response_no_store, ScriptResult *result, bool *admitted)
{
    if (admitted != NULL) *admitted = false;
    if (runtime == NULL || context == NULL || source == NULL
        || module_name == NULL || result == NULL || admitted == NULL) {
        return JS_EXCEPTION;
    }
    int strip_flags = script_module_strip_flags(source_length);
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    BrowserScriptBytecodeKey key;
    uint64_t key_started_ns = js_rt_monotonic_time_ns();
    bool cacheable = module_bytecode_key_init(
        runtime, &key, partition, source, source_length, module_name,
        response_url, strip_flags);
    result->module_key_us +=
        (js_rt_monotonic_time_ns() - key_started_ns) / 1000u;
    if (cacheable) {
        JSValue restored = JS_UNDEFINED;
        if (module_bytecode_restore(runtime, context, &key, result,
                                    admitted, &restored)) {
            return restored;
        }
        js_rt_saturating_add_size(&result->module_bytecode_cache_misses, 1);
    }
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    /* The strip flag is runtime-wide and read when each function definition
       is created, so it covers exactly this module's parse. Imports resolved
       inside JS_Eval run through runtime_module_loader(), which clears it
       again before anything else can compile. */
    JSRuntime *js_runtime = JS_GetRuntime(context);
    int previous_strip = JS_GetStripInfo(js_runtime);
    JS_SetStripInfo(js_runtime, strip_flags);
#endif
    uint64_t parse_started_ns = js_rt_monotonic_time_ns();
    uint64_t nested_before_ns = runtime->module_nested_ns;
    JSValue compiled = js_rt_compile_source_type(
        context, source, source_length, module_name, JS_EVAL_TYPE_MODULE,
        SCRIPT_COMPILE_SOURCE_MODULE, result, admitted);
    uint64_t parse_finished_ns = js_rt_monotonic_time_ns();
    /* Exclusive: imports loaded and compiled inside this JS_Eval are
       charged to their own modules. */
    uint64_t nested_ns = runtime->module_nested_ns - nested_before_ns;
    uint64_t parse_ns = parse_finished_ns - parse_started_ns;
    result->module_parse_us +=
        (parse_ns > nested_ns ? parse_ns - nested_ns : 0) / 1000u;
    result->module_source_bytes += source_length;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    JS_SetStripInfo(js_runtime, previous_strip);
#endif
    if (cacheable && *admitted && !JS_IsException(compiled)) {
        if (response_no_store) {
            /* As for a classic script: no-store asks that the response not
               be kept, and a small module's bytecode carries its text. */
            js_rt_saturating_add_size(
                &result->module_bytecode_cache_no_store_skips, 1);
        } else {
            module_bytecode_store(runtime, context, &key, compiled, result);
            result->module_store_us +=
                (js_rt_monotonic_time_ns() - parse_finished_ns) / 1000u;
        }
    }
    return compiled;
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
    /* A request with no module parent comes from a classic script (or a
       host evaluation); its base URL is the request's referrer. */
    if (!runtime_module_edge_register(
            runtime, resolved, parent_index,
            parent_index == SIZE_MAX ? base : NULL, policy, credentials,
            NULL)) {
        return NULL;
    }
    return js_strdup(context, resolved);
}

static JSModuleDef *runtime_module_load_record(JSContext *context,
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
    const ScriptModuleBaseEntry *entry = &runtime->module_bases[module_index];
    const char *referrer = entry->classic_referrer_url;
    if (stored_parent_index != UINT16_MAX
        && parent_index < runtime->module_base_count) {
        referrer = runtime->module_bases[parent_index].response_url;
    }
    if (referrer == NULL) {
        JS_ThrowReferenceError(context,
                               "module referrer is missing for '%s'",
                               module_name);
        return NULL;
    }
    char request_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char referrer_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    const char *entry_policy = js_rt_runtime_module_referrer_policy_text(
        entry->effective_referrer_policy);
    if (strlen(entry->request_url) >= sizeof(request_url)
        || strlen(referrer) >= sizeof(referrer_url)
        || entry_policy == NULL
        || strlen(entry_policy) >= sizeof(referrer_policy)) {
        JS_ThrowRangeError(context, "module request metadata is too large");
        return NULL;
    }
    snprintf(request_url, sizeof(request_url), "%s", entry->request_url);
    snprintf(referrer_url, sizeof(referrer_url), "%s", referrer);
    snprintf(referrer_policy, sizeof(referrer_policy), "%s",
             entry_policy);
    TilefinchCredentialsMode credentials = entry->credentials;
    ScriptModuleLoadRequest request = {
        .request_url = request_url,
        .referrer_url = referrer_url,
        .referrer_policy = referrer_policy,
        .csp_grant = entry->csp_grant,
        .credentials = credentials
    };
    runtime->active_module_credentials = credentials;
    if (tilefinch_trace_module_order()) {
        fprintf(stderr, "tilefinch: module-load %s <- %s\n", request_url,
                referrer_url);
    }
    ScriptModuleLoadResult loaded = {0};
    uint64_t fetch_started_ns = js_rt_monotonic_time_ns();
    bool fetched = runtime->module_load(runtime->module_opaque, &request,
                                        &loaded);
    runtime->result.module_fetch_us +=
        (js_rt_monotonic_time_ns() - fetch_started_ns) / 1000u;
    if (!fetched
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
    uint64_t nested_before_ns = runtime->module_nested_ns;
    size_t restores_before = runtime->result.module_bytecode_cache_hits;
    script_runtime_note_script_source(
        runtime, loaded.source_length, loaded.source_length);
    JSValue compiled = js_rt_module_compile_external(
        runtime, context, loaded.source, loaded.source_length, module_name,
        loaded.response_url, loaded.response_no_store, &runtime->result,
        &admitted);
    /* Restores are timed as module_bytecode_restore_us. Exclusive of the
       imports this compile loaded (each counts its own compile). */
    if (runtime->result.module_bytecode_cache_hits == restores_before) {
        uint64_t elapsed_ns = js_rt_monotonic_time_ns() - compile_started_ns;
        uint64_t nested_ns = runtime->module_nested_ns - nested_before_ns;
        runtime->result.module_compile_us +=
            (elapsed_ns > nested_ns ? elapsed_ns - nested_ns : 0) / 1000u;
        runtime->result.module_compile_count++;
    }
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

/* Charges one nested load to the importer's nested total. The load's own
   imports were added to that total while it ran, but they are already inside
   its wall time, so the total becomes its value before the load plus the
   load's inclusive time: every importer up the chain subtracts each
   descendant exactly once. */
static JSModuleDef *runtime_module_load_timed(JSContext *context,
                                              const char *module_name,
                                              void *opaque)
{
    ScriptRuntime *runtime = opaque;
    uint64_t nested_before_ns =
        runtime == NULL ? 0 : runtime->module_nested_ns;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    JSModuleDef *module = runtime_module_load_record(
        context, module_name, opaque);
    if (runtime != NULL) {
        runtime->module_nested_ns = nested_before_ns
            + (js_rt_monotonic_time_ns() - started_ns);
    }
    return module;
}

static JSModuleDef *runtime_module_loader(JSContext *context,
                                          const char *module_name,
                                          void *opaque)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    /* QuickJS resolves a module's imports inside the JS_Eval that compiled
       it, i.e. while that module's strip flag is still installed. Nothing
       loaded or run from here inherits it; the dependency's own compile
       chooses its flag. */
    JSRuntime *js_runtime = JS_GetRuntime(context);
    int previous_strip = JS_GetStripInfo(js_runtime);
    JS_SetStripInfo(js_runtime, 0);
    JSModuleDef *module = runtime_module_load_timed(
        context, module_name, opaque);
    JS_SetStripInfo(js_runtime, previous_strip);
    return module;
#else
    return runtime_module_load_timed(context, module_name, opaque);
#endif
}

void js_rt_runtime_module_metadata_clear(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    for (size_t i = 0; i < runtime->module_base_count; i++) {
        budget_free(runtime->budget, runtime->module_bases[i].response_url);
        budget_free(runtime->budget, runtime->module_bases[i].request_url);
        budget_free(runtime->budget,
                    runtime->module_bases[i].classic_referrer_url);
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

void *script_runtime_module_loader_opaque(
    const ScriptRuntime *runtime, ScriptModuleLoadCallback load)
{
    return runtime != NULL && load != NULL && runtime->module_load == load
        ? runtime->module_opaque : NULL;
}

#ifndef TILEFINCH_NO_TRACE
/* Site-census diagnosis (TILEFINCH_TRACE_CENSUS): one line per classic
   external-script bytecode lookup and per store attempt. Lab builds only. */
static void census_trace_classic_bytecode(const char *record,
                                          const char *result, size_t bytes,
                                          size_t bytecode, uint32_t ordinal,
                                          const char *url)
{
    if (!tilefinch_trace_census()) return;
    printf("census-bytecode-%s result=%s bytes=%zu bytecode=%zu ordinal=%u "
           "url=%.300s\n", record, result, bytes, bytecode,
           (unsigned) ordinal, url == NULL ? "" : url);
}
#define CENSUS_TRACE_CLASSIC_BYTECODE(record, result, bytes, bytecode,    \
                                      ordinal, url)                        \
    census_trace_classic_bytecode(record, result, bytes, bytecode,         \
                                  ordinal, url)
#else
#define CENSUS_TRACE_CLASSIC_BYTECODE(record, result, bytes, bytecode,    \
                                      ordinal, url)                        \
    ((void) 0)
#endif

/*
 * Classic external-script bytecode (the session's CLASSIC table). Keyed like
 * a module: compile name, request URL (the HTTP cache key), top-level site,
 * ResourceLoader segment ordinal, compile type and the SHA-256 of the exact
 * bytes compiled. The lookup sits after fetching and every CSP, SRI, MIME
 * and quota check, and a hit passes the same compile-admission size policy
 * as a compile. An opaque-origin realm has no partition to key by.
 */
static bool classic_bytecode_key_init(
    ScriptRuntime *runtime, BrowserScriptBytecodeKey *key,
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT], const char *source,
    size_t source_length, const char *name, const char *cache_url,
    uint32_t ordinal)
{
    memset(key, 0, sizeof(*key));
    if (runtime->session == NULL || source == NULL || source_length == 0
        || name == NULL || name[0] == '\0' || cache_url == NULL
        || cache_url[0] == '\0'
        || runtime->session->maximum_classic_bytecode_bytes == 0
        /* Not network responses: a data: script's URL is its source and a
           blob: URL is minted afresh by every page that makes one. */
        || strncasecmp(cache_url, "data:", 5) == 0
        || strncasecmp(cache_url, "blob:", 5) == 0
        || script_runtime_origin_is_opaque(runtime)
        || runtime->bridge.top_level_url == NULL
        || !tilefinch_url_site_key(runtime->bridge.top_level_url, partition,
                                   TILEFINCH_ORIGIN_SERIALIZED_LIMIT)) {
        return false;
    }
    if (runtime->module_bytecode_generation == 0) {
        runtime->module_bytecode_generation =
            browser_session_module_bytecode_generation(runtime->session);
    }
    *key = (BrowserScriptBytecodeKey) {
        .module_name = name,
        .response_url = cache_url,
        .partition_key = partition,
        .source = (const unsigned char *) source,
        .source_length = source_length,
        .compile_flags = (uint32_t) JS_EVAL_TYPE_GLOBAL,
        .ordinal = ordinal
    };
    return true;
}

/*
 * Deferred classic stores.
 *
 * Serializing a compiled script (JS_WriteObject) and copying it into the
 * table used to sit between a script's compile and its execution, on the
 * load's critical path: 141 ms of host time across the census corpus on a
 * first visit, an estimated two seconds or more on the PSP for one large
 * news page. Now the load only queues the store: it digests the source
 * (the key needs the exact bytes, which are not kept) and retains the
 * compiled top-level function, which QuickJS would otherwise free once the
 * script has run. Idle work (script_runtime_store_pending_bytecode)
 * serializes one queued script per turn into the table.
 *
 * The retained function is the cheapest thing that can still be
 * serialized: no source copy, no early serialization. Its functions are
 * shared with the closures the script created, so it keeps alive only the
 * parts of the script nothing else references (typically the top level and
 * run-once wrappers). Lazily compiled functions the script called during
 * the load are serialized with their bodies (they would otherwise be
 * written as stubs that compile on first call): the same functions from the
 * same bytes, compiled earlier.
 *
 * The queue belongs to the realm and dies with it. A page left (or a realm
 * replaced) before its idle turns stored everything still holds valid
 * compiles, so teardown stores the oldest of them up to
 * SCRIPT_BYTECODE_TEARDOWN_FLUSH_BYTES of source and drops the rest: that
 * bounds what the next navigation pays (serialization is the cheap part of
 * a store; the source digest was taken during the load). A cache clear
 * (the session's epoch moves), JavaScript heap pressure, and a queue that
 * would exceed the table's ceiling or entry count drop queued stores too.
 */
typedef struct ScriptBytecodePending {
    JSValue compiled;
    /* "name\0url\0partition\0" in one allocation. */
    char *strings;
    const char *url;
    const char *partition;
    size_t source_length;
    uint32_t ordinal;
    uint32_t compile_flags;
    uint32_t epoch;
    uint8_t source_digest[32];
} ScriptBytecodePending;

#define SCRIPT_BYTECODE_PENDING_LIMIT BROWSER_SCRIPT_BYTECODE_ENTRIES

static void pending_bytecode_release(ScriptRuntime *runtime,
                                     ScriptBytecodePending *entry,
                                     const char *reason)
{
    if (reason != NULL) {
        runtime->result.external_script_bytecode_deferred_dropped++;
        CENSUS_TRACE_CLASSIC_BYTECODE("store", reason, entry->source_length,
                                      0, entry->ordinal, entry->url);
    }
    (void) reason;
    JS_FreeValue(runtime->context, entry->compiled);
    budget_free(runtime->budget, entry->strings);
    runtime->bytecode_pending_bytes -=
        entry->source_length <= runtime->bytecode_pending_bytes
            ? entry->source_length : runtime->bytecode_pending_bytes;
}

/* Removes queue entry `at`, keeping the rest in order (oldest first). */
static void pending_bytecode_remove(ScriptRuntime *runtime, size_t at,
                                    const char *reason)
{
    ScriptBytecodePending *queue = runtime->bytecode_pending;
    pending_bytecode_release(runtime, &queue[at], reason);
    memmove(&queue[at], &queue[at + 1u],
            (runtime->bytecode_pending_count - at - 1u) * sizeof(*queue));
    runtime->bytecode_pending_count--;
}

/* An empty queue holds no memory. */
static void pending_bytecode_trim(ScriptRuntime *runtime)
{
    if (runtime->bytecode_pending_count != 0) return;
    budget_free(runtime->budget, runtime->bytecode_pending);
    runtime->bytecode_pending = NULL;
    runtime->bytecode_pending_bytes = 0;
}

void script_runtime_drop_pending_bytecode(ScriptRuntime *runtime,
                                          const char *reason)
{
    if (runtime == NULL || runtime->context == NULL) return;
    while (runtime->bytecode_pending_count != 0)
        pending_bytecode_remove(runtime, runtime->bytecode_pending_count - 1u,
                                reason);
    pending_bytecode_trim(runtime);
}

size_t script_runtime_pending_bytecode_count(const ScriptRuntime *runtime)
{
    return runtime == NULL ? 0 : runtime->bytecode_pending_count;
}

static bool pending_bytecode_same_record(const ScriptBytecodePending *entry,
                                         const BrowserScriptBytecodeKey *key)
{
    return entry->ordinal == key->ordinal
        && strcmp(entry->strings, key->module_name) == 0
        && strcmp(entry->url, key->response_url) == 0
        && strcmp(entry->partition, key->partition_key) == 0;
}

static bool pending_bytecode_key_digest(BrowserScriptBytecodeKey *key)
{
    if (!key->digest_ready)
        key->digest_ready = key->source != NULL && tilefinch_sha256_digest(
            key->source, key->source_length, key->source_digest);
    return key->digest_ready;
}

/* The queued compile of exactly this key, or JS_UNDEFINED. */
static JSValue pending_bytecode_lookup(ScriptRuntime *runtime,
                                       BrowserScriptBytecodeKey *key)
{
    for (size_t i = 0; i < runtime->bytecode_pending_count; i++) {
        ScriptBytecodePending *entry = &runtime->bytecode_pending[i];
        if (!pending_bytecode_same_record(entry, key)) continue;
        if (entry->source_length != key->source_length
            || entry->compile_flags != key->compile_flags
            || entry->epoch != runtime->session->script_bytecode_epoch
            || !pending_bytecode_key_digest(key)
            || memcmp(entry->source_digest, key->source_digest,
                      sizeof(entry->source_digest)) != 0)
            return JS_UNDEFINED;
        return JS_DupValue(runtime->context, entry->compiled);
    }
    return JS_UNDEFINED;
}

/* Heap the realm keeps free while a store is queued or serialized: the
   module store's reserve. */
static size_t pending_bytecode_heap_reserve(size_t source_length)
{
    return script_admission_work_reserve(
        source_length, SCRIPT_ADMISSION_STORE_HEAP_MULTIPLIER, SIZE_MAX);
}

/* Largest classic script source whose compile is queued for the table. A
   queued compile keeps the script's whole function tree in the realm heap
   until idle work stores it, and storing it needs about four times its
   source of heap more. Above this size that is a large share of a 5 MiB
   realm on exactly the pages that load such bundles: on the second site
   census no store above it ever completed, at any table ceiling, and
   queuing gitlab's 1.4 MB bundle moved its realm's out-of-memory point. */
#define CLASSIC_BYTECODE_ENTRY_SOURCE_LIMIT (768u * 1024u)

/* Queues a freshly compiled classic script's store. Skipped, never
   affecting the script, when the realm or the table has no room for it. A
   no-store response (`no_store`) runs from source every time: no-store
   asks that the response not be kept, and its bytecode carries its source
   text for Function.prototype.toString. */
static void classic_bytecode_store(ScriptRuntime *runtime,
                                   BrowserScriptBytecodeKey *key,
                                   JSValueConst compiled, bool no_store)
{
    uint64_t started_ns = js_rt_monotonic_time_ns();
    size_t source_length = key->source_length;
    size_t heap_reserve = pending_bytecode_heap_reserve(source_length);
    /* The copy lands in the page Budget, which keeps the presentation
       reserve. */
    const size_t budget_reserve = SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES;
    size_t budget_left = budget_remaining(runtime->session->budget);
    /* Classic bytecode keeps each function's source text, so it is never
       much smaller than the source (1.0-1.4x across the site census):
       checking that much room first avoids a serialization put() refuses.
       Scripts already queued will need their room too. */
    size_t minimum_bytecode = source_length;
    size_t queued = runtime->bytecode_pending_bytes;
    /* A queued store holds JavaScript heap: give it back before the page
       runs short, oldest first. */
    while (runtime->bytecode_pending_count != 0
           && script_runtime_heap_available(runtime) < heap_reserve) {
        pending_bytecode_remove(runtime, 0, "drop-heap-pressure");
        pending_bytecode_trim(runtime);
        queued = runtime->bytecode_pending_bytes;
    }
    const char *skip = NULL;
    if (no_store) {
        skip = "skip-no-store";
    } else if (source_length > CLASSIC_BYTECODE_ENTRY_SOURCE_LIMIT) {
        skip = "skip-entry-size";
    } else if (script_runtime_heap_available(runtime) < heap_reserve) {
        skip = "skip-heap-reserve";
    } else if (budget_left < budget_reserve
               || minimum_bytecode > budget_left - budget_reserve) {
        skip = "skip-budget-reserve";
    } else if (runtime->bytecode_pending_count
                   >= SCRIPT_BYTECODE_PENDING_LIMIT
               || queued > SIZE_MAX - minimum_bytecode
               || !browser_session_script_bytecode_may_fit(
                   runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC, key,
                   minimum_bytecode + queued,
                   runtime->module_bytecode_generation)) {
        skip = "skip-table-full";
    }
    ScriptBytecodePending *slot = NULL;
    char *strings = NULL;
    if (skip == NULL && !pending_bytecode_key_digest(key))
        skip = "skip-digest";
    if (skip == NULL) {
        size_t name = strlen(key->module_name) + 1u;
        size_t url = strlen(key->response_url) + 1u;
        size_t partition = strlen(key->partition_key) + 1u;
        if (runtime->bytecode_pending == NULL)
            runtime->bytecode_pending = budget_malloc_category(
                runtime->budget, BUDGET_CATEGORY_JAVASCRIPT,
                SCRIPT_BYTECODE_PENDING_LIMIT
                    * sizeof(*runtime->bytecode_pending));
        strings = runtime->bytecode_pending == NULL ? NULL
            : budget_malloc_category(runtime->budget,
                                     BUDGET_CATEGORY_JAVASCRIPT,
                                     name + url + partition);
        if (strings == NULL) {
            skip = "skip-queue-allocation";
            pending_bytecode_trim(runtime);
        } else {
            memcpy(strings, key->module_name, name);
            memcpy(strings + name, key->response_url, url);
            memcpy(strings + name + url, key->partition_key, partition);
            /* One queued store per record: the newer compile replaces it. */
            for (size_t i = 0; i < runtime->bytecode_pending_count; i++) {
                if (pending_bytecode_same_record(
                        &runtime->bytecode_pending[i], key)) {
                    pending_bytecode_remove(runtime, i, NULL);
                    break;
                }
            }
            slot = &runtime->bytecode_pending[
                runtime->bytecode_pending_count++];
            *slot = (ScriptBytecodePending) {
                .compiled = JS_DupValue(runtime->context, compiled),
                .strings = strings,
                .url = strings + name,
                .partition = strings + name + url,
                .source_length = source_length,
                .ordinal = key->ordinal,
                .compile_flags = key->compile_flags,
                .epoch = runtime->session->script_bytecode_epoch
            };
            memcpy(slot->source_digest, key->source_digest,
                   sizeof(slot->source_digest));
            runtime->bytecode_pending_bytes += source_length;
        }
    }
    runtime->result.external_script_bytecode_store_us +=
        (js_rt_monotonic_time_ns() - started_ns) / 1000u;
    if (skip != NULL) {
        runtime->result.external_script_bytecode_cache_admission_skips++;
        CENSUS_TRACE_CLASSIC_BYTECODE("store", skip, source_length, 0,
                                      key->ordinal, key->response_url);
        return;
    }
    runtime->result.external_script_bytecode_deferred++;
    CENSUS_TRACE_CLASSIC_BYTECODE("store", "deferred", source_length, 0,
                                  key->ordinal, key->response_url);
}

static void pending_bytecode_publish(const ScriptRuntime *runtime,
                                     ScriptResult *result)
{
    if (result == NULL) return;
    const ScriptResult *live = &runtime->result;
    result->external_script_bytecode_cache_stores =
        live->external_script_bytecode_cache_stores;
    result->external_script_bytecode_cache_stored_bytes =
        live->external_script_bytecode_cache_stored_bytes;
    result->external_script_bytecode_cache_admission_skips =
        live->external_script_bytecode_cache_admission_skips;
    result->external_script_bytecode_deferred_dropped =
        live->external_script_bytecode_deferred_dropped;
    result->external_script_bytecode_idle_store_us =
        live->external_script_bytecode_idle_store_us;
}

/* Stores the oldest queued compile; `stored_label` names a successful store
   in the census ledger. */
static bool pending_bytecode_store_oldest(ScriptRuntime *runtime,
                                          ScriptResult *result_snapshot,
                                          const char *stored_label);

bool script_runtime_store_pending_bytecode(ScriptRuntime *runtime,
                                           ScriptResult *result_snapshot)
{
    return pending_bytecode_store_oldest(runtime, result_snapshot, "stored");
}

void script_runtime_flush_pending_bytecode(ScriptRuntime *runtime,
                                           size_t source_budget)
{
    if (runtime == NULL || runtime->context == NULL) return;
    while (runtime->bytecode_pending_count != 0
           && runtime->bytecode_pending[0].source_length <= source_budget) {
        source_budget -= runtime->bytecode_pending[0].source_length;
        (void) pending_bytecode_store_oldest(runtime, NULL,
                                             "stored-teardown");
    }
    script_runtime_drop_pending_bytecode(runtime, "drop-navigation");
}

static bool pending_bytecode_store_oldest(ScriptRuntime *runtime,
                                          ScriptResult *result_snapshot,
                                          const char *stored_label)
{
    if (runtime == NULL || runtime->context == NULL
        || runtime->bytecode_pending_count == 0) return false;
    if (runtime->session == NULL || runtime->session->budget == NULL) {
        script_runtime_drop_pending_bytecode(runtime, "drop-no-session");
        pending_bytecode_publish(runtime, result_snapshot);
        return true;
    }
    uint64_t started_ns = js_rt_monotonic_time_ns();
    ScriptBytecodePending *entry = &runtime->bytecode_pending[0];
    if (entry->epoch != runtime->session->script_bytecode_epoch) {
        pending_bytecode_remove(runtime, 0, "drop-cache-cleared");
        pending_bytecode_trim(runtime);
        pending_bytecode_publish(runtime, result_snapshot);
        return true;
    }
    BrowserScriptBytecodeKey key = {
        .module_name = entry->strings,
        .response_url = entry->url,
        .partition_key = entry->partition,
        .source_length = entry->source_length,
        .compile_flags = entry->compile_flags,
        .ordinal = entry->ordinal,
        .digest_ready = true
    };
    memcpy(key.source_digest, entry->source_digest,
           sizeof(key.source_digest));
    const size_t budget_reserve = SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES;
    size_t budget_left = budget_remaining(runtime->session->budget);
    const char *result = NULL;
    size_t length = 0;
    if (script_runtime_heap_available(runtime)
            < pending_bytecode_heap_reserve(entry->source_length)) {
        result = "skip-heap-reserve";
    } else if (budget_left < budget_reserve
               || entry->source_length > budget_left - budget_reserve) {
        result = "skip-budget-reserve";
    } else if (!browser_session_script_bytecode_may_fit(
                   runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key,
                   entry->source_length,
                   runtime->module_bytecode_generation)) {
        result = "skip-table-full";
    } else {
        uint8_t *bytecode = JS_WriteObject(runtime->context, &length,
                                           entry->compiled,
                                           JS_WRITE_OBJ_BYTECODE);
        if (bytecode == NULL) {
            JSValue exception = JS_GetException(runtime->context);
            JS_FreeValue(runtime->context, exception);
            result = "serialize-failed";
        } else {
            budget_left = budget_remaining(runtime->session->budget);
            bool stored = length != 0 && budget_left >= budget_reserve
                && length <= budget_left - budget_reserve
                && browser_session_script_bytecode_put(
                       runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC,
                       &key, runtime->module_bytecode_generation, bytecode,
                       length);
            js_free(runtime->context, bytecode);
            result = stored ? stored_label : "put-refused";
        }
    }
    bool stored = result == stored_label;
    if (stored) {
        runtime->result.external_script_bytecode_cache_stores++;
        js_rt_saturating_add_size(
            &runtime->result.external_script_bytecode_cache_stored_bytes,
            length);
    } else {
        runtime->result.external_script_bytecode_cache_admission_skips++;
    }
    CENSUS_TRACE_CLASSIC_BYTECODE("store", result, entry->source_length,
                                  length, entry->ordinal, entry->url);
    pending_bytecode_remove(runtime, 0, NULL);
    pending_bytecode_trim(runtime);
    runtime->result.external_script_bytecode_idle_store_us +=
        (js_rt_monotonic_time_ns() - started_ns) / 1000u;
    pending_bytecode_publish(runtime, result_snapshot);
    return true;
}

/* Restores the bytecode for `key`, or returns JS_UNDEFINED with nothing
   pending when there is none or it does not restore (the entry is then
   dropped). A compile this load queued for storing is used as it is.
   *admitted is false only when compile admission refused the source,
   exactly as it would refuse a compile. */
static JSValue classic_bytecode_restore(ScriptRuntime *runtime,
                                        BrowserScriptBytecodeKey *key,
                                        const char *name, bool *admitted,
                                        bool *disk_hit)
{
    *admitted = true;
    *disk_hit = false;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    JSValue queued = pending_bytecode_lookup(runtime, key);
    if (!JS_IsUndefined(queued)) {
        if (!js_rt_admit_cached_compile_source(
                runtime->context, key->source_length, name,
                SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result)) {
            JS_FreeValue(runtime->context, queued);
            *admitted = false;
            return JS_UNDEFINED;
        }
        runtime->result.external_script_bytecode_restore_us +=
            (js_rt_monotonic_time_ns() - started_ns) / 1000u;
        runtime->result.external_script_bytecode_cache_hits++;
        runtime->result.external_script_bytecode_pending_hits++;
        /* Compiled earlier in this realm and still queued for storing. */
        js_rt_heavy_note_restored(runtime, key->source_length, false);
        return queued;
    }
    bool from_disk = false;
    BrowserSharedBody *cached = script_bytecode_lookup(
        runtime, BROWSER_SCRIPT_BYTECODE_CLASSIC, key, &runtime->result,
        &from_disk);
    if (cached == NULL) {
        runtime->result.external_script_bytecode_restore_us +=
            (js_rt_monotonic_time_ns() - started_ns) / 1000u;
        return JS_UNDEFINED;
    }
    if (!js_rt_admit_cached_compile_source(
            runtime->context, key->source_length, name,
            SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result)) {
        browser_shared_body_release(cached);
        *admitted = false;
        return JS_UNDEFINED;
    }
    JSValue compiled = JS_ReadObject(runtime->context, cached->data,
                                     cached->length, JS_READ_OBJ_BYTECODE);
    runtime->result.external_script_bytecode_restore_us +=
        (js_rt_monotonic_time_ns() - started_ns) / 1000u;
    size_t restored_bytes = cached->length;
    browser_shared_body_release(cached);
    if (JS_IsException(compiled)) {
        JSValue exception = JS_GetException(runtime->context);
        JS_FreeValue(runtime->context, exception);
        runtime->result.external_script_bytecode_cache_restore_failures++;
        browser_session_script_bytecode_invalidate(
            runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC, key);
        browser_session_script_disk_discard(
            runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC, key);
        return JS_UNDEFINED;
    }
    if (from_disk) runtime->result.script_bytecode_disk_hits++;
    *disk_hit = from_disk;
    runtime->result.external_script_bytecode_cache_hits++;
    js_rt_saturating_add_size(
        &runtime->result.external_script_bytecode_cache_bytes,
        restored_bytes);
    js_rt_heavy_note_restored(runtime, key->source_length, from_disk);
    return compiled;
}

/* A classic script's fallback encoding is its charset attribute when that
   names an encoding, else the document's (HTML "fetch a classic script").
   The response's Content-Type charset is not plumbed this far, so a legacy
   decode applies only to a source that is not valid UTF-8: a UTF-8 script
   on a legacy page is never re-decoded, and a legacy one no longer fails
   to compile on its first non-ASCII string literal. Returns a NUL-ended
   budget copy, or NULL to evaluate the bytes as they are. */
static char *js_rt_decode_classic_script_source(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t *length)
{
    if (runtime == NULL || source == NULL || length == NULL) return NULL;
    TilefinchEncoding encoding = TILEFINCH_ENCODING_NONE;
    size_t charset_length = 0;
    const char *charset = document_attribute(
        script_node, "charset", &charset_length);
    if (charset != NULL) {
        encoding = tilefinch_encoding_for_label(charset, charset_length);
    }
    if (!tilefinch_encoding_decodable(encoding)
        && runtime->document != NULL) {
        encoding = (TilefinchEncoding) runtime->document->encoding;
    }
    if (!tilefinch_encoding_decodable(encoding)
        || encoding == TILEFINCH_ENCODING_UTF8
        || tilefinch_utf8_valid((const unsigned char *) source, *length)) {
        return NULL;
    }
    size_t capacity = tilefinch_decoder_maximum_output(encoding, *length);
    if (capacity == SIZE_MAX) return NULL;
    char *decoded = budget_malloc(runtime->budget, capacity + 1u);
    if (decoded == NULL) return NULL;
    TilefinchDecoder decoder;
    tilefinch_decoder_init(&decoder, encoding);
    const unsigned char *input = (const unsigned char *) source;
    size_t remaining = *length;
    size_t written = tilefinch_decoder_decode(
        &decoder, &input, &remaining, (unsigned char *) decoded, capacity,
        true);
    decoded[written] = '\0';
    *length = written;
    return decoded;
}

static bool script_runtime_evaluate_external_typed_at(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *module_referrer_policy,
    TilefinchCredentialsMode module_credentials, bool module,
    bool dispatch_completion, const char *classic_cache_url,
    uint32_t classic_ordinal, bool response_no_store, ScriptResult *result)
{
    /* A NULL source is an installed app's classic script restored as
       bytecode only (browser_session_offline_script_*). */
    if (runtime == NULL || script_node == NULL
        || (source == NULL
            && (module || classic_cache_url == NULL
                || runtime->session == NULL))) return false;
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
    if (module && tilefinch_trace_startup_failure()) {
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
    /* Its source stays in the app pack and is read only when something
       needs the bytes: the page-trace wrapper or a failed restore. */
    BrowserSharedBody *source_lease = NULL;
    BrowserSharedBody *offline_bytecode = NULL;
    if (evaluated && source == NULL) {
        if (!page_diagnostic && dispatch_completion)
            offline_bytecode = browser_session_offline_script_bytecode(
                runtime->session, classic_cache_url, source_length);
        if (offline_bytecode == NULL) {
            source_lease = browser_session_offline_script_source(
                runtime->session, classic_cache_url, source_length);
            if (source_lease == NULL) evaluated = false;
            else source = (const char *) source_lease->data;
        }
        evaluated_source = source;
    }
    static const char trace_prefix[] = "with(__tilefinchGlobalProxy){";
    static const char trace_suffix[] = "\n}";
    /* A Module is always parsed in strict mode, where a WithStatement is a
       syntax error.  The capability tracer's `with` wrapper is therefore
       valid only for classic scripts.  Module globals are still observed by
       the proxied host objects installed by page_capability_trace_setup();
       compiling the author's module source unchanged preserves module
       grammar and strict-mode semantics. */
    char *decoded = NULL;
    if (evaluated && !module && evaluated_source != NULL) {
        decoded = js_rt_decode_classic_script_source(
            runtime, script_node, evaluated_source, &evaluated_length);
        if (decoded != NULL) evaluated_source = decoded;
    }
    if (evaluated && page_diagnostic && !module) {
        size_t body_length = evaluated_length;
        evaluated_length = sizeof(trace_prefix) - 1 + body_length
                           + sizeof(trace_suffix) - 1;
        instrumented = budget_malloc(runtime->budget, evaluated_length + 1);
        if (instrumented == NULL) evaluated = false;
        else {
            memcpy(instrumented, trace_prefix, sizeof(trace_prefix) - 1);
            memcpy(instrumented + sizeof(trace_prefix) - 1, evaluated_source,
                   body_length);
            memcpy(instrumented + sizeof(trace_prefix) - 1 + body_length,
                   trace_suffix, sizeof(trace_suffix));
            instrumented[evaluated_length] = '\0';
            evaluated_source = instrumented;
        }
    }
    if (evaluated) {
        const char *name =
            request_url == NULL ? "<external-script>" : request_url;
        bool classic_cacheable = !module && !page_diagnostic
            && classic_cache_url != NULL && runtime->session != NULL;
#ifndef TILEFINCH_NO_TRACE
        if (!module && !classic_cacheable && tilefinch_trace_census()) {
            CENSUS_TRACE_CLASSIC_BYTECODE(
                "lookup",
                page_diagnostic ? "ineligible-diagnostic"
                    : classic_cache_url == NULL ? "ineligible-no-cache-url"
                    : "ineligible-no-session",
                evaluated_length, 0, classic_ordinal, name);
        }
#endif
        if (classic_cacheable) {
            /* An installed app's bytecode comes with the app: restored with
               it (offline_bytecode) or attached to its response entry. */
            BrowserSharedBody *cached = offline_bytecode;
            if (cached == NULL && classic_ordinal == 0 && dispatch_completion
                && evaluated_source != NULL) {
                cached = browser_session_classic_script_bytecode_acquire(
                    runtime->session, classic_cache_url,
                    (const unsigned char *) evaluated_source,
                    evaluated_length);
            }
            char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
            BrowserScriptBytecodeKey key;
            bool keyed = cached == NULL && classic_bytecode_key_init(
                runtime, &key, partition, evaluated_source, evaluated_length,
                name, classic_cache_url, classic_ordinal);
#ifndef TILEFINCH_NO_TRACE
            /* Printed once the lookup is over: a persistent-tier read
               happens inside it. */
            const char *census_lookup = NULL;
            if (tilefinch_trace_census()) {
                JSValue queued = keyed
                    ? pending_bytecode_lookup(runtime, &key) : JS_UNDEFINED;
                census_lookup = cached != NULL ? "hit-installed"
                    : !keyed ? "ineligible-key"
                    : !JS_IsUndefined(queued) ? "hit-pending"
                    : browser_session_script_bytecode_diagnose(
                          runtime->session, BROWSER_SCRIPT_BYTECODE_CLASSIC,
                          &key);
                JS_FreeValue(runtime->context, queued);
            }
#endif
            JSValue compiled = JS_UNDEFINED;
            bool restored = false;
            bool cache_admitted = true;
            if (cached != NULL) {
                cache_admitted = js_rt_admit_cached_compile_source(
                        runtime->context, evaluated_length, name,
                        SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result);
                if (cache_admitted) {
                    uint64_t restore_started_ns = js_rt_monotonic_time_ns();
                    compiled = JS_ReadObject(
                        runtime->context, cached->data, cached->length,
                        JS_READ_OBJ_BYTECODE);
                    runtime->result.external_script_bytecode_restore_us +=
                        (js_rt_monotonic_time_ns() - restore_started_ns)
                        / 1000u;
                    restored = !JS_IsException(compiled);
                }
                if (restored) {
                    runtime->result.external_script_bytecode_cache_hits++;
                    js_rt_saturating_add_size(
                        &runtime->result.external_script_bytecode_cache_bytes,
                        cached->length);
                    /* An installed app's bytecode, restored from RAM. */
                    js_rt_heavy_note_restored(runtime, evaluated_length,
                                              false);
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
                    if (evaluated_source == NULL) {
                        source_lease = browser_session_offline_script_source(
                            runtime->session, classic_cache_url,
                            evaluated_length);
                        if (source_lease != NULL)
                            source = evaluated_source =
                                (const char *) source_lease->data;
                    }
                    if (evaluated_source != NULL)
                        browser_session_classic_script_bytecode_invalidate(
                            runtime->session, classic_cache_url,
                            (const unsigned char *) evaluated_source,
                            evaluated_length);
                }
                browser_shared_body_release(cached);
            } else if (keyed) {
                bool disk_hit = false;
                compiled = classic_bytecode_restore(
                    runtime, &key, name, &cache_admitted, &disk_hit);
                restored = !JS_IsUndefined(compiled);
#ifndef TILEFINCH_NO_TRACE
                if (disk_hit) census_lookup = "hit-disk";
#else
                (void) disk_hit;
#endif
            }
#ifndef TILEFINCH_NO_TRACE
            if (census_lookup != NULL)
                CENSUS_TRACE_CLASSIC_BYTECODE("lookup", census_lookup,
                                              evaluated_length, 0,
                                              classic_ordinal,
                                              classic_cache_url);
#endif
            if (!cache_admitted || (!restored && evaluated_source == NULL)) {
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
                } else if (keyed && !JS_IsException(compiled)) {
                    classic_bytecode_store(runtime, &key, compiled,
                                           response_no_store);
                }
            }
            if (evaluated) {
                evaluated = js_rt_evaluate_compiled_source(
                    runtime->context, compiled, name, evaluated_length,
                    &runtime->result);
            }
        } else if (module) {
            bool previous_no_store = runtime->module_root_no_store;
            runtime->module_root_no_store = response_no_store;
            evaluated = js_rt_evaluate_external_module_at(
                runtime->context, evaluated_source, evaluated_length, name,
                response_url != NULL ? response_url : request_url,
                module_referrer_policy, module_credentials,
                &runtime->result);
            runtime->module_root_no_store = previous_no_store;
        } else {
            evaluated = js_rt_evaluate_source_type_at(
                runtime->context, evaluated_source, evaluated_length, name,
                request_url, NULL, TILEFINCH_CREDENTIALS_SAME_ORIGIN,
                JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_EXTERNAL,
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
    budget_free(runtime->budget, decoded);
    if (!evaluated) {
        js_rt_capture_error_source_context(source, source_length, request_url,
                                     &runtime->result);
        browser_shared_body_release(source_lease);
        source_lease = NULL;
        /* Compilation/evaluation failure is an author-script error, not a
           failed document load.  In a bounded realm the failed evaluation
           may leave unreachable parser objects and wrapper cycles at the
           heap ceiling; collect them before allocating the script's error
           event or the later DOMContentLoaded task. */
        (void) script_runtime_collect_and_trim(runtime);
        runtime->result.external_scripts_failed++;
        runtime->result.success = true;
        size_t slot = 0;
        if (!script_runtime_document_refresh_failed(runtime)
            && js_rt_bridge_node_slot_for_handle(
                &runtime->bridge, script_handle, &slot)) {
            (void) script_runtime_dispatch_node(
                runtime, runtime->bridge.nodes[slot], "error", NULL);
        }
        js_rt_runtime_update_result(runtime, result);
        runtime->promise_rejection_state.active_source =
            previous_rejection_source;
        return false;
    }
    browser_shared_body_release(source_lease);
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
        module, true, module ? NULL : source_url, 0, false, result);
}

bool js_rt_evaluate_external_classic_dynamic(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool response_no_store)
{
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, source_url, source_url,
        runtime == NULL ? NULL : runtime->bridge.referrer_policy,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, false, true, source_url, 0,
        response_no_store, NULL);
}

bool script_runtime_evaluate_external_classic_cached(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, bool response_no_store, ScriptResult *result)
{
    const char *name = response_url == NULL ? request_url : response_url;
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, name, name,
        runtime == NULL ? NULL : runtime->bridge.referrer_policy,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, false, true, request_url, 0,
        response_no_store, result);
}

/* A segment's bytecode is keyed by its own bytes and its ordinal. */
bool js_rt_evaluate_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    size_t segment_index, bool response_no_store, bool final_segment)
{
    return script_runtime_evaluate_external_typed_at(
        runtime, script_node, source, source_length, source_url, source_url,
        runtime == NULL ? NULL : runtime->bridge.referrer_policy,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, false, final_segment,
        segment_index >= UINT32_MAX - 1u ? NULL : source_url,
        (uint32_t) segment_index + 1u, response_no_store, NULL);
}

bool js_rt_preflight_external_classic_segment(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    size_t segment_index, bool response_no_store)
{
    if (runtime == NULL || script_node == NULL || source == NULL) return false;
    const char *name =
        source_url == NULL ? "<external-script-preflight>" : source_url;
    const char *previous_source =
        runtime->promise_rejection_state.active_source;
    runtime->promise_rejection_state.active_source = name;
    js_rt_runtime_arm_watchdog(runtime);
    /* A compile (queued for storing, or stored bytecode) of exactly these
       bytes proves they compile; otherwise the compile below is queued, so
       the segment's evaluation runs it instead of compiling the same bytes
       a second time. */
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    BrowserScriptBytecodeKey key;
    bool keyed = source_url != NULL && segment_index < UINT32_MAX - 1u
        && classic_bytecode_key_init(
               runtime, &key, partition, source, source_length, name,
               source_url, (uint32_t) segment_index + 1u);
    if (keyed) {
        JSValue queued = pending_bytecode_lookup(runtime, &key);
        bool found = !JS_IsUndefined(queued);
        JS_FreeValue(runtime->context, queued);
        bool from_disk = false;
        BrowserSharedBody *cached = found ? NULL
            : script_bytecode_lookup(runtime, BROWSER_SCRIPT_BYTECODE_CLASSIC,
                                     &key, &runtime->result, &from_disk);

        bool hit = (found || cached != NULL)
            && js_rt_admit_cached_compile_source(
                   runtime->context, source_length, name,
                   SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result);
        browser_shared_body_release(cached);
        if (hit) {
            CENSUS_TRACE_CLASSIC_BYTECODE("preflight",
                                          from_disk ? "hit-disk" : "hit",
                                          source_length, 0, key.ordinal,
                                          source_url);
            runtime->promise_rejection_state.active_source = previous_source;
            return true;
        }
    }
    bool admitted = false;
    JSValue compiled = js_rt_compile_source_type(
        runtime->context, source, source_length, name, JS_EVAL_TYPE_GLOBAL,
        SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result, &admitted);
    bool ok = admitted && !JS_IsException(compiled);
    if (ok) {
        if (keyed)
            classic_bytecode_store(runtime, &key, compiled,
                                   response_no_store);
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
    if (tilefinch_trace_script_failures()) {
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

bool script_runtime_evaluate_external_module_context(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials, ScriptResult *result)
{
    return script_runtime_evaluate_external_module_response(
        runtime, script_node, source, source_length, request_url,
        response_url, effective_referrer_policy, credentials, false, result);
}

bool script_runtime_evaluate_external_module_response(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *request_url,
    const char *response_url, const char *effective_referrer_policy,
    TilefinchCredentialsMode credentials, bool response_no_store,
    ScriptResult *result)
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
        effective_referrer_policy, credentials, true, true, NULL, 0,
        response_no_store, result);
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
