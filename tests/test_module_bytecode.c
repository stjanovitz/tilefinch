/* Page ES-module compilation: source retention and the in-memory module
   bytecode cache. Each test drives the real ScriptRuntime module pipeline
   (root evaluation, the QuickJS loader callback, import resolution) with a
   small in-test module map in place of the network loader. */
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/session.h"
#include "tilefinch/session_persistence.h"
#include "tilefinch/sha256.h"
#include "tilefinch/viewport.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <lexbor/dom/interfaces/node.h>
#include <quickjs.h>

#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

#define MODULE_MAP_LIMIT 8u

/* request URL -> (response URL, source). Sources are owned by the test. */
typedef struct {
    const char *request_url[MODULE_MAP_LIMIT];
    const char *response_url[MODULE_MAP_LIMIT];
    char *source[MODULE_MAP_LIMIT];
    bool available[MODULE_MAP_LIMIT];
    size_t loads[MODULE_MAP_LIMIT];
    /* Simulated source-load latency (the synchronous loader's own time). */
    unsigned delay_ms[MODULE_MAP_LIMIT];
    size_t count;
    char referrer[MODULE_MAP_LIMIT][128];
} ModuleMap;

static char *copy_text(const char *text, size_t length)
{
    char *copy = malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

/* A module whose text is `body` followed by a comment that pads it to at
   least `minimum` bytes, so it lands on a chosen side of a size policy
   without moving the body's line numbers. */
static char *padded_source(const char *body, size_t minimum)
{
    size_t body_length = strlen(body);
    size_t length = body_length + 6u < minimum ? minimum : body_length + 6u;
    char *source = malloc(length + 1u);
    if (source == NULL) return NULL;
    memcpy(source, body, body_length);
    source[body_length] = '\n';
    source[body_length + 1u] = '/';
    source[body_length + 2u] = '*';
    memset(source + body_length + 3u, 'x', length - body_length - 5u);
    source[length - 2u] = '*';
    source[length - 1u] = '/';
    source[length] = '\0';
    return source;
}

static bool module_map_add(ModuleMap *map, const char *request_url,
                           const char *response_url, char *source)
{
    if (map->count >= MODULE_MAP_LIMIT || source == NULL) {
        free(source);
        return false;
    }
    map->request_url[map->count] = request_url;
    map->response_url[map->count] = response_url;
    map->source[map->count] = source;
    map->available[map->count] = true;
    map->count++;
    return true;
}

static void module_map_free(ModuleMap *map)
{
    for (size_t i = 0; i < map->count; i++) free(map->source[i]);
    memset(map, 0, sizeof(*map));
}

static bool module_map_load(void *opaque,
                            const ScriptModuleLoadRequest *request,
                            ScriptModuleLoadResult *result)
{
    ModuleMap *map = opaque;
    for (size_t i = 0; i < map->count; i++) {
        if (strcmp(request->request_url, map->request_url[i]) != 0) continue;
        if (!map->available[i]) return false;
        map->loads[i]++;
        if (map->delay_ms[i] != 0) {
            struct timespec wait = {
                .tv_sec = map->delay_ms[i] / 1000u,
                .tv_nsec = (long) (map->delay_ms[i] % 1000u) * 1000000L
            };
            while (nanosleep(&wait, &wait) != 0) {
            }
        }
        snprintf(map->referrer[i], sizeof(map->referrer[i]), "%s",
                 request->referrer_url == NULL ? "" : request->referrer_url);
        size_t length = strlen(map->source[i]);
        result->source = copy_text(map->source[i], length);
        result->source_length = length;
        result->response_url = copy_text(
            map->response_url[i], strlen(map->response_url[i]));
        if (result->source == NULL || result->response_url == NULL) {
            free(result->source);
            free(result->response_url);
            memset(result, 0, sizeof(*result));
            return false;
        }
        return true;
    }
    return false;
}

static void module_map_release(void *opaque, ScriptModuleLoadResult *result)
{
    (void) opaque;
    free(result->source);
    free(result->response_url);
    memset(result, 0, sizeof(*result));
}

typedef struct {
    Budget budget;
    PocDocument document;
    ScriptRuntimeOptions options;
    lxb_dom_node_t *script;
} ModuleFixture;

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

static bool fixture_open(ModuleFixture *fixture)
{
    static const char html[] =
        "<!doctype html><html><body>"
        "<script type=module src=/root.js></script></body></html>";
    memset(fixture, 0, sizeof(*fixture));
    budget_init(&fixture->budget, 48u * MIB);
    if (!budget_install_lexbor(&fixture->budget)
        || !document_parse(&fixture->document, &fixture->budget, html,
                           sizeof(html) - 1u, sizeof(html))) return false;
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)
        || !script_execution_policy_for_profile(
               SCRIPT_EXECUTION_PROFILE_LAB, &policy)) return false;
    fixture->options = (ScriptRuntimeOptions) {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
    };
    fixture->script = find_script(
        lxb_dom_interface_node(fixture->document.html));
    return fixture->script != NULL;
}

static bool fixture_close(ModuleFixture *fixture)
{
    document_destroy(&fixture->document);
    return fixture->budget.current == 0
        && budget_uninstall_lexbor(&fixture->budget);
}

/* Evaluate `root_source` as the external module root https://mod.test/root.js
   in a fresh realm and settle its jobs. The realm is left in *out_runtime. */
static bool run_root(ModuleFixture *fixture, ModuleMap *map,
                     const char *root_source, ScriptRuntime **out_runtime,
                     ScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000,
        "https://mod.test/", &fixture->options, result);
    *out_runtime = runtime;
    if (runtime == NULL) return false;
    script_runtime_set_module_loader(
        runtime, module_map_load, module_map_release, map);
    (void) script_runtime_evaluate_external_module_context(
        runtime, fixture->script, root_source, strlen(root_source),
        "https://mod.test/root.js", "https://mod.test/root.js", "",
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, result);
    for (size_t turn = 0; turn < 8; turn++) {
        if (!script_runtime_advance(runtime, 0, 32, result)) return false;
    }
    return true;
}

