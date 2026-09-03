#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/sha256.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/node.h>

#include "tilefinch/platform.h"
#include "../src/js_runtime_internal.h"

#define MIB (1024u * 1024u)

typedef struct {
    size_t calls;
} CallbackAbortCooperate;

static bool callback_abort_cooperate(void *context, const char *phase,
                                     size_t completed_work_units)
{
    (void) phase;
    (void) completed_work_units;
    CallbackAbortCooperate *state = context;
    state->calls++;
    return false;
}

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
/* Deterministic compile-abort driver: the cooperate service refuses to
   continue, so the parser's bounded interrupt poll aborts an admitted
   compile without depending on wall-clock timing. */
typedef struct {
    size_t calls;
} CompileAbortCooperate;

static bool compile_abort_cooperate(void *context, const char *phase,
                                    size_t completed_work_units)
{
    (void) phase;
    (void) completed_work_units;
    CompileAbortCooperate *state = context;
    state->calls++;
    return false;
}
#endif

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static int test_reduced_dom_event_counter(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    CHECK(runtime != NULL);
    JSContext *context = JS_NewContext(runtime);
    CHECK(context != NULL);

    JS_SetContextOpaque(context, NULL);
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);

    DomBridge bridge = {0};
    JS_SetContextOpaque(context, &bridge);
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);

    ScriptResult result = {0};
    bridge.result = &result;
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);
    CHECK(result.events_dispatched == 1);

    JS_SetContextOpaque(context, NULL);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return 0;
}

static int test_blank_recovery_author_work_census(void)
{
    JSRuntime *quickjs = JS_NewRuntime();
    CHECK(quickjs != NULL);
    ScriptRuntime runtime = {0};
    runtime.runtime = quickjs;

    /* These remain visible in ScriptResult.pending_tasks for diagnostics,
       but long-lived transports must not keep a blank page in recovery's
       deferred state forever. */
    runtime.result.pending_tasks = 4u;
    runtime.bridge.event_source_count = 1u;
    runtime.bridge.websocket_count = 1u;
    runtime.bridge.multiplayer.active = true;
    CHECK(!script_runtime_has_pending_author_work(&runtime));

    runtime.pending_timer_tasks = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.pending_timer_tasks = 0u;
    runtime.pending_network_tasks = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.pending_network_tasks = 0u;
    runtime.bridge.async_fetch_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.bridge.async_fetch_count = 0u;
    runtime.bridge.dynamic_script_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.bridge.dynamic_script_count = 0u;
    runtime.page_visibility_queue_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));

    JS_FreeRuntime(quickjs);
    return 0;
}

static lxb_dom_node_t *find_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 6 && memcmp(name, "script", 6) == 0)
            return node;
        lxb_dom_node_t *nested = find_script(node->first_child);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static lxb_dom_node_t *find_element_id(lxb_dom_node_t *node,
                                       const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *nested = find_element_id(node->first_child, wanted);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static bool digest_matches_hex(
    const uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    if (hex == NULL
        || strlen(hex) != TILEFINCH_SHA256_DIGEST_BYTES * 2u) return false;
    for (size_t index = 0; index < TILEFINCH_SHA256_DIGEST_BYTES; index++) {
        if (digits[digest[index] >> 4] != hex[index * 2u]
            || digits[digest[index] & 15u] != hex[index * 2u + 1u]) {
            return false;
        }
    }
    return true;
}

static bool collect_and_drain_finalizers(ScriptRuntime *runtime,
                                         ScriptResult *result)
{
    (void) script_runtime_collect_and_trim(runtime);
    /* QuickJS schedules FinalizationRegistry cleanup as promise jobs.  The
       runtime intentionally bounds each checkpoint at 64 jobs, so drain a
       few checkpoints rather than weakening that production boundary.
       Repeat collection after cleanup jobs: newly unreachable WeakRef
       targets are not guaranteed to retire in the first GC cycle. */
    for (size_t checkpoint = 0; checkpoint < 16; checkpoint++) {
        if (checkpoint != 0 && checkpoint % 4 == 0)
            (void) script_runtime_collect_and_trim(runtime);
        if (!script_runtime_advance(runtime, 0, 1024, result)) return false;
    }
    return true;
}

typedef struct {
    TilefinchCredentialsMode first_credentials;
    TilefinchCredentialsMode second_credentials;
    char first_referrer_url[256];
    char second_referrer_url[256];
    char first_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    char second_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    size_t calls;
    bool unexpected_url;
} ModuleCredentialProbe;

static char *probe_copy(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1u);
    if (copy != NULL) memcpy(copy, text, length + 1u);
    return copy;
}

static bool module_credential_probe_load(
    void *opaque, const ScriptModuleLoadRequest *request,
    ScriptModuleLoadResult *result)
{
    static const char first_request[] =
        "https://response-a.test/modules/child.js";
    static const char second_request[] =
        "https://response-b.test/modules/child.js";
    static const char first_response[] =
        "https://cdn-a.test/final/child.js";
    static const char second_response[] =
        "https://cdn-b.test/final/child.js";
    static const char source[] = "export const value=import.meta.url;";
    ModuleCredentialProbe *probe = opaque;
    if (probe == NULL || request == NULL || request->request_url == NULL
        || request->referrer_url == NULL
        || request->referrer_policy == NULL || result == NULL) return false;
    const char *response_url = NULL;
    if (strcmp(request->request_url, first_request) == 0) {
        probe->first_credentials = request->credentials;
        snprintf(probe->first_referrer_url,
                 sizeof(probe->first_referrer_url), "%s",
                 request->referrer_url);
        snprintf(probe->first_referrer_policy,
                 sizeof(probe->first_referrer_policy), "%s",
                 request->referrer_policy);
        response_url = first_response;
    } else if (strcmp(request->request_url, second_request) == 0) {
        probe->second_credentials = request->credentials;
        snprintf(probe->second_referrer_url,
                 sizeof(probe->second_referrer_url), "%s",
                 request->referrer_url);
        snprintf(probe->second_referrer_policy,
                 sizeof(probe->second_referrer_policy), "%s",
                 request->referrer_policy);
        response_url = second_response;
    } else {
        probe->unexpected_url = true;
        return false;
    }
    probe->calls++;
    result->source = probe_copy(source);
    result->source_length = sizeof(source) - 1u;
    result->response_url = probe_copy(response_url);
    if (result->source == NULL || result->response_url == NULL) {
        free(result->source);
        free(result->response_url);
        memset(result, 0, sizeof(*result));
        return false;
    }
    return true;
}

static void module_credential_probe_release(
    void *opaque, ScriptModuleLoadResult *result)
{
    (void) opaque;
    if (result == NULL) return;
    free(result->source);
    free(result->response_url);
    memset(result, 0, sizeof(*result));
}

static bool test_delayed_module_request_contexts(
    ScriptRuntime *runtime, lxb_dom_node_t *script, ScriptResult *result)
{
    static const char first_root[] =
        "globalThis.__loadCredentialA=()=>import('./child.js');";
    static const char second_root[] =
        "globalThis.__loadCredentialB=()=>import('./child.js');";
    static const char delayed_imports[] =
        "globalThis.pocSummary='MODULE-CREDENTIALS-PENDING';"
        "Promise.all([__loadCredentialA(),__loadCredentialB()])"
        ".then(values=>{globalThis.pocSummary='MODULE-CREDENTIALS:' + "
        "values[0].value+'|'+values[1].value})"
        ".catch(error=>{globalThis.pocSummary='MODULE-CREDENTIALS-ERROR:'"
        "+error});";
    static const char expected[] =
        "MODULE-CREDENTIALS:https://cdn-a.test/final/child.js|"
        "https://cdn-b.test/final/child.js";
    ModuleCredentialProbe probe = {0};
    script_runtime_set_module_loader(
        runtime, module_credential_probe_load,
        module_credential_probe_release, &probe);
    bool ok = script_runtime_evaluate_external_module_context(
            runtime, script, first_root, sizeof(first_root) - 1u,
            "https://request-a.test/root.js",
            "https://response-a.test/modules/root.js",
            "origin", TILEFINCH_CREDENTIALS_SAME_ORIGIN, result)
        && script_runtime_evaluate_external_module_context(
            runtime, script, second_root, sizeof(second_root) - 1u,
            "https://request-b.test/root.js",
            "https://response-b.test/modules/root.js",
            "no-referrer", TILEFINCH_CREDENTIALS_INCLUDE, result);
    /* Loader availability is mutable; the graph metadata is not. */
    script_runtime_set_module_loader(runtime, NULL, NULL, NULL);
    script_runtime_set_module_loader(
        runtime, module_credential_probe_load,
        module_credential_probe_release, &probe);
    ok = ok && script_runtime_evaluate_diagnostic(
        runtime, delayed_imports, "<delayed-module-imports>", result);
    for (size_t checkpoint = 0;
         ok && checkpoint < 8 && strcmp(result->summary, expected) != 0;
         checkpoint++) {
        ok = script_runtime_advance(runtime, 0, 32, result);
    }
    ok = ok && strcmp(result->summary, expected) == 0
        && probe.calls == 2 && !probe.unexpected_url
        && probe.first_credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
        && probe.second_credentials == TILEFINCH_CREDENTIALS_INCLUDE
        && strcmp(probe.first_referrer_url,
                  "https://response-a.test/modules/root.js") == 0
        && strcmp(probe.second_referrer_url,
                  "https://response-b.test/modules/root.js") == 0
        && strcmp(probe.first_referrer_policy, "origin") == 0
        && strcmp(probe.second_referrer_policy, "no-referrer") == 0;
    script_runtime_set_module_loader(runtime, NULL, NULL, NULL);
    return ok;
}

static int test_native_dynamic_code_policy(void)
{
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body></body></html>";
    PocDocument document;
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1u, sizeof(html)));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .dynamic_code_disabled = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char blocked[] =
        "(()=>{let blocked=typeof __tilefinchRunWorker==='undefined'?1:0;"
        "const indirect=eval,AsyncFunction="
        "(async function(){}).constructor,GeneratorFunction="
        "(function*(){}).constructor;for(const compile of["
        "()=>eval('1'),()=>indirect('1'),()=>Function('return 1'),"
        "()=>AsyncFunction('return 1'),()=>GeneratorFunction('yield 1')])"
        "{try{compile()}catch(error){if(error instanceof TypeError)blocked++}}"
        "globalThis.pocSummary='DYNAMIC-CODE-BLOCKED:'+blocked})()";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, blocked, "<dynamic-code-blocked>", &result)
          && strcmp(result.summary, "DYNAMIC-CODE-BLOCKED:6") == 0);
    static const char inline_allowed[] =
        "(()=>{const node=document.createElement('button');"
        "node.setAttribute('onclick',\"this.setAttribute('data-fired',"
        "event.type);return false\");document.body.appendChild(node);"
        "const accepted=node.dispatchEvent(new MouseEvent('click',"
        "{cancelable:true}));globalThis.pocSummary=!accepted&&"
        "node.getAttribute('data-fired')==='click'?"
        "'INLINE-HANDLER-ALLOWED':'INLINE-HANDLER-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, inline_allowed, "<inline-handler-allowed>", &result)
          && strcmp(result.summary, "INLINE-HANDLER-ALLOWED") == 0);
    script_runtime_destroy(runtime);

    options.dynamic_code_disabled = false;
    runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char allowed[] =
        "globalThis.pocSummary=eval('20+1')+Function('return 21')()"
        "+(0,eval)('21')===63?'DYNAMIC-CODE-ALLOWED':'FAILED'";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, allowed, "<dynamic-code-allowed>", &result)
          && strcmp(result.summary, "DYNAMIC-CODE-ALLOWED") == 0);
    script_runtime_destroy(runtime);

    static const char csp_blocked[] =
        "content-security-policy: script-src 'none'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://policy.test/",
        csp_blocked, sizeof(csp_blocked) - 1u, false));
    options.dynamic_code_disabled = true;
    runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char inline_blocked[] =
        "(()=>{const node=document.createElement('button');"
        "node.setAttribute('onclick',\"this.setAttribute('data-fired',"
        "'yes')\");document.body.appendChild(node);"
        "const accepted=node.dispatchEvent(new MouseEvent('click',"
        "{cancelable:true}));globalThis.pocSummary=accepted&&"
        "node.getAttribute('data-fired')===null?"
        "'INLINE-HANDLER-BLOCKED':'INLINE-HANDLER-ESCAPED'})()";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, inline_blocked, "<inline-handler-blocked>", &result)
          && strcmp(result.summary, "INLINE-HANDLER-BLOCKED") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0);
#endif
    return 0;
}

