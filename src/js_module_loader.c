/* ES-module pipeline: MIME admission, module graph registration, specifier
   normalization, the QuickJS module loader, and external classic/module
   evaluation entry points.  Split from js_runtime.c; shares the runtime
   internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"
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
 * Module bytecode cache (BrowserModuleBytecodeCache in session.h).
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
    ScriptRuntime *runtime, BrowserModuleBytecodeKey *key,
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
    *key = (BrowserModuleBytecodeKey) {
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

/* Serialize a freshly compiled module into the cache when there is room.
   Serialization is an optional accelerator beside a compiled module: it is
   skipped rather than allowed to consume the realm's remaining headroom, and
   a refusal never affects the module. */
static void module_bytecode_store(ScriptRuntime *runtime, JSContext *context,
                                  BrowserModuleBytecodeKey *key,
                                  JSValueConst compiled,
                                  ScriptResult *result)
{
    const size_t heap_floor = 64u * 1024u;
    size_t source_length = key->source_length;
    size_t heap_reserve = source_length > (SIZE_MAX - heap_floor) / 4u
        ? SIZE_MAX : heap_floor + source_length * 4u;
    /* The copy lands in the page Budget; keep the same reserve the script
       loader keeps for presentation work after scripts. */
    const size_t budget_reserve = 3u * 1024u * 1024u;
    size_t budget_left = budget_remaining(runtime->session->budget);
    size_t minimum_bytecode = source_length / 4u + 1u;
    /* Either tier may want the bytes: RAM when it can admit them, the
       optional persistent tier when it has no file for this key yet. */
    bool ram_wanted = browser_session_module_bytecode_may_fit(
        runtime->session, key, minimum_bytecode,
        runtime->module_bytecode_generation);
    bool disk_wanted = browser_session_module_bytecode_disk_wants(
        runtime->session, key, minimum_bytecode);
    if (script_runtime_heap_available(runtime) < heap_reserve
        || budget_left < budget_reserve
        || minimum_bytecode > budget_left - budget_reserve
        || (!ram_wanted && !disk_wanted)) {
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
    if (disk_wanted && length != 0
        && browser_session_module_bytecode_disk_store(
               runtime->session, key, bytecode, length))
        js_rt_saturating_add_size(&result->module_bytecode_disk_stores, 1);
    budget_left = budget_remaining(runtime->session->budget);
    bool stored = ram_wanted && length != 0 && budget_left >= budget_reserve
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
                                    BrowserModuleBytecodeKey *key,
                                    ScriptResult *result, bool *admitted,
                                    JSValue *module)
{
    BrowserSharedBody *cached = browser_session_module_bytecode_acquire(
        runtime->session, key, runtime->module_bytecode_generation);
    bool from_disk = false;
    if (cached == NULL
        && browser_session_module_bytecode_disk_enabled(runtime->session)) {
        /* The optional persistent tier: a verified file for this exact
           key, admitted exactly as a RAM hit below. Its synchronous read
           and verification are timed whether or not they find one. */
        BrowserSession *session = runtime->session;
        uint64_t load_started_ns = js_rt_monotonic_time_ns(),
            read_before = session->module_bytecode_disk_read_ns,
            verify_before = session->module_bytecode_disk_verify_ns;
        cached = browser_session_module_bytecode_disk_load(session, key);
        result->module_bytecode_disk_load_us +=
            (js_rt_monotonic_time_ns() - load_started_ns) / 1000u;
        result->module_bytecode_disk_read_us +=
            (session->module_bytecode_disk_read_ns - read_before) / 1000u;
        result->module_bytecode_disk_verify_us +=
            (session->module_bytecode_disk_verify_ns - verify_before) / 1000u;
        from_disk = cached != NULL;
    }
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
    if (from_disk && restored_module) {
        js_rt_saturating_add_size(&result->module_bytecode_disk_hits, 1);
        /* Keep it in RAM too when there is room, for the next load. */
        (void) browser_session_module_bytecode_put(
            runtime->session, key, runtime->module_bytecode_generation,
            cached->data, cached->length);
        result->module_bytecode_promote_us +=
            (js_rt_monotonic_time_ns() - read_ns) / 1000u;
    }
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
        if (from_disk)
            browser_session_module_bytecode_disk_discard(runtime->session,
                                                         key);
        else
            browser_session_module_bytecode_invalidate(runtime->session, key);
        return false;
    }
    js_rt_saturating_add_size(&result->module_bytecode_cache_hits, 1);
    js_rt_saturating_add_size(&result->module_bytecode_cache_bytes,
                              restored_bytes);
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
    ScriptResult *result, bool *admitted)
{
    if (admitted != NULL) *admitted = false;
    if (runtime == NULL || context == NULL || source == NULL
        || module_name == NULL || result == NULL || admitted == NULL) {
        return JS_EXCEPTION;
    }
    int strip_flags = script_module_strip_flags(source_length);
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    BrowserModuleBytecodeKey key;
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
        module_bytecode_store(runtime, context, &key, compiled, result);
        result->module_store_us +=
            (js_rt_monotonic_time_ns() - parse_finished_ns) / 1000u;
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
    JSValue compiled = js_rt_module_compile_external(
        runtime, context, loaded.source, loaded.source_length, module_name,
        loaded.response_url, &runtime->result, &admitted);
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
                    /* Serialization is an optional accelerator beside a
                       freshly compiled program. Do not consume the remaining
                       execution headroom to grow its temporary output buffer.
                       This is a conservative pressure heuristic, not a size
                       guarantee; ordinary serialization refusal still unwinds
                       below without discarding the compiled script. */
                    const size_t cache_floor = 64u * 1024u;
                    size_t cache_reserve = evaluated_length > (SIZE_MAX - cache_floor) / 4u
                        ? SIZE_MAX : cache_floor + evaluated_length * 4u;
                    bool may_fit =
                        script_runtime_heap_available(runtime) >= cache_reserve
                        && browser_session_classic_script_bytecode_may_fit(
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
        } else if (module) {
            evaluated = js_rt_evaluate_external_module_at(
                runtime->context, evaluated_source, evaluated_length, name,
                response_url != NULL ? response_url : request_url,
                module_referrer_policy, module_credentials,
                &runtime->result);
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