static int test_large_module_source_is_not_retained(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    /* thrower() is on line 2 of large.js; its padding follows it. */
    CHECK(module_map_add(&map, "https://mod.test/large.js",
              "https://mod.test/large.js",
              padded_source(
                  "export function big(){return 1}\n"
                  "export function thrower(){return new Error('probe').stack}\n"
                  "export class Box{get(){return (0,eval)("
                  "'(function evaluated(){return 4})').toString()+'|'+"
                  "Function('return 5').toString()}}",
                  24u * 1024u))
          && module_map_add(&map, "https://mod.test/small.js",
              "https://mod.test/small.js",
              copy_text("export function small(){return 2}", 34)));
    char *root = padded_source(
        "import {big,thrower,Box} from './large.js';"
        "import {small} from './small.js';"
        "function rootFn(){return 3}"
        "const stack=thrower();"
        "globalThis.pocSummary=JSON.stringify({big:big.toString(),"
        "small:small.toString(),root:rootFn.toString(),"
        "line:stack.includes('https://mod.test/large.js:2:'),"
        "dynamic:new Box().get()});",
        12u * 1024u);
    CHECK(root != NULL);
    ScriptRuntime *runtime = NULL;
    ScriptResult result;
    bool ran = run_root(&fixture, &map, root, &runtime, &result);
    static const char expected[] =
        "{\"big\":\"function big() {\\n    [native code]\\n}\","
        "\"small\":\"function small(){return 2}\","
        "\"root\":\"function rootFn() {\\n    [native code]\\n}\","
        "\"line\":true,"
        "\"dynamic\":\"function evaluated(){return 4}|"
        "function anonymous(\\n) {\\nreturn 5\\n}\"}";
    if (!ran || strcmp(result.summary, expected) != 0) {
        fprintf(stderr, "strip: ran=%d summary=%s error=%s\n", ran,
                result.summary, result.error);
    }
    CHECK(ran && strcmp(result.summary, expected) == 0);
    script_runtime_destroy(runtime);
    free(root);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The point of the policy: a large module's text does not stay in the page
   heap. 64 KiB inside one function body was retained whole before. */
static int test_large_module_heap_excludes_source(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    size_t body = 64u * 1024u;
    char *source = malloc(body + 128u);
    CHECK(source != NULL);
    int prefix = snprintf(source, 64u, "export function held(){/*");
    memset(source + prefix, 'y', body);
    snprintf(source + prefix + body, 64u, "*/return 6}");
    CHECK(module_map_add(&map, "https://mod.test/held.js",
                         "https://mod.test/held.js", source));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000,
        "https://mod.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_module_loader(
        runtime, module_map_load, module_map_release, &map);
    (void) script_runtime_collect_and_trim(runtime);
    size_t before = script_runtime_heap_used(runtime);
    static const char root[] =
        "import {held} from './held.js';"
        "globalThis.pocSummary='HELD:'+held()";
    (void) script_runtime_evaluate_external_module_context(
        runtime, fixture.script, root, sizeof(root) - 1u,
        "https://mod.test/root.js", "https://mod.test/root.js", "",
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, &result);
    (void) script_runtime_collect_and_trim(runtime);
    size_t after = script_runtime_heap_used(runtime);
    size_t growth = after > before ? after - before : 0;
    if (getenv("TILEFINCH_TEST_VERBOSE") != NULL
        || strcmp(result.summary, "HELD:6") != 0 || growth >= body / 2u) {
        fprintf(stderr, "held: summary=%s growth=%zu\n", result.summary,
                growth);
    }
    CHECK(strcmp(result.summary, "HELD:6") == 0 && growth < body / 2u);
    script_runtime_destroy(runtime);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Page scripts compile functions lazily (only on their first call). In a
   module whose text is stripped a deferred function keeps a copy of its own
   text until it compiles; one whose text is mostly comment stays eager,
   so the policy above still holds when it is never called. */
static int test_uncalled_commented_function_keeps_no_text(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    size_t body = 64u * 1024u;
    char *source = malloc(body + 128u);
    CHECK(source != NULL);
    int prefix = snprintf(source, 64u, "export function held(){/*");
    memset(source + prefix, 'y', body);
    snprintf(source + prefix + body, 64u, "*/return 6}");
    CHECK(module_map_add(&map, "https://mod.test/held.js",
                         "https://mod.test/held.js", source));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000,
        "https://mod.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_module_loader(
        runtime, module_map_load, module_map_release, &map);
    (void) script_runtime_collect_and_trim(runtime);
    size_t before = script_runtime_heap_used(runtime);
    static const char root[] =
        "import {held} from './held.js';"
        "globalThis.pocSummary='HELD:'+typeof held";
    (void) script_runtime_evaluate_external_module_context(
        runtime, fixture.script, root, sizeof(root) - 1u,
        "https://mod.test/root.js", "https://mod.test/root.js", "",
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, &result);
    (void) script_runtime_collect_and_trim(runtime);
    size_t after = script_runtime_heap_used(runtime);
    size_t growth = after > before ? after - before : 0;
    if (strcmp(result.summary, "HELD:function") != 0 || growth >= body / 2u) {
        fprintf(stderr, "uncalled held: summary=%s growth=%zu\n",
                result.summary, growth);
    }
    CHECK(strcmp(result.summary, "HELD:function") == 0 && growth < body / 2u);
    script_runtime_destroy(runtime);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

static int test_page_functions_compile_on_first_call(void)
{
    static const char small_source[] =
        "export function small(c){return ['small',c,'text','kept','for',"
        "'toString','of','this','function'].join('-')}";
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    /* thrower() is on line 3 of lazy.js; the module is stripped */
    CHECK(module_map_add(&map, "https://mod.test/lazy.js",
              "https://mod.test/lazy.js",
              padded_source(
                  "export function used(a){const parts=[a,'-used'];for(let i=0;i<3;i++)parts.push(i*a);return parts.join('')+['one','two','three','four'].map(w=>w.length).join('')}\n"
                  "export function unused(b){const t=[b,b*2,b*3].map(x=>x+1);return t.join(',')+['five','six','seven','eight'].map(w=>w.toUpperCase()).join('')}\n"
                  "export function thrower(){const e=new Error('probe');return e.stack.split('\\n')[1]+'|'+['padding','to','pass','the','threshold'].join('')}\n",
                  24u * 1024u))
          && module_map_add(&map, "https://mod.test/small.js",
              "https://mod.test/small.js",
              copy_text(small_source, sizeof(small_source) - 1u)));
    char *root = padded_source(
        "import {used,unused,thrower} from './lazy.js';"
        "import {small} from './small.js';"
        "const evaluated=(0,eval)('(function evaluated(){var x=1;'+'x+=1;'.repeat(40)+'return x})');"
        "globalThis.pocSummary=JSON.stringify({used:used(2),"
        "text:String(used),frame:thrower().split('|')[0].trim(),"
        "small:String(small),eval:evaluated(),kinds:typeof unused});",
        12u * 1024u);
    CHECK(root != NULL);
    ScriptRuntime *runtime = NULL;
    ScriptResult result;
    bool ran = run_root(&fixture, &map, root, &runtime, &result);
    static const char expected[] =
        "{\"used\":\"2-used0243354\","
        "\"text\":\"function used() {\\n    [native code]\\n}\","
        "\"frame\":\"at thrower (https://mod.test/lazy.js:3:44)\","
        "\"small\":\"function small(c){return ['small',c,'text','kept','for','toString','of','this','function'].join('-')}\","
        "\"eval\":41,\"kinds\":\"function\"}";
    size_t deferred = 0, compiled = 0;
    script_runtime_lazy_function_counts(runtime, &deferred, &compiled);
    /* used, unused and thrower; small() is under the size threshold and the
       eval'd function is always eager */
    if (!ran || strcmp(result.summary, expected) != 0
        || deferred != 3 || compiled != 2) {
        fprintf(stderr, "lazy page: ran=%d summary=%s error=%s deferred=%zu "
                        "compiled=%zu\n", ran, result.summary, result.error,
                deferred, compiled);
    }
    CHECK(ran && strcmp(result.summary, expected) == 0);
    CHECK(deferred == 3 && compiled == 2);
    script_runtime_destroy(runtime);
    free(root);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

static size_t reclaim_module_bytecode(void *opaque, size_t needed)
{
    return browser_session_module_bytecode_reclaim(opaque, needed);
}

/* ---- module bytecode cache ---- */

/* A graph with every module above the strip threshold except `small.js`,
   a response URL that differs from the request URL (import.meta.url and
   relative imports must use the response), and a dynamic import. */
static bool cache_map_init(ModuleMap *map, const char *dep_value)
{
    char dep[512];
    snprintf(dep, sizeof(dep),
             "import {leaf} from './leaf.js';"
             "export const dep=%s;"
             "export const depMeta=import.meta.url;"
             "export function depLeaf(){return leaf()}", dep_value);
    return module_map_add(map, "https://mod.test/dep.js",
                          "https://cdn.test/final/dep.js",
                          padded_source(dep, 12u * 1024u))
        && module_map_add(map, "https://cdn.test/final/leaf.js",
                          "https://cdn.test/final/leaf.js",
                          padded_source("export function leaf(){"
                                        "return import.meta.url}",
                                        10u * 1024u))
        && module_map_add(map, "https://mod.test/small.js",
                          "https://mod.test/small.js",
                          copy_text("export const small=2;", 21))
        && module_map_add(map, "https://mod.test/lazy.js",
                          "https://mod.test/lazy.js",
                          padded_source("export const lazy='lazy';",
                                        9u * 1024u));
}

static const char cache_root[] =
    "import {dep,depMeta,depLeaf} from './dep.js';"
    "import {small} from './small.js';"
    "import('./lazy.js').then(m=>{globalThis.pocSummary=[dep,small,m.lazy,"
    "depMeta,depLeaf(),import.meta.url].join('|')},"
    "e=>{globalThis.pocSummary='LAZY-FAILED:'+e})";

static size_t cache_map_loads(const ModuleMap *map)
{
    size_t total = 0;
    for (size_t i = 0; i < map->count; i++) total += map->loads[i];
    return total;
}

static int test_second_load_restores_modules(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(browser_session_init(&session, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 1024u * 1024u);
    fixture.options.session = &session;
    size_t baseline = fixture.budget.current;
    ModuleMap map = {0};
    CHECK(cache_map_init(&map, "'one'"));
    static const char expected_one[] =
        "one|2|lazy|https://cdn.test/final/dep.js|"
        "https://cdn.test/final/leaf.js|https://mod.test/root.js";

    /* First load: every module compiles; all but the tiny ones below the
       admission floor are stored. */
    ScriptRuntime *runtime = NULL;
    ScriptResult first;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &first));
    if (strcmp(first.summary, expected_one) != 0) {
        fprintf(stderr, "first: %s error=%s\n", first.summary, first.error);
    }
    CHECK(strcmp(first.summary, expected_one) == 0
          && first.module_bytecode_cache_hits == 0
          && first.module_bytecode_cache_misses == 5
          && first.module_bytecode_cache_stores == 5
          && first.module_bytecode_cache_stored_bytes != 0
          && browser_session_module_bytecode_entries(&session) == 5
          && browser_session_module_bytecode_bytes(&session)
                 <= 1024u * 1024u);
    size_t first_compiles = first.host_compile_attempts;
    size_t first_loads = cache_map_loads(&map);
    script_runtime_destroy(runtime);

    /* Second load of the same bytes in a new realm: every module restores,
       nothing compiles, and the graph behaves identically. The loader still
       fetches each module (the cache sits after every fetch check). */
    ScriptResult second;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &second));
    if (strcmp(second.summary, expected_one) != 0) {
        fprintf(stderr, "second: %s error=%s\n", second.summary,
                second.error);
    }
    CHECK(strcmp(second.summary, expected_one) == 0
          && second.module_bytecode_cache_hits == 5
          && second.module_bytecode_cache_misses == 0
          && second.module_bytecode_cache_restore_failures == 0
          && second.module_bytecode_cache_bytes
                 == first.module_bytecode_cache_stored_bytes
          && second.module_compile_count == 0
          && second.host_compile_attempts + 5 == first_compiles
          && cache_map_loads(&map) == 2u * first_loads);
    script_runtime_destroy(runtime);

    /* Changed bytes at the same URL miss, compile, and replace the entry. */
    module_map_free(&map);
    CHECK(cache_map_init(&map, "'two'"));
    ScriptResult third;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &third));
    CHECK(strncmp(third.summary, "two|", 4) == 0
          && third.module_bytecode_cache_hits == 4
          && third.module_bytecode_cache_misses == 1
          && third.module_bytecode_cache_stores == 1
          && browser_session_module_bytecode_entries(&session) == 5);
    script_runtime_destroy(runtime);

    /* Another top-level site is another partition: nothing is shared. */
    ScriptRuntimeOptions other_site = fixture.options;
    other_site.top_level_url = "https://other.test/";
    ScriptRuntimeOptions saved = fixture.options;
    fixture.options = other_site;
    ScriptResult fourth;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &fourth));
    CHECK(strncmp(fourth.summary, "two|", 4) == 0
          && fourth.module_bytecode_cache_hits == 0
          && fourth.module_bytecode_cache_misses == 5);
    script_runtime_destroy(runtime);
    fixture.options = saved;

    browser_session_cache_clear(&session);
    CHECK(browser_session_module_bytecode_entries(&session) == 0
          && fixture.budget.current == baseline);
    browser_session_destroy(&session);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* ---- persistent tier ---- */