int main(void)
{
    CHECK(test_reduced_dom_event_counter() == 0);
    CHECK(test_blank_recovery_author_work_census() == 0);
    CHECK(test_native_dynamic_code_policy() == 0);
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    CHECK(tilefinch_sha256_digest(NULL, 0, digest)
          && digest_matches_hex(
                 digest,
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855"));
    static const uint8_t abc[] = "abc";
    CHECK(tilefinch_sha256_digest(abc, sizeof(abc) - 1u, digest)
          && digest_matches_hex(
                 digest,
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad"));
    static const uint8_t two_block_vector[] =
        "abcdbcdecdefdefgefghfghighijhijk"
        "ijkljklmklmnlmnomnopnopq";
    CHECK(tilefinch_sha256_digest(
              two_block_vector, sizeof(two_block_vector) - 1u, digest)
          && digest_matches_hex(
                 digest,
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1")
          && !tilefinch_sha256_digest(NULL, 1, digest)
          && !tilefinch_sha256_digest(abc, sizeof(abc) - 1u, NULL));

    CHECK(script_module_mime_type_allowed("text/javascript")
          && script_module_mime_type_allowed(
                 "  Text/JavaScript1.5 ; charset=UTF-8")
          && script_module_mime_type_allowed("application/x-ecmascript")
          && script_module_mime_type_allowed("text/livescript;version=1")
          && !script_module_mime_type_allowed(NULL)
          && !script_module_mime_type_allowed("")
          && !script_module_mime_type_allowed("text/plain")
          && !script_module_mime_type_allowed("application/json")
          && !script_module_mime_type_allowed("text/javascript-module")
          && script_module_revalidated_mime_allowed(
                 "text/javascript", "")
          && script_module_revalidated_mime_allowed(
                 "text/javascript", "application/javascript")
          && !script_module_revalidated_mime_allowed(
                 "text/javascript", "text/plain")
          && !script_module_revalidated_mime_allowed(
                 "text/plain", "text/javascript"));
    bool attribute_module = false;
    static const char spaced_classic[] = " \tText/JavaScript1.5 \r";
    static const char spaced_module[] = "\n MoDuLe \t";
    CHECK(script_type_attribute_classify(
              spaced_classic, sizeof(spaced_classic) - 1u,
              &attribute_module)
          && !attribute_module
          && script_type_attribute_classify(
              spaced_module, sizeof(spaced_module) - 1u,
              &attribute_module)
          && attribute_module
          && !script_type_attribute_classify(
              " application/json ", 18u, &attribute_module));

    ScriptExecutionPolicy lab, strict, realistic, invalid;
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &lab)
          && lab.maximum_host_compile_source_bytes == 0
          && lab.slow_compile_threshold_us == 16000
          && lab.slow_callback_threshold_us == 16000
          && lab.maximum_host_compile_projected_us == 0
          && lab.modeled_compile_bytes_per_ms == 0);
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_PSP_STRICT, &strict)
          && strict.maximum_host_compile_source_bytes == 269u * 1024u
          && strict.maximum_host_compile_projected_us == 0
          && strict.modeled_compile_bytes_per_ms == 0);
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &realistic)
          && realistic.maximum_host_compile_source_bytes
               == 384u * 1024u
          && realistic.maximum_host_compile_source_bytes
               > strict.maximum_host_compile_source_bytes);
    invalid.maximum_host_compile_source_bytes = 7;
    CHECK(!script_execution_policy_for_profile(
               (ScriptExecutionProfile) 99, &invalid)
          && invalid.maximum_host_compile_source_bytes == 0);

    BrowserDeviceProfile device;
    BrowserConfig config;
    browser_device_profile_psp3000(&device);
    browser_config_init(&config, &device);
    CHECK(config.javascript.execution_policy.maximum_host_compile_source_bytes
              == realistic.maximum_host_compile_source_bytes
          && config.javascript.maximum_file_bytes
               <= config.javascript.execution_policy
                      .maximum_host_compile_source_bytes);

    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body><svg id=parsed-svg>"
        "<g id=parsed-group></g></svg><script id=target>"
        "globalThis.__streamProbe = new ReadableStream();"
        "</script></body></html>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17));
    size_t document_bytes = budget.current;

    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));

    /* An unavailable bytecode object and an explicitly disabled bytecode path
       both compile the same bootstrap sources exactly once. The restore fault
       occurs after deserialization, where admission used to be double-counted
       before source fallback. */
    ScriptResult unavailable_bytecode_result;
    ScriptResult disabled_bytecode_result;
    bool unavailable_fault_set = setenv(
        "TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE", "1", 1) == 0;
    bool unavailable_bytecode_ran = unavailable_fault_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 8u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &unavailable_bytecode_result);
    bool unavailable_fault_unset =
        unsetenv("TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE") == 0;
    bool disabled_bytecode_set =
        setenv("TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE", "1", 1) == 0;
    bool disabled_bytecode_ran = disabled_bytecode_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 8u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &disabled_bytecode_result);
    bool disabled_bytecode_unset =
        unsetenv("TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE") == 0;
    if (!unavailable_bytecode_ran || !disabled_bytecode_ran) {
        fprintf(stderr,
                "bootstrap fallback probe: unavailable=%d error=\"%s\" "
                "disabled=%d error=\"%s\"\n",
                unavailable_bytecode_ran,
                unavailable_bytecode_result.error,
                disabled_bytecode_ran, disabled_bytecode_result.error);
    }
    CHECK(unavailable_fault_set && unavailable_fault_unset
          && disabled_bytecode_set && disabled_bytecode_unset
          && unavailable_bytecode_ran && disabled_bytecode_ran
          && unavailable_bytecode_result.bootstrap_bytecode_restore_failures
               != 0
          && unavailable_bytecode_result.host_compile_attempts
               == disabled_bytecode_result.host_compile_attempts
          && unavailable_bytecode_result.host_compile_source_bytes
               == disabled_bytecode_result.host_compile_source_bytes
          && unavailable_bytecode_result.bootstrap_lazy_module_loads == 1
          && disabled_bytecode_result.bootstrap_lazy_module_loads == 1
          && unavailable_bytecode_result.bootstrap_lazy_module_failures == 0
          && disabled_bytecode_result.bootstrap_lazy_module_failures == 0
          && budget.current == document_bytes);

    /* Formatting made the core DOM source larger than its serialized
       bytecode. Tight heaps must follow the current representation sizes,
       not the historical fixed-size heuristic. */
    ScriptResult tight_heap_result;
    bool tight_window_set =
        setenv("TILEFINCH_JS_BOOT_WINDOW_KB", "1024", 1) == 0;
    bool tight_heap_ran = tight_window_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 6u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &tight_heap_result);
    bool tight_window_unset =
        unsetenv("TILEFINCH_JS_BOOT_WINDOW_KB") == 0;
    bool tight_heap_ok = tight_window_set && tight_heap_ran
        && tight_window_unset
        && tight_heap_result.bootstrap_bytecode_source_preferences == 0
        && tight_heap_result.bootstrap_bytecode_preferred_source_bytes == 0
        && tight_heap_result.bootstrap_bytecode_restores != 0
        && tight_heap_result.bootstrap_bytecode_stored_bytes != 0
        && tight_heap_result.bootstrap_bytecode_stored_bytes
             == tight_heap_result.bootstrap_bytecode_bytes
        && budget.current == document_bytes;
    if (!tight_heap_ok) {
        fprintf(stderr,
                "tight bootstrap run failed: set=%d ran=%d unset=%d "
                "error=\"%s\" preferences=%zu "
                "source-bytes=%zu restores=%zu failures=%zu "
                "stored=%zu bytecode=%zu current=%zu\n",
                tight_window_set, tight_heap_ran, tight_window_unset,
                tight_heap_result.error,
                tight_heap_result.bootstrap_bytecode_source_preferences,
                tight_heap_result.bootstrap_bytecode_preferred_source_bytes,
                tight_heap_result.bootstrap_bytecode_restores,
                tight_heap_result.bootstrap_bytecode_restore_failures,
                tight_heap_result.bootstrap_bytecode_stored_bytes,
                tight_heap_result.bootstrap_bytecode_bytes,
                budget.current);
    }
    CHECK(tight_heap_ok);

    /* A limit below the first built-in source must reject it before QuickJS
       parsing begins and fully roll runtime ownership back. */
    ScriptExecutionPolicy tiny = strict;
    tiny.maximum_host_compile_source_bytes = 1;
    ScriptResult one_shot_result;
    CHECK(!scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &tiny,
              &one_shot_result)
          && one_shot_result.host_compile_attempts == 1
          && one_shot_result.host_compile_rejections == 1
          && one_shot_result.host_compile_source_bytes == 0
          && one_shot_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && budget.current == document_bytes);
    ScriptRuntimeOptions tiny_options = {
        .viewport = viewport,
        .execution_policy = tiny,
        .defer_document_scripts = true
    };
    ScriptResult tiny_result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 4u * MIB, 1000,
        "https://example.test/", &tiny_options, &tiny_result);
    CHECK(runtime == NULL
          && tiny_result.host_compile_attempts == 1
          && tiny_result.host_compile_rejections == 1
          && tiny_result.host_compile_source_bytes == 0
          && tiny_result.max_nonpreemptible_compile_bytes == 0
          && tiny_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && tiny_result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_INTERNAL
          && strstr(tiny_result.error, "rejected before compile") != NULL
          && budget.current == document_bytes);

    /* Projected-time admission mirrors the byte ceiling: the throughput
       model is enforced before QuickJS parsing begins, fails truthfully,
       and rolls runtime ownership back to zero. */
    ScriptExecutionPolicy modeled = strict;
    modeled.maximum_host_compile_source_bytes = 0;
    modeled.maximum_host_compile_projected_us = 1000;
    modeled.modeled_compile_bytes_per_ms = 1;
    ScriptResult projected_result;
    CHECK(!scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &modeled,
              &projected_result)
          && projected_result.host_compile_attempts == 1
          && projected_result.host_compile_rejections == 1
          && projected_result.host_compile_projected_rejections == 1
          && projected_result.host_compile_projected_rejected_bytes != 0
          && projected_result.host_compile_rejected_source_bytes == 0
          && projected_result.host_compile_source_bytes == 0
          && projected_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME
          && strstr(projected_result.error, "modeled compile time") != NULL
          && budget.current == document_bytes);

    /* A generous model admits everything and leaves the projected counters
       untouched. */
    ScriptExecutionPolicy generous = strict;
    generous.maximum_host_compile_projected_us = UINT64_MAX;
    generous.modeled_compile_bytes_per_ms = SIZE_MAX;
    ScriptResult generous_result;
    CHECK(scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &generous,
              &generous_result)
          && generous_result.host_compile_rejections == 0
          && generous_result.host_compile_projected_rejections == 0
          && generous_result.host_compile_projected_rejected_bytes == 0
          && generous_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED);

    strict.slow_compile_threshold_us = UINT64_MAX;
    strict.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = strict,
        .defer_document_scripts = true,
        .allow_test_network_primitive_overrides = true
    };
    ScriptResult result;
    /* DOM functionality probes below churn ~1100 nodes; the pristine
       (unpatched) QuickJS interpreter at -O0 needs more than a second. */
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success
          && result.host_compile_attempts >= 5
          && result.host_compile_rejections == 0
          && result.host_compile_source_limit_bytes
               == strict.maximum_host_compile_source_bytes
          && result.nonpreemptible_compile_count == 0
          && result.nonpreemptible_callback_count == 0);

    puts("test: large network delivery enters from the collected heap floor");
    size_t response_heap_before = script_runtime_heap_remaining(runtime);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{for(let i=0;i<256;i++){const cycle={"
              "pad:'x'.repeat(1024)};cycle.self=cycle;}"
              "globalThis.pocSummary='NETWORK-PRESSURE-READY'})()",
              "<network-pressure-setup>", &result)
          && strcmp(result.summary, "NETWORK-PRESSURE-READY") == 0);
    size_t response_heap_with_cycles = script_runtime_heap_remaining(runtime);
    CHECK(response_heap_with_cycles < response_heap_before
          && js_rt_prepare_network_response_delivery(
                 runtime, 512u * 1024u - 1u) == 0
          && script_runtime_heap_remaining(runtime)
                 == response_heap_with_cycles);
    (void) js_rt_prepare_network_response_delivery(runtime, 512u * 1024u);
    CHECK(script_runtime_heap_remaining(runtime) > response_heap_with_cycles);

    puts("test: bounded Gamepad API publishes one stable PSP controller");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__gamepadEvents=[];const bridge="
              "Object.getOwnPropertyDescriptor(globalThis,"
              "'__tilefinchUpdateGamepad');"
              "addEventListener('gamepadconnected',e=>"
              "__gamepadEvents.push(e.type+':'+e.gamepad.index));"
              "addEventListener('gamepaddisconnected',e=>"
              "__gamepadEvents.push(e.type+':'+e.gamepad.index));"
              "globalThis.pocSummary=bridge&&!bridge.writable&&"
              "!bridge.configurable&&navigator.getGamepads()[0]===null?"
              "'GAMEPAD-HIDDEN':'GAMEPAD-LEAKED'",
              "<gamepad-install>", &result)
          && strcmp(result.summary, "GAMEPAD-HIDDEN") == 0);
    TilefinchGamepadState gamepad = {
        .buttons = (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_PRIMARY)
            | (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_DPAD_UP),
        .axes = {INT16_MAX, -INT16_MAX, 0, 0},
        .timestamp_ms = 1234,
        .connected = true
    };
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const first=navigator.getGamepads(),p=first[0],"
              "second=navigator.getGamepads()[0];"
              "globalThis.__gamepadRef=p;"
              "globalThis.__gamepadButtonsRef=p.buttons;"
              "globalThis.pocSummary="
              "first.length===1&&p===second&&p.id==='PSP Built-in Controller'"
              "&&p.mapping==='standard'&&p.buttons.length===17"
              "&&p.buttons[0].pressed&&p.buttons[12].value===1"
              "&&!p.buttons[1].pressed&&p.axes.length===4"
              "&&p.axes[0]===1&&p.axes[1]===-1&&p.timestamp===1234?"
              "'GAMEPAD-MAPPED':'GAMEPAD-BAD'})()",
              "<gamepad-connected>", &result)
          && strcmp(result.summary, "GAMEPAD-MAPPED") == 0
          && script_runtime_advance(runtime, 0, 8, &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=__gamepadEvents.join(',')",
              "<gamepad-connect-event>", &result)
          && strcmp(result.summary, "gamepadconnected:0") == 0);
    gamepad.axes[0] = 8192;
    gamepad.axes[1] = -4096;
    gamepad.timestamp_ms = 1240;
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const p=navigator.getGamepads()[0];"
              "globalThis.pocSummary=p===__gamepadRef&&"
              "p.buttons===__gamepadButtonsRef&&p.buttons[0].pressed&&"
              "p.buttons[12].pressed&&!p.buttons[1].pressed&&"
              "p.axes[0]>0.24&&p.axes[0]<0.26&&"
              "p.axes[1]>-0.13&&p.axes[1]<-0.12&&p.timestamp===1240?"
              "'GAMEPAD-AXIS-UPDATED':'GAMEPAD-BAD'})()",
              "<gamepad-axis-only>", &result)
          && strcmp(result.summary, "GAMEPAD-AXIS-UPDATED") == 0);
    CHECK(tilefinch_gamepad_state_update(
              &gamepad, false, 0, 0, 0, 1250)
          && script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_advance(runtime, 0, 8, &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=navigator.getGamepads()[0]===null&&"
              "__gamepadEvents.join(',')==='gamepadconnected:0,"
              "gamepaddisconnected:0'?'GAMEPAD-DISCONNECTED':'GAMEPAD-BAD'",
              "<gamepad-disconnected>", &result)
          && strcmp(result.summary, "GAMEPAD-DISCONNECTED") == 0);

    puts("test: Tilefinch page-controls requests require user activation");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "navigator.tilefinch.requestPageControls().then(()=>"
              "globalThis.pocSummary='PAGE-CONTROLS-DIRECT-BAD',error=>"
              "globalThis.pocSummary=error.name==='NotAllowedError'&&"
              "navigator.tilefinch.pageControlsExitChord==='Start+Select'"
              "?'PAGE-CONTROLS-DIRECT-BLOCKED':'PAGE-CONTROLS-WRONG');"
              "const control=document.createElement('button');"
              "control.id='page-controls-target';document.body.append(control);"
              "control.addEventListener('click',()=>navigator.tilefinch."
              "requestPageControls().then(()=>globalThis.pocSummary="
              "'PAGE-CONTROLS-REQUESTED'));",
              "<page-controls-setup>", &result)
          && strcmp(result.summary, "PAGE-CONTROLS-DIRECT-BLOCKED") == 0
          && !script_runtime_page_fullscreen_active(runtime));
    lxb_dom_node_t *page_controls_target = find_element_id(
        lxb_dom_interface_node(document.html), "page-controls-target");
    CHECK(page_controls_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, page_controls_target, &result)
          && strcmp(result.summary, "PAGE-CONTROLS-REQUESTED") == 0
          && script_runtime_page_fullscreen_active(runtime)
          && script_runtime_exit_page_fullscreen(runtime));

    puts("test: asynchronous callback entry observes cancellation");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__callbackBoundaryRan=0;"
              "setTimeout(()=>{globalThis.__callbackBoundaryRan=1},0);"
              "globalThis.pocSummary='CALLBACK-BOUNDARY-ARMED'",
              "<callback-boundary-arm>", &result)
          && strcmp(result.summary, "CALLBACK-BOUNDARY-ARMED") == 0);
    CallbackAbortCooperate callback_abort = {0};
    TilefinchPlatformServices callback_abort_services = {
        .context = &callback_abort,
        .cooperate = callback_abort_cooperate
    };
    size_t callback_poll_calls_before =
        result.host_callback_calls_with_interrupt_polls;
    tilefinch_platform_set_services(&callback_abort_services);
    bool callback_advanced = script_runtime_advance(runtime, 1, 1, &result);
    tilefinch_platform_set_services(NULL);
    CHECK(!callback_advanced && result.interrupted
          && callback_abort.calls == 1
          && result.host_callback_calls_with_interrupt_polls
                 > callback_poll_calls_before);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=String(globalThis.__callbackBoundaryRan)",
              "<callback-boundary-observe>", &result)
          && strcmp(result.summary, "0") == 0);

    static const char hardening_probe[] =
        "(()=>{const hidden=!Object.keys(globalThis).some(key=>"
        "key.startsWith('__tilefinch')),lookup=Object.getOwnPropertyDescriptor("
        "globalThis,'__tilefinchDiagnosticLookup'),wrap="
        "Object.getOwnPropertyDescriptor(globalThis,'__tilefinchWrap'),native="
        "Object.getOwnPropertyDescriptor(globalThis,'__tilefinchClone'),retire="
        "Object.getOwnPropertyDescriptor(globalThis,"
        "'__tilefinchRetireNativeNodeState');"
        "let privateBlocked=false,evalBlocked=false;"
        "try{__tilefinchDiagnosticLookup('timers')}catch(error){"
        "privateBlocked=error instanceof ReferenceError}"
        "try{__tilefinchDiagnosticLookup('timers.length=0')}catch(error){"
        "evalBlocked=error instanceof ReferenceError}"
        "const node=document.createElement('div'),handle=node.__handle;"
        "node.__handle=2147483647;let redefineBlocked=false;"
        "try{Object.defineProperty(node,'__handle',{value:1})}"
        "catch(error){redefineBlocked=error instanceof TypeError}"
        "globalThis.pocSummary=hidden&&lookup&&!lookup.enumerable"
        "&&!lookup.writable&&!lookup.configurable&&wrap&&!wrap.enumerable"
        "&&!wrap.writable&&!wrap.configurable&&privateBlocked&&evalBlocked"
        "&&native&&!native.enumerable&&!native.writable&&!native.configurable"
        "&&retire&&!retire.enumerable&&!retire.writable"
        "&&!retire.configurable"
        "&&node.__handle===handle&&redefineBlocked"
        "?'REALM-HARDENING-OK':'REALM-HARDENING-FAILED:'+"
        "JSON.stringify({hidden,visible:Object.keys(globalThis).filter("
        "key=>key.startsWith('__tilefinch')).slice(0,16),"
        "lookup:[!!lookup,lookup?.enumerable,"
        "lookup?.writable,lookup?.configurable],wrap:[!!wrap,"
        "wrap?.enumerable,wrap?.writable,wrap?.configurable],"
        "native:[!!native,native?.enumerable,native?.writable,"
        "native?.configurable],retire:[!!retire,retire?.enumerable,"
        "retire?.writable,retire?.configurable],"
        "privateBlocked,evalBlocked,handle,nodeHandle:node.__handle,"
        "redefineBlocked});})()";
    bool hardening_ok = script_runtime_evaluate_diagnostic(
        runtime, hardening_probe, "<realm-hardening-probe>", &result);
    if (!hardening_ok
        || strcmp(result.summary, "REALM-HARDENING-OK") != 0) {
        fprintf(stderr, "hardening probe: ok=%d summary=%s error=%s\n",
                hardening_ok, result.summary, result.error);
    }
    CHECK(hardening_ok
          && strcmp(result.summary, "REALM-HARDENING-OK") == 0);

    puts("test: exception formatting cannot poison the runtime");
    static const char hostile_exception_probe[] =
        "throw {[Symbol.toPrimitive](){throw new Error('secondary')}}";
    CHECK(!script_runtime_evaluate_diagnostic(
              runtime, hostile_exception_probe,
              "<hostile-exception-formatting>", &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='EXCEPTION-RECOVERED'",
              "<hostile-exception-recovery>", &result)
          && strcmp(result.summary, "EXCEPTION-RECOVERED") == 0);
    static const char hostile_rejection_probe[] =
        "globalThis.__hostileRejection=Promise.reject({"
        "[Symbol.toPrimitive](){throw new Error('secondary')},"
        "get stack(){throw new Error('secondary-stack')}});"
        "globalThis.pocSummary='REJECTION-FORMATTED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, hostile_rejection_probe,
              "<hostile-rejection-formatting>", &result)
          && strcmp(result.summary, "REJECTION-FORMATTED") == 0
          && script_runtime_evaluate_diagnostic(
              runtime, "__hostileRejection.catch(()=>{});"
                       "globalThis.pocSummary='REJECTION-RECOVERED'",
              "<hostile-rejection-recovery>", &result)
          && strcmp(result.summary, "REJECTION-RECOVERED") == 0);

    puts("test: selector-list commas ignore quoted syntax");
    static const char quoted_selector_probe[] =
        "(()=>{const a=document.createElement('div'),"
        "b=document.createElement('div');a.title='(';a.id='quoted-open';"
        "b.id='quoted-other';document.body.append(a,b);"
        "const first=document.querySelectorAll('[title=\"(\"], #quoted-other'),"
        "second=document.querySelectorAll('[title=\"a,b\"], #quoted-other');"
        "a.title='a,b';const third=document.querySelectorAll("
        "'[title=\"a,b\"], #quoted-other');a.remove();b.remove();"
        "globalThis.pocSummary=first.length===2&&second.length===1"
        "&&third.length===2?'QUOTED-SELECTOR-OK':'QUOTED-SELECTOR-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, quoted_selector_probe, "<quoted-selector-probe>",
              &result)
          && strcmp(result.summary, "QUOTED-SELECTOR-OK") == 0);

    puts("test: lower-level web compatibility regressions stay fixed");
    static const char compatibility_probe[] =
        "(async()=>{const source=new ArrayBuffer(8),"
        "view=new DataView(source,2,3);view.setUint8(0,41);"
        "const cloned=structuredClone(view),dataViewOk="
        "cloned instanceof DataView&&cloned.byteOffset===2"
        "&&cloned.byteLength===3&&cloned.getUint8(0)===41;"
        "const form=document.createElement('form'),"
        "select=document.createElement('select'),"
        "first=document.createElement('option'),"
        "second=document.createElement('option');"
        "first.value='first';second.value='second';"
        "select.append(first,second);form.append(select);"
        "document.body.append(form);select.selectedIndex=1;form.reset();"
        "const selectOk=select.selectedIndex===0&&first.selected"
        "&&!second.selected;form.remove();"
        "const flex=document.createElement('div');flex.style.flex='2 3 25%';"
        "document.body.append(flex);const flexValue=flex.style.flex,"
        "flexOk=flexValue==='2 3 25%';flex.remove();"
        "const stream=new ReadableStream({start(){}}),reader=stream.getReader(),"
        "pending=reader.read().then(()=>'',error=>error.name);"
        "let releaseThrew=false;try{reader.releaseLock()}catch(_){"
        "releaseThrew=true}const pendingName=await pending,streamOk="
        "!releaseThrew&&pendingName==='TypeError'&&!stream.locked;"
        "globalThis.pocSummary=dataViewOk&&selectOk&&flexOk&&streamOk"
        "?'COMPATIBILITY-REGRESSIONS-OK':'COMPATIBILITY-REGRESSIONS-FAILED:'"
        "+JSON.stringify({dataViewOk,selectOk,flexOk,streamOk,flexValue});"
        "})().catch(error=>{"
        "globalThis.pocSummary='COMPATIBILITY-REGRESSIONS-ERROR:'+error});";
    bool compatibility_ok = script_runtime_evaluate_diagnostic(
        runtime, compatibility_probe, "<compatibility-regressions>", &result);
    for (size_t tick = 0; compatibility_ok && tick < 8
         && strncmp(result.summary, "COMPATIBILITY-REGRESSIONS-", 26) != 0;
         tick++) {
        compatibility_ok = script_runtime_advance(runtime, 0, 64, &result);
    }
    if (!compatibility_ok
        || strcmp(result.summary, "COMPATIBILITY-REGRESSIONS-OK") != 0) {
        fprintf(stderr, "compatibility probe: ok=%d summary=%s error=%s\n",
                compatibility_ok, result.summary, result.error);
    }
    CHECK(compatibility_ok
          && strcmp(result.summary, "COMPATIBILITY-REGRESSIONS-OK") == 0);

    puts("test: oversized data script fails visibly and remains counted");
    size_t oversized_data_failed_before = result.dynamic_scripts_failed;
    static const char oversized_data_script_probe[] =
        "(()=>{const script=document.createElement('script');"
        "script.src='data:text/javascript,'+'x'.repeat(16384);"
        "script.addEventListener('load',()=>{"
        "globalThis.pocSummary='OVERSIZED-DATA-LOADED'});"
        "script.addEventListener('error',()=>{"
        "globalThis.pocSummary='OVERSIZED-DATA-ERROR'});"
        "document.head.appendChild(script);"
        "globalThis.pocSummary='OVERSIZED-DATA-PENDING';})()";
    bool oversized_data_ok = script_runtime_evaluate_diagnostic(
        runtime, oversized_data_script_probe, "<oversized-data-script>",
        &result);
    for (size_t tick = 0; oversized_data_ok && tick < 16
         && strcmp(result.summary, "OVERSIZED-DATA-ERROR") != 0; tick++) {
        oversized_data_ok = script_runtime_advance(runtime, 0, 128, &result);
    }
    CHECK(oversized_data_ok
          && strcmp(result.summary, "OVERSIZED-DATA-ERROR") == 0
          && result.dynamic_scripts_failed
                 == oversized_data_failed_before + 1u);

    static const char trusted_dispatch_setup[] =
        "globalThis.__tilefinchTrustedDispatchHits=0;"
        "document.querySelector('script').addEventListener("
        "'trusted-dispatch',()=>{"
        "globalThis.__tilefinchTrustedDispatchHits++});"
        "globalThis.__tilefinchWrap=()=>({dispatchEvent(){return false}});"
        "globalThis.__tilefinchDispatchHandle=()=>false;";
    lxb_dom_node_t *trusted_dispatch_target = find_script(
        lxb_dom_interface_node(document.html));
    CHECK(trusted_dispatch_target != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, trusted_dispatch_setup,
              "<trusted-dispatch-setup>", &result)
          && script_runtime_dispatch_node(
              runtime, trusted_dispatch_target,
              "trusted-dispatch", &result)
          && !result.last_event_cancelled);
    static const char trusted_dispatch_check[] =
        "globalThis.pocSummary="
        "__tilefinchTrustedDispatchHits===1?'TRUSTED-DISPATCH-OK':"
        "'TRUSTED-DISPATCH-FAILED:'+__tilefinchTrustedDispatchHits";
    bool trusted_dispatch_ok = script_runtime_evaluate_diagnostic(
        runtime, trusted_dispatch_check,
        "<trusted-dispatch-check>", &result);
    if (!trusted_dispatch_ok
        || strcmp(result.summary, "TRUSTED-DISPATCH-OK") != 0) {
        fprintf(stderr, "trusted dispatch probe: ok=%d summary=%s error=%s\n",
                trusted_dispatch_ok, result.summary, result.error);
    }
    CHECK(trusted_dispatch_ok
          && strcmp(result.summary, "TRUSTED-DISPATCH-OK") == 0);

    static const char bounded_ancestor_probe[] =
        "(()=>{const first=document.createElement('div'),"
        "second=document.createElement('div');"
        "first.__tilefinchDetachedParent=second;"
        "second.__tilefinchDetachedParent=first;let blocked=false;"
        "try{first.dispatchEvent(new Event('cycle'))}catch(error){"
        "blocked=error instanceof DOMException&&"
        "error.name==='HierarchyRequestError'}"
        "first.__tilefinchDetachedParent=null;"
        "second.__tilefinchDetachedParent=null;"
        "globalThis.pocSummary=blocked?'ANCESTOR-BOUND-OK':"
        "'ANCESTOR-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, bounded_ancestor_probe, "<ancestor-bound-probe>",
              &result)
          && strcmp(result.summary, "ANCESTOR-BOUND-OK") == 0);

    static const char bounded_clone_probe[] =
        "(()=>{const host=document.createElement('div');"
        "host.innerHTML='<i>'.repeat(70)+'leaf'+'</i>'.repeat(70);"
        "let blocked=false;try{host.cloneNode(true)}catch(error){"
        "blocked=error instanceof Error}"
        "globalThis.pocSummary=blocked?'CLONE-BOUND-OK':"
        "'CLONE-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, bounded_clone_probe, "<clone-bound-probe>", &result)
          && strcmp(result.summary, "CLONE-BOUND-OK") == 0);

    static const char clone_control_state_probe[] =
        "(()=>{const original=document.createElement('input');"
        "original.value='original';const clone=original.cloneNode(false);"
        "clone.value='clone';globalThis.pocSummary="
        "original.value==='original'&&clone.value==='clone'"
        "?'CLONE-CONTROL-STATE-OK':'CLONE-CONTROL-STATE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, clone_control_state_probe,
              "<clone-control-state-probe>", &result)
          && strcmp(result.summary, "CLONE-CONTROL-STATE-OK") == 0);

    static const char foreign_template_probe[] =
        "(()=>{const node=document.createElementNS("
        "'http://www.w3.org/2000/svg','template');"
        "const noContent=node.content==null;let setterBounded=false;"
        "try{node.innerHTML='<g></g>';setterBounded=true}"
        "catch(error){setterBounded=error instanceof Error}"
        "globalThis.pocSummary=noContent&&setterBounded"
        "?'FOREIGN-TEMPLATE-OK':'FOREIGN-TEMPLATE-FAILED';})()";
    bool foreign_template_ok = script_runtime_evaluate_diagnostic(
        runtime, foreign_template_probe,
        "<foreign-template-probe>", &result);
    if (!foreign_template_ok
        || strcmp(result.summary, "FOREIGN-TEMPLATE-OK") != 0) {
        fprintf(stderr, "foreign template probe: ok=%d summary=%s error=%s\n",
                foreign_template_ok, result.summary, result.error);
    }
    CHECK(foreign_template_ok
          && strcmp(result.summary, "FOREIGN-TEMPLATE-OK") == 0);

    static const char observer_isolation_probe[] =
        "(()=>{const target=document.createElement('div');"
        "document.body.appendChild(target);let survivor=false;"
        "const throwing=new MutationObserver(()=>{throw new Error('probe')}),"
        "healthy=new MutationObserver(()=>{survivor=true});"
        "throwing.observe(target,{attributes:true});"
        "healthy.observe(target,{attributes:true});"
        "target.setAttribute('data-probe','one');"
        "return Promise.resolve().then(()=>{target.setAttribute("
        "'data-probe','two');return Promise.resolve()}).then(()=>{"
        "throwing.disconnect();healthy.disconnect();target.remove();"
        "globalThis.pocSummary=survivor?'OBSERVER-ISOLATION-OK':"
        "'OBSERVER-ISOLATION-FAILED'})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, observer_isolation_probe,
              "<observer-isolation-probe>", &result)
          && strcmp(result.summary, "OBSERVER-ISOLATION-OK") == 0);

    static const char observer_lifecycle_probe[] =
        "(()=>{const parent=document.createElement('div'),"
        "child=document.createElement('span');parent.appendChild(child);"
        "document.body.appendChild(parent);let records=[];"
        "const mo=new MutationObserver(items=>records.push(...items));"
        "mo.observe(parent,{childList:true,subtree:true,attributes:true});"
        "parent.removeChild(child);child.setAttribute('data-after','yes');"
        "let invalidMargin=false,invalidThreshold=false;"
        "try{new IntersectionObserver(()=>{},{rootMargin:'1em'})}"
        "catch(error){invalidMargin=error instanceof SyntaxError}"
        "try{new IntersectionObserver(()=>{},{threshold:2})}"
        "catch(error){invalidThreshold=error instanceof RangeError}"
        "const io=new IntersectionObserver(()=>{},"
        "{root:document,rootMargin:'10px 5%',threshold:[1,.5,.5,0]});"
        "io.observe(document.body);return Promise.resolve().then(()=>{"
        "const queued=io.takeRecords();return Promise.resolve().then(()=>{"
        "const transient=records.some(record=>record.type==='childList')"
        "&&records.some(record=>record.type==='attributes'"
        "&&record.target===child);const normalized="
        "io.rootMargin==='10px 5% 10px 5%'"
        "&&io.thresholds.join(',')==='0,0.5,1'&&queued.length===1;"
        "mo.disconnect();io.disconnect();parent.remove();"
        "globalThis.pocSummary=transient&&normalized&&invalidMargin"
        "&&invalidThreshold?'OBSERVER-LIFECYCLE-OK':"
        "'OBSERVER-LIFECYCLE-FAILED:'+JSON.stringify({transient,"
        "normalized,invalidMargin,invalidThreshold,records:records.length,"
        "queued:queued.length})})})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, observer_lifecycle_probe,
              "<observer-lifecycle-probe>", &result)
          && strcmp(result.summary, "OBSERVER-LIFECYCLE-OK") == 0);

    lxb_dom_node_t *script = find_script(
        lxb_dom_interface_node(document.html));
    CHECK(script != NULL);
    size_t oversized_length =
        strict.maximum_host_compile_source_bytes + 1;
    char *oversized = malloc(oversized_length + 1);
    CHECK(oversized != NULL);
    memset(oversized, ';', oversized_length);
    oversized[oversized_length] = '\0';
    size_t attempts_before = result.host_compile_attempts;
    size_t compiled_before = result.host_compile_source_bytes;
    CHECK(!script_runtime_evaluate_external(
              runtime, script, oversized, oversized_length,
              "https://example.test/oversized.js", &result)
          && result.success
          && result.host_compile_attempts == attempts_before + 1
          && result.host_compile_rejections == 1
          && result.host_compile_rejected_source_bytes == oversized_length
          && result.host_compile_source_bytes == compiled_before
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_EXTERNAL
          && result.last_compile_source_bytes == oversized_length
          && strcmp(result.last_compile_source_name,
                    "https://example.test/oversized.js") == 0
          && result.max_nonpreemptible_compile_bytes < oversized_length
          && result.external_scripts_failed == 1
          && result.external_scripts_loaded == 0);
    free(oversized);

    static const char diagnostic[] =
        "globalThis.pocSummary='RESPONSIVE-AFTER-REJECTION'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, diagnostic, "<post-rejection>", &result)
          && result.success
          && strcmp(result.summary, "RESPONSIVE-AFTER-REJECTION") == 0
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_DIAGNOSTIC
          && result.host_compile_rejections == 1
          && result.host_callback_calls != 0
          && result.nonpreemptible_compile_count == 0
          && result.nonpreemptible_callback_count == 0);

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    puts("test: watchdog aborts an admitted compile through parser polls");
    size_t abort_source_length = 48u * 1024u;
    char *abort_source = malloc(abort_source_length + 1);
    CHECK(abort_source != NULL);
    for (size_t at = 0; at < abort_source_length; at += 4) {
        memcpy(abort_source + at, "a=1;", 4);
    }
    abort_source[abort_source_length] = '\0';
    CompileAbortCooperate abort_cooperate = {0};
    TilefinchPlatformServices abort_services = {
        .context = &abort_cooperate,
        .cooperate = compile_abort_cooperate
    };
    tilefinch_platform_set_services(&abort_services);
    size_t aborts_before = result.host_compile_watchdog_aborts;
    CHECK(!script_runtime_evaluate_diagnostic(
              runtime, abort_source, "<compile-abort>", &result)
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ABORTED_WATCHDOG
          && result.host_compile_watchdog_aborts == aborts_before + 1
          && result.host_compile_watchdog_aborted_bytes
               >= abort_source_length
          && abort_cooperate.calls != 0
          && strstr(result.error, "compilation interrupted") != NULL);
    tilefinch_platform_set_services(NULL);
    free(abort_source);
    /* The realm stays healthy after a truthful compile abort. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='RESPONSIVE-AFTER-ABORT'",
              "<post-abort>", &result)
          && result.success
          && strcmp(result.summary, "RESPONSIVE-AFTER-ABORT") == 0
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED);
#endif

    puts("test: null external script completion is successful");
    size_t loaded_before_null = result.external_scripts_loaded;
    size_t failed_before_null = result.external_scripts_failed;
    static const char null_completion[] = "null";
    CHECK(script_runtime_evaluate_external(
              runtime, script, null_completion,
              sizeof(null_completion) - 1,
              "https://example.test/null-completion.js", &result)
          && result.success
          && result.external_scripts_loaded == loaded_before_null + 1
          && result.external_scripts_failed == failed_before_null);

    size_t callbacks_before_promise = result.host_callback_calls;
    static const char promise_job[] =
        "Promise.resolve().then(()=>{"
        "globalThis.pocSummary='PROMISE-JOB-OBSERVED'})";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, promise_job, "<promise-job-probe>", &result)
          && strcmp(result.summary, "PROMISE-JOB-OBSERVED") == 0
          && result.host_callback_calls > callbacks_before_promise);

    puts("test: promise rejection lifecycle telemetry distinguishes undefined");
    size_t rejections_created_before = result.promise_rejections_created;
    size_t rejections_handled_before = result.promise_rejections_handled;
    size_t rejections_undefined_before = result.promise_rejections_undefined;
    static const char rejection_probe[] =
        "globalThis.__tilefinchUnhandledProbe=Promise.reject(undefined);"
        "globalThis.__tilefinchHandledProbe=Promise.reject(new Error('handled'));"
        "globalThis.__tilefinchHandledProbe.catch(()=>{});";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, rejection_probe, "<promise-rejection-probe>", &result)
          && result.promise_rejections == 1
          && result.promise_rejections_created
               == rejections_created_before + 2
          && result.promise_rejections_handled
               == rejections_handled_before + 1
          && result.promise_rejections_undefined
               == rejections_undefined_before + 1);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "__tilefinchUnhandledProbe.catch(()=>{})",
              "<promise-rejection-handle>", &result)
          && result.promise_rejections == 0
          && result.promise_rejections_handled
               == rejections_handled_before + 2);

    static const char webcrypto_digest_probe[] =
        "(()=>{let synchronous=false,arrayPromise,typedPromise,viewPromise,"
        "unsupportedPromise,typePromise,quotaPromise;try{"
        "const expected='ba7816bf8f01cfea414140de5dae2223'"
        "+'b00361a396177a9cb410ff61f20015ad';"
        "const whole=new Uint8Array([97,98,99]);"
        "const framed=new Uint8Array([0,97,98,99,255]);"
        "arrayPromise=crypto.subtle.digest('SHA-256',whole.buffer);"
        "typedPromise=crypto.subtle.digest('sha-256',framed.subarray(1,4));"
        "viewPromise=crypto.subtle.digest({name:'SHA-256'},"
        "new DataView(framed.buffer,1,3));"
        "unsupportedPromise=crypto.subtle.digest('SHA-1',whole);"
        "typePromise=crypto.subtle.digest('SHA-256','abc');"
        "quotaPromise=crypto.subtle.digest('SHA-256',"
        "new Uint8Array(1024*1024+1));"
        "const hex=value=>[...new Uint8Array(value)]"
        ".map(byte=>byte.toString(16).padStart(2,'0')).join('');"
        "Promise.all([arrayPromise.then(value=>value instanceof ArrayBuffer"
        "&&hex(value)===expected),typedPromise.then(value=>hex(value)===expected),"
        "viewPromise.then(value=>hex(value)===expected),"
        "unsupportedPromise.then(()=>false,error=>error.name==='NotSupportedError'),"
        "typePromise.then(()=>false,error=>error.name==='TypeError'),"
        "quotaPromise.then(()=>false,error=>error.name==='RangeError')])"
        ".then(checks=>{globalThis.pocSummary=!synchronous"
        "&&arrayPromise instanceof Promise&&checks.every(Boolean)"
        "?'WEBCRYPTO-DIGEST-OK':'WEBCRYPTO-DIGEST-FAILED'});"
        "}catch(error){synchronous=true;globalThis.pocSummary="
        "'WEBCRYPTO-DIGEST-SYNC-THROW:'+error;}})();";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webcrypto_digest_probe, "<webcrypto-digest-probe>",
              &result)
          && strcmp(result.summary, "WEBCRYPTO-DIGEST-OK") == 0);

    static const char image_factory_probe[] =
        "(()=>{const image=new Image(37,19),called=Image(11,7);"
        "globalThis.pocSummary=typeof Image==='function'"
        "&&Image.prototype===HTMLImageElement.prototype"
        "&&image instanceof Image&&image instanceof HTMLImageElement"
        "&&image.tagName==='IMG'&&image.getAttribute('width')==='37'"
        "&&image.getAttribute('height')==='19'&&called instanceof Image"
        "&&called.getAttribute('width')==='11'"
        "&&called.getAttribute('height')==='7'"
        "?'IMAGE-FACTORY-OK':'IMAGE-FACTORY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, image_factory_probe, "<image-factory-probe>", &result)
          && strcmp(result.summary, "IMAGE-FACTORY-OK") == 0);

    puts("test: page visibility pauses visual tasks and preserves edges");
    static const char visibility_setup[] =
        "globalThis.__visibilityEdges=[];globalThis.__hiddenRaf=0;"
        "globalThis.__visibilityHandlerEdges=[];globalThis.__hiddenTimer=0;"
        "if(typeof __tilefinchPageVisible!=='undefined'||"
        "typeof __tilefinchApplyPageVisibility!=='undefined')"
        "throw new Error('visibility host bridge leaked');"
        "document.onvisibilitychange=()=>__visibilityHandlerEdges.push("
        "document.visibilityState);document.addEventListener("
        "'visibilitychange',()=>__visibilityEdges.push("
        "document.visibilityState));requestAnimationFrame(()=>__hiddenRaf++);"
        "setTimeout(()=>__hiddenTimer++,0);";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, visibility_setup, "<visibility-setup>", &result)
          && script_runtime_set_page_visibility(runtime, false)
          && script_runtime_advance(runtime, 16, 16, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=document.hidden&&"
                 "document.visibilityState==='hidden'&&"
                 "__visibilityEdges.join(',')==='hidden'&&"
                 "__visibilityHandlerEdges.join(',')==='hidden'&&"
                 "__hiddenTimer===1&&__hiddenRaf===0"
                 "?'VISIBILITY-HIDDEN-OK':'VISIBILITY-HIDDEN-FAILED'",
                 "<visibility-hidden>", &result)
          && strcmp(result.summary, "VISIBILITY-HIDDEN-OK") == 0);
    CHECK(script_runtime_set_page_visibility(runtime, true)
          && script_runtime_advance(runtime, 16, 16, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=!document.hidden&&"
                 "document.visibilityState==='visible'&&"
                 "__visibilityEdges.join(',')==='hidden,visible'&&"
                 "__visibilityHandlerEdges.join(',')==='hidden,visible'&&"
                 "__hiddenRaf===1"
                 "?'VISIBILITY-RESTORED-OK':'VISIBILITY-RESTORE-FAILED'",
                 "<visibility-restored>", &result)
          && strcmp(result.summary, "VISIBILITY-RESTORED-OK") == 0);
    CHECK(script_runtime_set_page_visibility(runtime, false)
          && script_runtime_set_page_visibility(runtime, true)
          && script_runtime_advance(runtime, 0, 1, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=document.hidden&&"
                 "__visibilityEdges.slice(-1)[0]==='hidden'"
                 "?'VISIBILITY-QUEUED-HIDDEN-OK':"
                 "'VISIBILITY-QUEUED-HIDDEN-FAILED'",
                 "<visibility-queued-hidden>", &result)
          && strcmp(result.summary, "VISIBILITY-QUEUED-HIDDEN-OK") == 0
          && script_runtime_advance(runtime, 0, 1, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=!document.hidden&&"
                 "__visibilityEdges.slice(-2).join(',')==='hidden,visible'"
                 "?'VISIBILITY-QUEUED-VISIBLE-OK':"
                 "'VISIBILITY-QUEUED-VISIBLE-FAILED'",
                 "<visibility-queued-visible>", &result)
          && strcmp(result.summary, "VISIBILITY-QUEUED-VISIBLE-OK") == 0);

    puts("test: page fullscreen requires native user activation");
    static const char fullscreen_setup[] =
        "(()=>{const target=document.createElement('div');"
        "target.id='fullscreen-target';document.body.append(target);"
        "globalThis.__fullscreenChanges=0;"
        "document.addEventListener('fullscreenchange',()=>"
        "globalThis.__fullscreenChanges++);"
        "target.requestFullscreen().then(()=>"
        "globalThis.pocSummary='FULLSCREEN-DIRECT-BAD',error=>"
        "globalThis.pocSummary=error.name==='NotAllowedError'"
        "?'FULLSCREEN-DIRECT-BLOCKED':'FULLSCREEN-DIRECT-WRONG');"
        "target.addEventListener('click',()=>target.requestFullscreen().then("
        "()=>globalThis.pocSummary=document.fullscreenElement===target"
        "?'FULLSCREEN-ENTERED':'FULLSCREEN-ENTER-FAILED'));})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, fullscreen_setup, "<fullscreen-setup>", &result)
          && strcmp(result.summary, "FULLSCREEN-DIRECT-BLOCKED") == 0);
    lxb_dom_node_t *fullscreen_target = find_element_id(
        lxb_dom_interface_node(document.html), "fullscreen-target");
    CHECK(fullscreen_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, fullscreen_target, &result)
          && strcmp(result.summary, "FULLSCREEN-ENTERED") == 0
          && script_runtime_page_fullscreen_active(runtime));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "document.exitFullscreen().then(()=>globalThis.pocSummary="
              "document.fullscreenElement===null&&__fullscreenChanges===2"
              "?'FULLSCREEN-EXITED':'FULLSCREEN-EXIT-FAILED')",
              "<fullscreen-exit>", &result)
          && strcmp(result.summary, "FULLSCREEN-EXITED") == 0
          && !script_runtime_page_fullscreen_active(runtime));
    /* Entry and exit queue the same bounded render-fixup a real frame would
       service. Drain it before the later raw timer-cap probe. */
    CHECK(script_runtime_advance(runtime, 16, 16, &result));

    puts("test: bounded game audio decodes and starts under user activation");
    static const char game_audio_setup[] =
        "(()=>{const bytes=new Uint8Array(52),view=new DataView(bytes.buffer),"
        "text=(at,value)=>{for(let i=0;i<value.length;i++)bytes[at+i]="
        "value.charCodeAt(i)},u16=(at,value)=>view.setUint16(at,value,true),"
        "u32=(at,value)=>view.setUint32(at,value,true);text(0,'RIFF');"
        "u32(4,44);text(8,'WAVE');text(12,'fmt ');u32(16,16);u16(20,1);"
        "u16(22,1);u32(24,44100);u32(28,88200);u16(32,2);u16(34,16);"
        "text(36,'data');u32(40,8);view.setInt16(44,1000,true);"
        "view.setInt16(46,-1000,true);view.setInt16(48,2000,true);"
        "view.setInt16(50,-2000,true);const context=new AudioContext(),"
        "target=document.createElement('button');target.id='game-audio-target';"
        "document.body.append(target);context.resume().then(()=>"
        "globalThis.pocSummary='GAME-AUDIO-DIRECT-BAD',error=>"
        "globalThis.pocSummary=error.name==='NotAllowedError'"
        "?'GAME-AUDIO-DIRECT-BLOCKED':'GAME-AUDIO-DIRECT-WRONG');"
        "context.decodeAudioData(bytes.buffer).then(buffer=>{"
        "globalThis.__gameAudioBuffer=buffer;target.addEventListener('click',"
        "()=>{context.resume().then(()=>{const source=context.createBufferSource();"
        "source.buffer=buffer;source.connect(context.destination);source.start();"
        "globalThis.__gameAudioSource=source;globalThis.__gameAudioEnded=[];"
        "source.addEventListener('ended',()=>__gameAudioEnded.push('listener'));"
        "source.onended=()=>__gameAudioEnded.push('handler');"
        "globalThis.pocSummary=context.state==='running'&&buffer.length===4"
        "?'GAME-AUDIO-STARTED':'GAME-AUDIO-START-FAILED'})})})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_setup, "<game-audio-setup>", &result)
          && strcmp(result.summary, "GAME-AUDIO-DIRECT-BLOCKED") == 0);
    lxb_dom_node_t *game_audio_target = find_element_id(
        lxb_dom_interface_node(document.html), "game-audio-target");
    CHECK(game_audio_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, game_audio_target, &result)
          && strcmp(result.summary, "GAME-AUDIO-STARTED") == 0);
    int16_t game_audio_samples[8] = {0};
    CHECK(runtime->game_audio != NULL
          && tilefinch_game_audio_mix(runtime->game_audio,
                                      game_audio_samples, 4)
          && game_audio_samples[0] == 1000
          && game_audio_samples[2] == -1000);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=__gameAudioSource._voice===0&&"
                 "__gameAudioEnded.includes('listener')&&"
                 "__gameAudioEnded.includes('handler')&&"
                 "__gameAudioEnded.length===2"
                 "?'GAME-AUDIO-ENDED-OK':'GAME-AUDIO-ENDED-FAILED'",
                 "<game-audio-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-ENDED-OK") == 0);
    puts("test: game audio supports bounded synthesis, panning, and schedules");
    static const char game_audio_synthesis_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,gain="
        "context.createGain(),pan=context.createStereoPanner(),"
        "osc=context.createOscillator();gain.gain.value=.5;pan.pan.value=1;"
        "osc.frequency.value=11025;osc.connect(gain).connect(pan).connect("
        "context.destination);globalThis.__gameOscEnded=0;"
        "globalThis.__gameSynthGain=gain;"
        "osc.onended=()=>__gameOscEnded++;const now=context.currentTime;"
        "osc.start(now+.005);osc.stop(now+.008);"
        "globalThis.pocSummary=osc.type==='sine'&&pan.pan.value===1"
        "?'GAME-AUDIO-SYNTHESIS-STARTED':"
        "'GAME-AUDIO-SYNTHESIS-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_synthesis_probe,
              "<game-audio-synthesis>", &result)
          && strcmp(result.summary, "GAME-AUDIO-SYNTHESIS-STARTED") == 0);
    int16_t game_audio_synthesis_samples[800];
    memset(game_audio_synthesis_samples, 0x55,
           sizeof(game_audio_synthesis_samples));
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
                                  game_audio_synthesis_samples, 400));
    bool oscillator_silent_before_start = true;
    bool oscillator_right_channel_played = false;
    for (size_t frame = 0; frame < 400; frame++) {
        if (game_audio_synthesis_samples[frame * 2u] != 0)
            oscillator_silent_before_start = false;
        if (frame < 180u
            && game_audio_synthesis_samples[frame * 2u + 1u] != 0)
            oscillator_silent_before_start = false;
        if (frame >= 180u
            && game_audio_synthesis_samples[frame * 2u + 1u] != 0)
            oscillator_right_channel_played = true;
    }
    CHECK(oscillator_silent_before_start && oscillator_right_channel_played);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=__gameOscEnded===1"
                 "?'GAME-AUDIO-SYNTHESIS-ENDED':"
                 "'GAME-AUDIO-SYNTHESIS-END-FAILED'",
                 "<game-audio-synthesis-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-SYNTHESIS-ENDED") == 0);
    puts("test: game-audio gain envelopes advance without JavaScript ticks");
    static const char game_audio_envelope_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,gain=__gameSynthGain,"
        "osc=context.createOscillator();gain.gain.value=0;"
        "osc.frequency.value=440;osc.connect(gain).connect(context.destination);"
        "osc.start();const now=context.currentTime;gain.gain.cancelScheduledValues(now);"
        "gain.gain.setValueAtTime(0,now);gain.gain.setTargetAtTime(.5,now,.002);"
        "gain.gain.setTargetAtTime(0,now+.02,.004);"
        "globalThis.__gameEnvelopeOsc=osc;globalThis.pocSummary="
        "typeof gain.gain.setTargetAtTime==='function'"
        "?'GAME-AUDIO-ENVELOPE-SCHEDULED':'GAME-AUDIO-ENVELOPE-MISSING'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_envelope_probe,
              "<game-audio-envelope>", &result)
          && strcmp(result.summary, "GAME-AUDIO-ENVELOPE-SCHEDULED") == 0);
    int envelope_peak = 0, envelope_tail = 0;
    int16_t game_audio_envelope_samples[
        TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    for (size_t block = 0; block < 8; block++) {
        CHECK(tilefinch_game_audio_mix(
            runtime->game_audio, game_audio_envelope_samples,
            TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
        for (size_t sample = 0;
             sample < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; sample++) {
            int magnitude = game_audio_envelope_samples[sample] < 0
                ? -(int) game_audio_envelope_samples[sample]
                : (int) game_audio_envelope_samples[sample];
            if (magnitude > envelope_peak) envelope_peak = magnitude;
        }
        if (block == 7u) {
            for (size_t sample =
                     (TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES - 32u) * 2u;
                 sample < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; sample++) {
                int magnitude = game_audio_envelope_samples[sample] < 0
                    ? -(int) game_audio_envelope_samples[sample]
                    : (int) game_audio_envelope_samples[sample];
                if (magnitude > envelope_tail) envelope_tail = magnitude;
            }
        }
    }
    CHECK(envelope_peak > 4000 && envelope_tail < 128);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__gameEnvelopeOsc.stop();globalThis.pocSummary="
              "'GAME-AUDIO-ENVELOPE-STOPPED'",
              "<game-audio-envelope-stop>", &result)
          && strcmp(result.summary, "GAME-AUDIO-ENVELOPE-STOPPED") == 0
          && script_runtime_advance(runtime, 0, 4, &result));
    static const char game_audio_loop_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,source="
        "context.createBufferSource(),pan=context.createStereoPanner();"
        "source.buffer=__gameAudioBuffer;source.loop=true;"
        "source.loopStart=1/44100;source.loopEnd=3/44100;"
        "source.connect(pan).connect(context.destination);source.start();"
        "pan.pan.value=-1;globalThis.__gameLoopSource=source;"
        "globalThis.pocSummary='GAME-AUDIO-LOOP-STARTED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_loop_probe,
              "<game-audio-loop>", &result)
          && strcmp(result.summary, "GAME-AUDIO-LOOP-STARTED") == 0);
    int16_t game_audio_loop_samples[12] = {0};
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
                                  game_audio_loop_samples, 6)
          && game_audio_loop_samples[0] == 1000
          && game_audio_loop_samples[1] == 0
          && game_audio_loop_samples[2] == -1000
          && game_audio_loop_samples[4] == 2000
          && game_audio_loop_samples[6] == -1000
          && game_audio_loop_samples[8] == 2000);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__gameLoopSource.stop();globalThis.pocSummary="
              "'GAME-AUDIO-LOOP-STOPPED'",
              "<game-audio-loop-stop>", &result)
          && strcmp(result.summary, "GAME-AUDIO-LOOP-STOPPED") == 0
          && script_runtime_advance(runtime, 0, 4, &result));
    static const char game_audio_bounds_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,sources=[];"
        "for(let i=0;i<4;i++){const source=context.createBufferSource();"
        "source.buffer=__gameAudioBuffer;source.loop=true;"
        "source.connect(context.destination);source.start();sources.push(source)}"
        "let bounded=false;try{const extra=context.createBufferSource();"
        "extra.buffer=__gameAudioBuffer;extra.connect(context.destination);"
        "extra.start()}catch(error){bounded=error.name==='QuotaExceededError'}"
        "globalThis.__stoppedAudioEnded=0;for(const source of sources){"
        "source.onended=()=>__stoppedAudioEnded++;source.stop()}"
        "let pinned=false;try{const pending=context.createBufferSource();"
        "pending.buffer=__gameAudioBuffer;pending.connect(context.destination);"
        "pending.start()}catch(error){pinned=error.name==='QuotaExceededError'}"
        "globalThis.pocSummary=bounded&&pinned"
        "?'GAME-AUDIO-BOUNDS-OK':'GAME-AUDIO-BOUNDS-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_bounds_probe,
              "<game-audio-bounds>", &result)
          && strcmp(result.summary, "GAME-AUDIO-BOUNDS-OK") == 0);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "(()=>{let reused=false;try{const source="
                 "__gameAudioBuffer._context.createBufferSource();"
                 "source.buffer=__gameAudioBuffer;"
                 "source.connect(__gameAudioBuffer._context.destination);"
                 "source.start();source.stop();reused=true}catch(error){}"
                 "globalThis.pocSummary=__stoppedAudioEnded===4&&reused"
                 "?'GAME-AUDIO-STOP-ENDED-OK':"
                 "'GAME-AUDIO-STOP-ENDED-FAILED'})()",
                 "<game-audio-stop-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-STOP-ENDED-OK") == 0);
    script_runtime_suspend_game_audio(runtime);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=__gameAudioBuffer._context.state==="
              "'suspended'?'GAME-AUDIO-HOST-SUSPEND-OK':"
              "'GAME-AUDIO-HOST-SUSPEND-FAILED'",
              "<game-audio-host-suspend>", &result)
          && strcmp(result.summary, "GAME-AUDIO-HOST-SUSPEND-OK") == 0);
    static const char game_audio_reentrant_close_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,source="
        "context.createBufferSource();source.buffer=__gameAudioBuffer;"
        "source.connect(context.destination);let safe=false;try{source.start(0,"
        "{valueOf(){context.close();return 0}})}catch(error){safe="
        "context.state==='closed'}globalThis.pocSummary=safe"
        "?'GAME-AUDIO-REENTRANT-CLOSE-OK':"
        "'GAME-AUDIO-REENTRANT-CLOSE-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_reentrant_close_probe,
              "<game-audio-reentrant-close>", &result)
          && strcmp(result.summary,
                    "GAME-AUDIO-REENTRANT-CLOSE-OK") == 0
          && runtime->game_audio == NULL);

    static const char responsive_embedding_probe[] =
        "(()=>{const picture=document.createElement('picture'),"
        "source=document.createElement('source'),img=document.createElement('img');"
        "source.srcset='data:,selected';picture.appendChild(source);"
        "img.src='fallback.png';img.srcset='small.png 100w, large.png 400w';"
        "img.sizes='calc(25vw + 10px)';picture.appendChild(img);"
        "document.body.appendChild(picture);const selected=img.currentSrc;"
        "source.media='not all';const fallback=img.currentSrc;"
        "const frame=document.createElement('iframe'),view=frame.contentWindow;"
        "frame.srcdoc='<p>first</p>';document.body.appendChild(frame);"
        "const first=frame.contentDocument.body.textContent;"
        "frame.srcdoc='<p>second</p>';"
        "const second=frame.contentDocument.body.textContent,"
        "stable=view===frame.contentWindow"
        "&&frame.contentDocument.defaultView===view;"
        "frame.removeAttribute('srcdoc');"
        "const cleared=frame.contentDocument.body.textContent;"
        "globalThis.pocSummary=selected==='data:,selected'"
        "&&fallback==='https://example.test/large.png'"
        "&&first==='first'&&second==='second'&&cleared===''"
        "&&stable?'RESPONSIVE-EMBEDDING-OK':'RESPONSIVE-EMBEDDING-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, responsive_embedding_probe,
              "<responsive-embedding-probe>", &result)
          && strcmp(result.summary, "RESPONSIVE-EMBEDDING-OK") == 0);

    static const char media_factory_probe[] =
        "(()=>{const audio=new Audio(),sourced=new Audio('sounds/tone.mp3'),"
        "called=Audio(),video=document.createElement('video'),"
        "source=document.createElement('source');"
        "source.src='sounds/fallback.ogg';called.appendChild(source);"
        "const skipped=document.createElement('source'),"
        "selected=document.createElement('source');"
        "skipped.src='movie.webm';skipped.type='video/webm';"
        "const hidden=document.createElement('source');"
        "hidden.src='hidden.mp4';hidden.type='video/mp4';"
        "hidden.media='not all';video.appendChild(hidden);"
        "selected.src='movie.mp4';selected.type='video/mp4';"
        "video.appendChild(skipped);video.appendChild(selected);"
        "const events=[];for(const name of ['seeking','seeked'])"
        "audio.addEventListener(name,()=>events.push(name));"
        "const promise=audio.play();promise.catch(()=>{});"
        "audio.currentTime=1.5;audio.volume=.25;"
        "globalThis.pocSummary=typeof Audio==='function'"
        "&&Audio.prototype===HTMLAudioElement.prototype"
        "&&audio instanceof Audio&&video instanceof HTMLVideoElement"
        "&&called instanceof HTMLAudioElement&&audio.preload==='auto'"
        "&&sourced.currentSrc==='https://example.test/sounds/tone.mp3'"
        "&&called.currentSrc==='https://example.test/sounds/fallback.ogg'"
        "&&video.currentSrc==='https://example.test/movie.mp4'"
        "&&audio.canPlayType('audio/mpeg')===''&&promise instanceof Promise"
        "&&audio.paused&&audio.currentTime===1.5&&audio.volume===.25"
        "&&events.join(',')==='seeking,seeked'"
        "&&audio.readyState===HTMLMediaElement.HAVE_NOTHING"
        "&&audio.networkState===HTMLMediaElement.NETWORK_EMPTY"
        "?'MEDIA-FACTORY-OK':'MEDIA-FACTORY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, media_factory_probe, "<media-factory-probe>", &result)
          && strcmp(result.summary, "MEDIA-FACTORY-OK") == 0);

    static const char collator_probe[] =
        "(()=>{const collator=new Intl.Collator(undefined,{sensitivity:'base',"
        "numeric:true}),compare=collator.compare,called=Intl.Collator('en-US',"
        "{ignorePunctuation:true}),options=collator.resolvedOptions();"
        "globalThis.pocSummary=typeof Intl.Collator==='function'"
        "&&collator instanceof Intl.Collator&&called instanceof Intl.Collator"
        "&&compare('Éclair','eclair')===0&&compare('item2','item10')<0"
        "&&called.compare('a-b','ab')===0&&options.sensitivity==='base'"
        "&&options.numeric===true&&Intl.Collator.supportedLocalesOf(['en-US'])[0]"
        "==='en-US'?'COLLATOR-OK':'COLLATOR-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, collator_probe, "<collator-probe>", &result)
          && strcmp(result.summary, "COLLATOR-OK") == 0);

    static const char additional_intl_probe[] =
        "(()=>{const relative=new Intl.RelativeTimeFormat('en-US',"
        "{numeric:'auto'}),list=new Intl.ListFormat('en-US',"
        "{type:'disjunction'}),names=new Intl.DisplayNames('en-US',"
        "{type:'language',fallback:'none'});"
        "const relativeParts=relative.formatToParts(-2,'days'),"
        "listParts=list.formatToParts(['red','green','blue']);"
        "globalThis.pocSummary=relative.format(-1,'day')==='yesterday'"
        "&&relative.format(2,'hours')==='in 2 hours'"
        "&&relativeParts.some(part=>part.type==='integer'&&part.value==='2')"
        "&&list.format(['red','green'])==='red or green'"
        "&&list.format(['red','green','blue'])==='red, green, or blue'"
        "&&listParts.filter(part=>part.type==='element').length===3"
        "&&names.of('fr')==='French'&&names.of('zz')===undefined"
        "&&Intl.RelativeTimeFormat.supportedLocalesOf(['en-US'])[0]==='en-US'"
        "?'ADDITIONAL-INTL-OK':'ADDITIONAL-INTL-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, additional_intl_probe, "<additional-intl-probe>",
              &result)
          && strcmp(result.summary, "ADDITIONAL-INTL-OK") == 0);

    static const char clipboard_probe[] =
        "(async()=>{const area=document.createElement('textarea');"
        "area.value='copy this text';document.body.appendChild(area);"
        "area.setSelectionRange(5,9);area.select();area.setSelectionRange(5,9);"
        "const copied=document.execCommand('copy'),legacy=await navigator.clipboard.readText();"
        "await navigator.clipboard.writeText('modern text');"
        "const modern=await navigator.clipboard.readText();area.remove();"
        "globalThis.pocSummary=copied&&legacy==='this'&&modern==='modern text'"
        "&&document.queryCommandSupported('copy')"
        "?'CLIPBOARD-OK':'CLIPBOARD-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, clipboard_probe, "<clipboard-probe>", &result)
          && strcmp(result.summary, "CLIPBOARD-OK") == 0
          && result.clipboard_writes == 2
          && strcmp(result.last_clipboard_text, "modern text") == 0);

    static const char document_location_probe[] =
        "(()=>{const before=location.href,host=location.hostname;"
        "const initial=document.URL===before&&document.documentURI===before"
        "&&document.baseURI===before&&document.domain===host;"
        "document.domain=host;history.replaceState(null,'','#standards');"
        "const live=document.URL===location.href"
        "&&document.documentURI===location.href"
        "&&document.baseURI===location.href;let rejected=false;"
        "try{document.domain=host.includes('.')?host.slice(host.indexOf('.')+1)"
        ":'invalid.example';}catch(error){rejected=error instanceof DOMException"
        "&&error.name==='SecurityError';}history.replaceState(null,'',before);"
        "globalThis.pocSummary=initial&&live&&rejected&&document.domain===host"
        "?'DOCUMENT-LOCATION-OK':'DOCUMENT-LOCATION-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, document_location_probe, "<document-location-probe>",
              &result)
          && strcmp(result.summary, "DOCUMENT-LOCATION-OK") == 0);

    static const char frozen_base_probe[] =
        "(()=>{const before=location.href,head=document.querySelector('head'),"
        "base=document.createElement('base');base.setAttribute('href','assets/');"
        "head.appendChild(base);const frozen=new URL('assets/',before).href,"
        "moved=new URL('/moved/page.html',before).href;"
        "history.replaceState(null,'',moved);const stayed=document.baseURI===frozen;"
        "base.setAttribute('href','changed/');const changed="
        "new URL('changed/',moved).href,changedNow=document.baseURI===changed;"
        "document.body.setAttribute('data-unrelated','yes');"
        "const again=new URL('/again/index.html',before).href;"
        "history.replaceState(null,'',again);const stayedAgain="
        "document.baseURI===changed;base.setAttribute('href','data:text/plain,no');"
        "const invalidNow=document.baseURI===again,third="
        "new URL('/third/index.html',before).href;history.replaceState(null,'',third);"
        "const invalidStayed=document.baseURI===again;base.remove();"
        "const fallback=document.baseURI===location.href;"
        "history.replaceState(null,'',before);"
        "globalThis.pocSummary=stayed&&changedNow&&stayedAgain&&invalidNow"
        "&&invalidStayed&&fallback"
        "&&document.baseURI===before?'FROZEN-BASE-OK':'FROZEN-BASE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frozen_base_probe, "<frozen-base-probe>", &result)
          && strcmp(result.summary, "FROZEN-BASE-OK") == 0);
    (void) script_runtime_collect_and_trim(runtime);

    static const char namespaced_attribute_probe[] =
        "(()=>{const svg=document.createElementNS('http://www.w3.org/2000/svg',"
        "'svg'),use=document.createElementNS('http://www.w3.org/2000/svg','use'),"
        "xlink='http://www.w3.org/1999/xlink';svg.appendChild(use);"
        "use.setAttributeNS(xlink,'xlink:href','#mark');const set="
        "use.getAttributeNS(xlink,'href')==='#mark'"
        "&&use.hasAttributeNS(xlink,'href');use.removeAttributeNS(xlink,'href');"
        "globalThis.pocSummary=set&&!use.hasAttributeNS(xlink,'href')"
        "?'NAMESPACE-ATTRIBUTE-OK':'NAMESPACE-ATTRIBUTE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, namespaced_attribute_probe,
              "<namespaced-attribute-probe>", &result)
          && strcmp(result.summary, "NAMESPACE-ATTRIBUTE-OK") == 0);

    static const char svg_element_probe[] =
        "(()=>{const svgNS='http://www.w3.org/2000/svg',"
        "htmlNS='http://www.w3.org/1999/xhtml',"
        "parsed=document.getElementById('parsed-svg'),"
        "group=document.getElementById('parsed-group'),"
        "made=document.createElementNS(svgNS,'svg'),"
        "path=document.createElementNS(svgNS,'path'),"
        "html=document.createElementNS(htmlNS,'div'),"
        "plain=document.createElementNS(null,'widget');made.appendChild(path);"
        "let namespaceError=false,characterError=false;"
        "try{document.createElementNS(null,'x:item')}catch(error){"
        "namespaceError=error instanceof DOMException"
        "&&error.name==='NamespaceError'}"
        "try{document.createElementNS(svgNS,'bad name')}catch(error){"
        "characterError=error instanceof DOMException"
        "&&error.name==='InvalidCharacterError'}"
        "const detached=document.implementation.createDocument(svgNS,'svg'),"
        "detachedRoot=detached.documentElement;"
        "const ok=typeof SVGElement==='function'"
        "&&Object.getPrototypeOf(SVGElement.prototype)===Element.prototype"
        "&&parsed instanceof SVGElement"
        "&&parsed instanceof Element&&!(parsed instanceof HTMLElement)"
        "&&parsed.namespaceURI===svgNS&&parsed.tagName==='svg'"
        "&&group instanceof SVGElement&&group.ownerSVGElement===parsed"
        "&&made instanceof SVGElement&&path instanceof SVGElement"
        "&&path.ownerSVGElement===made&&path.viewportElement===made"
        "&&html instanceof HTMLDivElement&&!(html instanceof SVGElement)"
        "&&plain instanceof Element&&!(plain instanceof HTMLElement)"
        "&&!(plain instanceof SVGElement)&&plain.namespaceURI===null"
        "&&detachedRoot instanceof SVGElement"
        "&&detachedRoot.namespaceURI===svgNS"
        "&&namespaceError&&characterError;"
        "globalThis.pocSummary=ok?'SVG-ELEMENT-OK':'SVG-ELEMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, svg_element_probe, "<svg-element-probe>", &result)
          && strcmp(result.summary, "SVG-ELEMENT-OK") == 0);

    static const char window_event_target_probe[] =
        "(()=>{let calls=0;const listener=()=>calls++,"
        "type='tilefinch-window-event-target-probe';"
        "EventTarget.prototype.addEventListener.call(window,type,listener);"
        "window.dispatchEvent(new Event(type));"
        "EventTarget.prototype.removeEventListener.call(window,type,listener);"
        "window.dispatchEvent(new Event(type));"
        "globalThis.pocSummary=window instanceof Window&&"
        "Object.getPrototypeOf(Window.prototype)===EventTarget.prototype&&"
        "calls===1?'WINDOW-EVENT-TARGET-OK':"
        "'WINDOW-EVENT-TARGET-FAILED:'+calls;})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, window_event_target_probe,
              "<window-event-target-probe>", &result)
          && strcmp(result.summary, "WINDOW-EVENT-TARGET-OK") == 0);

    static const char canvas_2d_probe[] =
        "(()=>{const canvas=document.createElement('canvas'),"
        "defaults=canvas.width===300&&canvas.height===150;"
        "canvas.width=2;canvas.height=1;const context=canvas.getContext('2d'),"
        "same=context===canvas.getContext('2D'),"
        "unsupported=canvas.getContext('webgl')===null;"
        "context.fillStyle='rebeccapurple';const named=context.fillStyle;"
        "context.fillStyle='not-a-color';const invalidRetained="
        "context.fillStyle===named;context.fillStyle='blue';"
        "context.fillRect(0,0,2,1);context.save();context.fillStyle='red';"
        "context.globalAlpha=.5;context.fillRect(0,0,1,1);"
        "context.globalAlpha=2;const alphaRetained=context.globalAlpha===.5;"
        "context.restore();const restored=context.fillStyle==='#0000ff'"
        "&&context.globalAlpha===1,pixel=context.getImageData(0,0,1,1).data;"
        "context.clearRect(1,0,1,1);const clear="
        "context.getImageData(1,0,1,1).data;"
        "const supplied=new ImageData(new Uint8ClampedArray([1,2,3,4]),1);"
        "context.putImageData(supplied,1,0);const put="
        "context.getImageData(1,0,1,1).data;"
        "const cloned=context.createImageData(supplied),cloneSized="
        "cloned.width===1&&cloned.height===1&&cloned!==supplied"
        "&&cloned.data.every(value=>value===0);"
        "const dirtyCanvas=document.createElement('canvas');"
        "dirtyCanvas.width=3;dirtyCanvas.height=1;"
        "const dirtyContext=dirtyCanvas.getContext('2d'),dirtySource="
        "new ImageData(new Uint8ClampedArray([255,0,0,255,0,255,0,255,"
        "0,0,255,255]),3,1);"
        "dirtyContext.putImageData(dirtySource,0,0,1,0,1,1);"
        "const dirtyPixels=dirtyContext.getImageData(0,0,3,1).data,"
        "dirtyPut=dirtyPixels[3]===0&&dirtyPixels[4]===0"
        "&&dirtyPixels[5]===255&&dirtyPixels[6]===0"
        "&&dirtyPixels[7]===255&&dirtyPixels[11]===0;"
        "dirtyContext.clearRect(0,0,3,1);"
        "dirtyContext.putImageData(dirtySource,0,0,2,0,-2,1);"
        "const negativeDirty=dirtyContext.getImageData(0,0,3,1).data,"
        "negativeDirtyPut=negativeDirty[0]===255&&negativeDirty[3]===255"
        "&&negativeDirty[4]===0&&negativeDirty[5]===255"
        "&&negativeDirty[7]===255&&negativeDirty[11]===0;"
        "let dirtyRange=false;try{dirtyContext.putImageData("
        "dirtySource,Infinity,0)}catch(error){dirtyRange="
        "error instanceof DOMException&&error.name==='NotSupportedError'}"
        "dirtyContext.fillStyle='red';dirtyContext.fillRect(0,0,3,1);"
        "dirtyContext.save();dirtyContext.translate(2,0);"
        "dirtyContext.lineWidth=7;dirtyContext.reset();"
        "dirtyContext.restore();const apiReset="
        "dirtyContext.lineWidth===1&&dirtyContext.fillStyle==='#000000'"
        "&&dirtyContext.getTransform().e===0"
        "&&dirtyContext.getImageData(0,0,3,1).data.every(value=>value===0);"
        "const compositeCanvas=document.createElement('canvas');"
        "compositeCanvas.width=1;compositeCanvas.height=1;"
        "const compositeContext=compositeCanvas.getContext('2d');"
        "compositeContext.fillStyle='red';compositeContext.fillRect(0,0,1,1);"
        "compositeContext.globalCompositeOperation='destination-out';"
        "compositeContext.globalAlpha=.5;compositeContext.fillRect(0,0,1,1);"
        "compositeContext.globalCompositeOperation='invalid';"
        "const composite=compositeContext.getImageData(0,0,1,1).data,"
        "compositeMode=compositeContext.globalCompositeOperation;"
        "context.fillStyle='hsl(120 100% 25%)';const hsl=context.fillStyle;"
        "canvas.setAttribute('width','2');const reset="
        "context.fillStyle==='#000000'&&context.globalAlpha===1"
        "&&context.getImageData(0,0,1,1).data.every(value=>value===0);"
        "canvas.width=513;canvas.height=257;context.fillStyle='#123456';"
        "context.fillRect(0,0,1,1);const boundedPixel="
        "context.getImageData(0,0,1,1).data,bounded="
        "canvas.width===480&&canvas.height===240"
        "&&boundedPixel[0]===18&&boundedPixel[1]===52"
        "&&boundedPixel[2]===86&&boundedPixel[3]===255;"
        "canvas.width=2147483647;canvas.height=0;const zeroBound="
        "canvas.width===480&&canvas.height===0;"
        "let quota=false,index=false;try{context.createImageData(513,257)}"
        "catch(error){quota=error instanceof DOMException"
        "&&error.name==='QuotaExceededError'}"
        "try{context.getImageData(0,0,0,1)}catch(error){"
        "index=error instanceof DOMException&&error.name==='IndexSizeError'}"
        "const ok=defaults&&context instanceof CanvasRenderingContext2D"
        "&&same&&unsupported&&invalidRetained&&named==='#663399'"
        "&&alphaRetained&&restored&&pixel[0]===128&&pixel[1]===0"
        "&&pixel[2]===128&&pixel[3]===255"
        "&&clear.every(value=>value===0)&&put[0]===1&&put[1]===2"
        "&&put[2]===3&&put[3]===4&&composite[0]===255"
        "&&composite[1]===0&&composite[2]===0&&composite[3]===128"
        "&&compositeMode==='destination-out'"
        "&&hsl==='#008000'&&reset&&bounded&&zeroBound"
        "&&quota&&index&&supplied.width===1&&supplied.height===1"
        "&&supplied.colorSpace==='srgb'&&cloneSized&&dirtyPut"
        "&&negativeDirtyPut&&dirtyRange&&apiReset;"
        "globalThis.pocSummary=ok?"
        "'CANVAS-2D-OK':'CANVAS-2D-FAILED:'+JSON.stringify({defaults,same,"
        "unsupported,invalidRetained,named,alphaRetained,restored,"
        "pixel:[...pixel],clear:[...clear],put:[...put],"
        "composite:[...composite],compositeMode,hsl,reset,bounded,zeroBound,"
        "quota,index,cloneSized,dirtyPut,negativeDirtyPut,dirtyRange,"
        "apiReset});})()";
    bool canvas_2d_ok = script_runtime_evaluate_diagnostic(
        runtime, canvas_2d_probe, "<canvas-2d-probe>", &result);
    if (!canvas_2d_ok || strcmp(result.summary, "CANVAS-2D-OK") != 0) {
        fprintf(stderr, "canvas 2d probe: ok=%d summary=%s error=%s\n",
                canvas_2d_ok, result.summary, result.error);
    }
    CHECK(canvas_2d_ok && strcmp(result.summary, "CANVAS-2D-OK") == 0);

    static const char image_bitmap_probe[] =
        "(async()=>{const data=new ImageData(new Uint8ClampedArray(["
        "255,0,0,255,0,0,255,255]),2,1),"
        "bitmap=await createImageBitmap(data,1,0,1,1,{resizeWidth:2,"
        "resizeHeight:2,resizeQuality:'pixelated'}),"
        "ratio=await createImageBitmap(data,{resizeWidth:4}),"
        "canvas=document.createElement('canvas');canvas.width=2;canvas.height=2;"
        "const context=canvas.getContext('2d');context.drawImage(bitmap,0,0);"
        "const pixel=context.getImageData(1,1,1,1).data,good="
        "bitmap instanceof ImageBitmap&&bitmap.width===2&&bitmap.height===2"
        "&&ratio.width===4&&ratio.height===2"
        "&&pixel[0]===0&&pixel[1]===0&&pixel[2]===255&&pixel[3]===255;"
        "bitmap.close();ratio.close();let closed=false,illegal=false;"
        "try{context.drawImage(bitmap,0,0)}"
        "catch(error){closed=error.name==='InvalidStateError'}"
        "try{new ImageBitmap()}catch(error){illegal=error instanceof TypeError}"
        "globalThis.pocSummary=good&&closed&&illegal?'IMAGE-BITMAP-OK':"
        "'IMAGE-BITMAP-FAILED';})().catch(error=>globalThis.pocSummary="
        "'IMAGE-BITMAP-ERROR:'+error)";
    bool image_bitmap_ok = script_runtime_evaluate_diagnostic(
        runtime, image_bitmap_probe, "<image-bitmap-probe>", &result);
    if (!image_bitmap_ok || strcmp(result.summary, "IMAGE-BITMAP-OK") != 0)
        fprintf(stderr, "image bitmap probe: ok=%d summary=%s error=%s\n",
                image_bitmap_ok, result.summary, result.error);
    CHECK(image_bitmap_ok && strcmp(result.summary, "IMAGE-BITMAP-OK") == 0);

    static const char canvas_save_overflow_probe[] =
        "(()=>{const canvas=document.createElement('canvas'),"
        "context=canvas.getContext('2d');context.fillStyle='red';"
        "for(let i=0;i<17;i++)context.save();context.fillStyle='blue';"
        "context.restore();const paired=context.fillStyle==='#0000ff';"
        "for(let i=0;i<16;i++)context.restore();const restored="
        "context.fillStyle==='#ff0000';globalThis.pocSummary=paired&&restored"
        "?'CANVAS-SAVE-OVERFLOW-OK':'CANVAS-SAVE-OVERFLOW-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_save_overflow_probe,
              "<canvas-save-overflow-probe>", &result)
          && strcmp(result.summary, "CANVAS-SAVE-OVERFLOW-OK") == 0);

    /* Coercion may run arbitrary page code.  A stale canvas handle must not
       survive wrapper release plus a document subtree replacement between
       argument conversion and the native surface commit. */
    ImageResources canvas_commit_images = {.budget = &budget};
    script_runtime_set_images(runtime, &canvas_commit_images);
    static const char canvas_commit_lifetime_probe[] =
        "(()=>{const old=document.createElement('canvas');old.width=1;"
        "old.height=1;document.body.appendChild(old);const handle=old.__handle,"
        "lease=old.__tilefinchHandleLease,pixels=new Uint8ClampedArray(4);"
        "let released=false;"
        "const hostile={valueOf(){old.remove();released="
        "__tilefinchReleaseNodeWrapper(handle,lease);document.body.innerHTML="
        "'<canvas id=replacement width=1 height=1></canvas>';return 1;}};"
        "const committed=__tilefinchCommitCanvasSurface(handle,hostile,1,"
        "pixels,0,0,1,1),replacement=document.getElementById('replacement');"
        "globalThis.pocSummary=!committed&&replacement&&released"
        "?'CANVAS-COMMIT-LIFETIME-OK':'CANVAS-COMMIT-LIFETIME-FAILED:'+"
        "JSON.stringify({committed,released,replacement:!!replacement,"
        "connected:old.isConnected});})()";
    bool canvas_commit_lifetime_ok = script_runtime_evaluate_diagnostic(
        runtime, canvas_commit_lifetime_probe,
        "<canvas-commit-lifetime-probe>", &result);
    if (!canvas_commit_lifetime_ok
        || strcmp(result.summary, "CANVAS-COMMIT-LIFETIME-OK") != 0) {
        fprintf(stderr, "canvas commit lifetime probe: ok=%d summary=%s error=%s\n",
                canvas_commit_lifetime_ok, result.summary, result.error);
    }
    CHECK(canvas_commit_lifetime_ok
          && strcmp(result.summary, "CANVAS-COMMIT-LIFETIME-OK") == 0);

    static const char canvas_taint_source_probe[] =
        "(()=>{const image=document.createElement('img');"
        "image.id='cross-origin-canvas-source';"
        "image.src='https://images.example.test/private.png';"
        "document.body.appendChild(image);globalThis.pocSummary="
        "'CANVAS-TAINT-SOURCE-READY';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_taint_source_probe,
              "<canvas-taint-source-probe>", &result)
          && strcmp(result.summary, "CANVAS-TAINT-SOURCE-READY") == 0);
    lxb_dom_node_t *taint_source = find_element_id(
        lxb_dom_interface_node(runtime->document->html),
        "cross-origin-canvas-source");
    static unsigned char private_pixel[4] = {17u, 34u, 51u, 255u};
    canvas_commit_images.items = budget_calloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, 1,
        sizeof(*canvas_commit_images.items));
    CHECK(taint_source != NULL && canvas_commit_images.items != NULL);
    canvas_commit_images.capacity = 1u;
    canvas_commit_images.count = 1u;
    canvas_commit_images.items[0] = (ImageResource) {
        .node = taint_source,
        .pixels = private_pixel,
        .source_width = 1,
        .source_height = 1,
        .width = 1,
        .height = 1,
        .cross_origin = true
    };
    static const char image_decode_probe[] =
        "(async()=>{const image=document.getElementById("
        "'cross-origin-canvas-source');await image.decode();const bitmap="
        "await createImageBitmap(image),canvas=document.createElement('canvas'),"
        "context=canvas.getContext('2d');canvas.width=1;canvas.height=1;"
        "context.drawImage(bitmap,0,0);let tainted=false;try{context.getImageData("
        "0,0,1,1)}catch(error){tainted=error.name==='SecurityError'}bitmap.close();"
        "const empty="
        "document.createElement('img');let rejected=false;try{await empty.decode()}"
        "catch(error){rejected=error.name==='EncodingError'}"
        "globalThis.pocSummary=image.complete&&image.naturalWidth===1&&tainted"
        "&&rejected"
        "?'IMAGE-DECODE-OK':'IMAGE-DECODE-FAILED'})().catch(error=>"
        "globalThis.pocSummary='IMAGE-DECODE-ERROR:'+error)";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, image_decode_probe, "<image-decode-probe>", &result)
          && strcmp(result.summary, "IMAGE-DECODE-OK") == 0);
    static const char canvas_taint_probe[] =
        "(()=>{const image=document.getElementById('cross-origin-canvas-source'),"
        "canvas=document.createElement('canvas'),context=canvas.getContext('2d');"
        "canvas.width=1;canvas.height=1;context.drawImage(image,0,0);"
        "const denied=call=>{try{call();return false}catch(error){return "
        "error instanceof DOMException&&error.name==='SecurityError'}};"
        "const pixels=denied(()=>context.getImageData(0,0,1,1)),"
        "url=denied(()=>canvas.toDataURL()),blob=denied(()=>canvas.toBlob(()=>{}));"
        "const patterned=document.createElement('canvas'),"
        "patternContext=patterned.getContext('2d');patterned.width=1;"
        "patterned.height=1;patternContext.createPattern(image,'repeat');"
        "const pattern=denied(()=>patternContext.getImageData(0,0,1,1));"
        "const copied=document.createElement('canvas'),copy=copied.getContext('2d');"
        "copied.width=1;copied.height=1;copy.drawImage(canvas,0,0);"
        "const propagated=denied(()=>copy.getImageData(0,0,1,1));"
        "const webglSource=document.createElement('canvas'),webgl2d="
        "webglSource.getContext('2d');webglSource.width=1;webglSource.height=1;"
        "webgl2d.drawImage(image,0,0);globalThis.__taintedWebglSource=webglSource;"
        "canvas.width=1;const reset=context.getImageData(0,0,1,1).data[3]===0;"
        "globalThis.pocSummary=pixels&&url&&blob&&pattern&&propagated&&reset"
        "?'CANVAS-TAINT-OK':'CANVAS-TAINT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_taint_probe, "<canvas-taint-probe>", &result)
          && strcmp(result.summary, "CANVAS-TAINT-OK") == 0);

    /* The retained queue is capped at eight canvases.  Disconnect its first
       eight entries before their microtasks run, leaving a connected ninth
       entry outside the queue.  That ninth microtask must publish its own
       surface rather than assuming an earlier batch included it. */
    static const char canvas_ninth_commit_probe[] =
        "(()=>{const before=__tilefinchCanvasDiagnostics.surfaceCommits,"
        "canvases=[];for(let i=0;i<9;i++){const canvas="
        "document.createElement('canvas');canvas.width=1;canvas.height=1;"
        "document.body.appendChild(canvas);const context=canvas.getContext('2d');"
        "context.fillStyle='red';context.fillRect(0,0,1,1);canvases.push(canvas)}"
        "for(let i=0;i<8;i++)canvases[i].remove();globalThis.pocSummary="
        "'CANVAS-NINTH-COMMIT-PENDING';queueMicrotask(()=>{const committed="
        "__tilefinchCanvasDiagnostics.surfaceCommits-before;"
        "globalThis.pocSummary=committed===1?'CANVAS-NINTH-COMMIT-OK':"
        "'CANVAS-NINTH-COMMIT-FAILED:'+committed;canvases[8].remove()})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_ninth_commit_probe,
              "<canvas-ninth-commit-probe>", &result)
          && script_runtime_advance(runtime, 0, 32, &result)
          && result.success
          && strcmp(result.summary, "CANVAS-NINTH-COMMIT-OK") == 0);

    static const char webgl_basic_probe[] =
        "(()=>{const canvas=document.createElement('canvas');canvas.width=8;"
        "canvas.height=8;document.body.appendChild(canvas);const gl="
        "canvas.getContext('webgl'),same=gl===canvas.getContext('experimental-webgl'),"
        "exclusive=canvas.getContext('2d')===null,vs=gl.createShader(gl.VERTEX_SHADER),"
        "fs=gl.createShader(gl.FRAGMENT_SHADER);globalThis.__testWebgl=gl;"
        "const taintTexture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,taintTexture);"
        "let taintRejected=false;try{gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,"
        "gl.UNSIGNED_BYTE,__taintedWebglSource)}catch(error){taintRejected="
        "error.name==='SecurityError'}"
        "gl.shaderSource(vs,'attribute vec2 position;void main(){gl_Position='"
        "+'vec4(position,0.0,1.0);}');gl.compileShader(vs);"
        "gl.shaderSource(fs,'precision mediump float;uniform vec4 tint;void main()'"
        "+'{gl_FragColor=tint;}');gl.compileShader(fs);const program=gl.createProgram();"
        "gl.attachShader(program,vs);gl.attachShader(program,fs);gl.linkProgram(program);"
        "gl.useProgram(program);const buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buffer);"
        "gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,0,1]),gl.STATIC_DRAW);"
        "const location=gl.getAttribLocation(program,'position');gl.enableVertexAttribArray(location);"
        "gl.vertexAttribPointer(location,2,gl.FLOAT,false,0,0);"
        "gl.uniform4f(gl.getUniformLocation(program,'tint'),1,0,0,1);"
        "gl.clearColor(0,0,1,1);gl.clear(gl.COLOR_BUFFER_BIT);"
        "gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();const pixel=new Uint8Array(4);"
        "gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);"
        "const dvs=gl.createShader(gl.VERTEX_SHADER),dfs=gl.createShader(gl.FRAGMENT_SHADER),"
        "depthProgram=gl.createProgram();gl.shaderSource(dvs,'attribute vec3 position;'"
        "+'void main(){gl_Position=vec4(position,1.);}');gl.shaderSource(dfs,"
        "'precision mediump float;uniform vec4 tint;void main(){gl_FragColor=tint;}');"
        "gl.compileShader(dvs);gl.compileShader(dfs);gl.attachShader(depthProgram,dvs);"
        "gl.attachShader(depthProgram,dfs);gl.linkProgram(depthProgram);gl.useProgram(depthProgram);"
        "const depthBuffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,depthBuffer);"
        "gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,.5,1,-1,.5,0,1,.5,"
        "-1,-1,-.5,1,-1,-.5,0,1,-.5,-1,-1,.5,1,-1,.5,0,1,.5]),gl.STATIC_DRAW);"
        "const dp=gl.getAttribLocation(depthProgram,'position');gl.enableVertexAttribArray(dp);"
        "gl.vertexAttribPointer(dp,3,gl.FLOAT,false,0,0);const dt="
        "gl.getUniformLocation(depthProgram,'tint');gl.enable(gl.DEPTH_TEST);"
        "gl.clearColor(0,0,0,1);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);"
        "gl.uniform4f(dt,0,0,1,1);gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();"
        "gl.uniform4f(dt,1,0,0,1);gl.drawArrays(gl.TRIANGLES,3,3);gl.finish();"
        "gl.uniform4f(dt,0,1,0,1);gl.drawArrays(gl.TRIANGLES,6,3);gl.finish();"
        "const depthPixel=new Uint8Array(4);gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,depthPixel);"
        "gl.disable(gl.DEPTH_TEST);"
        "const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);"
        "gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);"
        "gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);"
        "gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,"
        "new Uint8Array([0,255,0,255]));const tvs=gl.createShader(gl.VERTEX_SHADER),"
        "tfs=gl.createShader(gl.FRAGMENT_SHADER),textured=gl.createProgram();"
        "gl.shaderSource(tvs,'attribute vec2 position;attribute vec2 texcoord;'"
        "+'varying vec2 uv;void main(){uv=texcoord;gl_Position=vec4(position,0.,1.);}');"
        "gl.shaderSource(tfs,'precision mediump float;varying vec2 uv;uniform sampler2D image;'"
        "+'void main(){gl_FragColor=texture2D(image,uv);}');gl.compileShader(tvs);"
        "gl.compileShader(tfs);gl.attachShader(textured,tvs);gl.attachShader(textured,tfs);"
        "gl.linkProgram(textured);gl.useProgram(textured);gl.bindBuffer(gl.ARRAY_BUFFER,buffer);"
        "const p2=gl.getAttribLocation(textured,'position');gl.enableVertexAttribArray(p2);"
        "gl.vertexAttribPointer(p2,2,gl.FLOAT,false,0,0);const uvBuffer=gl.createBuffer();"
        "gl.bindBuffer(gl.ARRAY_BUFFER,uvBuffer);gl.bufferData(gl.ARRAY_BUFFER,"
        "new Float32Array([0,0,1,0,.5,1]),gl.STATIC_DRAW);const uv="
        "gl.getAttribLocation(textured,'texcoord');gl.enableVertexAttribArray(uv);"
        "gl.vertexAttribPointer(uv,2,gl.FLOAT,false,0,0);gl.clear(gl.COLOR_BUFFER_BIT);"
        "gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();const texturedPixel=new Uint8Array(4);"
        "gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,texturedPixel);"
        "const rejectedVs=gl.createShader(gl.VERTEX_SHADER),rejectedFs="
        "gl.createShader(gl.FRAGMENT_SHADER),rejected=gl.createProgram();"
        "gl.shaderSource(rejectedVs,'attribute vec2 position;void main(){for(int i=0;i<2;i++){}'"
        "+'gl_Position=vec4(position,0.,1.);}');gl.compileShader(rejectedVs);"
        "gl.shaderSource(rejectedFs,'void main(){gl_FragColor=vec4(1.);}');"
        "gl.compileShader(rejectedFs);gl.attachShader(rejected,rejectedVs);"
        "gl.attachShader(rejected,rejectedFs);gl.linkProgram(rejected);"
        "const second=document.createElement('canvas').getContext('webgl'),"
        "third=document.createElement('canvas').getContext('webgl');"
        "const ok=gl&&same&&exclusive&&second&&third===null&&taintRejected"
        "&&gl.getContextAttributes().alpha===false"
        "&&gl.getProgramParameter(program,gl.LINK_STATUS)"
        "&&pixel[0]>240&&pixel[1]<8&&pixel[2]<8&&pixel[3]>240"
        "&&depthPixel[0]>240&&depthPixel[1]<8&&depthPixel[2]<8"
        "&&texturedPixel[0]<8&&texturedPixel[1]>240&&texturedPixel[2]<8"
        "&&canvas.toDataURL().startsWith('data:image/png;base64,')"
        "&&!gl.getProgramParameter(rejected,gl.LINK_STATUS)"
        "&&gl.getParameter(gl.MAX_TEXTURE_SIZE)===512;"
        "globalThis.pocSummary=ok?'WEBGL-BASIC-OK':'WEBGL-BASIC-FAILED:'"
        "+Array.from(pixel).join(',')+':'+gl.getProgramInfoLog(rejected);})()";
    bool webgl_basic_ok = script_runtime_evaluate_diagnostic(
        runtime, webgl_basic_probe, "<webgl-basic-probe>", &result);
    if (!webgl_basic_ok || strcmp(result.summary, "WEBGL-BASIC-OK") != 0) {
        fprintf(stderr, "WebGL basic probe: ok=%d summary=%s error=%s\n",
                webgl_basic_ok, result.summary, result.error);
    }
    CHECK(webgl_basic_ok && strcmp(result.summary, "WEBGL-BASIC-OK") == 0);

    static const char webgl_state_probe[] =
        "(()=>{const g=globalThis.__testWebgl,c=g.canvas,v="
        "g.createShader(g.VERTEX_SHADER),f=g.createShader(g.FRAGMENT_SHADER);"
        "g.shaderSource(v,'attribute vec2 p;void main(){gl_Position='"
        "+'vec4(p,0.,1.);}');g.shaderSource(f,'precision mediump float;'"
        "+'uniform vec4 color;void main(){gl_FragColor=color;}');"
        "g.compileShader(v);g.compileShader(f);const p=g.createProgram();"
        "g.attachShader(p,v);g.attachShader(p,f);g.linkProgram(p);g.useProgram(p);"
        "const b=g.createBuffer();g.bindBuffer(g.ARRAY_BUFFER,b);"
        "g.bufferData(g.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,0,1]),"
        "g.STATIC_DRAW);const a=g.getAttribLocation(p,'p');"
        "g.enableVertexAttribArray(a);g.vertexAttribPointer(a,2,g.FLOAT,false,0,0);"
        "const color=g.getUniformLocation(p,'color'),pixel=new Uint8Array(4);"
        "g.clearColor(0,0,1,1);g.clear(g.COLOR_BUFFER_BIT);g.enable(g.BLEND);"
        "g.uniform4f(color,1,0,0,.5);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const replace=pixel[0]>240&&pixel[1]<8&&pixel[2]<8&&pixel[3]>120;"
        "g.blendFunc(g.SRC_ALPHA,g.ONE_MINUS_SRC_ALPHA);"
        "g.uniform4f(color,0,1,0,.5);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const alpha=pixel[0]>110&&pixel[0]<145&&pixel[1]>110&&pixel[1]<145;"
        "g.disable(g.BLEND);g.clearColor(0,0,0,1);g.clear(g.COLOR_BUFFER_BIT);"
        "g.bufferData(g.ARRAY_BUFFER,new Float32Array([-.8,-.8,.8,-.8,.8,.8]),"
        "g.STATIC_DRAW);g.uniform4f(color,1,1,1,1);g.drawArrays(g.LINE_LOOP,0,3);"
        "g.finish();g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const loop=pixel[0]>200;const frames=[];for(let i=0;i<5;i++)"
        "frames.push(g.createFramebuffer());const bounded=frames[4]===null;"
        "g.deleteFramebuffer(frames[0]);const reclaimed=!!g.createFramebuffer();"
        "while(g.getError()!==g.NO_ERROR){}g.blendFunc(g.DST_COLOR,g.ONE);"
        "const stateError=g.getError()===g.INVALID_ENUM&&!g.isContextLost();"
        "const emptyRead=new Uint8Array(0);g.readPixels(0,0,0,0,g.RGBA,"
        "g.UNSIGNED_BYTE,emptyRead);const zeroRead=g.getError()===g.NO_ERROR;"
        "g.blendFuncSeparate(g.ONE,g.ZERO,g.SRC_ALPHA,g.ONE_MINUS_SRC_ALPHA);"
        "const splitError=g.getError()===g.INVALID_OPERATION;"
        "c.width=640;c.height=480;const boundedSize=g.drawingBufferWidth===362"
        "&&g.drawingBufferHeight===272;g.clearColor(.25,.5,.75,1);"
        "g.clear(g.COLOR_BUFFER_BIT|g.DEPTH_BUFFER_BIT);g.finish();"
        "const largePixel=new Uint8Array(4);g.readPixels(181,136,1,1,g.RGBA,"
        "g.UNSIGNED_BYTE,largePixel);const largeSurface=!g.isContextLost()"
        "&&largePixel[0]>55&&largePixel[1]>115&&largePixel[2]>175;"
        "c.width=2147483647;c.height=0;const zeroBound="
        "g.drawingBufferWidth===480&&g.drawingBufferHeight===0"
        "&&!g.isContextLost();"
        "globalThis.pocSummary=replace&&alpha&&loop&&bounded&&reclaimed&&stateError"
        "&&zeroRead"
        "&&splitError"
        "&&boundedSize&&largeSurface&&zeroBound"
        "?'WEBGL-STATE-OK':'WEBGL-STATE-FAILED:'+Array.from(pixel).join(',');})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_state_probe, "<webgl-state-probe>", &result)
          && strcmp(result.summary, "WEBGL-STATE-OK") == 0);

    /* MDN's 3D tutorial composes projection and model-view mat4 uniforms in
       its vertex shader. The bounded backend combines that chain before
       sending one fixed-function transform to the native renderer. */
    static const char webgl_two_matrix_probe[] =
        "(()=>{const g=globalThis.__testWebgl,c=g.canvas;c.width=16;c.height=16;"
        "const v=g.createShader(g.VERTEX_SHADER),f=g.createShader(g.FRAGMENT_SHADER),"
        "p=g.createProgram();g.shaderSource(v,'attribute vec4 aVertexPosition;'"
        "+'uniform mat4 uModelViewMatrix;uniform mat4 uProjectionMatrix;'"
        "+'void main(void){gl_Position=uProjectionMatrix*uModelViewMatrix*'"
        "+'aVertexPosition;}');g.shaderSource(f,'precision mediump float;'"
        "+'uniform vec4 color;void main(void){gl_FragColor=color;}');"
        "g.compileShader(v);g.compileShader(f);g.attachShader(p,v);g.attachShader(p,f);"
        "g.linkProgram(p);g.useProgram(p);const b=g.createBuffer();"
        "g.bindBuffer(g.ARRAY_BUFFER,b);g.bufferData(g.ARRAY_BUFFER,new Float32Array("
        "[-.25,-.25,.25,-.25,0,.25]),g.STATIC_DRAW);const a="
        "g.getAttribLocation(p,'aVertexPosition');g.enableVertexAttribArray(a);"
        "g.vertexAttribPointer(a,2,g.FLOAT,false,0,0);const projection=new Float32Array("
        "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]),model=new Float32Array("
        "[1,0,0,0,0,1,0,0,0,0,1,0,.5,0,0,1]);"
        "g.uniformMatrix4fv(g.getUniformLocation(p,'uProjectionMatrix'),false,projection);"
        "g.uniformMatrix4fv(g.getUniformLocation(p,'uModelViewMatrix'),false,model);"
        "g.uniform4f(g.getUniformLocation(p,'color'),1,0,0,1);g.clearColor(0,0,0,1);"
        "g.clear(g.COLOR_BUFFER_BIT);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "const moved=new Uint8Array(4),origin=new Uint8Array(4);"
        "g.readPixels(12,8,1,1,g.RGBA,g.UNSIGNED_BYTE,moved);"
        "g.readPixels(8,8,1,1,g.RGBA,g.UNSIGNED_BYTE,origin);"
        "globalThis.pocSummary=g.getProgramParameter(p,g.LINK_STATUS)&&moved[0]>240"
        "&&origin[0]<8?'WEBGL-TWO-MATRIX-OK':'WEBGL-TWO-MATRIX-FAILED:'"
        "+Array.from(moved).join(',')+':'+Array.from(origin).join(',');})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_two_matrix_probe, "<webgl-two-matrix-probe>",
              &result)
          && strcmp(result.summary, "WEBGL-TWO-MATRIX-OK") == 0);

    static const char webgl_lifetime_probe[] =
        "(()=>{const old=document.createElement('canvas');old.width=1;"
        "old.height=1;document.body.appendChild(old);const handle=old.__handle,"
        "lease=old.__tilefinchHandleLease,commands=new Float64Array(64),"
        "textures=new Float64Array(),hostile={valueOf(){old.remove();"
        "__tilefinchReleaseNodeWrapper(handle,lease);document.body.innerHTML="
        "'<canvas id=webgl-replacement width=1 height=1></canvas>';return 1;}};"
        "commands[0]=0;commands[1]=0x4000;commands[5]=1;"
        "const rendered=__tilefinchWebGLRender(handle,hostile,1,commands,[],textures);"
        "globalThis.pocSummary=!rendered&&document.getElementById('webgl-replacement')"
        "?'WEBGL-LIFETIME-OK':'WEBGL-LIFETIME-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_lifetime_probe, "<webgl-lifetime-probe>",
              &result)
          && strcmp(result.summary, "WEBGL-LIFETIME-OK") == 0);
    script_runtime_set_images(runtime, NULL);
    images_destroy(&canvas_commit_images);

    /* Direct packed-command natives are an untrusted boundary even though
       the authored bootstrap normally sends small finite coordinates. */
    static const char canvas_numeric_boundary_probe[] =
        "(()=>{const pixels=new Uint8ClampedArray(16),base="
        "[0,0,1,1,255,0,0,255,1,1],bad=[NaN,Infinity,Number.MAX_VALUE];"
        "let rejected=true;for(const value of bad){const command="
        "new Float64Array(base);command[0]=value;rejected="
        "rejected&&!__tilefinchCanvasRasterRectBatch(pixels,2,2,command);}"
        "const points=new Float64Array([0,0,1,0]),empty=new Float64Array(),"
        "identity=new Float64Array([1,0,0,1,0,0]);"
        "rejected=rejected&&!__tilefinchCanvasRasterPath(pixels,2,2,points,"
        "true,false,0,0,0,255,1,1,1,0,0,10,empty,0,"
        "new Float64Array([NaN,0,2,2]),empty,empty);"
        "const imageCommand=new Float64Array([0,1,1,0,0,1,1,0,0,1,1,0,1,"
        "Number.MAX_VALUE,1,0,0,1,0,0,0,0,2,2]);"
        "rejected=rejected&&!__tilefinchCanvasRasterImageBatch(pixels,2,2,"
        "[new Uint8ClampedArray(4)],imageCommand);"
        "rejected=rejected&&!__tilefinchCanvasRasterImage(pixels,2,2,"
        "new Uint8ClampedArray(4),1,1,0,0,1,1,0,0,1,1,false,1,1,"
        "new Float64Array([1,0,0,1,Number.MAX_VALUE,0]),"
        "new Float64Array([0,0,2,2]),empty);"
        "rejected=rejected&&!__tilefinchCanvasRasterText(pixels,2,2,'x',"
        "Number.MAX_VALUE,1,8,0,false,false,0,0,0,255,1,1,false,1,identity,"
        "new Float64Array([0,0,2,2]),1,empty);"
        "globalThis.pocSummary=rejected?'CANVAS-NUMERIC-BOUNDARY-OK':"
        "'CANVAS-NUMERIC-BOUNDARY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_numeric_boundary_probe,
              "<canvas-numeric-boundary-probe>", &result)
          && strcmp(result.summary, "CANVAS-NUMERIC-BOUNDARY-OK") == 0);

    /* A hostile stroke can multiply glyph pixels by a 33x33 neighborhood.
       The native call must degrade within its fixed work allowance and return
       control to JavaScript instead of monopolizing the browser thread. */
    static const char canvas_native_work_bound_probe[] =
        "(()=>{const canvas=document.createElement('canvas');canvas.width=512;"
        "canvas.height=256;const context=canvas.getContext('2d');"
        "context.font='64px sans-serif';context.lineWidth=16;"
        "context.strokeText('W'.repeat(256),0,96);"
        "context.fillStyle='red';context.fillRect(0,0,1,1);"
        "globalThis.pocSummary=context.getImageData(0,0,1,1).data[3]>0"
        "?'CANVAS-NATIVE-WORK-BOUND-OK':'CANVAS-NATIVE-WORK-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_native_work_bound_probe,
              "<canvas-native-work-bound-probe>", &result)
          && strcmp(result.summary, "CANVAS-NATIVE-WORK-BOUND-OK") == 0);

    /* A batch is one native call and therefore gets one allowance.  Resetting
       the allowance for every command lets a bounded 64-command batch multiply
       the browser-thread stall even though every individual rectangle fits. */
    static const char canvas_native_batch_work_bound_probe[] =
        "(()=>{const pixels=new Uint8ClampedArray(512*256*4),"
        "commands=new Float64Array(64*10);"
        "for(let i=0;i<64;i++)commands.set([0,0,512,256,i===63?255:0,"
        "i===63?0:255,0,255,1,1],i*10);"
        "const completed=__tilefinchCanvasRasterRectBatch(pixels,512,256,commands),"
        "bounded=pixels[0]===0&&pixels[1]===255,"
        "continued=__tilefinchCanvasRasterRect(pixels,512,256,0,0,1,1,"
        "255,0,0,255,1,1);globalThis.pocSummary=completed===2&&bounded&&continued"
        "&&pixels[0]===255&&pixels[3]===255"
        "?'CANVAS-NATIVE-BATCH-WORK-BOUND-OK':"
        "'CANVAS-NATIVE-BATCH-WORK-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_native_batch_work_bound_probe,
              "<canvas-native-batch-work-bound-probe>", &result)
          && strcmp(result.summary,
                    "CANVAS-NATIVE-BATCH-WORK-BOUND-OK") == 0);

    static const char class_list_probe[] =
        "(()=>{const node=document.createElement('div');"
        "node.className='one two';const removed=!node.classList.toggle('two'),"
        "added=node.classList.toggle('three'),forced=node.classList.toggle('four',false);"
        "node.classList.add('five','six');node.classList.remove('one','five');"
        "const replaced=node.classList.replace('six','seven');"
        "globalThis.pocSummary=removed&&added&&!forced&&replaced"
        "&&node.classList.value==='three seven'&&node.classList.length===2"
        "&&node.classList.item(1)==='seven'"
        "?'CLASS-LIST-OK':'CLASS-LIST-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, class_list_probe, "<class-list-probe>", &result)
          && strcmp(result.summary, "CLASS-LIST-OK") == 0);

    static const char element_collection_probe[] =
        "(()=>{const host=document.createElement('section'),"
        "first=document.createElement('span'),"
        "second=document.createElement('span'),"
        "nested=document.createElement('em');"
        "host.innerHTML='<i></i>'.repeat(140);"
        "first.className='shared alpha';second.className='shared beta';"
        "nested.className='shared alpha';second.appendChild(nested);"
        "host.append(first,second);document.body.appendChild(host);"
        "const documentClass=document.getElementsByClassName('shared alpha'),"
        "elementClass=host.getElementsByClassName('shared'),"
        "elementTag=host.getElementsByTagName('span'),"
        "documentTag=document.getElementsByTagName('em');"
        "const ok=documentClass.length===2&&documentClass.item(0)===first"
        "&&documentClass.item(1)===nested&&elementClass.length===3"
        "&&elementTag.length===2&&documentTag.length===1"
        "&&document.getElementsByClassName('   ').length===0;host.remove();"
        "globalThis.pocSummary=ok?'ELEMENT-COLLECTION-OK':"
        "'ELEMENT-COLLECTION-FAILED';})()";
    bool element_collection_ok = script_runtime_evaluate_diagnostic(
        runtime, element_collection_probe,
        "<element-collection-probe>", &result);
    if (!element_collection_ok
        || strcmp(result.summary, "ELEMENT-COLLECTION-OK") != 0) {
        fprintf(stderr, "element collection probe: ok=%d summary=%s "
                "error=%s\n", element_collection_ok,
                result.summary, result.error);
    }
    CHECK(element_collection_ok
          && strcmp(result.summary, "ELEMENT-COLLECTION-OK") == 0);

    /* Exercise the logical request queue independently of libcurl timing.
       The fake native surface records launch order and returns inert IDs;
       completions are delivered through the same host callback used by the
       real scheduler.  This keeps the quota, FIFO, cancellation, and timeout
       assertions deterministic and makes teardown leaks visible to the
       budget check below. */
    static const char network_queue_probe[] =
        "(async()=>{const nativeFetch=globalThis.__tilefinchFetchAsync,"
        "nativeCancel=globalThis.__tilefinchCancelNetwork,starts=[],cancels=[];"
        "let nextNative=1000;globalThis.__tilefinchFetchAsync=(method,url)=>{"
        "starts.push(String(url));return nextNative++;};"
        "globalThis.__tilefinchCancelNetwork=id=>{cancels.push(Number(id));"
        "return true;};const raw={status:200,url:'https://example.test/ok',"
        "contentType:'text/plain',headers:'content-type: text/plain\\n',"
        "body:'ok'};const requests=[];for(let i=0;i<12;i++)requests.push("
        "fetch('https://example.test/fifo/'+i).then(value=>value.text()));"
        "const waitingAbort=new AbortController,waitingCancelled=fetch("
        "'https://example.test/cancel-waiting',{signal:waitingAbort.signal})"
        ".then(()=>'',error=>error.name);waitingAbort.abort();"
        "let delivered=1000;while(delivered<1012){const end=nextNative;"
        "while(delivered<end)__tilefinchDeliverNetwork(delivered++,true,raw);"
        "await Promise.resolve();}const bodies=await Promise.all(requests),"
        "cancelName=await waitingCancelled;const fifo=starts.length===12"
        "&&starts.every((url,index)=>url==='https://example.test/fifo/'+index);"
        "const abortControllers=[],abortPromises=[],abortStart=starts.length;"
        "for(let i=0;i<5;i++){const controller=new AbortController;"
        "abortControllers.push(controller);abortPromises.push(fetch("
        "'https://example.test/abort-fifo/'+i,{signal:controller.signal})"
        ".catch(()=>{}));}abortControllers[0].abort();await Promise.resolve();"
        "const abortReleased=starts[abortStart+4]"
        "==='https://example.test/abort-fifo/4';"
        "for(const controller of abortControllers)controller.abort();"
        "await Promise.all(abortPromises);"
        "const controllers=[],quotaPromises=[];for(let i=0;i<129;i++){"
        "const controller=new AbortController;controllers.push(controller);"
        "quotaPromises.push(fetch('https://example.test/quota/'+i,{"
        "signal:controller.signal}).then(()=>'',error=>error.name));}"
        "await Promise.resolve();const countQuota=await quotaPromises[128];"
        "let insideXhrSend=true,xhrQuotaError=false,xhrQuotaReentered=false;"
        "const quotaXhr=new XMLHttpRequest;quotaXhr.open('GET',"
        "'https://example.test/quota-xhr');quotaXhr.onerror=()=>{"
        "xhrQuotaError=true;if(insideXhrSend)xhrQuotaReentered=true;};"
        "quotaXhr.send();insideXhrSend=false;const xhrDeferred="
        "!xhrQuotaError&&!xhrQuotaReentered;__tilefinchPumpTimers(0,4);"
        "for(const controller of controllers)controller.abort();"
        "await Promise.all(quotaPromises);const byteQuota=await fetch("
        "'https://example.test/byte-quota',{method:'POST',"
        "body:'x'.repeat(132000)}).then(()=>'',error=>error.name);"
        "const blockers=[];for(let i=0;i<4;i++){const controller="
        "new AbortController;blockers.push(controller);fetch("
        "'https://example.test/timeout-blocker/'+i,{signal:controller.signal})"
        ".catch(()=>{});}let timeoutEvent=false;const xhr=new XMLHttpRequest;"
        "xhr.open('GET','https://example.test/queued-timeout');xhr.timeout=5;"
        "xhr.ontimeout=()=>{timeoutEvent=true;};xhr.send();"
        "const queuedBeforeTimeout=__tilefinchNetworkQueueStats.waiting===1;"
        "__tilefinchPumpTimers(5,16);for(const controller of blockers)"
        "controller.abort();await Promise.resolve();const uploadEvents=[],"
        "uploadXhr=new XMLHttpRequest,recordUpload=label=>event=>uploadEvents."
        "push([label,event.isTrusted,event instanceof ProgressEvent,"
        "event.lengthComputable,event.loaded,event.total].join(':'));"
        "for(const type of ['loadstart','progress','load','loadend'])"
        "uploadXhr.upload.addEventListener(type,recordUpload('u-'+type));"
        "uploadXhr.addEventListener('loadend',recordUpload('x-loadend'));"
        "uploadXhr.open('POST','https://example.test/upload');"
        "uploadXhr.send('abcde');const uploadNative=nextNative-1;"
        "__tilefinchDeliverNetwork(uploadNative,true,raw);await Promise.resolve();"
        "const progressShape=new ProgressEvent('shape',{lengthComputable:true,"
        "loaded:3,total:5}),progressShapeOk=Object.getOwnPropertyNames("
        "progressShape).join(',')==='isTrusted'&&Object.getOwnPropertyNames("
        "ProgressEvent.prototype).join(',')==='lengthComputable,loaded,total,constructor'"
        "&&progressShape.lengthComputable&&progressShape.loaded===3"
        "&&progressShape.total===5;"
        "const uploadOk=uploadEvents.join('|')==="
        "'u-loadstart:true:true:true:0:5|u-progress:true:true:true:5:5|'"
        "+'u-load:true:true:true:5:5|u-loadend:true:true:true:5:5|'"
        "+'x-loadend:true:true:true:2:2';const stats="
        "__tilefinchNetworkQueueStats;globalThis.__tilefinchFetchAsync=nativeFetch;"
        "globalThis.__tilefinchCancelNetwork=nativeCancel;"
        "globalThis.pocSummary=fifo&&bodies.every(value=>value==='ok')"
        "&&cancelName==='AbortError'&&countQuota==='RangeError'"
        "&&xhrDeferred&&xhrQuotaError&&!xhrQuotaReentered"
        "&&abortReleased&&byteQuota==='RangeError'&&queuedBeforeTimeout&&timeoutEvent"
        "&&uploadOk&&progressShapeOk"
        "&&xhr.readyState===4&&xhr.status===0&&stats.peakCount===128"
        "&&stats.rejected===3&&stats.rejectedBytes>=1"
        "&&stats.cancelled===139&&stats.timedOut===1"
        "&&stats.completed===13&&stats.active===0&&stats.waiting===0"
        "&&stats.currentCount===0&&cancels.length===13"
        "?'NETWORK-QUEUE-OK':'NETWORK-QUEUE-FAILED:'+JSON.stringify(stats);"
        "})().catch(error=>{globalThis.pocSummary='NETWORK-QUEUE-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool network_queue_ok = script_runtime_evaluate_diagnostic(
        runtime, network_queue_probe, "<network-queue-probe>", &result);
    for (size_t tick = 0; network_queue_ok && tick < 32
         && strncmp(result.summary, "NETWORK-QUEUE-", 14) != 0; tick++) {
        network_queue_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!network_queue_ok
        || strcmp(result.summary, "NETWORK-QUEUE-OK") != 0) {
        fprintf(stderr, "network queue probe: ok=%d summary=%s error=%s "
                "admitted=%zu completed=%zu rejected=%zu cancelled=%zu "
                "timedout=%zu peak=%zu peakbytes=%zu active=%zu pending=%zu\n",
                network_queue_ok, result.summary, result.error,
                result.async_network_logical_admitted,
                result.async_network_logical_completed,
                result.async_network_logical_rejected,
                result.async_network_logical_cancelled,
                result.async_network_logical_timed_out,
                result.async_network_logical_peak,
                result.async_network_logical_peak_bytes,
                result.async_network_active_native,
                result.async_network_pending_logical);
    }
    CHECK(network_queue_ok
          && strcmp(result.summary, "NETWORK-QUEUE-OK") == 0
          && result.async_network_logical_admitted == 152
          && result.async_network_logical_completed == 13
          && result.async_network_logical_rejected == 3
          && result.async_network_logical_cancelled == 139
          && result.async_network_logical_timed_out == 1
          && result.async_network_logical_peak == 128
          && result.async_network_logical_peak_bytes <= 256u * 1024u
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    static const char local_blob_network_probe[] =
        "(()=>{globalThis.pocSummary='LOCAL-BLOB-PENDING';const stats="
        "__tilefinchNetworkQueueStats,before=[stats.admitted,stats.launched,"
        "stats.currentCount,stats.localBlobReads,stats.localBlobFailures],"
        "checks=[],type='application/x-tilefinch-local';"
        "const fetchBlob=new Blob([new Uint8Array([0,65,255])],{type}),"
        "fetchURL=URL.createObjectURL(fetchBlob),fetchJob=fetch(fetchURL)"
        ".then(response=>{checks.push(response.status===200,response.url==="
        "fetchURL,response.headers.get('content-type')===type,response.body!=="
        "null);return response.arrayBuffer()}).then(buffer=>checks.push("
        "new Uint8Array(buffer).join(',')==='0,65,255'));"
        "URL.revokeObjectURL(fetchURL);const order=[],xhrBlob="
        "new Blob(['local-xhr'],{type:'text/plain'}),xhrURL="
        "URL.createObjectURL(xhrBlob),xhrJob=new Promise(resolve=>{const xhr="
        "new XMLHttpRequest;xhr.onreadystatechange=event=>{order.push(xhr.readyState);"
        "checks.push(event.isTrusted,event.constructor===Event,"
        "!(event instanceof ProgressEvent))};xhr.onload=event=>{order.push(5);"
        "checks.push(event.isTrusted,event instanceof ProgressEvent)};"
        "xhr.onloadend=event=>{order.push(6);checks.push(event.isTrusted,"
        "event instanceof ProgressEvent,event.lengthComputable,"
        "event.loaded===9,event.total===9);"
        "checks.push(xhr.status===200,xhr.responseURL===xhrURL,"
        "xhr.getResponseHeader('content-type')==='text/plain',"
        "new TextDecoder().decode(xhr.response)==='local-xhr',"
        "order.join(',')==='1,9,2,3,4,5,6');resolve()};xhr.open('GET',xhrURL);"
        "xhr.responseType='arraybuffer';xhr.send();order.push(9);"
        "URL.revokeObjectURL(xhrURL)});const stressBlob=new Blob(['local']),"
        "stressURL=URL.createObjectURL(stressBlob),stress=[];"
        "for(let i=0;i<8;i++)stress.push(new Promise(resolve=>{const xhr="
        "new XMLHttpRequest;xhr.onload=()=>resolve(xhr.responseText==='local');"
        "xhr.onerror=()=>resolve(false);xhr.open('GET',stressURL);xhr.send()}));"
        "URL.revokeObjectURL(stressURL);const revokedJob=fetch(stressURL)"
        ".then(()=>false,error=>error instanceof TypeError);Promise.all("
        "[fetchJob,xhrJob,revokedJob,...stress]).then(values=>{checks.push("
        "values[2]===true,values.slice(3).every(Boolean),"
        "stats.admitted===before[0],stats.launched===before[1],"
        "stats.currentCount===before[2],stats.localBlobReads-before[3]===10,"
        "stats.localBlobFailures-before[4]===1);globalThis.pocSummary="
        "checks.every(Boolean)?'LOCAL-BLOB-OK':'LOCAL-BLOB-FAILED:'+"
        "checks.join(',')+':order='+order.join(',')}).catch(error=>"
        "globalThis.pocSummary='LOCAL-BLOB-ERROR:'+String(error&&error.stack||"
        "error));})()";
    bool local_blob_network_ok = script_runtime_evaluate_diagnostic(
        runtime, local_blob_network_probe, "<local-blob-network-probe>",
        &result);
    for (size_t tick = 0; local_blob_network_ok && tick < 32
         && strncmp(result.summary, "LOCAL-BLOB-", 11) != 0; tick++) {
        local_blob_network_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!local_blob_network_ok
        || strcmp(result.summary, "LOCAL-BLOB-OK") != 0) {
        fprintf(stderr, "local blob network probe: ok=%d summary=%s error=%s\n",
                local_blob_network_ok, result.summary, result.error);
    }
    CHECK(local_blob_network_ok
          && strcmp(result.summary, "LOCAL-BLOB-OK") == 0
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    static const char large_base64_probe[] =
        "(()=>{const encoded='A'.repeat(823120),decoded=atob(encoded);"
        "globalThis.pocSummary=decoded.length===617340"
        "&&decoded.charCodeAt(0)===0"
        "&&decoded.charCodeAt(decoded.length-1)===0"
        "?'LARGE-BASE64-OK':'LARGE-BASE64-FAILED:'+decoded.length})()";
    bool large_base64_ok = script_runtime_evaluate_diagnostic(
        runtime, large_base64_probe, "<large-base64-probe>", &result);
    if (!large_base64_ok
        || strcmp(result.summary, "LARGE-BASE64-OK") != 0) {
        fprintf(stderr, "large base64 probe: ok=%d summary=%s error=%s\n",
                large_base64_ok, result.summary, result.error);
    }
    CHECK(large_base64_ok
          && strcmp(result.summary, "LARGE-BASE64-OK") == 0);

    static const char indexeddb_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);"
        "tx.addEventListener('abort',()=>reject(tx.error));});"
        "const opening=indexedDB.open('tilefinch-probe',2);let upgraded=false;"
        "opening.addEventListener('upgradeneeded',event=>{upgraded="
        "event.oldVersion===0&&event.newVersion===2;const store="
        "opening.result.createObjectStore('items',{keyPath:'id'});"
        "store.createIndex('tags','tags',{multiEntry:true});});"
        "const db=await request(opening),write=db.transaction('items',"
        "'readwrite'),store=write.objectStore('items'),writeDone="
        "finished(write);await Promise.all([request(store.put({id:2,"
        "name:'two',tags:['even','all']})),request(store.put({id:1,"
        "name:'one',tags:['odd','all']})),writeDone]);"
        "const read=db.transaction('items','readonly'),readDone="
        "finished(read),one=await request(read.objectStore('items').get(1)),"
        "all=await request(read.objectStore('items').getAll()),"
        "even=await request(read.objectStore('items').index('tags').getAll("
        "IDBKeyRange.only('even')));await readDone;const cursorTx="
        "db.transaction('items'),cursorDone=finished(cursorTx),cursorRequest="
        "cursorTx.objectStore('items').index('tags').openCursor(),seen=[];"
        "let cursor=await request(cursorRequest);while(cursor){seen.push("
        "cursor.key+':'+cursor.primaryKey);cursor.continue();cursor=await "
        "request(cursor.request);}await cursorDone;db.close();"
        "const reopened=await request(indexedDB.open('tilefinch-probe')),"
        "againTx=reopened.transaction('items'),againDone=finished(againTx),"
        "again=await request(againTx.objectStore('items').get(2));"
        "await againDone;reopened.close();await request(indexedDB.deleteDatabase("
        "'tilefinch-probe'));const telemetry=__tilefinchIndexedDBStats;"
        "globalThis.pocSummary=upgraded&&db instanceof IDBDatabase"
        "&&opening instanceof IDBOpenDBRequest&&store instanceof IDBObjectStore"
        "&&one.name==='one'&&all.length===2&&all[0].id===1&&all[1].id===2"
        "&&even.length===1&&even[0].id===2&&again.name==='two'"
        "&&seen.join(',')==='all:1,all:2,even:2,odd:1'"
        "&&telemetry.records===0&&telemetry.bytes===0&&telemetry.opens===2"
        "&&telemetry.deletes===1?'INDEXEDDB-OK':'INDEXEDDB-FAILED:'+"
        "JSON.stringify(telemetry);})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_probe, "<indexeddb-probe>", &result);
    for (size_t tick = 0; indexeddb_ok && tick < 16
         && strncmp(result.summary, "INDEXEDDB-", 10) != 0; tick++) {
        indexeddb_ok = script_runtime_advance(runtime, 0, 1024, &result);
    }
    if (!indexeddb_ok || strcmp(result.summary, "INDEXEDDB-OK") != 0) {
        fprintf(stderr, "indexeddb probe: ok=%d summary=%s error=%s\n",
                indexeddb_ok, result.summary, result.error);
    }
    CHECK(indexeddb_ok && strcmp(result.summary, "INDEXEDDB-OK") == 0
          && result.indexed_db_opens == 2
          && result.indexed_db_deletes == 1
          && result.indexed_db_transactions == 5
          && result.indexed_db_requests == 7
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0
          && result.indexed_db_peak_bytes > 0
          && result.indexed_db_quota_errors == 0
          /* The same realm loaded Game Audio, Canvas, and Streams earlier;
             IndexedDB is the fourth deferred standards module admitted. */
          && result.bootstrap_lazy_module_loads == 5
          && result.bootstrap_lazy_module_failures == 0);

    static const char indexeddb_failure_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-failure',beforeQuota="
        "__tilefinchIndexedDBStats.quotaErrors,opening=indexedDB.open(name,1);"
        "opening.addEventListener('upgradeneeded',()=>opening.result."
        "createObjectStore('records',{autoIncrement:true}));const parallel="
        "indexedDB.open(name,1),parallelPromise=request(parallel),db=await "
        "request(opening),parallelDb=await parallelPromise;parallelDb.close();"
        "const aborting=db.transaction('records','readwrite'),"
        "abortDone=finished(aborting).then(()=>'',error=>error.name),store="
        "aborting.objectStore('records'),first=request(store.add('temp')),"
        "duplicate=request(store.add('duplicate',1)).then(()=>'',error=>"
        "error.name),trailing=request(store.get(1)).then(()=>'',error=>"
        "error.name),abortResults=await Promise.all([first,duplicate,trailing,"
        "abortDone]);const write=db.transaction('records','readwrite'),"
        "writeDone=finished(write),restoredKey=await request(write.objectStore("
        "'records').add('kept'));await writeDone;const quota=db.transaction("
        "'records','readwrite'),quotaDone=finished(quota).then(()=>'',error=>"
        "error.name),quotaStore=quota.objectStore('records'),quotaRequest="
        "request(quotaStore.put(new ArrayBuffer(5*1024*1024),2)).then(()=>'',"
        "error=>error.name),quotaTailRequest=request(quotaStore.get(1)).then("
        "()=>'',error=>error.name),quotaResults=await Promise.all([quotaRequest,"
        "quotaTailRequest,quotaDone]),[quotaName,quotaTrailing,quotaDoneName]="
        "quotaResults;"
        "const verify=db.transaction('records'),verifyDone=finished(verify),"
        "kept=await request(verify.objectStore('records').get(1)),count=await "
        "request(verify.objectStore('records').count());await verifyDone;"
        "const originalIncludes=Array.prototype.includes;"
        "Array.prototype.includes=()=>false;const locked=db.transaction("
        "'records','readwrite'),lockedDone="
        "finished(locked),lockedValue=request(locked.objectStore('records')."
        "get(1)),queued=db.transaction('records','readwrite'),queuedDone="
        "finished(queued),queuedValue=request(queued.objectStore('records')."
        "get(1));let queuedSettled=false;queuedDone.then(()=>queuedSettled=true);"
        "const lockedResult=await lockedValue,queuedBeforeRelease="
        "!queuedSettled;await lockedDone;const queuedResult=await queuedValue;"
        "await queuedDone;Array.prototype.includes=originalIncludes;"
        "const timersBefore=__tilefinchPendingTimers(),timers=[];for(let i=0;"
        "i<160;i++){const id=setTimeout(()=>{},1000);if(!id)break;timers.push(id)}"
        "const saturated=db.transaction('records'),saturatedDone=finished("
        "saturated);await saturatedDone;for(const id of timers)clearTimeout(id);"
        "db.close();const bad=indexedDB.open(name,2);bad.addEventListener("
        "'upgradeneeded',()=>{bad.result.createObjectStore('partial');bad."
        "transaction.objectStore('records').createIndex('temporary','value');"
        "throw new Error('upgrade failure')});const badName=await request(bad)"
        ".then(()=>'',error=>error.name);let retryOld=-1,rolledBack=false;"
        "const retry=indexedDB.open(name,2);retry.addEventListener("
        "'upgradeneeded',event=>{retryOld=event.oldVersion;rolledBack="
        "!retry.result.objectStoreNames.contains('partial')&&retry.result."
        "objectStoreNames.contains('records')&&!retry.transaction.objectStore("
        "'records').indexNames.contains('temporary')});const reopened=await "
        "request(retry),check=reopened.transaction('records'),checkDone="
        "finished(check),persisted=await request(check.objectStore('records')."
        "get(1));await checkDone;reopened.close();await request(indexedDB."
        "deleteDatabase(name));const versioned=indexedDB.open(name,2);"
        "versioned.addEventListener('upgradeneeded',()=>versioned.result."
        "createObjectStore('parallel'));const current=indexedDB.open(name),"
        "versionedDb=await request(versioned),currentDb=await request(current),"
        "currentVersion=currentDb.version;versionedDb.close();currentDb.close();"
        "await request(indexedDB.deleteDatabase(name));const doomedOpen="
        "indexedDB.open('tilefinch-delete-live',1);doomedOpen.addEventListener("
        "'upgradeneeded',()=>doomedOpen.result.createObjectStore('items'));"
        "const doomedDb=await request(doomedOpen),doomedTx=doomedDb.transaction("
        "'items','readwrite'),doomedDone=finished(doomedTx).then(()=>'',error=>"
        "error.name);doomedTx.objectStore('items').put('temporary',1);"
        "const deleteLive=request(indexedDB.deleteDatabase("
        "'tilefinch-delete-live'));const doomedName=await doomedDone;"
        "await deleteLive;globalThis.pocSummary="
        "abortResults[0]===1"
        "&&abortResults[1]==='ConstraintError'&&abortResults[2]==='AbortError'"
        "&&abortResults[3]==='ConstraintError'&&restoredKey===1"
        "&&quotaName==='QuotaExceededError'&&quotaTrailing==='AbortError'"
        "&&quotaDoneName==='QuotaExceededError'&&kept==='kept'&&count===1"
        "&&lockedResult==='kept'&&queuedBeforeRelease&&queuedResult==='kept'"
        "&&timers.length+timersBefore===128"
        "&&badName==='AbortError'&&retryOld===1&&rolledBack"
        "&&persisted==='kept'&&__tilefinchIndexedDBStats.quotaErrors"
        "===beforeQuota+1&&currentVersion===2&&doomedName==='AbortError'"
        "&&__tilefinchIndexedDBStats.records===0"
        "&&__tilefinchIndexedDBStats.bytes===0"
        "?'INDEXEDDB-FAILURES-OK':'INDEXEDDB-FAILURES-FAILED:'"
        "+JSON.stringify({abortResults,restoredKey,quotaName,quotaTrailing,"
        "quotaDoneName,kept,count,lockedResult,queuedBeforeRelease,queuedResult,"
        "timers:timers.length,timersBefore,badName,"
        "retryOld,rolledBack,persisted,currentVersion,doomedName,"
        "stats:__tilefinchIndexedDBStats});})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-FAILURES-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool indexeddb_failures_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_failure_probe, "<indexeddb-failure-probe>",
        &result);
    for (size_t tick = 0; indexeddb_failures_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-FAILURES-", 19) != 0; tick++) {
        indexeddb_failures_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_failures_ok
        || strcmp(result.summary, "INDEXEDDB-FAILURES-OK") != 0) {
        fprintf(stderr, "indexeddb failure probe: ok=%d summary=%s error=%s\n",
                indexeddb_failures_ok, result.summary, result.error);
    }
    CHECK(indexeddb_failures_ok
          && strcmp(result.summary, "INDEXEDDB-FAILURES-OK") == 0
          && result.indexed_db_quota_errors == 1
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    /* Record identity for array keys is derived by serializing the key. If
       that serializer is the live JSON.stringify, a page can replace it and
       make the map lookup in delete() miss while the range scan still finds
       the record and still subtracts its bytes. Two range deletes then drive
       stats.records and stats.bytes negative, after which the quota check
       (delta > BYTE_LIMIT - stats.bytes) stops bounding anything. */
    static const char indexeddb_key_token_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-keytoken',"
        "opening=indexedDB.open(name,1);opening.addEventListener("
        "'upgradeneeded',()=>opening.result.createObjectStore('arr'));"
        "const db=await request(opening),write=db.transaction('arr',"
        "'readwrite'),writeDone=finished(write),store=write.objectStore('arr');"
        "await Promise.all([request(store.put({v:1},[1])),"
        "request(store.put({v:2},[2])),writeDone]);"
        "const seeded=__tilefinchIndexedDBStats.records,"
        "seededBytes=__tilefinchIndexedDBStats.bytes,"
        "nativeStringify=JSON.stringify;let remaining=-1,records=-1,bytes=-1;"
        "JSON.stringify=()=>'poisoned';"
        "try{for(let i=0;i<2;i++){const tx=db.transaction('arr','readwrite'),"
        "txDone=finished(tx);await Promise.all([request(tx.objectStore('arr')."
        "delete(IDBKeyRange.bound([0],[9]))),txDone]);}"
        "const read=db.transaction('arr'),readDone=finished(read);"
        "remaining=(await request(read.objectStore('arr').getAll())).length;"
        "await readDone;records=__tilefinchIndexedDBStats.records;"
        "bytes=__tilefinchIndexedDBStats.bytes;}"
        "finally{JSON.stringify=nativeStringify;}db.close();"
        "await request(indexedDB.deleteDatabase(name));"
        "globalThis.pocSummary=seeded===2&&seededBytes>0&&remaining===0"
        "&&records===0&&bytes===0?'INDEXEDDB-KEYTOKEN-OK':"
        "'INDEXEDDB-KEYTOKEN-FAILED:'+JSON.stringify({seeded,seededBytes,"
        "remaining,records,bytes});})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-KEYTOKEN-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool indexeddb_key_token_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_key_token_probe, "<indexeddb-key-token-probe>",
        &result);
    for (size_t tick = 0; indexeddb_key_token_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-KEYTOKEN-", 19) != 0; tick++) {
        indexeddb_key_token_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_key_token_ok
        || strcmp(result.summary, "INDEXEDDB-KEYTOKEN-OK") != 0) {
        fprintf(stderr,
                "indexeddb key-token probe: ok=%d summary=%s error=%s\n",
                indexeddb_key_token_ok, result.summary, result.error);
    }
    CHECK(indexeddb_key_token_ok
          && strcmp(result.summary, "INDEXEDDB-KEYTOKEN-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_unique_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-unique',"
        "opening=indexedDB.open(name,1);opening.addEventListener("
        "'upgradeneeded',()=>{const store=opening.result.createObjectStore("
        "'items',{keyPath:'id'});store.createIndex('email','email',{unique:true})});"
        "const db=await request(opening),first=db.transaction('items','readwrite'),"
        "firstDone=finished(first);await Promise.all([request(first.objectStore("
        "'items').add({id:1,email:'same@example.test'})),firstDone]);"
        "const duplicate=db.transaction('items','readwrite'),"
        "duplicateDone=finished(duplicate).then(()=>'',error=>error.name),"
        "duplicateRequest=request(duplicate.objectStore('items').add("
        "{id:2,email:'same@example.test'})).then(()=>'',error=>error.name),"
        "duplicateNames=await Promise.all([duplicateRequest,duplicateDone]);"
        "db.close();await request(indexedDB.deleteDatabase(name));"
        "const existing=indexedDB.open(name,1);existing.addEventListener("
        "'upgradeneeded',()=>existing.result.createObjectStore('items',"
        "{keyPath:'id'}));const existingDb=await request(existing),"
        "seed=existingDb.transaction('items','readwrite'),seedDone=finished(seed),"
        "seedStore=seed.objectStore('items');await Promise.all(["
        "request(seedStore.add({id:1,email:'same@example.test'})),"
        "request(seedStore.add({id:2,email:'same@example.test'})),seedDone]);"
        "existingDb.close();let createName='';const upgrade=indexedDB.open(name,2);"
        "upgrade.addEventListener('upgradeneeded',()=>{try{upgrade.transaction."
        "objectStore('items').createIndex('email','email',{unique:true})}"
        "catch(error){createName=error.name;upgrade.transaction.abort()}});"
        "const upgradeName=await request(upgrade).then(()=>'',error=>error.name);"
        "await request(indexedDB.deleteDatabase(name));"
        "globalThis.pocSummary=duplicateNames[0]==='ConstraintError'"
        "&&duplicateNames[1]==='ConstraintError'&&createName==='ConstraintError'"
        "&&upgradeName==='AbortError'?'INDEXEDDB-UNIQUE-OK':"
        "'INDEXEDDB-UNIQUE-FAILED:'+JSON.stringify({duplicateNames,createName,"
        "upgradeName});})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-UNIQUE-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_unique_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_unique_probe, "<indexeddb-unique-probe>", &result);
    for (size_t tick = 0; indexeddb_unique_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-UNIQUE-", 17) != 0; tick++) {
        indexeddb_unique_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_unique_ok
        || strcmp(result.summary, "INDEXEDDB-UNIQUE-OK") != 0) {
        fprintf(stderr, "indexeddb unique probe: ok=%d summary=%s error=%s\n",
                indexeddb_unique_ok, result.summary, result.error);
    }
    CHECK(indexeddb_unique_ok
          && strcmp(result.summary, "INDEXEDDB-UNIQUE-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    /* hardening.js runs once, over a snapshot of the globals that exist at
       that moment, so a __tilefinch global created lazily on a later write used
       to stay enumerable for the rest of the page. And the host entry points
       the event loop invokes by name every tick were left writable, which
       makes each of them a way for page script to take over the tick. */
    static const char entry_point_hardening_probe[] =
        "(()=>{const lazy=['__tilefinchSubmittedFormHandle',"
        "'__tilefinchSubmittedSubmitterHandle','__tilefinchFragmentInsertCount',"
        "'__tilefinchFragmentInsertText','__tilefinchLastFramePost',"
        "'__tilefinchBase64Error','__tilefinchSelectedControl',"
        "'__tilefinchParentAppendBypass','__tilefinchMutationSuppressed',"
        "'__tilefinchNow'],"
        "entries=['__tilefinchCommitSameDocument','__tilefinchDeliverNetwork',"
        "'__tilefinchRecordEventHandler','__tilefinchReportUncaught',"
        "'__tilefinchRunTask',"
        "'__tilefinchDispatchDOMContentLoaded',"
        "'__tilefinchIntersectionRecheck','__tilefinchParserMutationCheckpoint',"
        "'__tilefinchMediaRecheck',"
        "'__tilefinchPendingNetworkRequests','__tilefinchPendingTimers',"
        "'__tilefinchPumpTimers','__tilefinchRebindDocument',"
        "'__tilefinchRecordResourceTiming','__tilefinchRefreshNamedProperties',"
        "'__tilefinchRestoreSameDocument','__tilefinchRestoreSectionState',"
        "'__tilefinchSaveSectionState','__tilefinchSetFrameWindowState',"
        "'__tilefinchWrapRemote','__tilefinchWrapRemoteRelation',"
        "'__tilefinchWrapRemoteSelector','__tilefinchWrapRemoteStable'];"
        "for(const name of lazy)globalThis[name]=globalThis[name];"
        "const enumerable=lazy.filter(name=>Object.getOwnPropertyDescriptor("
        "globalThis,name)?.enumerable!==false),"
        "writable=entries.filter(name=>{const d="
        "Object.getOwnPropertyDescriptor(globalThis,name);"
        "return !d||typeof d.value!=='function'||d.writable!==false"
        "||d.configurable!==false;});"
        "const pumpBefore=__tilefinchPumpTimers,taskBefore=__tilefinchRunTask;"
        "try{globalThis.__tilefinchPumpTimers=()=>0;}catch(error){}"
        "try{globalThis.__tilefinchRunTask=(_,callback)=>callback();}catch(error){}"
        "const held=__tilefinchPumpTimers===pumpBefore"
        "&&__tilefinchRunTask===taskBefore;"
        "globalThis.pocSummary=enumerable.length===0&&writable.length===0"
        "&&held?'HARDENING-OK':'HARDENING-FAILED:'"
        "+enumerable.join(',')+'|'+writable.join(',')+'|'+held;})()";
    bool entry_hardening_ok = script_runtime_evaluate_diagnostic(
        runtime, entry_point_hardening_probe,
        "<entry-point-hardening-probe>", &result);
    if (!entry_hardening_ok
        || strcmp(result.summary, "HARDENING-OK") != 0) {
        fprintf(stderr, "entry hardening: ok=%d summary=%s error=%s\n",
                entry_hardening_ok, result.summary, result.error);
    }
    CHECK(entry_hardening_ok
          && strcmp(result.summary, "HARDENING-OK") == 0);

    static const char frame_window_probe[] =
        "(()=>{const frame=document.createElement('iframe'),"
        "child=frame.contentWindow;globalThis.pocSummary=child"
        "&&child.window===child&&child.self===child&&child.globalThis===child"
        "&&child.parent===globalThis&&child.String===String"
        "&&child.String.prototype===String.prototype"
        "&&typeof child.eval==='function'&&typeof child.postMessage==='function'"
        "&&child.eval('String===globalThis.String')"
        "?'FRAME-WINDOW-GLOBALS-OK':'FRAME-WINDOW-GLOBALS-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frame_window_probe, "<frame-window-probe>", &result)
          && strcmp(result.summary, "FRAME-WINDOW-GLOBALS-OK") == 0);
    size_t frame_windows_before = result.root_frame_windows;

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const frame=document.createElement('iframe');"
              "document.body.appendChild(frame);globalThis.crossFrame=frame;"
              "globalThis.crossWindow=frame.contentWindow;"
              "globalThis.pocSummary=String(frame.__handle)})()",
              "<cross-frame-setup>", &result));
    long cross_frame_handle = strtol(result.summary, NULL, 10);
    CHECK(cross_frame_handle > 0
          && script_runtime_set_frame_window_state(
              runtime, cross_frame_handle, true, true, false,
              "https://parent.test/child?entry=enabled", &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=crossWindow.location.search"
              "==='?entry=enabled'?'FRAME-LOCATION-OK':"
              "'FRAME-LOCATION-FAILED'",
              "<same-origin-frame-location>", &result)
          && strcmp(result.summary, "FRAME-LOCATION-OK") == 0
          && script_runtime_set_frame_window_state(
              runtime, cross_frame_handle, true, false, false, NULL, &result)
          && result.root_frame_windows == frame_windows_before + 1
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=crossFrame.contentWindow===crossWindow"
              "&&crossWindow.closed===false"
              "&&crossWindow.document===undefined"
              "&&crossWindow.location===undefined"
              "&&crossWindow.eval===undefined"
              "&&crossWindow.pocSummary===undefined"
              "&&('document' in crossWindow)===false"
              "&&('pocSummary' in crossWindow)===false"
              "&&crossWindow.window===crossWindow"
              "&&crossWindow.opener===null"
              "&&Reflect.set(crossWindow,'postMessage',()=>{})===false?"
              "'CROSS-WINDOW-OK':'CROSS-WINDOW-FAILED'",
              "<cross-frame-check>", &result)
          && strcmp(result.summary, "CROSS-WINDOW-OK") == 0
          && script_runtime_set_frame_window_state(
              runtime, cross_frame_handle, false, false, false, NULL, &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=crossWindow.closed?"
              "'CLOSED-WINDOW-OK':'CLOSED-WINDOW-FAILED';crossFrame.remove()",
              "<closed-frame-check>", &result)
          && strcmp(result.summary, "CLOSED-WINDOW-OK") == 0);

    static const char frame_retention_probe[] =
        "(()=>{const frames=[];for(let i=0;i<24;i++){const frame="
        "document.createElement('iframe');frames.push(frame);void "
        "frame.contentWindow}let source=null;const receive=event=>{"
        "source=event.source};addEventListener('message',receive,{once:true});"
        "postMessage({probe:true},'*');__tilefinchPumpTimers(0,4);"
        "globalThis.pocSummary=__tilefinchRootCensus.frameWindows<=16"
        "&&source===window?'FRAME-RETENTION-OK':"
        "'FRAME-RETENTION-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frame_retention_probe,
              "<frame-retention-probe>", &result)
          && strcmp(result.summary, "FRAME-RETENTION-OK") == 0);

    static const char callback_task_probe[] =
        "(()=>{const xhr=new XMLHttpRequest();"
        "xhr.addEventListener('readystatechange',()=>{"
        "throw new Error('task-probe')});xhr.open('GET','data:text/plain,ok');"
        "globalThis.pocSummary='CALLBACK-TASK-PROBE-OK';})()";
    size_t callback_errors_before = result.uncaught_callback_errors;
    bool callback_task_ok = script_runtime_evaluate_diagnostic(
        runtime, callback_task_probe, "<callback-task-probe>", &result);
    bool callback_task_valid = callback_task_ok
        && strcmp(result.summary, "CALLBACK-TASK-PROBE-OK") == 0
        && result.uncaught_callback_errors == callback_errors_before + 1
        && strstr(result.last_uncaught_callback_error, "task-probe") != NULL
        && strstr(result.last_uncaught_callback_error, "<browser-") == NULL
        && strstr(result.last_uncaught_callback_error, "call (native)") == NULL
        && strstr(result.last_uncaught_callback_task, "realm=top") != NULL
        && strstr(result.last_uncaught_callback_task,
                  "task=xhr:readystatechange:state=1#") != NULL;
    if (!callback_task_valid) {
        fprintf(stderr,
                "callback task probe: ok=%d summary=%s count=%zu error=%s "
                "task=%s roots=%zu\n", callback_task_ok, result.summary,
                result.uncaught_callback_errors,
                result.last_uncaught_callback_error,
                result.last_uncaught_callback_task,
                result.root_node_wrappers);
    }
    CHECK(callback_task_valid);

    static const char node_move_probe[] =
        "(()=>{const left=document.createElement('div'),"
        "right=document.createElement('div'),first=document.createElement('i'),"
        "moved=document.createElement('b'),last=document.createElement('u');"
        "left.append(first,moved,last);document.body.append(left,right);"
        "const appendReturn=right.appendChild(moved);"
        "right.appendChild(moved);"
        "const selfReturn=left.insertBefore(first,first);"
        "left.insertBefore(moved,last);"
        "let appendCycle=false,insertCycle=false;"
        "try{left.appendChild(left)}catch(_){appendCycle=true}"
        "const anchor=document.createElement('em');moved.appendChild(anchor);"
        "try{moved.insertBefore(left,anchor)}catch(_){insertCycle=true}"
        "globalThis.pocSummary=appendReturn===moved&&selfReturn===first"
        "&&left.childNodes.length===3&&left.firstChild===first"
        "&&first.nextSibling===moved&&moved.nextSibling===last"
        "&&right.childNodes.length===0&&appendCycle&&insertCycle"
        "?'NODE-MOVE-OK':'NODE-MOVE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, node_move_probe, "<node-move-probe>", &result)
          && strcmp(result.summary, "NODE-MOVE-OK") == 0);

    static const char rebound_matches_probe[] =
        "(()=>{const host=document.createElement('section'),"
        "child=document.createElement('span'),cached="
        "document.documentElement.matches;host.className='host';"
        "child.className='target';host.appendChild(child);"
        "globalThis.pocSummary=cached.call(child,'.target')"
        "&&!cached.call(child,'.host')&&cached.call(host,'.host')"
        "?'REBOUND-MATCHES-OK':'REBOUND-MATCHES-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, rebound_matches_probe, "<rebound-matches-probe>",
              &result)
          && strcmp(result.summary, "REBOUND-MATCHES-OK") == 0);

    static const char replace_child_probe[] =
        "(()=>{const parent=document.createElement('div'),"
        "old=document.createElement('i'),replacement=document.createElement('b');"
        "old.textContent='old';replacement.textContent='new';parent.appendChild(old);"
        "const returned=parent.replaceChild(replacement,old);"
        "const fragment=document.createDocumentFragment(),first="
        "document.createElement('u'),second=document.createElement('em');"
        "fragment.append(first,second);const fragmentReturn="
        "parent.replaceChild(fragment,replacement);let rejected=false;"
        "try{parent.replaceChild(document.createElement('q'),old)}catch(_){rejected=true}"
        "globalThis.pocSummary=returned===old&&fragmentReturn===replacement"
        "&&parent.childNodes.length===2&&parent.firstChild===first"
        "&&first.nextSibling===second&&fragment.childNodes.length===0&&rejected"
        "?'REPLACE-CHILD-OK':'REPLACE-CHILD-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, replace_child_probe, "<replace-child-probe>", &result)
          && strcmp(result.summary, "REPLACE-CHILD-OK") == 0);

    /* jQuery's buildFragment retains parsed child wrappers, clears their
       temporary container, then appends those detached nodes to a fragment.
       textContent replacement must preserve those referenced Node objects. */
    static const char fragment_builder_probe[] =
        "(()=>{const temporary=document.createElement('div'),"
        "destination=document.createElement('main');temporary.innerHTML="
        "'<a id=fragment-a>one</a><span>two</span>';const retained="
        "[...temporary.childNodes],firstHandle=retained[0].__handle;"
        "temporary.textContent='';const fragment=document.createDocumentFragment();"
        "for(const child of retained)fragment.appendChild(child);"
        "destination.appendChild(fragment);globalThis.pocSummary="
        "temporary.childNodes.length===0&&fragment.childNodes.length===0"
        "&&destination.childNodes.length===2&&retained[0].__handle==="
        "firstHandle&&retained[0].parentNode===destination"
        "&&destination.textContent==='onetwo'?'FRAGMENT-BUILDER-OK':"
        "'FRAGMENT-BUILDER-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, fragment_builder_probe, "<fragment-builder-probe>",
              &result)
          && strcmp(result.summary, "FRAGMENT-BUILDER-OK") == 0);

    /* Lexbor owns template.content through its host template even though the
       DocumentFragment is parentless.  Releasing the content wrapper must
       not independently destroy that fragment while the template survives. */
    static const char template_content_lifetime_probe[] =
        "(()=>{const template=document.createElement('template');"
        "template.innerHTML='<b>owned</b>';const content=template.content,"
        "handle=content.__handle,lease=content.__tilefinchHandleLease;"
        "const released=__tilefinchReleaseNodeWrapper(handle,lease);"
        "globalThis.__tilefinchOwnedTemplate=template;globalThis.pocSummary="
        "released===false&&template.content.querySelector('b').textContent"
        "==='owned'?'TEMPLATE-CONTENT-LIFETIME-OK':"
        "'TEMPLATE-CONTENT-LIFETIME-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, template_content_lifetime_probe,
              "<template-content-lifetime-probe>", &result)
          && strcmp(result.summary, "TEMPLATE-CONTENT-LIFETIME-OK") == 0);

    /* innerHTML has the same replace-all lifetime contract as textContent:
       removed children become detached, not invalid, while wrappers exist. */
    static const char retained_inner_html_probe[] =
        "(()=>{const host=document.createElement('section');host.innerHTML="
        "'<div><b id=retained-inner>old</b></div>';const retained="
        "host.querySelector('#retained-inner'),parent=retained.parentNode,"
        "handle=retained.__handle;host.innerHTML='<p>new</p>';"
        "const destination=document.createElement('aside');destination."
        "appendChild(parent);globalThis.pocSummary=retained.__handle===handle"
        "&&retained.parentNode===parent&&parent.parentNode===destination"
        "&&retained.textContent==='old'&&host.textContent==='new'"
        "?'RETAINED-INNER-HTML-OK':'RETAINED-INNER-HTML-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, retained_inner_html_probe,
              "<retained-inner-html-probe>", &result)
          && strcmp(result.summary, "RETAINED-INNER-HTML-OK") == 0);

    static const char shadow_content_and_event_probe[] =
        "(()=>{const host=document.createElement('div'),"
        "light=document.createElement('span');light.id='light-before';"
        "host.appendChild(light);document.body.appendChild(host);"
        "const root=host.attachShadow({mode:'open'}),"
        "slot=document.createElement('slot');root.appendChild(slot);"
        "let slotSeen=false,hostSeen=false,documentSeen=false;"
        "slot.addEventListener('tilefinch-noncomposed',()=>slotSeen=true);"
        "host.addEventListener('tilefinch-noncomposed',()=>hostSeen=true);"
        "document.addEventListener('tilefinch-noncomposed',"
        "()=>documentSeen=true,{once:true});"
        "light.dispatchEvent(new Event('tilefinch-noncomposed',"
        "{bubbles:true,composed:false}));"
        "host.innerHTML='<b id=light-after>new</b>';"
        "const htmlOk=host.shadowRoot===root&&root.firstChild===slot"
        "&&host.querySelector('#light-after')!==null"
        "&&host.innerHTML.includes('light-after')"
        "&&!host.innerHTML.includes('<slot');"
        "host.textContent='plain';const textOk=host.shadowRoot===root"
        "&&root.firstChild===slot&&host.textContent==='plain';"
        "const removed=host.firstChild;"
        "const returned=host.removeChild(removed);"
        "globalThis.pocSummary=slotSeen&&hostSeen&&documentSeen&&htmlOk"
        "&&textOk&&returned===removed?'SHADOW-CONTENT-EVENT-OK':"
        "'SHADOW-CONTENT-EVENT-FAILED:'+JSON.stringify({slotSeen,"
        "hostSeen,documentSeen,htmlOk,textOk,returned:returned===removed});"
        "host.remove()})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, shadow_content_and_event_probe,
              "<shadow-content-event-probe>", &result)
          && strcmp(result.summary, "SHADOW-CONTENT-EVENT-OK") == 0);

    static const char delegated_focus_probe[] =
        "(()=>{const host=document.createElement('div');"
        "document.body.appendChild(host);const root=host.attachShadow("
        "{mode:'open',delegatesFocus:true}),button=document.createElement("
        "'button');button.textContent='focus';root.appendChild(button);"
        "host.focus();const delegated=document.activeElement===host"
        "&&root.activeElement===button;"
        "let arrowRejected=false;try{customElements.define("
        "'x-tilefinch-arrow',()=>{})}catch(error){"
        "arrowRejected=error instanceof TypeError}"
        "globalThis.pocSummary=delegated&&arrowRejected"
        "?'DELEGATED-FOCUS-CONSTRUCTOR-OK':"
        "'DELEGATED-FOCUS-CONSTRUCTOR-FAILED:'+JSON.stringify({"
        "delegated,arrowRejected,documentActive:"
        "document.activeElement?.localName,shadowActive:"
        "root.activeElement?.localName});host.remove()})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, delegated_focus_probe,
              "<delegated-focus-constructor-probe>", &result)
          && strcmp(result.summary,
                    "DELEGATED-FOCUS-CONSTRUCTOR-OK") == 0);

    static const char custom_elements_probe[] =
        "(()=>{const events=[],existing=document.createElement('x-tilefinch-probe');"
        "existing.setAttribute('observed','before');document.body.appendChild(existing);"
        "const undefinedBefore=!existing.matches(':defined')&&document.body.matches(':defined');"
        "let resolved=false;customElements.whenDefined('x-tilefinch-probe').then("
        "constructor=>{resolved=constructor===Probe;globalThis.pocSummary="
        "undefinedBefore&&resolved&&existing instanceof Probe&&existing.matches(':defined')"
        "&&existing.constructed===1&&events.join(',')==='attribute:null:before,connected,attribute:before:after,connected,disconnected'"
        "&&created instanceof Probe&&created.matches(':defined')&&sheet.cssRules.length===1"
        "?'CUSTOM-ELEMENTS-OK':'CUSTOM-ELEMENTS-FAILED:'+JSON.stringify({undefinedBefore,resolved,events,existing:existing.constructed,defined:existing.matches(':defined'),existingState:__tilefinchGetCustomState(existing.__handle),createdState:__tilefinchGetCustomState(created.__handle),createdDefined:created.matches(':defined')});});"
        "class Probe extends HTMLElement{static observedAttributes=['observed'];"
        "constructor(){super();this.constructed=(this.constructed||0)+1;}"
        "connectedCallback(){events.push('connected')}disconnectedCallback(){events.push('disconnected')}"
        "attributeChangedCallback(name,oldValue,newValue){events.push('attribute:'+oldValue+':'+newValue)}}"
        "customElements.define('x-tilefinch-probe',Probe);existing.setAttribute('observed','after');"
        "const created=document.createElement('x-tilefinch-probe');document.body.appendChild(created);"
        "existing.remove();const sheet=new CSSStyleSheet();sheet.replaceSync('x{color:red}');})()";
    bool custom_elements_ok = script_runtime_evaluate_diagnostic(
        runtime, custom_elements_probe, "<custom-elements-probe>", &result);
    if (!custom_elements_ok
        || strcmp(result.summary, "CUSTOM-ELEMENTS-OK") != 0) {
        fprintf(stderr, "custom elements probe: ok=%d summary=%s error=%s\n",
                custom_elements_ok, result.summary, result.error);
    }
    CHECK(custom_elements_ok
          && strcmp(result.summary, "CUSTOM-ELEMENTS-OK") == 0);

    static const char customized_builtin_identity_probe[] =
        "(()=>{const wrong=document.createElement('div');"
        "wrong.setAttribute('is','x-tilefinch-button');"
        "const right=document.createElement('button');"
        "right.setAttribute('is','x-tilefinch-button');"
        "document.body.append(wrong,right);"
        "class TilefinchButton extends HTMLButtonElement{constructor(){"
        "super();this.upgraded=true}}"
        "customElements.define('x-tilefinch-button',TilefinchButton,"
        "{extends:'button'});customElements.upgrade(document.body);"
        "globalThis.pocSummary=!(wrong instanceof TilefinchButton)"
        "&&!wrong.upgraded&&right instanceof TilefinchButton&&right.upgraded"
        "?'CUSTOMIZED-BUILTIN-IDENTITY-OK':"
        "'CUSTOMIZED-BUILTIN-IDENTITY-FAILED:'+JSON.stringify({"
        "wrongInstance:wrong instanceof TilefinchButton,"
        "wrongUpgraded:wrong.upgraded,rightInstance:right instanceof "
        "TilefinchButton,rightUpgraded:right.upgraded,"
        "wrongState:wrong.__tilefinchCustomElementState,"
        "rightState:right.__tilefinchCustomElementState});"
        "wrong.remove();right.remove()})()";
    bool customized_builtin_ok = script_runtime_evaluate_diagnostic(
        runtime, customized_builtin_identity_probe,
        "<customized-builtin-identity-probe>", &result);
    if (!customized_builtin_ok
        || strcmp(result.summary, "CUSTOMIZED-BUILTIN-IDENTITY-OK") != 0) {
        fprintf(stderr, "customized builtin probe: ok=%d summary=%s error=%s\n",
                customized_builtin_ok, result.summary, result.error);
    }
    CHECK(customized_builtin_ok
          && strcmp(result.summary,
                    "CUSTOMIZED-BUILTIN-IDENTITY-OK") == 0);

    static const char constructed_stylesheet_probe[] =
        "(()=>{const first=new CSSStyleSheet(),second=new CSSStyleSheet();"
        "first.replaceSync('/* leading */ @import url(ignored.css);"
        ".constructed-a{color:red}.constructed-b{display:block}');"
        "const importRemoved=first.cssRules.length===2"
        "&&first.cssRules.every(rule=>rule.parentStyleSheet===first);"
        "const inserted=first.insertRule('.constructed-mid{width:3px}',1)===1"
        "&&first.cssRules[1].cssText.includes('constructed-mid');"
        "first.deleteRule(1);second.replaceSync('.constructed-last{height:4px}');"
        "document.adoptedStyleSheets=[first,second];"
        "const nodes=document.querySelectorAll("
        "'style[data-tilefinch-constructed]'),"
        "ordered=nodes.length===2&&nodes[0].parentNode===document.documentElement"
        "&&nodes[1]===document.documentElement.lastChild,"
        "before=nodes[0].textContent;"
        "first.replaceSync('.constructed-live{color:blue}');"
        "const live=nodes[0].textContent!==before"
        "&&nodes[0].textContent.includes('constructed-live');"
        "let duplicate=false,ordinary=false,syntax=false;"
        "try{document.adoptedStyleSheets=[first,first]}catch(error){"
        "duplicate=error.name==='NotAllowedError'}"
        "try{document.adoptedStyleSheets=[document.createElement('style').sheet]}"
        "catch(error){ordinary=error.name==='NotAllowedError'}"
        "try{first.replaceSync('.broken{')}catch(error){"
        "syntax=error.name==='SyntaxError'}"
        "document.adoptedStyleSheets=[second];"
        "const removed=!nodes[0].isConnected"
        "&&document.adoptedStyleSheets.length===1"
        "&&document.adoptedStyleSheets[0]===second;"
        "globalThis.pocSummary=importRemoved&&inserted&&ordered&&live"
        "&&duplicate&&ordinary&&syntax&&removed"
        "?'CONSTRUCTED-STYLESHEET-OK':'CONSTRUCTED-STYLESHEET-FAILED:'"
        "+JSON.stringify({importRemoved,inserted,ordered,live,duplicate,"
        "ordinary,syntax,removed,count:nodes.length});})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, constructed_stylesheet_probe,
              "<constructed-stylesheet-probe>", &result)
          && strcmp(result.summary, "CONSTRUCTED-STYLESHEET-OK") == 0);

    static const char observer_reobserve_and_select_probe[] =
        "(()=>{const target=document.createElement('div'),"
        "select=document.createElement('select'),"
        "option=document.createElement('option'),"
        "selected=document.createElement('selectedcontent');"
        "option.value='one';option.textContent='ONE';"
        "select.append(option,selected);document.body.append(target,select);"
        "select.selectedIndex=0;let resizeDeliveries=0,mutations=0;"
        "const resize=new ResizeObserver(()=>{resizeDeliveries++;"
        "globalThis.__tilefinchReobserveDeliveries=resizeDeliveries});"
        "resize.observe(target);resize.disconnect();resize.observe(target);"
        "const mutation=new MutationObserver(items=>mutations+=items.length);"
        "mutation.observe(selected,{childList:true,subtree:true});"
        "void select.value;void select.value;__tilefinchResizeRecheck();"
        "globalThis.__tilefinchReobserveCleanup=()=>{"
        "resize.disconnect();mutation.disconnect();target.remove();select.remove();"
        "return mutations};globalThis.pocSummary='OBSERVER-REOBSERVE-PENDING'})()";
    bool observer_reobserve_ok = script_runtime_evaluate_diagnostic(
        runtime, observer_reobserve_and_select_probe,
        "<observer-reobserve-select-probe>", &result)
        && script_runtime_advance(runtime, 16, 32, &result)
        && script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const resizeDeliveries="
            "globalThis.__tilefinchReobserveDeliveries||0,"
            "mutations=globalThis.__tilefinchReobserveCleanup();"
            "globalThis.pocSummary=resizeDeliveries===1&&mutations===0"
            "?'OBSERVER-REOBSERVE-SELECT-OK':"
            "'OBSERVER-REOBSERVE-SELECT-FAILED:'+JSON.stringify({"
            "resizeDeliveries,mutations})})()",
            "<observer-reobserve-select-result>", &result);
    if (!observer_reobserve_ok
        || strcmp(result.summary, "OBSERVER-REOBSERVE-SELECT-OK") != 0) {
        fprintf(stderr, "observer reobserve probe: ok=%d summary=%s error=%s\n",
                observer_reobserve_ok, result.summary, result.error);
    }
    CHECK(observer_reobserve_ok
          && strcmp(result.summary,
                    "OBSERVER-REOBSERVE-SELECT-OK") == 0);

    static const char event_source_pending_event_cap_probe[] =
        "(()=>{const source=new EventSource('https://events.test/feed');"
        "let errors=0;source.onerror=()=>errors++;"
        "for(let i=0;i<80&&!source._closed;i++)"
        "source._chunk('data:'+('x'.repeat(1024))+'\\n');"
        "const bounded=source._closed&&source.readyState===EventSource.CLOSED"
        "&&source._data.length===0&&source._dataBytes===0&&errors===1;"
        "source.close();globalThis.pocSummary=bounded"
        "?'EVENT-SOURCE-PENDING-CAP-OK':'EVENT-SOURCE-PENDING-CAP-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, event_source_pending_event_cap_probe,
              "<event-source-pending-cap-probe>", &result)
          && strcmp(result.summary,
                    "EVENT-SOURCE-PENDING-CAP-OK") == 0);

    static const char match_media_probe[] =
        "(()=>{const initial=innerWidth,"
        "range=matchMedia('(400px <= width <= 500px)'),"
        "minimum=matchMedia('(width >= 30em)'),"
        "orientation=matchMedia('screen and (orientation: landscape)'),"
        "notPrint=matchMedia('not print and (min-width: 100px)'),"
        "print=matchMedia('print and (min-width: 100px)'),"
        "monochrome=matchMedia('(monochrome)'),"
        "zeroMonochrome=matchMedia('(monochrome: 0)'),"
        "reducedMotion=matchMedia('(prefers-reduced-motion: reduce)'),"
        "ordinaryMotion=matchMedia('(prefers-reduced-motion: no-preference)');"
        "let events=0,eventShape=false;"
        "const listener={handleEvent(event){events++;eventShape="
        "event instanceof MediaQueryListEvent&&event instanceof Event"
        "&&event.media===range.media&&!event.matches}};"
        "range.addEventListener('change',listener);innerWidth=300;"
        "__tilefinchMediaRecheck();__tilefinchMediaRecheck();"
        "range.removeEventListener('change',listener);innerWidth=initial;"
        "__tilefinchMediaRecheck();"
        "globalThis.pocSummary=range instanceof EventTarget"
        "&&minimum.matches&&orientation.matches"
        "&&notPrint.matches&&!print.matches&&!monochrome.matches"
        "&&zeroMonochrome.matches&&reducedMotion.matches"
        "&&!ordinaryMotion.matches&&events===1&&eventShape"
        "?'MATCH-MEDIA-OK':'MATCH-MEDIA-FAILED:'+JSON.stringify({"
        "range:range.matches,minimum:minimum.matches,"
        "orientation:orientation.matches,notPrint:notPrint.matches,"
        "print:print.matches,monochrome:monochrome.matches,"
        "zeroMonochrome:zeroMonochrome.matches,"
        "reducedMotion:reducedMotion.matches,"
        "ordinaryMotion:ordinaryMotion.matches,events,eventShape});})()";
    bool match_media_ok = script_runtime_evaluate_diagnostic(
        runtime, match_media_probe, "<match-media-probe>", &result);
    if (!match_media_ok || strcmp(result.summary, "MATCH-MEDIA-OK") != 0) {
        fprintf(stderr, "match media probe: ok=%d summary=%s error=%s\n",
                match_media_ok, result.summary, result.error);
    }
    CHECK(match_media_ok && strcmp(result.summary, "MATCH-MEDIA-OK") == 0);

    /* The probes above intentionally retain globals, constructors, and
       detached fixtures. Start the independent wrapper-lifetime pressure
       suite in a fresh realm so its fixed heap and native budget measure
       handle recycling rather than unrelated conformance-test order. */
    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Detached nodes remain live while referenced, while unreferenced churn
       is reclaimed by wrapper finalization without aliasing old handles. */
    size_t handle_exhaustions_before = result.dom_handle_exhaustions;
    static const char dom_handle_reuse_probe[] =
        "(()=>{const host=document.createElement('div'),detached="
        "document.createElement('span'),detachedHandle=detached.__handle;"
        "document.body.appendChild(host);detached.textContent='detached';"
        "document.body.appendChild(detached);detached.remove();const stale="
        "document.createTextNode('stale');host.appendChild(stale);const "
        "staleHandle=stale.__handle;host.textContent='';let created=0;"
        "for(let i=0;i<1100;i++){const node=document.createTextNode('node-'+i);"
        "if(!node)break;host.appendChild(node);host.textContent='';created++;"
        "if((i&31)===31)__tilefinchClearNodeCache();}const replacement="
        "document.createTextNode('replacement'),replacementHandle="
        "replacement&&replacement.__handle;host.appendChild(replacement);"
        "stale.data='still-live';host.appendChild(stale);const staleSafe="
        "stale.__handle===staleHandle&&stale.data==='still-live'"
        "&&replacement.data==='replacement';"
        "detached.textContent="
        "'retained-detached';host.appendChild(detached);const detachedSafe="
        "detached.__handle===detachedHandle&&detached.textContent==="
        "'retained-detached'&&detached.parentNode?.__handle===host.__handle;"
        "globalThis.pocSummary=created===1100&&staleSafe&&detachedSafe"
        "?'DOM-HANDLE-REUSE-OK':'DOM-HANDLE-REUSE-FAILED:'"
        "+JSON.stringify({created,staleHandle,replacementHandle,staleSafe,"
        "detachedSafe,staleData:stale.data,replacementData:replacement.data,"
        "staleParent:stale.parentNode?.__handle===host.__handle});})()";
    bool dom_handle_reuse_ok = script_runtime_evaluate_diagnostic(
        runtime, dom_handle_reuse_probe, "<dom-handle-reuse-probe>",
        &result);
    if (!dom_handle_reuse_ok
        || strcmp(result.summary, "DOM-HANDLE-REUSE-OK") != 0) {
        fprintf(stderr, "DOM handle reuse probe: ok=%d summary=%s error=%s "
                "live=%zu peak=%zu high-water=%zu reuses=%zu "
                "exhaustions=%zu budget=%zu failures=%zu heap=%zu\n",
                dom_handle_reuse_ok, result.summary,
                result.error, result.dom_handle_slots_live,
                result.dom_handle_slots_peak,
                result.dom_handle_slots_high_water,
                result.dom_handle_slot_reuses,
                result.dom_handle_exhaustions, budget.current,
                budget.failure_count,
                script_runtime_heap_remaining(runtime));
    }
    CHECK(dom_handle_reuse_ok
          && strcmp(result.summary, "DOM-HANDLE-REUSE-OK") == 0
          && result.dom_handle_exhaustions == handle_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_peak
               <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_high_water
               <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    static const char connected_wrapper_setup[] =
        "(()=>{const node=document.createElement('section');"
        "node.setAttribute('data-tilefinch-wrapper-probe','connected');"
        "document.body.appendChild(node);globalThis.__tilefinchProbeHandle="
        "node.__handle;globalThis.__tilefinchProbeOldLease="
        "node.__tilefinchHandleLease;globalThis.pocSummary="
        "globalThis.__tilefinchWeakNodeCache&&node.__tilefinchHandleLease>0"
        "?'WEAK-NODE-CACHE-SETUP-OK':'WEAK-NODE-CACHE-SETUP-FAILED';"
        "__tilefinchClearNodeCache();})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, connected_wrapper_setup,
              "<connected-wrapper-setup>", &result)
          && strcmp(result.summary, "WEAK-NODE-CACHE-SETUP-OK") == 0);
    size_t connected_preserves_before =
        result.dom_handle_connected_preserves;
    bool connected_collected = collect_and_drain_finalizers(
        runtime, &result)
        && collect_and_drain_finalizers(runtime, &result);
    if (!connected_collected || result.dom_handle_connected_preserves
        <= connected_preserves_before) {
        fprintf(stderr, "connected wrapper collection: ok=%d live=%zu "
                "peak=%zu releases=%zu preserves=%zu stale=%zu\n",
                connected_collected, result.dom_handle_slots_live,
                result.dom_handle_slots_peak,
                result.dom_handle_wrapper_releases,
                result.dom_handle_connected_preserves,
                result.dom_handle_stale_releases);
    }
    CHECK(connected_collected
          && result.dom_handle_connected_preserves
               > connected_preserves_before);

    /* Reacquiring a connected native node after its wrapper is collected
       keeps the handle stable.  The old lease models a cleanup job that was
       already queued before reacquisition; it must not release the new
       wrapper's handle. */
    static const char connected_wrapper_reacquire[] =
        "(()=>{const node=document.querySelector("
        "'[data-tilefinch-wrapper-probe=connected]'),same=node&&node.__handle"
        "===globalThis.__tilefinchProbeHandle,newLease=node&&"
        "node.__tilefinchHandleLease,staleRejected=!__tilefinchReleaseNodeWrapper("
        "globalThis.__tilefinchProbeHandle,globalThis.__tilefinchProbeOldLease);"
        "if(node){node.textContent='reacquired';node.remove();}"
        "globalThis.__tilefinchRetainedDetached=node;globalThis.pocSummary="
        "same&&newLease>globalThis.__tilefinchProbeOldLease&&staleRejected"
        "&&node.textContent==='reacquired'?'CONNECTED-REACQUIRE-OK':"
        "'CONNECTED-REACQUIRE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, connected_wrapper_reacquire,
              "<connected-wrapper-reacquire>", &result)
          && strcmp(result.summary, "CONNECTED-REACQUIRE-OK") == 0);
    size_t wrapper_releases_before_retained =
        result.dom_handle_wrapper_releases;
    CHECK(collect_and_drain_finalizers(runtime, &result));
    /* Cache-cleared wrappers from the preceding churn may now retire here.
       Take the retained-node baseline after that intentional drain. */
    wrapper_releases_before_retained =
        result.dom_handle_wrapper_releases;
    static const char retained_detached_probe[] =
        "(()=>{const node=globalThis.__tilefinchRetainedDetached,handle="
        "node.__handle;node.textContent='retained-after-gc';"
        "document.body.appendChild(node);const usable=node.isConnected"
        "&&node.__handle===handle&&node.textContent==='retained-after-gc';"
        "node.remove();node.id='tilefinch-release-probe';node.removeAttribute('id');"
        "globalThis.__tilefinchDroppedHandle=node.__handle;"
        "globalThis.__tilefinchDroppedLease=node.__tilefinchHandleLease;"
        "globalThis.__tilefinchRetainedDetached=null;globalThis.pocSummary="
        "usable?'RETAINED-DETACHED-OK':'RETAINED-DETACHED-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, retained_detached_probe,
              "<retained-detached-probe>", &result)
          && strcmp(result.summary, "RETAINED-DETACHED-OK") == 0
          && result.dom_handle_wrapper_releases
               == wrapper_releases_before_retained);
    /* Run a separate checkpoint after dropping the final strong reference.
       FinalizationRegistry timing is deliberately unspecified, so require a
       healthy drain here and prove reclamation with the bounded multi-wave
       churn below instead of requiring this one wrapper to retire promptly. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='DROP-DRAIN'",
              "<retained-detached-drain>", &result)
          && strcmp(result.summary, "DROP-DRAIN") == 0);
    CHECK(collect_and_drain_finalizers(runtime, &result));

    size_t churn_releases_before = result.dom_handle_wrapper_releases;
    size_t churn_reuses_before = result.dom_handle_slot_reuses;
    size_t churn_exhaustions_before = result.dom_handle_exhaustions;
    static const char removed_wrapper_churn[] =
        "(()=>{let created=0;for(let i=0;i<600;i++){const node="
        "document.createElement('i');if(!node)break;"
        "document.body.appendChild(node);node.remove();created++;}"
        "globalThis.pocSummary=created===600?'REMOVED-WRAPPER-CHURN-OK':"
        "'REMOVED-WRAPPER-CHURN-FAILED:'+created;})()";
    for (size_t round = 0; round < 4; round++) {
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, removed_wrapper_churn,
            "<removed-wrapper-churn>", &result);
        bool summarized = evaluated
            && strcmp(result.summary, "REMOVED-WRAPPER-CHURN-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!evaluated || !summarized || !collected) {
            fprintf(stderr, "removed wrapper churn round=%zu evaluated=%d "
                    "summary=%s error=%s collected=%d live=%zu peak=%zu "
                    "releases=%zu reuses=%zu exhaustions=%zu\n",
                    round, evaluated, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_slots_peak,
                    result.dom_handle_wrapper_releases,
                    result.dom_handle_slot_reuses,
                    result.dom_handle_exhaustions);
        }
        CHECK(evaluated && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases
               >= churn_releases_before + 2400
          && result.dom_handle_slot_reuses >= churn_reuses_before + 1800
          && result.dom_handle_exhaustions == churn_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_peak
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    /* Start detached-subtree ownership in a fresh bounded realm so it
       measures its own transient rather than unrelated test-order debt. */
    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 12u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Descendants of a detached root still have native parent pointers, but
       none is connected to the document.  All three wrappers in every
       removed subtree must therefore become recyclable after collection. */
    size_t subtree_releases_before = result.dom_handle_wrapper_releases;
    size_t subtree_exhaustions_before = result.dom_handle_exhaustions;
    static const char detached_subtree_churn[] =
        "(()=>{let created=0;for(let i=0;i<100;i++){const root="
        "document.createElement('div'),child=document.createElement('span'),"
        "leaf=document.createTextNode('leaf');child.appendChild(leaf);"
        "root.appendChild(child);document.body.appendChild(root);root.remove();"
        "created+=3;}__tilefinchClearNodeCache();globalThis.pocSummary=created===300?"
        "'DETACHED-SUBTREE-CHURN-OK':'DETACHED-SUBTREE-CHURN-FAILED:'"
        "+created;})()";
    /* The preceding four 600-wrapper waves already prove cross-checkpoint
       recycling beyond the realm cap. This independent three-level wave
       validates detached-subtree ownership and bounded handle shape. */
    for (size_t round = 0; round < 1; round++) {
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, detached_subtree_churn,
            "<detached-subtree-churn>", &result);
        bool summarized = evaluated
            && strcmp(result.summary, "DETACHED-SUBTREE-CHURN-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!evaluated || !summarized || !collected) {
            fprintf(stderr, "detached subtree churn round=%zu evaluated=%d "
                    "summary=%s error=%s collected=%d live=%zu peak=%zu "
                    "budget=%zu failures=%zu heap-remaining=%zu\n",
                    round, evaluated, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_slots_peak, budget.current,
                    budget.failure_count,
                    script_runtime_heap_remaining(runtime));
        }
        CHECK(evaluated && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases >= subtree_releases_before
          && result.dom_handle_exhaustions == subtree_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 12u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Explicit wrapper retirement follows the same native invalidation path
       as the finalizers exercised by the preceding 2400-wrapper churn.
       Listener, handler, and form-state maps have fixed 128-entry bounds and
       must be purged as each detached identity retires. */
    static const char native_state_churn_first[] =
        "(()=>{const callback=globalThis.__tilefinchStateCallback||="
        "function(){};for(let i=0;i<96;i++){const node="
        "document.createElement('i');node.addEventListener('click',callback);"
        "node.onclick=callback;node.value='v'+i;document.body.appendChild(node);"
        "node.remove();__tilefinchReleaseNodeWrapper(node.__handle,"
        "node.__tilefinchHandleLease);}"
        "__tilefinchClearNodeCache();globalThis.pocSummary="
        "'NATIVE-STATE-FIRST-OK';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_state_churn_first,
              "<native-state-churn-first>", &result)
          && strcmp(result.summary, "NATIVE-STATE-FIRST-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));
    static const char native_state_churn_second[] =
        "(()=>{const before={l:__tilefinchRetentionStats.listenerDrops,"
        "h:__tilefinchRetentionStats.handlerDrops},callback="
        "globalThis.__tilefinchStateCallback;"
        "for(let i=0;i<96;i++){const node=document.createElement('i');"
        "node.addEventListener('click',callback);node.onclick=callback;"
        "node.value='v'+i;document.body.appendChild(node);node.remove();}"
        "globalThis.pocSummary=__tilefinchRetentionStats.listenerDrops===before.l"
        "&&__tilefinchRetentionStats.handlerDrops===before.h?"
        "'NATIVE-STATE-RETIREMENT-OK':'NATIVE-STATE-RETIREMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_state_churn_second,
              "<native-state-churn-second>", &result)
          && strcmp(result.summary, "NATIVE-STATE-RETIREMENT-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Explicitly retiring a detached child and then its root invalidates every
       registered handle in the subtree. Native invalidation must retire every
       child-keyed listener, handler, and form-state entry. A later batch keeps
       its detached wrappers alive through the evaluation and verifies those
       still-live states are counted without overflowing their bounded maps. */
    static const char native_subtree_churn_first[] =
        "(()=>{const callback=()=>{};for(let i=0;i<80;i++){const root="
        "document.createElement('div'),child=document.createElement('input');"
        "child.addEventListener('click',callback);child.onclick=callback;"
        "child.value='v'+i;root.appendChild(child);document.body.appendChild(root);"
        "root.remove();__tilefinchReleaseNodeWrapper(child.__handle,"
        "child.__tilefinchHandleLease);__tilefinchReleaseNodeWrapper(root.__handle,"
        "root.__tilefinchHandleLease);}__tilefinchClearNodeCache();globalThis.pocSummary="
        "'NATIVE-SUBTREE-FIRST-OK';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_subtree_churn_first,
              "<native-subtree-churn-first>", &result)
          && strcmp(result.summary, "NATIVE-SUBTREE-FIRST-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));
    static const char native_subtree_churn_second[] =
        "(()=>{const before={l:__tilefinchRetentionStats.listenerDrops,"
        "h:__tilefinchRetentionStats.handlerDrops,"
        "nl:__tilefinchRootCensus.nativeListenerTargets,"
        "nh:__tilefinchRootCensus.nativeHandlerTargets},callback=()=>{};"
        "globalThis.__tilefinchNativeSubtreeBefore=before;"
        "for(let i=0;i<64;i++){const root=document.createElement('div'),"
        "child=document.createElement('input');child.addEventListener("
        "'click',callback);child.onclick=callback;child.value='v'+i;"
        "root.appendChild(child);document.body.appendChild(root);root.remove();}"
        "__tilefinchClearNodeCache();"
        "globalThis.pocSummary=__tilefinchRetentionStats.listenerDrops===before.l"
        "&&__tilefinchRetentionStats.handlerDrops===before.h"
        "&&__tilefinchRootCensus.nativeListenerTargets===before.nl+64"
        "&&__tilefinchRootCensus.nativeHandlerTargets===before.nh+64?"
        "'NATIVE-SUBTREE-RETIREMENT-OK':"
        "'NATIVE-SUBTREE-RETIREMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_subtree_churn_second,
              "<native-subtree-churn-second>", &result)
          && strcmp(result.summary,
                    "NATIVE-SUBTREE-RETIREMENT-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Replacing an element with another element carrying the same id must
       create a distinct DOM identity. The old detached wrapper stays usable
       until the probe explicitly retires it after the identity assertion. */
    size_t replacement_releases_before = result.dom_handle_wrapper_releases;
    size_t replacement_exhaustions_before = result.dom_handle_exhaustions;
    static const char same_id_replacement_churn[] =
        "(()=>{const host=document.createElement('section');"
        "document.body.appendChild(host);let retained="
        "document.createElement('div');retained.setAttribute("
        "'id','tilefinch-stable-rebind');"
        "host.appendChild(retained);let replaced=0,ok=true;"
        "for(let i=0;i<150;i++){const old=retained,"
        "oldHandle=old.__handle;old.remove();"
        "host.innerHTML='<div id=\"tilefinch-stable-rebind\"></div>';"
        "const found=document.getElementById('tilefinch-stable-rebind');"
        "old.textContent='detached-'+i;"
        "if(!found||found===old||found.__handle===oldHandle||"
        "old.textContent!=='detached-'+i){ok=false;break;}"
        "__tilefinchReleaseNodeWrapper(old.__handle,old.__tilefinchHandleLease);"
        "retained=found;replaced++;}retained?.removeAttribute('id');"
        "retained?.remove();if(retained)__tilefinchReleaseNodeWrapper("
        "retained.__handle,retained.__tilefinchHandleLease);host.remove();"
        "__tilefinchReleaseNodeWrapper(host.__handle,host.__tilefinchHandleLease);"
        "globalThis.pocSummary="
        "ok&&replaced===150?'SAME-ID-REPLACEMENT-OK':"
        "'SAME-ID-REPLACEMENT-FAILED:'+JSON.stringify({ok,replaced});})()";
    for (size_t round = 0; round < 1; round++) {
        bool replacement_ok = script_runtime_evaluate_diagnostic(
            runtime, same_id_replacement_churn,
            "<same-id-replacement-churn>", &result);
        bool summarized = replacement_ok
            && strcmp(result.summary, "SAME-ID-REPLACEMENT-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!replacement_ok || !summarized || !collected) {
            fprintf(stderr, "same-id replacement round=%zu ok=%d "
                    "summary=%s error=%s collected=%d live=%zu "
                    "releases=%zu exhaustions=%zu\n",
                    round, replacement_ok, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_wrapper_releases,
                    result.dom_handle_exhaustions);
        }
        CHECK(replacement_ok && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases
               >= replacement_releases_before + 150
          && result.dom_handle_exhaustions == replacement_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    /* Page capability diagnostics may wrap classic scripts in a `with`
       environment, but modules are intrinsically strict and must be compiled
       unchanged.  This caught every diagnostic-enabled module failing before
       its first statement on modern application shells. */
    static const char page_trace_setup[] =
        "globalThis.__tilefinchPageTrace={seen:new Map(),missing:new Set(),"
        "events:[]};globalThis.__tilefinchGlobalProxy=new Proxy(globalThis,{"
        "has(target,key){return Reflect.has(target,key)},"
        "get(target,key,receiver){return Reflect.get(target,key,receiver)}});";
    static const char traced_module[] =
        "globalThis.pocSummary='TRACED-MODULE-EXECUTED';"
        "export const ready=true;";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, page_trace_setup, "<page-trace-setup>", &result)
          && script_runtime_evaluate_external_typed(
              runtime, script, traced_module, sizeof(traced_module) - 1,
              "https://example.test/traced-module.js", true, &result)
          && result.success
          && strcmp(result.summary, "TRACED-MODULE-EXECUTED") == 0
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_MODULE);

    puts("test: JavaScript browser identity matches native client hints");
    static const char browser_identity_probe[] =
        "const d=navigator.userAgentData,b=d.brands[0];"
        "d.getHighEntropyValues(['uaFullVersion','fullVersionList'])"
        ".then(v=>globalThis.pocSummary="
        "b.brand+'|'+b.version+'|'+v.uaFullVersion+'|'"
        "+v.fullVersionList[0].brand+'|'"
        "+v.fullVersionList[0].version);";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, browser_identity_probe, "<browser-identity-probe>",
              &result)
          && script_runtime_advance(runtime, 0, 16, &result)
          && strcmp(result.summary,
                    TILEFINCH_BROWSER_BRAND "|"
                    TILEFINCH_BROWSER_BRAND_VERSION "|"
                    TILEFINCH_BROWSER_FULL_VERSION "|"
                    TILEFINCH_BROWSER_BRAND "|"
                    TILEFINCH_BROWSER_FULL_VERSION) == 0);

    puts("test: detached runtimes reject new network work");
    script_runtime_detach_document(runtime, &document);
    static const char detached_fetch_probe[] =
        "fetch('/after-detach').then(()=>{"
        "globalThis.pocSummary='DETACHED-FETCH-RESOLVED'"
        "}).catch(error=>{globalThis.pocSummary=error instanceof TypeError"
        "?'DETACHED-FETCH-REJECTED':'DETACHED-FETCH-WRONG:'+error});";
    bool detached_fetch_ok = script_runtime_evaluate_diagnostic(
        runtime, detached_fetch_probe, "<detached-fetch-probe>", &result);
    for (size_t tick = 0; detached_fetch_ok && tick < 8
         && strcmp(result.summary, "DETACHED-FETCH-REJECTED") != 0; tick++) {
        detached_fetch_ok = script_runtime_advance(runtime, 0, 64, &result);
    }
    CHECK(detached_fetch_ok
          && strcmp(result.summary, "DETACHED-FETCH-REJECTED") == 0
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    script_runtime_destroy(runtime);
    runtime = NULL;

    puts("test: classic external bytecode reuses the bounded HTTP cache");
    BrowserSession bytecode_session = {0};
    ScriptRuntimeOptions bytecode_options = options;
    bytecode_options.session = &bytecode_session;
    static const char cached_script_source[] =
        "globalThis.pocSummary='CLASSIC-BYTECODE-CACHE-OK'";
    static const char cached_script_url[] =
        "https://example.test/shared-classic.js";
    size_t bytecode_baseline = budget.current;
    CHECK(browser_session_init(
              &bytecode_session, &budget, 512u * 1024u)
          && browser_session_cache_put_http(
              &bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, "cache-v1", NULL,
              "text/javascript", "public,max-age=3600", NULL, 1));
    ScriptResult first_bytecode_result = {0};
    ScriptRuntime *first_bytecode_runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &bytecode_options,
        &first_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(first_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              first_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, &first_bytecode_result)
          && strcmp(first_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && first_bytecode_result.external_script_bytecode_cache_misses == 1
          && first_bytecode_result.external_script_bytecode_cache_stores == 1
          && first_bytecode_result.external_script_bytecode_cache_hits == 0);
    size_t first_compile_attempts =
        first_bytecode_result.host_compile_attempts;
    script_runtime_destroy(first_bytecode_runtime);

    ScriptResult second_bytecode_result = {0};
    ScriptRuntime *second_bytecode_runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &bytecode_options,
        &second_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(second_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              second_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, &second_bytecode_result)
          && strcmp(second_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && second_bytecode_result.external_script_bytecode_cache_hits == 1
          && second_bytecode_result.external_script_bytecode_cache_misses == 0
          && second_bytecode_result.external_script_bytecode_cache_bytes != 0
          && second_bytecode_result.host_compile_attempts + 1
                 == first_compile_attempts);
    script_runtime_destroy(second_bytecode_runtime);
    browser_session_destroy(&bytecode_session);
    CHECK(budget.current == bytecode_baseline);

    puts("test: install-time classic bytecode restores without compilation");
    BrowserSession installed_bytecode_session = {0};
    CHECK(browser_session_init(
              &installed_bytecode_session, &budget, 512u * 1024u)
          && browser_session_cache_put_http(
              &installed_bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, "cache-v1", NULL,
              "text/javascript", "public,max-age=3600", NULL, 1));
    unsigned char *installed_bytecode = NULL;
    size_t installed_bytecode_length = 0;
    CHECK(script_compile_classic_bytecode(
              &budget, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              128u * 1024u, &installed_bytecode,
              &installed_bytecode_length)
          && installed_bytecode != NULL && installed_bytecode_length != 0
          && browser_session_classic_script_bytecode_put(
              &installed_bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, installed_bytecode,
              installed_bytecode_length));
    budget_free(&budget, installed_bytecode);
    ScriptRuntimeOptions installed_bytecode_options = options;
    installed_bytecode_options.session = &installed_bytecode_session;
    ScriptResult installed_bytecode_result = {0};
    ScriptRuntime *installed_bytecode_runtime =
        script_runtime_create_configured(
            &document, &budget, 16u * MIB, 8000,
            "https://example.test/", &installed_bytecode_options,
            &installed_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(installed_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              installed_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, &installed_bytecode_result)
          && strcmp(installed_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && installed_bytecode_result.external_script_bytecode_cache_hits == 1
          && installed_bytecode_result.external_script_bytecode_cache_misses
                 == 0);
    script_runtime_destroy(installed_bytecode_runtime);
    browser_session_destroy(&installed_bytecode_session);
    CHECK(budget.current == bytecode_baseline);

    size_t module_budget_baseline = budget.current;

    /* Module-map entries intentionally live for their realm's lifetime.
       Exercise the two-root delayed-import scenario in its own bounded realm
       and prove that destroying it returns every native allocation. */
    ScriptResult module_result;
    ScriptRuntime *module_runtime = script_runtime_create_configured(
        &document, &budget, 4u * MIB, 1000,
        "https://example.test/", &options, &module_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(module_runtime != NULL && module_result.success
          && script != NULL
          && test_delayed_module_request_contexts(
              module_runtime, script, &module_result));
    script_runtime_destroy(module_runtime);
    CHECK(budget.current == module_budget_baseline);

    /* FinalizationRegistry cleanup is a native QuickJS job: an exception from
       its callback is returned directly by JS_ExecutePendingJob. This tests
       the host's fatal/nonfatal boundary without a private hook or changing
       the browser's queueMicrotask implementation. */
    /* Parser interrupt polls make a blown creation deadline observable
       during bootstrap compiles, so the watchdog budget must genuinely
       cover runtime creation on unoptimized builds. */
    ScriptResult job_result;
    ScriptRuntime *job_runtime = script_runtime_create_configured(
        &document, &budget, 8u * MIB, 1000,
        "https://job-exception.test/", &options, &job_result);
    CHECK(job_runtime != NULL && job_result.success);
    static const char install_authored_finalizer[] =
        "globalThis.pocSummary='AUTHORED-FINALIZER-PENDING';"
        "globalThis.__authoredFinalizer=new FinalizationRegistry(()=>{"
        "Promise.resolve().then(()=>{globalThis.pocSummary="
        "'AUTHORED-FINALIZER-CONTINUED'});"
        "throw new Error('author says out of memory')});"
        "globalThis.__authoredFinalizerTarget={};"
        "__authoredFinalizerTarget.self=__authoredFinalizerTarget;"
        "__authoredFinalizer.register(__authoredFinalizerTarget,'held');";
    CHECK(script_runtime_evaluate_diagnostic(
              job_runtime, install_authored_finalizer,
              "<install-authored-finalizer>", &job_result)
          && script_runtime_evaluate_diagnostic(
              job_runtime, "globalThis.__authoredFinalizerTarget=null",
              "<release-authored-finalizer>", &job_result));
    size_t authored_failures_before = budget.failure_count;
    size_t authored_errors_before = job_result.uncaught_callback_errors;
    (void) script_runtime_collect_and_trim(job_runtime);
    CHECK(script_runtime_advance(job_runtime, 0, 8, &job_result)
          && job_result.success
          && strcmp(job_result.summary,
                    "AUTHORED-FINALIZER-CONTINUED") == 0
          && job_result.uncaught_callback_errors
                 == authored_errors_before + 1
          && strstr(job_result.last_uncaught_callback_error,
                    "author says out of memory") != NULL
          && budget.failure_count == authored_failures_before);

    /* A completed timeout is retained as diagnostic history until the next
       public result update. It must not make an unrelated job in a newly
       armed watchdog slice fatal. Keep this target cyclic so collection, and
       therefore its native cleanup job, happens only after the timeout. */
    static const char install_stale_finalizer[] =
        "globalThis.pocSummary='STALE-FINALIZER-PENDING';"
        "globalThis.__staleFinalizer=new FinalizationRegistry(()=>{"
        "Promise.resolve().then(()=>{globalThis.pocSummary="
        "'STALE-FINALIZER-CONTINUED'});"
        "throw new Error('ordinary author finalizer')});"
        "globalThis.__staleFinalizerTarget={};"
        "__staleFinalizerTarget.self=__staleFinalizerTarget;"
        "__staleFinalizer.register(__staleFinalizerTarget,'held');";
    CHECK(script_runtime_evaluate_diagnostic(
        job_runtime, install_stale_finalizer,
        "<install-stale-finalizer>", &job_result));
    CHECK(!script_runtime_evaluate_diagnostic(
              job_runtime, "for(;;){}", "<intentional-timeout>",
              &job_result)
          && job_result.interrupted);
    CHECK(script_runtime_evaluate_diagnostic(
              job_runtime, "globalThis.__staleFinalizerTarget=null",
              "<release-stale-finalizer>", &job_result)
          && job_result.interrupted);
    size_t stale_failures_before = budget.failure_count;
    size_t stale_errors_before = job_result.uncaught_callback_errors;
    (void) script_runtime_collect_and_trim(job_runtime);
    CHECK(script_runtime_advance(job_runtime, 0, 8, &job_result)
          && job_result.success && !job_result.interrupted
          && strcmp(job_result.summary,
                    "STALE-FINALIZER-CONTINUED") == 0
          && job_result.uncaught_callback_errors == stale_errors_before + 1
          && strstr(job_result.last_uncaught_callback_error,
                    "ordinary author finalizer") != NULL
          && budget.failure_count == stale_failures_before);
    script_runtime_destroy(job_runtime);

    document_destroy(&document);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);

    /* Watchdog cadence, including the parser interrupt polls, must remain
       page-invisible: identical seeds and URLs produce identical replay
       entropy and clock observations regardless of the watchdog budget. */
    puts("test: parser interrupt polls stay replay-invisible");
    script_runtime_configure_deterministic_replay(true, 77);
    static const char replay_html[] =
        "<!doctype html><html><body></body></html>";
    static const char replay_probe[] =
        "globalThis.pocSummary=[Math.random(),Math.random(),Date.now(),"
        "performance.now()].join(',')";
    char replay_first[512] = {0};
    char replay_second[512] = {0};
    for (int round = 0; round < 2; round++) {
        PocDocument replay_document;
        CHECK(document_parse(&replay_document, &budget, replay_html,
                             sizeof(replay_html) - 1, 17));
        ScriptRuntimeOptions replay_options = {
            .viewport = viewport,
            .execution_policy = lab,
            .defer_document_scripts = true
        };
        ScriptResult replay_result;
        ScriptRuntime *replay_runtime = script_runtime_create_configured(
            &replay_document, &budget, 4u * MIB,
            round == 0 ? 1000 : 4000,
            "https://replay.test/", &replay_options, &replay_result);
        CHECK(replay_runtime != NULL
              && script_runtime_evaluate_diagnostic(
                     replay_runtime, replay_probe, "<replay-probe>",
                     &replay_result)
              && replay_result.success);
        snprintf(round == 0 ? replay_first : replay_second,
                 sizeof(replay_first), "%s", replay_result.summary);
        script_runtime_destroy(replay_runtime);
        document_destroy(&replay_document);
        CHECK(budget.current == 0);
    }
    script_runtime_configure_deterministic_replay(false, 0);
    CHECK(replay_first[0] != '\0'
          && strcmp(replay_first, replay_second) == 0);

    Budget navigation_budget;
    budget_init(&navigation_budget, 2u * MIB);
    NavigationSession navigation;
    CHECK(navigation_init(&navigation, &navigation_budget, 2)
          && navigation_set_script_execution_policy(&navigation, &realistic)
          && navigation.script_execution_policy
                 .maximum_host_compile_source_bytes
               == realistic.maximum_host_compile_source_bytes);
    navigation_destroy(&navigation);
    CHECK(navigation_budget.current == 0
          && budget_active_allocations(&navigation_budget, NULL) == 0);

    puts("tilefinch-js-responsiveness-tests: all checks passed");
    return 0;
}
