#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/sha256.h"
#include "tilefinch/viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/node.h>

#include "tilefinch/platform.h"

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
          && strict.maximum_host_compile_source_bytes == 256u * 1024u
          && strict.maximum_host_compile_projected_us == 0
          && strict.modeled_compile_bytes_per_ms == 0);
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &realistic)
          && realistic.maximum_host_compile_source_bytes
               == strict.maximum_host_compile_source_bytes);
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
        "context.fillStyle='hsl(120 100% 25%)';const hsl=context.fillStyle;"
        "canvas.setAttribute('width','2');const reset="
        "context.fillStyle==='#000000'&&context.globalAlpha===1"
        "&&context.getImageData(0,0,1,1).data.every(value=>value===0);"
        "canvas.width=257;canvas.height=257;context.fillStyle='#123456';"
        "context.fillRect(0,0,1,1);const bounded="
        "context.getImageData(0,0,1,1).data.every(value=>value===0);"
        "let quota=false,index=false;try{context.createImageData(257,257)}"
        "catch(error){quota=error instanceof DOMException"
        "&&error.name==='QuotaExceededError'}"
        "try{context.getImageData(0,0,0,1)}catch(error){"
        "index=error instanceof DOMException&&error.name==='IndexSizeError'}"
        "const ok=defaults&&context instanceof CanvasRenderingContext2D"
        "&&same&&unsupported&&invalidRetained&&named==='#663399'"
        "&&alphaRetained&&restored&&pixel[0]===128&&pixel[1]===0"
        "&&pixel[2]===128&&pixel[3]===255"
        "&&clear.every(value=>value===0)&&put[0]===1&&put[1]===2"
        "&&put[2]===3&&put[3]===4&&hsl==='#008000'&&reset&&bounded"
        "&&quota&&index&&supplied.width===1&&supplied.height===1"
        "&&supplied.colorSpace==='srgb';globalThis.pocSummary=ok?"
        "'CANVAS-2D-OK':'CANVAS-2D-FAILED:'+JSON.stringify({defaults,same,"
        "unsupported,invalidRetained,named,alphaRetained,restored,"
        "pixel:[...pixel],clear:[...clear],put:[...put],hsl,reset,bounded,"
        "quota,index});})()";
    bool canvas_2d_ok = script_runtime_evaluate_diagnostic(
        runtime, canvas_2d_probe, "<canvas-2d-probe>", &result);
    if (!canvas_2d_ok || strcmp(result.summary, "CANVAS-2D-OK") != 0) {
        fprintf(stderr, "canvas 2d probe: ok=%d summary=%s error=%s\n",
                canvas_2d_ok, result.summary, result.error);
    }
    CHECK(canvas_2d_ok && strcmp(result.summary, "CANVAS-2D-OK") == 0);

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
        "controller.abort();await Promise.resolve();const stats="
        "__tilefinchNetworkQueueStats;globalThis.__tilefinchFetchAsync=nativeFetch;"
        "globalThis.__tilefinchCancelNetwork=nativeCancel;"
        "globalThis.pocSummary=fifo&&bodies.every(value=>value==='ok')"
        "&&cancelName==='AbortError'&&countQuota==='RangeError'"
        "&&abortReleased&&byteQuota==='RangeError'&&queuedBeforeTimeout&&timeoutEvent"
        "&&xhr.readyState===4&&xhr.status===0&&stats.peakCount===128"
        "&&stats.rejected===2&&stats.rejectedBytes>=1"
        "&&stats.cancelled===139&&stats.timedOut===1"
        "&&stats.completed===12&&stats.active===0&&stats.waiting===0"
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
          && result.async_network_logical_admitted == 151
          && result.async_network_logical_completed == 12
          && result.async_network_logical_rejected == 2
          && result.async_network_logical_cancelled == 139
          && result.async_network_logical_timed_out == 1
          && result.async_network_logical_peak == 128
          && result.async_network_logical_peak_bytes <= 256u * 1024u
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

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
          /* The same realm loaded Canvas and Streams earlier; IndexedDB is
             the third deferred standards module admitted on demand. */
          && result.bootstrap_lazy_module_loads == 3
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
        "const timers=[];for(let i=0;"
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
        "&&timers.length===128"
        "&&badName==='AbortError'&&retryOld===1&&rolledBack"
        "&&persisted==='kept'&&__tilefinchIndexedDBStats.quotaErrors"
        "===beforeQuota+1&&currentVersion===2&&doomedName==='AbortError'"
        "&&__tilefinchIndexedDBStats.records===0"
        "&&__tilefinchIndexedDBStats.bytes===0"
        "?'INDEXEDDB-FAILURES-OK':'INDEXEDDB-FAILURES-FAILED:'"
        "+JSON.stringify({abortResults,restoredKey,quotaName,quotaTrailing,"
        "quotaDoneName,kept,count,lockedResult,queuedBeforeRelease,queuedResult,"
        "timers:timers.length,badName,"
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
        "const pumpBefore=__tilefinchPumpTimers;"
        "try{globalThis.__tilefinchPumpTimers=()=>0;}catch(error){}"
        "const held=__tilefinchPumpTimers===pumpBefore;"
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
        "(()=>{const xhr=new XMLHttpRequest();xhr.readyState=3;"
        "xhr.addEventListener('readystatechange',()=>{"
        "throw new Error('task-probe')});xhr.emit('readystatechange');"
        "globalThis.pocSummary='CALLBACK-TASK-PROBE-OK';})()";
    size_t callback_errors_before = result.uncaught_callback_errors;
    bool callback_task_ok = script_runtime_evaluate_diagnostic(
        runtime, callback_task_probe, "<callback-task-probe>", &result);
    bool callback_task_valid = callback_task_ok
        && strcmp(result.summary, "CALLBACK-TASK-PROBE-OK") == 0
        && result.uncaught_callback_errors == callback_errors_before + 1
        && strstr(result.last_uncaught_callback_error, "task-probe") != NULL
        && strstr(result.last_uncaught_callback_task, "realm=top") != NULL
        && strstr(result.last_uncaught_callback_task,
                  "task=xhr:readystatechange:state=3#") != NULL;
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