static void disk_test_clear(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir != NULL) {
        struct dirent *entry;
        char path[512];
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            (void) remove(path);
        }
        closedir(dir);
    }
    (void) rmdir(directory);
}

static size_t disk_test_files(const char *directory, char *first,
                              size_t capacity)
{
    size_t count = 0;
    DIR *dir = opendir(directory);
    if (dir == NULL) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (count++ == 0 && first != NULL)
            snprintf(first, capacity, "%s/%s", directory, entry->d_name);
    }
    closedir(dir);
    return count;
}

/* Store, reload, refuse damage and foreign keys, never rewrite, and stay
   off without a directory or (for writes) without permission. */
static int test_session_module_bytecode_disk(void)
{
    static const char directory[] = "module-bytecode-disk-unit";
    disk_test_clear(directory);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 256u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 100u * 1024u);
    size_t baseline = budget.current;
    static unsigned char bytecode[3000];
    for (size_t i = 0; i < sizeof(bytecode); i++)
        bytecode[i] = (unsigned char) (i * 7u);
    static const char source[] = "export const a=1;";
    BrowserModuleBytecodeKey key = {
        .module_name = "https://s.test/a.js",
        .response_url = "https://s.test/a.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source,
        .source_length = sizeof(source) - 1u
    };
    /* Off by default. */
    CHECK(!browser_session_module_bytecode_disk_enabled(&session)
          && browser_session_module_bytecode_disk_load(&session, &key) == NULL
          && !browser_session_module_bytecode_disk_store(
                 &session, &key, bytecode, sizeof(bytecode)));
    /* Read-only: never writes. */
    browser_session_module_bytecode_set_disk(&session, directory, false);
    CHECK(browser_session_module_bytecode_disk_enabled(&session)
          && !browser_session_module_bytecode_disk_wants(
                 &session, &key, sizeof(bytecode))
          && !browser_session_module_bytecode_disk_store(
                 &session, &key, bytecode, sizeof(bytecode))
          && disk_test_files(directory, NULL, 0) == 0);
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key, bytecode, sizeof(bytecode)));
    char file[512] = {0};
    CHECK(disk_test_files(directory, file, sizeof(file)) == 1);
    /* Once written, never rewritten. */
    CHECK(!browser_session_module_bytecode_disk_wants(
              &session, &key, sizeof(bytecode))
          && !browser_session_module_bytecode_disk_store(
                 &session, &key, bytecode, sizeof(bytecode)));
    BrowserSharedBody *body =
        browser_session_module_bytecode_disk_load(&session, &key);
    CHECK(body != NULL && body->length == sizeof(bytecode)
          && memcmp(body->data, bytecode, sizeof(bytecode)) == 0);
    browser_shared_body_release(body);
    /* Refusing the shared-body wrapper after reading a valid payload must
       not delete it or poison the read-only rejection cache. */
    for (int writable = 0; writable < 2; writable++) {
        browser_session_module_bytecode_set_disk(&session, directory, writable);
        size_t rejected = session.module_bytecode_disk_rejects;
        budget_inject_failure_after(&budget, 1);
        CHECK(browser_session_module_bytecode_disk_load(&session, &key) == NULL);
        budget_clear_failure_injection(&budget);
        CHECK(session.module_bytecode_disk_rejects == rejected
              && disk_test_files(directory, NULL, 0) == 1);
        body = browser_session_module_bytecode_disk_load(&session, &key);
        CHECK(body != NULL);
        browser_shared_body_release(body);
    }
    /* Other bytes at the same URL are another key: a miss. */
    static const char other_source[] = "export const a=2;";
    BrowserModuleBytecodeKey other = key;
    other.source = (const unsigned char *) other_source;
    other.digest_ready = false;
    CHECK(browser_session_module_bytecode_disk_load(&session, &other)
          == NULL);
    /* A damaged payload is rejected, never handed out; with writes on it is
       removed, so the next compile's store replaces it. */
    FILE *damage = fopen(file, "r+b");
    CHECK(damage != NULL && fseek(damage, -10, SEEK_END) == 0);
    int original = fgetc(damage);
    CHECK(fseek(damage, -10, SEEK_END) == 0
          && fputc(original ^ 0x55, damage) != EOF && fclose(damage) == 0);
    size_t rejects = session.module_bytecode_disk_rejects;
    CHECK(browser_session_module_bytecode_disk_load(&session, &key) == NULL
          && session.module_bytecode_disk_rejects == rejects + 1u
          && disk_test_files(directory, NULL, 0) == 0
          && browser_session_module_bytecode_disk_wants(
                 &session, &key, sizeof(bytecode))
          && browser_session_module_bytecode_disk_store(
                 &session, &key, bytecode, sizeof(bytecode)));
    body = browser_session_module_bytecode_disk_load(&session, &key);
    CHECK(body != NULL && body->length == sizeof(bytecode));
    browser_shared_body_release(body);
    /* Read-only: a truncated file is refused once, kept, and not read
       again this session. */
    FILE *truncate_file = fopen(file, "wb");
    CHECK(truncate_file != NULL
          && fwrite("TFMB", 1, 4, truncate_file) == 4
          && fclose(truncate_file) == 0);
    browser_session_module_bytecode_set_disk(&session, directory, false);
    rejects = session.module_bytecode_disk_rejects;
    size_t misses = session.module_bytecode_disk_misses;
    CHECK(browser_session_module_bytecode_disk_load(&session, &key) == NULL
          && browser_session_module_bytecode_disk_load(&session, &key) == NULL
          && session.module_bytecode_disk_rejects == rejects + 1u
          && session.module_bytecode_disk_misses == misses
          && disk_test_files(directory, NULL, 0) == 1);
    /* So is one the engine could not restore. */
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(remove(file) == 0
          && browser_session_module_bytecode_disk_store(
                 &session, &key, bytecode, sizeof(bytecode)));
    browser_session_module_bytecode_set_disk(&session, directory, false);
    browser_session_module_bytecode_disk_discard(&session, &key);
    CHECK(browser_session_module_bytecode_disk_load(&session, &key) == NULL
          && disk_test_files(directory, NULL, 0) == 1);
    browser_session_module_bytecode_set_disk(&session, directory, true);
    browser_session_module_bytecode_disk_discard(&session, &key);
    CHECK(disk_test_files(directory, NULL, 0) == 0);
    browser_session_module_bytecode_set_disk(&session, "", true);
    CHECK(!browser_session_module_bytecode_disk_enabled(&session));
    CHECK(budget.current == baseline);
    browser_session_destroy(&session);
    disk_test_clear(directory);
    return 0;
}

static bool disk_test_touch(const char *path)
{
    FILE *file = fopen(path, "wb");
    return file != NULL && fwrite("x", 1, 1, file) == 1 && fclose(file) == 0;
}

/* Across sessions: the first write removes temporary files and another
   engine build's files, the directory never passes its file ceiling, and
   clearing the cache empties it (read-only: stops reading it). */
static int test_session_module_bytecode_disk_housekeeping(void)
{
    static const char directory[] = "module-bytecode-disk-house";
    disk_test_clear(directory);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 256u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 100u * 1024u);
    static const unsigned char bytecode[64] = {1, 2, 3};
    static const char source_a[] = "export const a=1;",
        source_b[] = "export const b=1;";
    BrowserModuleBytecodeKey key_a = {
        .module_name = "https://s.test/a.js",
        .response_url = "https://s.test/a.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source_a,
        .source_length = sizeof(source_a) - 1u
    }, key_b = key_a;
    key_b.module_name = key_b.response_url = "https://s.test/b.js";
    key_b.source = (const unsigned char *) source_b;
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key_a, bytecode, sizeof(bytecode)));
    char file[512] = {0};
    CHECK(disk_test_files(directory, file, sizeof(file)) == 1);
    const char *name = strrchr(file, '/') + 1;
    char prefix[16] = {0};
    memcpy(prefix, name, 8);

    /* A later session (another launch, or another engine build). */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%032d.tmp", directory, prefix, 7);
    CHECK(disk_test_touch(path));
    snprintf(path, sizeof(path), "%s/%s-%032d.tfmb", directory,
             strcmp(prefix, "00000000") == 0 ? "11111111" : "00000000", 7);
    CHECK(disk_test_touch(path));
    snprintf(path, sizeof(path), "%s/unrelated.txt", directory);
    CHECK(disk_test_touch(path));
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(disk_test_files(directory, NULL, 0) == 4);
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key_b, bytecode, sizeof(bytecode)));
    /* The two records and the unrelated file remain. */
    CHECK(disk_test_files(directory, NULL, 0) == 3
          && session.module_bytecode_disk_file_count == 2);
    CHECK(remove(path) == 0);

    /* A full directory must not be swept during module compilation: b's
       record and fakes fill it exactly. A store visits only one slice. */
    CHECK(remove(file) == 0);
    for (unsigned i = 0; i + 1u < BROWSER_MODULE_BYTECODE_DISK_FILE_COUNT_LIMIT;
         i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix,
                 i + 100u);
        CHECK(disk_test_touch(path));
    }
    browser_session_module_bytecode_set_disk(&session, directory, true);
    size_t removed_before = session.module_bytecode_disk_removed;
    CHECK(!browser_session_module_bytecode_disk_store(
              &session, &key_a, bytecode, sizeof(bytecode))
          && !session.module_bytecode_disk_scanned
          && session.module_bytecode_disk_scan_visits
                 <= BROWSER_MODULE_BYTECODE_DISK_SCAN_SLICE
          && disk_test_files(directory, NULL, 0)
                 == BROWSER_MODULE_BYTECODE_DISK_FILE_COUNT_LIMIT);
    for (unsigned i = 0; i < BROWSER_MODULE_BYTECODE_DISK_SCAN_LIMIT
                         && !session.module_bytecode_disk_scanned; i++) {
        size_t visits = session.module_bytecode_disk_scan_visits;
        CHECK(browser_session_module_bytecode_disk_maintenance(&session));
        CHECK(session.module_bytecode_disk_scan_visits - visits
                  <= BROWSER_MODULE_BYTECODE_DISK_SCAN_SLICE);
    }
    /* Accounting is complete and the directory full: compilation still
       declines rather than evicting (idle maintenance evicts; see
       test_session_module_bytecode_disk_eviction). */
    CHECK(session.module_bytecode_disk_scanned
          && session.module_bytecode_disk_scan_cursor == NULL
          && !browser_session_module_bytecode_disk_store(
              &session, &key_a, bytecode, sizeof(bytecode))
          && session.module_bytecode_disk_removed == removed_before
          && session.module_bytecode_disk_file_count
                 == BROWSER_MODULE_BYTECODE_DISK_FILE_COUNT_LIMIT);

    /* Clearing the cache: read-only stops reading, writable empties it. */
    browser_session_module_bytecode_set_disk(&session, directory, false);
    CHECK(browser_session_persistence_clear(&session,
              BROWSER_SESSION_PERSIST_CACHE)
          == BROWSER_SESSION_PERSISTENCE_OK);
    CHECK(browser_session_module_bytecode_disk_load(&session, &key_a) == NULL
          && disk_test_files(directory, NULL, 0)
                 == BROWSER_MODULE_BYTECODE_DISK_FILE_COUNT_LIMIT);
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_persistence_clear(&session,
              BROWSER_SESSION_PERSIST_CACHE)
          == BROWSER_SESSION_PERSISTENCE_OK);
    CHECK(disk_test_files(directory, NULL, 0) == 0
          && browser_session_module_bytecode_disk_load(&session, &key_a)
                 == NULL);
    /* Stale cleanup is sliced too, and an unfinished cursor is released on
       reconfiguration, explicit clear, and session teardown. */
    for (unsigned i = 0; i < 20; i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tmp", directory, prefix, i);
        CHECK(disk_test_touch(path));
    }
    browser_session_module_bytecode_set_disk(&session, directory, true);
    removed_before = session.module_bytecode_disk_removed;
    CHECK(!browser_session_module_bytecode_disk_store(
        &session, &key_a, bytecode, sizeof(bytecode)));
    CHECK(session.module_bytecode_disk_scan_cursor != NULL
          && session.module_bytecode_disk_removed - removed_before
                 <= BROWSER_MODULE_BYTECODE_DISK_SCAN_SLICE);
    browser_session_module_bytecode_set_disk(&session, directory, false);
    CHECK(session.module_bytecode_disk_scan_cursor == NULL);
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_module_bytecode_disk_clear(&session));

    /* Oversized existing storage is not a reason to erase it during load;
       idle maintenance trims it. */
    snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix, 999u);
    FILE *large = fopen(path, "wb");
    CHECK(large != NULL);
    CHECK(ftruncate(fileno(large), BROWSER_MODULE_BYTECODE_DISK_TOTAL_LIMIT + 1u)
          == 0 && fclose(large) == 0);
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(!browser_session_module_bytecode_disk_store(
        &session, &key_a, bytecode, sizeof(bytecode)));
    CHECK(session.module_bytecode_disk_scanned
          && !session.module_bytecode_disk_scan_failed
          && session.module_bytecode_disk_scan_cursor == NULL
          && disk_test_files(directory, NULL, 0) == 1);
    CHECK(browser_session_module_bytecode_disk_maintenance(&session)
          && disk_test_files(directory, NULL, 0) == 0
          && session.module_bytecode_disk_total_bytes == 0);
    CHECK(browser_session_module_bytecode_disk_clear(&session));
    for (unsigned i = 0; i < 20; i++) {
        snprintf(path, sizeof(path), "%s/unrelated-%u", directory, i);
        CHECK(disk_test_touch(path));
    }
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_module_bytecode_disk_maintenance(&session)
          && session.module_bytecode_disk_scan_cursor != NULL);
    browser_session_destroy(&session);
    CHECK(session.module_bytecode_disk_scan_cursor == NULL && budget.current == 0);
    disk_test_clear(directory);
    return 0;
}

/* A file of `size` bytes (at least one) last modified at `mtime`. */
static bool disk_test_aged(const char *path, time_t mtime, off_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite("x", 1, 1, file) == 1
        && (size <= 1 || ftruncate(fileno(file), size) == 0);
    ok = fclose(file) == 0 && ok;
    struct utimbuf times = { mtime, mtime };
    return ok && utime(path, &times) == 0;
}

static bool disk_test_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

/* Runs idle maintenance until it reports no work, each call bounded to one
   slice of removals; returns the number of calls, or 0 past `limit`. */
static unsigned disk_test_idle(BrowserSession *session, unsigned limit)
{
    for (unsigned calls = 1; calls <= limit; calls++) {
        size_t removed = session->module_bytecode_disk_removed;
        if (!browser_session_module_bytecode_disk_maintenance(session))
            return calls;
        if (session->module_bytecode_disk_removed - removed
                > BROWSER_MODULE_BYTECODE_DISK_SCAN_SLICE) return 0;
    }
    return 0;
}

/* A full cache keeps admitting modules: idle maintenance (never a compile)
   removes this build's oldest records down to the low-water marks, keeps
   the newest and every file that is not this tier's, a failed scan only
   defers writes until a later retry, and a read-only tier never deletes. */
static int test_session_module_bytecode_disk_eviction(void)
{
    static const char directory[] = "module-bytecode-disk-evict";
    disk_test_clear(directory);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 256u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 100u * 1024u);
    static const unsigned char bytecode[64] = {4, 5, 6};
    static const char source_a[] = "export const a=1;",
        source_b[] = "export const b=1;";
    BrowserModuleBytecodeKey key_a = {
        .module_name = "https://s.test/a.js",
        .response_url = "https://s.test/a.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source_a,
        .source_length = sizeof(source_a) - 1u
    }, key_b = key_a;
    key_b.module_name = key_b.response_url = "https://s.test/b.js";
    key_b.source = (const unsigned char *) source_b;
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key_a, bytecode, sizeof(bytecode)));
    char file[512] = {0};
    CHECK(disk_test_files(directory, file, sizeof(file)) == 1);
    char prefix[16] = {0};
    memcpy(prefix, strrchr(file, '/') + 1, 8);

    /* Last deploy's modules: a's record (written now) is the newest; the
       rest were written in order, long ago. */
    const unsigned limit = BROWSER_MODULE_BYTECODE_DISK_FILE_COUNT_LIMIT,
        low = BROWSER_MODULE_BYTECODE_DISK_LOW_WATER_FILES;
    const time_t long_ago = 1000000000;
    char path[512];
    for (unsigned i = 0; i + 1u < limit; i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix,
                 i + 100u);
        CHECK(disk_test_aged(path, long_ago + (time_t) i, 0));
    }
    /* Not this tier's names: never removed, however old. */
    static const char *const unrelated[] = {
        "keep.txt", "unrelated.tfmb", "0000000-00000000000000000000000000000000.tfmb"
    };
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, unrelated[i]);
        CHECK(disk_test_aged(path, long_ago - 10, 0));
    }
    const size_t others = sizeof(unrelated) / sizeof(unrelated[0]);

    /* Read-only: idle maintenance does nothing. */
    browser_session_module_bytecode_set_disk(&session, directory, false);
    CHECK(disk_test_idle(&session, 4) == 1
          && disk_test_files(directory, NULL, 0) == limit + others);

    /* Writable: compiling b only continues accounting, and still declines
       once the full directory is counted. */
    browser_session_module_bytecode_set_disk(&session, directory, true);
    size_t removed_before = session.module_bytecode_disk_removed;
    CHECK(!browser_session_module_bytecode_disk_store(
        &session, &key_b, bytecode, sizeof(bytecode)));
    for (unsigned i = 0; i < 1000u && !session.module_bytecode_disk_scanned;
         i++) (void) browser_session_module_bytecode_disk_wants(
                  &session, &key_b, sizeof(bytecode));
    CHECK(session.module_bytecode_disk_scanned
          && session.module_bytecode_disk_file_count == limit
          && !browser_session_module_bytecode_disk_store(
                 &session, &key_b, bytecode, sizeof(bytecode))
          && session.module_bytecode_disk_removed == removed_before
          && disk_test_files(directory, NULL, 0) == limit + others);

    /* Idle maintenance evicts the oldest down to the low-water mark. */
    CHECK(disk_test_idle(&session, 20000u) != 0);
    CHECK(session.module_bytecode_disk_scanned
          && !session.module_bytecode_disk_evicting
          && session.module_bytecode_disk_file_count == low
          && session.module_bytecode_disk_removed - removed_before
                 == limit - low
          && disk_test_files(directory, NULL, 0) == low + others);
    for (unsigned i = 0; i + 1u < limit; i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix,
                 i + 100u);
        CHECK(disk_test_exists(path) == (i >= limit - low));
    }
    for (size_t i = 0; i < others; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, unrelated[i]);
        CHECK(disk_test_exists(path));
    }
    BrowserSharedBody *body =
        browser_session_module_bytecode_disk_load(&session, &key_a);
    CHECK(body != NULL);
    browser_shared_body_release(body);
    /* The new deploy's module is admitted, and a later session's scan finds
       room without evicting again. */
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key_b, bytecode, sizeof(bytecode)));
    browser_session_module_bytecode_set_disk(&session, directory, true);
    removed_before = session.module_bytecode_disk_removed;
    CHECK(disk_test_idle(&session, 20000u) != 0
          && session.module_bytecode_disk_file_count == low + 1u
          && session.module_bytecode_disk_removed == removed_before);
    CHECK(browser_session_module_bytecode_disk_clear(&session)
          && disk_test_files(directory, NULL, 0) == others);

    /* The byte ceiling too: sixteen 1.5 MiB records are past the point
       where a maximum-size module fits; the oldest go until 18 MiB. */
    const off_t large = (off_t) (3u * MIB / 2u);
    for (unsigned i = 0; i < 16u; i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix,
                 i + 100u);
        CHECK(disk_test_aged(path, long_ago + (time_t) i, large));
    }
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(disk_test_idle(&session, 20000u) != 0
          && session.module_bytecode_disk_total_bytes
                 == 12u * (size_t) large
          && session.module_bytecode_disk_total_bytes
                 <= BROWSER_MODULE_BYTECODE_DISK_LOW_WATER_BYTES
          && disk_test_files(directory, NULL, 0) == 12u + others);
    for (unsigned i = 0; i < 16u; i++) {
        snprintf(path, sizeof(path), "%s/%s-%032u.tfmb", directory, prefix,
                 i + 100u);
        CHECK(disk_test_exists(path) == (i >= 4u));
    }
    CHECK(browser_session_module_bytecode_disk_store(
        &session, &key_a, bytecode, sizeof(bytecode)));
    CHECK(browser_session_module_bytecode_disk_clear(&session));

    /* A scan that stops at its visit ceiling defers writes, then retries
       after a backoff and admits them once the directory is countable. */
    for (unsigned i = 0; i < BROWSER_MODULE_BYTECODE_DISK_SCAN_LIMIT; i++) {
        snprintf(path, sizeof(path), "%s/filler-%u", directory, i);
        CHECK(disk_test_touch(path));
    }
    browser_session_module_bytecode_set_disk(&session, directory, true);
    CHECK(disk_test_idle(&session, 20000u) != 0
          && session.module_bytecode_disk_scan_failed
          && !browser_session_module_bytecode_disk_store(
                 &session, &key_a, bytecode, sizeof(bytecode)));
    size_t visits = session.module_bytecode_disk_scan_visits;
    CHECK(!browser_session_module_bytecode_disk_maintenance(&session)
          && session.module_bytecode_disk_scan_visits == visits);
    for (unsigned i = 0; i < BROWSER_MODULE_BYTECODE_DISK_SCAN_LIMIT; i++) {
        snprintf(path, sizeof(path), "%s/filler-%u", directory, i);
        CHECK(remove(path) == 0);
    }
    for (unsigned i = 0; i < 20000u && !session.module_bytecode_disk_scanned;
         i++) (void) browser_session_module_bytecode_disk_maintenance(&session);
    CHECK(session.module_bytecode_disk_scanned
          && browser_session_module_bytecode_disk_store(
                 &session, &key_a, bytecode, sizeof(bytecode)));
    CHECK(browser_session_module_bytecode_disk_clear(&session));

    /* So does one that cannot open the directory. */
    static const char blocked[] = "module-bytecode-disk-blocked";
    disk_test_clear(blocked);
    (void) remove(blocked);
    CHECK(disk_test_touch(blocked));
    browser_session_module_bytecode_set_disk(&session, blocked, true);
    CHECK(browser_session_module_bytecode_disk_maintenance(&session)
          && session.module_bytecode_disk_scan_failed
          && !browser_session_module_bytecode_disk_store(
                 &session, &key_a, bytecode, sizeof(bytecode)));
    CHECK(remove(blocked) == 0);
    for (unsigned i = 0; i < 20000u && !session.module_bytecode_disk_scanned;
         i++) (void) browser_session_module_bytecode_disk_maintenance(&session);
    CHECK(session.module_bytecode_disk_scanned
          && browser_session_module_bytecode_disk_store(
                 &session, &key_a, bytecode, sizeof(bytecode))
          && disk_test_files(blocked, NULL, 0) == 1);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    disk_test_clear(blocked);
    disk_test_clear(directory);
    return 0;
}

/* Rewrites every record in the directory to verified bytes that are not a
   module: they pass the file checks and fail the restore. */
static bool disk_test_poison(const char *directory)
{
    JSRuntime *js = JS_NewRuntime();
    JSContext *context = js == NULL ? NULL : JS_NewContext(js);
    size_t length = 0;
    uint8_t *other = context == NULL ? NULL
        : JS_WriteObject(context, &length, JS_NewInt32(context, 7), 0);
    bool ok = other != NULL;
    DIR *dir = ok ? opendir(directory) : NULL;
    struct dirent *entry;
    while (ok && dir != NULL && (entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".tfmb") == NULL) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        unsigned char header[76];
        FILE *file = fopen(path, "rb");
        ok = file != NULL && fread(header, 1, sizeof(header), file)
            == sizeof(header);
        if (file != NULL) fclose(file);
        header[8] = (unsigned char) length;
        header[9] = (unsigned char) (length >> 8);
        header[10] = header[11] = 0;
        ok = ok && tilefinch_sha256_digest(other, length, header + 44);
        file = ok ? fopen(path, "wb") : NULL;
        ok = file != NULL
            && fwrite(header, 1, sizeof(header), file) == sizeof(header)
            && fwrite(other, 1, length, file) == length;
        if (file != NULL) ok = fclose(file) == 0 && ok;
    }
    if (dir != NULL) closedir(dir);
    if (other != NULL) js_free(context, other);
    if (context != NULL) JS_FreeContext(context);
    if (js != NULL) JS_FreeRuntime(js);
    return ok;
}

/* A fresh session (nothing in RAM) restores every module from files a
   previous session wrote: no compile, identical behaviour. */
static int test_disk_tier_restores_across_sessions(void)
{
    static const char directory[] = "module-bytecode-disk-e2e";
    disk_test_clear(directory);
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    CHECK(cache_map_init(&map, "'one'"));
    static const char expected_one[] =
        "one|2|lazy|https://cdn.test/final/dep.js|"
        "https://cdn.test/final/leaf.js|https://mod.test/root.js";
    BrowserSession writer;
    CHECK(browser_session_init(&writer, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&writer, 1024u * 1024u);
    browser_session_module_bytecode_set_disk(&writer, directory, true);
    fixture.options.session = &writer;
    ScriptRuntime *runtime = NULL;
    ScriptResult first;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &first));
    CHECK(strcmp(first.summary, expected_one) == 0
          && first.module_bytecode_disk_stores == 5
          && disk_test_files(directory, NULL, 0) == 5);
    script_runtime_destroy(runtime);
    browser_session_destroy(&writer);

    BrowserSession reader;
    CHECK(browser_session_init(&reader, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&reader, 1024u * 1024u);
    browser_session_module_bytecode_set_disk(&reader, directory, false);
    fixture.options.session = &reader;
    ScriptResult second;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &second));
    if (strcmp(second.summary, expected_one) != 0)
        fprintf(stderr, "disk second: %s error=%s\n", second.summary,
                second.error);
    CHECK(strcmp(second.summary, expected_one) == 0
          && second.module_bytecode_disk_hits == 5
          && second.module_bytecode_cache_hits == 5
          && second.module_compile_count == 0
          && second.module_bytecode_disk_stores == 0);
    script_runtime_destroy(runtime);
    browser_session_destroy(&reader);

    /* Records the engine cannot restore: a writable session falls back to
       compiling, replaces them, and the next session restores again. */
    CHECK(disk_test_poison(directory));
    BrowserSession repair;
    CHECK(browser_session_init(&repair, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&repair, 1024u * 1024u);
    browser_session_module_bytecode_set_disk(&repair, directory, true);
    fixture.options.session = &repair;
    ScriptResult third;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &third));
    CHECK(strcmp(third.summary, expected_one) == 0
          && third.module_bytecode_cache_restore_failures == 5
          && third.module_compile_count == first.module_compile_count
          && third.module_bytecode_disk_stores == 5
          && repair.module_bytecode_disk_rejects == 5);
    script_runtime_destroy(runtime);
    browser_session_destroy(&repair);
    CHECK(browser_session_init(&reader, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&reader, 1024u * 1024u);
    browser_session_module_bytecode_set_disk(&reader, directory, false);
    fixture.options.session = &reader;
    ScriptResult fourth;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &fourth));
    CHECK(strcmp(fourth.summary, expected_one) == 0
          && fourth.module_bytecode_disk_hits == 5
          && fourth.module_compile_count == 0
          && fourth.module_bytecode_disk_load_us
                 >= fourth.module_bytecode_disk_read_us);
    script_runtime_destroy(runtime);
    browser_session_destroy(&reader);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    disk_test_clear(directory);
    return 0;
}

/* A record that no longer restores is dropped and the source compiled: the
   page cannot tell. */
static int test_unrestorable_entry_falls_back_to_source(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(browser_session_init(&session, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 1024u * 1024u);
    fixture.options.session = &session;
    ModuleMap map = {0};
    CHECK(cache_map_init(&map, "'one'"));
    ScriptRuntime *runtime = NULL;
    ScriptResult result;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &result));
    script_runtime_destroy(runtime);
    CHECK(session.module_bytecode != NULL);
    size_t damaged = 0;
    for (size_t i = 0; i < BROWSER_MODULE_BYTECODE_ENTRIES; i++) {
        BrowserModuleBytecodeEntry *entry =
            &session.module_bytecode->entries[i];
        if (entry->bytecode == NULL
            || strcmp(entry->module_name, "https://mod.test/dep.js") != 0)
            continue;
        entry->bytecode->length = 3;
        damaged++;
    }
    CHECK(damaged == 1);
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &result));
    if (strncmp(result.summary, "one|2|lazy|", 11) != 0) {
        fprintf(stderr, "fallback: %s error=%s\n", result.summary,
                result.error);
    }
    CHECK(strncmp(result.summary, "one|2|lazy|", 11) == 0
          && result.module_bytecode_cache_restore_failures == 1
          && result.module_bytecode_cache_hits == 4
          && result.module_bytecode_cache_misses == 1
          && result.module_bytecode_cache_stores == 1);
    script_runtime_destroy(runtime);
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &result));
    CHECK(result.module_bytecode_cache_hits == 5
          && result.module_bytecode_cache_restore_failures == 0);
    script_runtime_destroy(runtime);
    browser_session_destroy(&session);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* When a restored module's import cannot load, the page sees what it sees
   after a compile whose import fails - and a later import retries it. */
static int test_restored_module_import_failure_matches_compile(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(browser_session_init(&session, &fixture.budget, 512u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 1024u * 1024u);
    fixture.options.session = &session;
    ModuleMap map = {0};
    CHECK(cache_map_init(&map, "'one'"));
    ScriptRuntime *runtime = NULL;
    ScriptResult result;
    CHECK(run_root(&fixture, &map, cache_root, &runtime, &result));
    script_runtime_destroy(runtime);
    static const char retry[] =
        "globalThis.pocSummary='PENDING';"
        "import('https://mod.test/dep.js').then("
        "m=>{globalThis.pocSummary='RETRY:'+m.dep+'|'+m.depLeaf()},"
        "e=>{globalThis.pocSummary='RETRY-FAILED:'+e})";
    const char *outcomes[2][2] = {{0}};
    /* Restored graph first (root and dep come from the cache), then the
       same failure with nothing cached. */
    for (int restore = 1; restore >= 0; restore--) {
        if (!restore) browser_session_cache_clear(&session);
        map.available[1] = false;  /* leaf.js */
        CHECK(run_root(&fixture, &map, cache_root, &runtime, &result));
        ScriptModuleMapStatus root = script_runtime_module_map_status(
            runtime, "https://mod.test/root.js");
        outcomes[restore][0] = root == SCRIPT_MODULE_MAP_FAILED
            && result.external_scripts_failed == 1 ? "failed" : "other";
        size_t hits = result.module_bytecode_cache_hits;
        map.available[1] = true;
        CHECK(script_runtime_evaluate_diagnostic(runtime, retry, "<retry>",
                                                 &result));
        for (size_t turn = 0; turn < 8; turn++) {
            CHECK(script_runtime_advance(runtime, 0, 32, &result));
        }
        outcomes[restore][1] = strcmp(
            result.summary, "RETRY:one|https://cdn.test/final/leaf.js") == 0
            ? "retried" : result.summary;
        if (restore) CHECK(hits >= 2);
        else CHECK(hits == 0);
        script_runtime_destroy(runtime);
    }
    if (strcmp(outcomes[1][0], outcomes[0][0]) != 0
        || strcmp(outcomes[1][1], outcomes[0][1]) != 0
        || strcmp(outcomes[0][0], "failed") != 0
        || strcmp(outcomes[0][1], "retried") != 0) {
        fprintf(stderr, "compile: %s/%s restore: %s/%s\n", outcomes[0][0],
                outcomes[0][1], outcomes[1][0], outcomes[1][1]);
    }
    CHECK(strcmp(outcomes[1][0], outcomes[0][0]) == 0
          && strcmp(outcomes[1][1], outcomes[0][1]) == 0
          && strcmp(outcomes[0][0], "failed") == 0
          && strcmp(outcomes[0][1], "retried") == 0);
    browser_session_destroy(&session);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Session-level bounds: ceiling, per-realm protection, compile-flag and
   source identity, captive portal, reclaim, and exact Budget return. */
static int test_session_module_bytecode_bounds(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 256u * 1024u));
    size_t baseline = budget.current;
    static unsigned char bytecode[40u * 1024u];
    memset(bytecode, 0x5a, sizeof(bytecode));
    static const char source_a[] = "export const a=1;";
    static const char source_b[] = "export const b=2;";
    BrowserModuleBytecodeKey key_a = {
        .module_name = "https://s.test/a.js",
        .response_url = "https://s.test/a.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source_a,
        .source_length = sizeof(source_a) - 1u
    };
    /* Disabled until a limit is set. */
    CHECK(!browser_session_module_bytecode_put(
        &session, &key_a, 1, bytecode, sizeof(bytecode)));
    browser_session_module_bytecode_set_limit(&session, 100u * 1024u);
    uint32_t first = browser_session_module_bytecode_generation(&session);
    uint32_t second = browser_session_module_bytecode_generation(&session);
    CHECK(first != 0 && second != 0 && first != second);
    CHECK(browser_session_module_bytecode_put(
        &session, &key_a, first, bytecode, sizeof(bytecode)));
    /* Same record, different bytes or compile flags: miss. */
    BrowserModuleBytecodeKey changed = key_a;
    changed.source = (const unsigned char *) source_b;
    changed.digest_ready = false;
    BrowserModuleBytecodeKey flagged = key_a;
    flagged.compile_flags = 1;
    flagged.digest_ready = false;
    CHECK(browser_session_module_bytecode_acquire(&session, &changed, first)
              == NULL
          && browser_session_module_bytecode_acquire(&session, &flagged,
                                                     first) == NULL);
    BrowserSharedBody *hit = browser_session_module_bytecode_acquire(
        &session, &key_a, first);
    CHECK(hit != NULL && hit->length == sizeof(bytecode));
    browser_shared_body_release(hit);
    /* Two 40 KiB records fill a 100 KiB ceiling; a third from the same
       realm is refused rather than evicting one that realm used... */
    char names[3][64];
    BrowserModuleBytecodeKey keys[3];
    for (int i = 0; i < 3; i++) {
        snprintf(names[i], sizeof(names[i]), "https://s.test/m%d.js", i);
        keys[i] = key_a;
        keys[i].module_name = names[i];
        keys[i].response_url = names[i];
        keys[i].digest_ready = false;
    }
    CHECK(browser_session_module_bytecode_put(
        &session, &keys[0], first, bytecode, sizeof(bytecode)));
    CHECK(!browser_session_module_bytecode_may_fit(
              &session, &keys[1], sizeof(bytecode), first)
          && !browser_session_module_bytecode_put(
              &session, &keys[1], first, bytecode, sizeof(bytecode))
          && browser_session_module_bytecode_entries(&session) == 2);
    /* ...while a later realm may evict the least recently used. */
    CHECK(browser_session_module_bytecode_put(
              &session, &keys[1], second, bytecode, sizeof(bytecode))
          && browser_session_module_bytecode_entries(&session) == 2
          && browser_session_module_bytecode_bytes(&session)
                 <= 100u * 1024u);
    key_a.digest_ready = false;
    CHECK(browser_session_module_bytecode_acquire(&session, &key_a, second)
          == NULL);
    /* Nothing is read or written during a captive sign-in. */
    CHECK(browser_session_captive_portal_begin(&session,
                                               "http://portal.test/"));
    keys[1].digest_ready = false;
    CHECK(browser_session_module_bytecode_acquire(&session, &keys[1],
                                                  second) == NULL
          && !browser_session_module_bytecode_put(
              &session, &keys[2], second + 1, bytecode, 64));
    browser_session_captive_portal_end(&session);
    /* The reclaim hook path releases everything it is asked for. */
    size_t held = browser_session_module_bytecode_bytes(&session);
    CHECK(browser_session_module_bytecode_reclaim(&session, SIZE_MAX) == held
          && browser_session_module_bytecode_entries(&session) == 0);
    browser_session_module_bytecode_set_limit(&session, 0);
    CHECK(session.module_bytecode == NULL && budget.current == baseline);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* The cache holds room the page is not using: an allocation that would be
   refused evicts it instead. */
static int test_budget_reclaims_module_bytecode_before_refusing(void)
{
    Budget budget;
    budget_init(&budget, 1u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * 1024u));
    browser_session_module_bytecode_set_limit(&session, 512u * 1024u);
    static unsigned char bytecode[384u * 1024u];
    static const char source[] = "export const held=1;";
    BrowserModuleBytecodeKey key = {
        .module_name = "https://s.test/held.js",
        .response_url = "https://s.test/held.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source,
        .source_length = sizeof(source) - 1u
    };
    uint32_t generation = browser_session_module_bytecode_generation(
        &session);
    CHECK(browser_session_module_bytecode_put(
        &session, &key, generation, bytecode, sizeof(bytecode)));
    size_t room = budget_remaining(&budget);
    size_t request = room + 128u * 1024u;
    CHECK(budget_malloc(&budget, request) == NULL);
    budget_set_reclaim_hook(&budget, reclaim_module_bytecode,
                            &session);
    void *allocation = budget_malloc(&budget, request);
    CHECK(allocation != NULL
          && browser_session_module_bytecode_entries(&session) == 0
          && budget.reclaim_calls == 1);
    budget_free(&budget, allocation);
    budget_set_reclaim_hook(&budget, NULL, NULL);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* import() from a classic script names a module no module script has
   requested yet. Its referrer is the classic script's base URL (here the
   document). chatgpt.com's inline bootstrap does exactly this; the rejection
   made its startup watchdog reload every first visit. */
static int test_classic_script_dynamic_import(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    ModuleMap map = {0};
    CHECK(module_map_add(&map, "https://mod.test/fresh.js",
              "https://mod.test/fresh.js",
              copy_text("export {w} from './inner.js';export const v=7;", 46))
          && module_map_add(&map, "https://mod.test/inner.js",
              "https://mod.test/inner.js",
              copy_text("export const w=8;", 17)));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000,
        "https://mod.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_module_loader(
        runtime, module_map_load, module_map_release, &map);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary='PENDING';import('./fresh.js').then("
        "m=>{globalThis.pocSummary='OK:'+m.v+m.w},"
        "e=>{globalThis.pocSummary='FAIL:'+e})",
        "<inline-script>", &result));
    for (size_t turn = 0; turn < 8; turn++) {
        CHECK(script_runtime_advance(runtime, 0, 32, &result));
    }
    char summary[256];
    snprintf(summary, sizeof(summary), "%s", result.summary);
    bool ok = strcmp(summary, "OK:78") == 0
        && strcmp(map.referrer[0], "https://mod.test/") == 0
        && strcmp(map.referrer[1], "https://mod.test/fresh.js") == 0;
    if (!ok) fprintf(stderr, "classic import: %s referrers=%s,%s\n",
                     summary, map.referrer[0], map.referrer[1]);
    script_runtime_destroy(runtime);
    module_map_free(&map);
    CHECK(ok);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The configured script-source total is a floor: past it a script is still
   admitted while the page Budget keeps the realm's reserve free. On the PSP
   chatgpt.com's startup committed ~2 MiB of modules, and every module its
   send path imports was then refused. */
static int test_script_source_total_follows_memory_pressure(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    fixture.options.maximum_scripts = 8;
    fixture.options.maximum_script_bytes = 64u * 1024u;
    fixture.options.maximum_script_file_bytes = 32u * 1024u;
    const char *outcomes[2] = {0};
    for (int grow = 0; grow < 2; grow++) {
        ScriptResult result;
        memset(&result, 0, sizeof(result));
        ScriptRuntime *runtime = script_runtime_create_configured(
            &fixture.document, &fixture.budget, 8u * MIB, 8000,
            "https://mod.test/", &fixture.options, &result);
        CHECK(runtime != NULL);
        size_t reserve = fixture.budget.limit / 5u;
        if (grow) script_runtime_enable_heap_growth(runtime, 16u * MIB,
                                                    reserve);
        /* Three 20 KB scripts fit the 64 KiB total; a fourth passes it. */
        bool fits = true;
        for (int i = 0; i < 3; i++) {
            ScriptQuotaReservation early = {0};
            fits = fits && script_runtime_script_quota_reserve(
                    runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 20000u, &early)
                == SCRIPT_QUOTA_RESERVE_GRANTED
                && script_runtime_script_quota_commit(runtime, &early,
                                                      20000u);
        }
        ScriptQuotaReservation second = {0};
        ScriptQuotaReserveResult past = script_runtime_script_quota_reserve(
            runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 20000u, &second);
        bool committed = past == SCRIPT_QUOTA_RESERVE_GRANTED
            && script_runtime_script_quota_commit(runtime, &second, 20000u);
        if (!committed && second.active)
            script_runtime_script_quota_abort(runtime, &second);
        outcomes[grow] = !fits ? "first-refused"
            : committed ? "admitted" : "refused";
        script_runtime_destroy(runtime);
    }
    if (strcmp(outcomes[0], "refused") != 0
        || strcmp(outcomes[1], "admitted") != 0)
        fprintf(stderr, "source total: fixed=%s growth=%s\n",
                outcomes[0], outcomes[1]);
    CHECK(strcmp(outcomes[0], "refused") == 0
          && strcmp(outcomes[1], "admitted") == 0);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Nested imports load inside the importer's compile (QuickJS resolves them
   in the JS_Eval that compiles it), so each module's compile/parse time must
   exclude exactly its imports' loads: root -> a -> b -> c, where a is
   expensive to parse and c is slow to fetch. Each loader call has to add
   its load exclusive of its own children to the nested total; adding the
   inclusive time at every level subtracted c from a twice, which zeroed
   a's (and root's) compile and left the parts summing well below the whole. */
static char *parse_heavy_module(const char *import_line, size_t functions)
{
    static const char item[] =
        "export function f%05zu(x){let s=0;for(let i=0;i<x;i++){"
        "s+=[i,i+1,i*2].map(v=>v^i).reduce((p,q)=>p+q,0);"
        "s-=[i,i+2,i*3].map(v=>v|i).reduce((p,q)=>p*q,1);"
        "s^=[i,i+3,i*4].filter(v=>v&i).reduce((p,q)=>p-q,2);"
        "s+=[i,i+4,i*5].map(v=>v<<i).reduce((p,q)=>p%%q,3)}return s}\n";
    size_t prefix = strlen(import_line);
    size_t capacity = prefix + functions * (sizeof(item) + 8u) + 1u;
    char *source = malloc(capacity);
    if (source == NULL) return NULL;
    memcpy(source, import_line, prefix);
    size_t used = prefix;
    for (size_t i = 0; i < functions; i++) {
        used += (size_t) snprintf(source + used, capacity - used, item, i);
    }
    return source;
}

static int test_nested_module_compile_time_is_exclusive(void)
{
    ModuleFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char b_source[] =
        "import {c} from './c.js';export const b=c+1;";
    static const char c_source[] = "export const c=1;";
    ModuleMap map = {0};
    CHECK(module_map_add(&map, "https://mod.test/a.js",
                         "https://mod.test/a.js",
                         parse_heavy_module(
                             "import {b} from './b.js';\n"
                             "export const a=b+1;\n", 1500u))
          && module_map_add(&map, "https://mod.test/b.js",
                            "https://mod.test/b.js",
                            copy_text(b_source, strlen(b_source)))
          && module_map_add(&map, "https://mod.test/c.js",
                            "https://mod.test/c.js",
                            copy_text(c_source, strlen(c_source))));
    map.delay_ms[1] = 10u;
    map.delay_ms[2] = 120u;
    static const char root[] =
        "import {a} from './a.js';globalThis.pocSummary='A:'+a";
    ScriptRuntime *runtime = NULL;
    ScriptResult result;
    CHECK(run_root(&fixture, &map, root, &runtime, &result));
    /* The root's compile, inclusive of every nested load and compile. */
    unsigned long long total =
        result.compile_us[SCRIPT_COMPILE_SOURCE_MODULE];
    unsigned long long fetch = result.module_fetch_us;
    unsigned long long parts = fetch + result.module_key_us
        + result.module_parse_us + result.module_store_us;
    unsigned long long compile_only = total > fetch ? total - fetch : 0;
    bool ok = strcmp(result.summary, "A:3") == 0
        && result.module_compile_count == 3u
        && fetch >= 130000u
        /* Nothing counted twice upward: the exclusive parts fit inside
           the whole, and module compile excludes every source load. */
        && parts <= total + 1000u
        && result.module_compile_us <= compile_only + 1000u
        /* Nothing subtracted twice: the parts account for the whole
           (a's parse dominates what is not a source load), and a's
           exclusive compile is most of the non-load time. */
        && total - (parts < total ? parts : total) < compile_only / 4u
        && result.module_compile_us >= compile_only / 2u;
    if (!ok || getenv("TILEFINCH_TEST_VERBOSE") != NULL) {
        fprintf(stderr,
                "nested compile: summary=%s error=%s count=%zu total=%llu "
                "fetch=%llu key=%llu parse=%llu store=%llu compile=%llu\n",
                result.summary, result.error, result.module_compile_count,
                total, fetch,
                result.module_key_us, result.module_parse_us,
                result.module_store_us, result.module_compile_us);
    }
    CHECK(ok);
    script_runtime_destroy(runtime);
    module_map_free(&map);
    CHECK(fixture_close(&fixture));
    return 0;
}

int main(void)
{
    CHECK(test_large_module_heap_excludes_source() == 0);
    CHECK(test_large_module_source_is_not_retained() == 0);
    CHECK(test_uncalled_commented_function_keeps_no_text() == 0);
    puts("module source retention: PASS");
    CHECK(test_page_functions_compile_on_first_call() == 0);
    puts("lazy page functions: PASS");
    CHECK(test_session_module_bytecode_bounds() == 0);
    CHECK(test_budget_reclaims_module_bytecode_before_refusing() == 0);
    puts("module bytecode session bounds: PASS");
    CHECK(test_second_load_restores_modules() == 0);
    CHECK(test_unrestorable_entry_falls_back_to_source() == 0);
    CHECK(test_restored_module_import_failure_matches_compile() == 0);
    puts("module bytecode cache: PASS");
    CHECK(test_session_module_bytecode_disk() == 0);
    CHECK(test_session_module_bytecode_disk_housekeeping() == 0);
    CHECK(test_session_module_bytecode_disk_eviction() == 0);
    CHECK(test_disk_tier_restores_across_sessions() == 0);
    puts("module bytecode persistent tier: PASS");
    CHECK(test_classic_script_dynamic_import() == 0);
    puts("classic script dynamic import: PASS");
    CHECK(test_script_source_total_follows_memory_pressure() == 0);
    puts("script source total follows memory pressure: PASS");
    CHECK(test_nested_module_compile_time_is_exclusive() == 0);
    puts("nested module compile time is exclusive: PASS");
    return 0;
}
