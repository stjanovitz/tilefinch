/* Installed-app launch through the real offline route on a BrowserEngine.

   Treadline Arena is staged into the session cache, installed with the real
   preview/save path and opened through psp_offline_store_handle_url, exactly
   as the PSP Library does. The test checks that the installed-app heap floor
   reaches only that app's realm, and reports what the running game needs
   from the page Budget besides JavaScript (the evidence behind the default
   floor). TILEFINCH_APP_LAUNCH_FRAMES lengthens the measured soak. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/psp_offline_store.h"
#include "tilefinch/sha256.h"
#include "tilefinch/web_app_manifest.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "script_test_support.h"

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define KIB 1024u
#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false;                                                        \
    }                                                                        \
} while (0)

#define TREADLINE_BASE "https://games.test/examples/treadline-arena/"

static char *read_source(const char *relative, size_t *length)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", TILEFINCH_TEST_SOURCE_DIR,
             relative);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) size = ftell(file);
    char *data = size > 0 && fseek(file, 0, SEEK_SET) == 0
        ? malloc((size_t) size + 1u) : NULL;
    bool okay = data != NULL
        && fread(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!okay) {
        free(data);
        return NULL;
    }
    data[size] = '\0';
    *length = (size_t) size;
    return data;
}

static BrowserEngine *make_engine(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    if (!browser_config_apply_psp_memory_profile(
            &config, BROWSER_PSP_MEMORY_REALISTIC)) return NULL;
    /* Mirror the shipping EBOOT's live-cache preference. */
    config.session_cache_limit = 640u * KIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    /* Measurement override only; the assertions use the default floor. */
    const char *heap_env = getenv("TILEFINCH_APP_LAUNCH_HEAP_MB");
    if (heap_env != NULL)
        config.javascript.installed_app_heap_limit =
            (size_t) strtoul(heap_env, NULL, 10) * MIB;
    char fonts[7][512];
    static const char *names[7] = {
        "DejaVuSans-Latin.ttf", "DejaVuSerif-Latin.ttf",
        "DejaVuSans-Oblique-Latin.ttf", "DejaVuSans-Bold-Latin.ttf",
        "DejaVuSerif-Bold-Latin.ttf", "TilefinchSans-Regular.ttf",
        "TilefinchSans-Bold.ttf"
    };
    for (size_t at = 0; at < 7u; at++)
        snprintf(fonts[at], sizeof(fonts[at]), "%s/fonts/%s",
                 TILEFINCH_TEST_SOURCE_DIR, names[at]);
    if (!browser_config_set_font_paths(
            &config, fonts[0], fonts[1], fonts[2], fonts[3], fonts[4],
            fonts[5], fonts[6], 1536u * KIB)) return NULL;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine: %s\n", error);
    return engine;
}

static bool cache_resource(BrowserEngine *engine, const char *page_url,
                           const char *url, const char *content_type,
                           TilefinchRequestDestination destination,
                           const char *data, size_t length)
{
    Budget *budget = browser_engine_budget(engine);
    unsigned char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1u);
    if (copy == NULL) return false;
    memcpy(copy, data, length);
    copy[length] = 0;
    BrowserSharedBody *body = browser_shared_body_take(budget, copy, length);
    if (body == NULL) {
        budget_free(budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = page_url,
        .top_level_url = page_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = destination
    };
    TilefinchResourceGrant grant = {
        .destination = destination, .mode = context.mode,
        .credentials = context.credentials, .final_same_origin = true,
        .final_same_site = true, .mime_validated = true
    };
    bool stored = browser_session_cache_put_http_shared_resource(
        browser_engine_session(engine), url, body, "", "", content_type,
        "public,max-age=3600", "", 1u, &context, &grant);
    browser_shared_body_release(body);
    return stored;
}

/* Stage Treadline's responses as a live page would have left them, then
   install it under `page_url` (its query selects a qualification mode).
   The files are the canonical package list's stylesheet and scripts
   (examples/treadline-arena/package-files.txt); a qualification package
   adds the entries it marks "qualification". */
static bool install_treadline(BrowserEngine *engine, OfflineLibrary *library,
                              const char *page_url, bool qualification,
                              uint32_t *id)
{
    size_t list_length = 0;
    char *list = read_source("examples/treadline-arena/package-files.txt",
                             &list_length);
    CHECK(list != NULL);
    /* Installation needs the whole working set in the live cache, as the
       Game Profile documents (the Memory cache setting). */
    CHECK(browser_session_cache_set_maximum_bytes(
        browser_engine_session(engine), 2u * MIB));
    size_t staged = 0;
    for (char *line = strtok(list, "\n"); line != NULL;
         line = strtok(NULL, "\n")) {
        char name[64], tag[32] = {0};
        int fields = sscanf(line, "%63s %31s", name, tag);
        if (fields < 1 || name[0] == '#') continue;
        if (fields == 2 && (!qualification || strcmp(tag, "qualification") != 0))
            continue;
        const char *dot = strrchr(name, '.');
        bool script = dot != NULL && strcmp(dot, ".js") == 0;
        bool style = dot != NULL && strcmp(dot, ".css") == 0;
        if (!script && !style) continue;
        char relative[160], url[192];
        snprintf(relative, sizeof(relative), "examples/treadline-arena/%s",
                 name);
        snprintf(url, sizeof(url), TREADLINE_BASE "%s", name);
        size_t length = 0;
        char *data = read_source(relative, &length);
        bool cached = data != NULL
            && cache_resource(engine, page_url, url,
                              script ? "text/javascript" : "text/css",
                              script ? TILEFINCH_DESTINATION_SCRIPT
                                     : TILEFINCH_DESTINATION_STYLE,
                              data, length);
        free(data);
        if (!cached) { free(list); return false; }
        staged++;
    }
    free(list);
    CHECK(staged >= 8u);
    size_t html_length = 0, manifest_length = 0;
    char *html = read_source("examples/treadline-arena/index.html",
                             &html_length);
    char *manifest_json = read_source(
        "examples/treadline-arena/manifest.webmanifest", &manifest_length);
    CHECK(html != NULL && manifest_json != NULL);
    PocDocument document;
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    bool okay = document_parse(&document, browser_engine_budget(engine),
                               html, html_length, 512);
    okay = okay && tilefinch_web_app_manifest_parse(
        manifest_json, manifest_length, TREADLINE_BASE "manifest.webmanifest",
        page_url, &manifest, error, sizeof(error));
    okay = okay && offline_library_save_web_app(
        library, &document, browser_engine_session(engine), page_url,
        &manifest, NULL, 0, id, error, sizeof(error));
    if (!okay) fprintf(stderr, "install: %s\n", error);
    document_destroy(&document);
    free(html);
    free(manifest_json);
    browser_session_cache_clear(browser_engine_session(engine));
    return okay;
}

static bool launch(PspOfflineStore *store, BrowserEngine *engine,
                   BrowserProfile *profile, uint32_t id)
{
    char url[96];
    snprintf(url, sizeof(url), "https://tilefinch.local/offline/app?id=%u",
             (unsigned) id);
    PspOfflineRouteResult result = psp_offline_store_handle_url(
        store, engine, profile, url, NULL, true);
    if (result != PSP_OFFLINE_ROUTE_PAGE)
        fprintf(stderr, "launch: %s\n", psp_offline_store_status(store));
    return result == PSP_OFFLINE_ROUTE_PAGE;
}

static void budget_reset_peaks(Budget *budget)
{
    budget->peak = budget->current;
    for (unsigned at = 0; at < BUDGET_CATEGORY_COUNT; at++) {
        budget->categories[at].peak = budget->categories[at].current;
        budget->global_peak_categories[at] = budget->categories[at].current;
    }
}

static size_t realm_floor(BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    ScriptHeapGrowth growth;
    script_runtime_heap_growth(navigation->page.runtime, &growth);
    return navigation->page.runtime == NULL ? 0 : growth.floor;
}

static bool source_resident(BrowserEngine *engine, const char *url)
{
    BrowserSession *session = browser_engine_session(engine);
    for (size_t at = 0; at < BROWSER_CACHE_ENTRIES; at++)
        if (session->cache[at].data != NULL
            && strcmp(session->cache[at].url, url) == 0) return true;
    return false;
}

/* The installed-app heap reaches only the app launched from the library,
   and the page Budget that pays for it is unchanged. */
static bool installed_app_heap_scope(const char *directory)
{
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    Budget *budget = browser_engine_budget(engine);
    const size_t page_limit = budget->limit;
    NavigationSession *navigation = browser_engine_navigation(engine);
    OfflineLibrary library;
    offline_library_init(&library, budget, directory);
    uint32_t id = 0;
    static const char page_url[] =
        TREADLINE_BASE "index.html?qualification=soak";
    CHECK(install_treadline(engine, &library, page_url, true, &id));
    PspOfflineStore store;
    psp_offline_store_init(&store, budget, browser_engine_session(engine),
                           directory);
    BrowserProfile *profile = browser_profile_create(budget);
    CHECK(profile != NULL);
    budget_reset_peaks(budget);
    CHECK(launch(&store, engine, profile, id));
    size_t launch_peak = budget->peak;
    size_t launch_js_peak = budget->categories[BUDGET_CATEGORY_JAVASCRIPT].peak;
    size_t launch_heap = script_runtime_heap_used(navigation->page.runtime);
    fprintf(stderr, "launch: runtime=%d scripts=%zu/%zu failed=%zu raises=%zu "
            "armed=%zu floor=%zu error=%s\n",
            navigation->page.runtime != NULL, navigation->script_loaded,
            navigation->script_discovered, navigation->script_failed,
            navigation->installed_app_heap_raises,
            navigation->next_top_level_js_heap, realm_floor(engine),
            navigation->page.script_result.error);
    CHECK(navigation->page.runtime != NULL
          && navigation->next_top_level_js_heap == 0u
          && realm_floor(engine) == (getenv("TILEFINCH_APP_LAUNCH_HEAP_MB")
                  == NULL ? (size_t) BROWSER_PSP_APP_INSTALLED_APP_HEAP_MB * MIB
                          : (size_t) strtoul(getenv(
                                "TILEFINCH_APP_LAUNCH_HEAP_MB"), NULL, 10)
                                * MIB)
          && budget->limit == page_limit);

    /* Soak the self-driving qualification match and record what the page
       holds besides QuickJS. */
    const char *frames_env = getenv("TILEFINCH_APP_LAUNCH_FRAMES");
    unsigned frames = frames_env == NULL ? 240u
        : (unsigned) strtoul(frames_env, NULL, 10);
    size_t heap_peak = 0, js_budget_peak = 0, other_peak = 0;
    size_t total_peak = 0;
    budget_reset_peaks(budget);
    for (unsigned frame = 0; frame < frames; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed)
              && browser_engine_render_frame(engine, NULL));
        size_t heap = script_runtime_heap_used(navigation->page.runtime);
        size_t js = budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current;
        if (heap > heap_peak) heap_peak = heap;
        if (js > js_budget_peak) js_budget_peak = js;
        if (budget->current - js > other_peak)
            other_peak = budget->current - js;
        if (budget->current > total_peak) total_peak = budget->current;
    }
    ScriptHeapGrowth growth;
    script_runtime_heap_growth(navigation->page.runtime, &growth);
    printf("OFFLINE-APP-HEAP launch_page_peak=%zu launch_js_budget_peak=%zu "
           "launch_js_heap=%zu\n", launch_peak, launch_js_peak, launch_heap);
    printf("OFFLINE-APP-HEAP frames=%u floor=%zu limit=%zu peak_limit=%zu "
           "raises=%zu pregrows=%zu refusals=%zu js_heap_peak=%zu "
           "js_budget_peak=%zu non_js_peak=%zu page_peak=%zu "
           "page_limit=%zu rejections=%zu\n",
           frames, growth.floor, growth.limit, growth.peak_limit,
           growth.raises, growth.pregrows, growth.refusals, heap_peak,
           js_budget_peak, other_peak, total_peak, page_limit,
           script_runtime_heap_rejections(navigation->page.runtime));
    for (unsigned category = 0; category < BUDGET_CATEGORY_COUNT; category++)
        printf("OFFLINE-APP-HEAP category=%s peak=%zu at_global_peak=%zu\n",
               budget_category_name((BudgetCategory) category),
               budget->categories[category].peak,
               budget->global_peak_categories[category]);
    ScriptResult probe;
    CHECK(script_runtime_evaluate_diagnostic(
        navigation->page.runtime,
        "globalThis.pocSummary=String(!!globalThis.__treadlineBootReady)+':'"
        "+document.getElementById('score').textContent+':'"
        "+document.getElementById('arena').textContent",
        "<treadline-probe>", &probe));
    fprintf(stderr, "soak: scripts=%zu/%zu failed=%zu bytecode-hits=%zu "
            "misses=%zu state=%s\n",
            navigation->script_loaded, navigation->script_discovered,
            navigation->script_failed,
            navigation->page.script_result.external_script_bytecode_cache_hits,
            navigation->page.script_result
                .external_script_bytecode_cache_misses, probe.summary);
    CHECK(script_runtime_heap_rejections(navigation->page.runtime) == 0
          && navigation->script_failed == 0
          /* All seven deferred game scripts, and the qualification.js
             a ?qualification= page fetches, ran from bytecode; the game
             scripts' source never left the Memory Stick. */
          && navigation->page.script_result
                 .external_script_bytecode_cache_hits == 8u
          && !source_resident(engine, TREADLINE_BASE "game.js")
          && !source_resident(engine, TREADLINE_BASE "bots.js")
          && !source_resident(engine, TREADLINE_BASE "arena-generator.js")
          && !source_resident(engine, TREADLINE_BASE "practice.js")
          && !source_resident(engine, TREADLINE_BASE "campaign.js")
          && !source_resident(engine, TREADLINE_BASE "controls.js")
          && !source_resident(engine, TREADLINE_BASE "music.js")
          && browser_engine_session(engine)->offline_deferred_failures == 0);

    /* The floor is really admitted and still Budget-accounted: with the
       page Budget held at its growth reserve (so no realm may grow), the
       app can still allocate up to its floor, charged to the JavaScript
       category, and nothing past it. An ordinary 5 MiB realm could not.
       The probe is sized from what the game leaves, not a fixed number, so
       ordinary game growth does not turn this into a size test; the game's
       headroom is checked on its own below. FLOOR_PROBE_SLACK covers
       QuickJS's own overhead for the allocation (array object, GC
       threshold); 64 KiB was enough when measured (2026-10), and the rest
       absorbs collection-timing variance. */
    enum { FLOOR_PROBE_SLACK = 512u * 1024u };
    /* The Game Profile promises installed games room to grow: Treadline,
       about 5.6 MiB in use, must leave at least this much of the 9 MiB
       floor. It had about 3.4 MiB (2026-10); a failure here means the game
       grew by about 0.9 MiB, which should be a decision, not drift. */
    const size_t minimum_headroom = 5u * MIB / 2u;
    size_t reserve = budget->limit / 8u;
    size_t spare = budget_remaining(budget);
    CHECK(spare > reserve + 2u * MIB);
    void *pressure = budget_malloc(budget, spare - reserve - MIB);
    CHECK(pressure != NULL);
    size_t js_before = budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current;
    ScriptHeapGrowth before;
    script_runtime_heap_growth(navigation->page.runtime, &before);
    size_t used = script_runtime_heap_used(navigation->page.runtime);
    size_t headroom = before.floor > used ? before.floor - used : 0u;
    fprintf(stderr, "floor headroom: floor=%zu used=%zu headroom=%zu "
            "minimum=%zu\n", before.floor, used, headroom, minimum_headroom);
    CHECK(headroom >= minimum_headroom);
    /* Whole 64 KiB units, and always past what an ordinary realm admits. */
    size_t probe_bytes = (headroom - FLOOR_PROBE_SLACK) & ~(size_t) 0xffffu;
    CHECK(used + probe_bytes > 5u * MIB);
    char probe_source[512];
    int probe_length = snprintf(
        probe_source, sizeof(probe_source),
        "globalThis.__floorProbe=new Uint8Array(%zu);"
        "__floorProbe[%zu]=7;var refused='no';"
        "try{globalThis.__pastFloor=new Uint8Array(%zu);}"
        "catch(e){refused='yes';}"
        "globalThis.pocSummary='floor:'+__floorProbe[%zu]+':'+refused;",
        probe_bytes, probe_bytes - 1u, (size_t) before.floor,
        probe_bytes - 1u);
    CHECK(probe_length > 0 && (size_t) probe_length < sizeof(probe_source));
    ScriptResult big;
    CHECK(script_runtime_evaluate_diagnostic(
        navigation->page.runtime, probe_source, "<floor-probe>", &big));
    ScriptHeapGrowth after;
    script_runtime_heap_growth(navigation->page.runtime, &after);
    size_t js_after = budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current;
    fprintf(stderr, "floor probe: %s probe=%zu js=%zu->%zu limit=%zu->%zu\n",
            big.summary, probe_bytes, js_before, js_after, before.limit,
            after.limit);
    CHECK(strcmp(big.summary, "floor:7:yes") == 0
          && js_after >= js_before + probe_bytes
          && after.limit == before.limit
          && budget->current <= budget->limit);
    CHECK(script_runtime_evaluate_diagnostic(
        navigation->page.runtime,
        "globalThis.__floorProbe=null;globalThis.__pastFloor=null;",
        "<floor-release>", &big));
    budget_free(budget, pressure);

    /* Leaving the app in the same tab returns to the ordinary heap. */
    static const char next[] =
        "<!doctype html><title>Next</title><script>var x=1;</script>";
    CHECK(browser_engine_commit_html(
              engine, "https://example.test/next.html", next,
              sizeof(next) - 1u, true)
          && navigation->page.runtime != NULL
          && realm_floor(engine) == 5u * MIB);
    /* An ordinary navigation to the app's own URL is not a library launch. */
    static const char again[] =
        "<!doctype html><title>Again</title><script>var y=2;</script>";
    CHECK(browser_engine_commit_html(engine, page_url, again,
                                     sizeof(again) - 1u, true)
          && realm_floor(engine) == 5u * MIB
          && navigation->installed_app_heap_raises == 1u);
    /* A launch that fails before committing leaves nothing armed. */
    CHECK(psp_offline_store_handle_url(
              &store, engine, profile,
              "https://tilefinch.local/offline/app?id=4040", NULL, true)
          == PSP_OFFLINE_ROUTE_ERROR
          && navigation->next_top_level_js_heap == 0u);

    psp_offline_store_destroy(&store);
    browser_profile_destroy(profile);
    CHECK(offline_library_remove(&library, id));
    browser_engine_destroy(engine);
    return true;
}

static uint32_t fnv32(const unsigned char *data, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t at = 0; at < length; at++)
        hash = (hash ^ data[at]) * UINT32_C(16777619);
    return hash;
}

static void base64(const unsigned char *data, size_t length, char *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0;
    for (size_t at = 0; at < length; at += 3) {
        uint32_t value = (uint32_t) data[at] << 16
            | (at + 1 < length ? (uint32_t) data[at + 1] << 8 : 0)
            | (at + 2 < length ? data[at + 2] : 0);
        output[used++] = alphabet[(value >> 18) & 63];
        output[used++] = alphabet[(value >> 12) & 63];
        output[used++] = at + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        output[used++] = at + 2 < length ? alphabet[value & 63] : '=';
    }
    output[used] = '\0';
}

#define TRIGGERS_BASE "https://apps.test/triggers/"

/* Each way a launched app can need a script's source reads it on demand,
   and the script that needs nothing runs from bytecode alone. */
static bool on_demand_triggers(const char *directory)
{
    static const char plain[] =
        "globalThis.ran=(globalThis.ran||'')+'plain;';"
        "globalThis.probeText=String(function probe(){return 41+1;});";
    static const char sri[] = "globalThis.ran=(globalThis.ran||'')+'sri;';";
    static const char broken[] =
        "globalThis.ran=(globalThis.ran||'')+'broken;';";
    static const char dynamic[] =
        "globalThis.ran=(globalThis.ran||'')+'dynamic;';";
    unsigned char digest[32];
    char digest64[48];
    CHECK(tilefinch_sha256_digest(
        (const uint8_t *) sri, sizeof(sri) - 1u, digest));
    base64(digest, sizeof(digest), digest64);
    char html[1024];
    int html_length = snprintf(
        html, sizeof(html),
        "<!doctype html><title>Triggers</title>"
        "<link rel=manifest href=manifest.webmanifest>"
        "<script src=plain.js></script>"
        "<script src=sri.js integrity=\"sha256-%s\"></script>"
        "<script src=broken.js></script>"
        "<script>var s=document.createElement('script');s.src='dynamic.js';"
        "document.head.appendChild(s);</script>", digest64);
    CHECK(html_length > 0 && (size_t) html_length < sizeof(html));

    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    static const char page_url[] = TRIGGERS_BASE "index.html";
    static const struct { const char *name; const char *text; } files[] = {
        {"plain.js", plain}, {"sri.js", sri}, {"broken.js", broken},
        {"dynamic.js", dynamic}
    };
    CHECK(browser_session_cache_set_maximum_bytes(session, 2u * MIB));
    for (size_t at = 0; at < 4u; at++) {
        char url[128];
        snprintf(url, sizeof(url), TRIGGERS_BASE "%s", files[at].name);
        CHECK(cache_resource(engine, page_url, url, "text/javascript",
                             TILEFINCH_DESTINATION_SCRIPT, files[at].text,
                             strlen(files[at].text)));
    }
    PocDocument document;
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Triggers\"}";
    CHECK(document_parse(&document, budget, html, (size_t) html_length, 512)
          && tilefinch_web_app_manifest_parse(
              manifest_json, sizeof(manifest_json) - 1u,
              TRIGGERS_BASE "manifest.webmanifest", page_url, &manifest,
              error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, budget, directory);
    uint32_t id = 0;
    CHECK(offline_library_save_web_app(
        &library, &document, session, page_url, &manifest, NULL, 0, &id,
        error, sizeof(error)));
    document_destroy(&document);
    browser_session_cache_clear(session);

    /* Damage broken.js's bytecode header (not its source) and re-seal the
       package checksum, so only the compiler artifact is unusable. */
    char pack_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, id, ".app.pack", pack_path,
                                    sizeof(pack_path)));
    OfflineLibraryItem *item = offline_library_find_mutable(&library, id);
    size_t pack_length = (size_t) item->audio_bytes;
    unsigned char *pack = malloc(pack_length);
    FILE *file = fopen(pack_path, "rb");
    CHECK(pack != NULL && file != NULL
          && fread(pack, 1, pack_length, file) == pack_length
          && fclose(file) == 0);
    unsigned char *body = memmem(pack, pack_length, broken,
                                 sizeof(broken) - 1u);
    CHECK(body != NULL && body + sizeof(broken) - 1u < pack + pack_length);
    body[sizeof(broken) - 1u] ^= 0xffu;
    item->auxiliary_hash = fnv32(pack, pack_length);
    file = fopen(pack_path, "wb");
    CHECK(file != NULL
          && fwrite(pack, 1, pack_length, file) == pack_length
          && fclose(file) == 0);
    free(pack);

    PspOfflineStore store;
    psp_offline_store_init(&store, budget, session, directory);
    store.library = library;
    BrowserProfile *profile = browser_profile_create(budget);
    CHECK(profile != NULL && launch(&store, engine, profile, id));
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned frame = 0; frame < 8u; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed)
              && browser_engine_render_frame(engine, NULL));
    }
    ScriptResult probe;
    CHECK(navigation->page.runtime != NULL
          && script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "globalThis.pocSummary=globalThis.ran+'|'+globalThis.probeText",
              "<triggers-probe>", &probe));
    const ScriptResult *page = &navigation->page.script_result;
    fprintf(stderr, "triggers: %s hits=%zu misses=%zu restore-failures=%zu "
            "failures=%zu\n", probe.summary,
            page->external_script_bytecode_cache_hits,
            page->external_script_bytecode_cache_misses,
            page->external_script_bytecode_cache_restore_failures,
            session->offline_deferred_failures);
    CHECK(strncmp(probe.summary, "plain;sri;broken;dynamic;|", 26) == 0
          && page->external_script_bytecode_cache_restore_failures == 1u
          && session->offline_deferred_failures == 0u
          /* Bytecode only: never read. */
          && !source_resident(engine, TRIGGERS_BASE "plain.js")
          /* Integrity metadata, a failed restore and a dynamic insertion
             each read their source from the pack. */
          && source_resident(engine, TRIGGERS_BASE "sri.js")
          && source_resident(engine, TRIGGERS_BASE "broken.js")
          && source_resident(engine, TRIGGERS_BASE "dynamic.js"));
    printf("OFFLINE-APP-TRIGGERS %s\n", probe.summary);
    psp_offline_store_destroy(&store);
    browser_profile_destroy(profile);
    CHECK(offline_library_remove(&library, id));
    browser_engine_destroy(engine);
    return true;
}

static uint32_t le32_at(const unsigned char *data)
{
    return (uint32_t) data[0] | (uint32_t) data[1] << 8
        | (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24;
}

/* Model an engine update for an installed app: its stored artifacts carry
   another compiler ABI and its index entry predates fingerprints. */
static bool make_stale(OfflineLibrary *library, uint32_t id)
{
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    OfflineLibraryItem *item = offline_library_find_mutable(library, id);
    if (item == NULL || !offline_library_item_path(library, id, ".app.pack",
                                                   path, sizeof(path)))
        return false;
    size_t length = (size_t) item->audio_bytes, used = 16u;
    unsigned char *pack = malloc(length);
    FILE *file = fopen(path, "rb");
    bool okay = pack != NULL && file != NULL
        && fread(pack, 1, length, file) == length;
    if (file != NULL) fclose(file);
    for (uint32_t at = 0; okay && at < le32_at(pack + 12); at++) {
        uint32_t body = le32_at(pack + used + 4u);
        uint32_t bytecode = le32_at(pack + used + 8u);
        if (bytecode != 0) pack[used + 12u] ^= 0x5au;
        used += 16u;
        for (unsigned text = 0; text < 4u; text++)
            used += 4u + le32_at(pack + used);
        used += 14u * 4u + body + bytecode;
    }
    file = okay ? fopen(path, "wb") : NULL;
    okay = file != NULL && fwrite(pack, 1, length, file) == length;
    if (file != NULL && fclose(file) != 0) okay = false;
    if (okay) {
        item->auxiliary_hash = fnv32(pack, length);
        item->duration_ms = 0;
    }
    free(pack);
    return okay && used == length;
}

/* Library > Saved after an engine update: the stale app is offered a
   recompile (or Open anyway), and Recompile publishes fresh bytecode that
   the next launch runs. */
static bool stale_library_flow(const char *directory)
{
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    OfflineLibrary library;
    offline_library_init(&library, budget, directory);
    uint32_t id = 0;
    CHECK(install_treadline(engine, &library, TREADLINE_BASE "index.html", false,
                            &id)
          && make_stale(&library, id) && offline_library_save(&library));
    PspOfflineStore store;
    psp_offline_store_init(&store, budget, session, directory);
    BrowserProfile *profile = browser_profile_create(budget);
    CHECK(profile != NULL && offline_library_load(&store.library));

    /* The offer, and each of its answers. */
    CHECK(psp_offline_store_offer_recompile(&store, id));
    const PspUiOfflineAppPreview *offer = psp_offline_store_app_preview(&store);
    CHECK(offer != NULL && offer->operation == PSP_UI_OFFLINE_APP_RECOMPILE
          && offer->captured_resources == 7u
          && strcmp(offer->name, "Treadline") == 0);
    CHECK(psp_offline_store_take_offer(&store, false) == id
          && psp_offline_store_app_preview(&store) == NULL);
    CHECK(psp_offline_store_offer_recompile(&store, id)
          && psp_offline_store_take_offer(&store, true) == id
          && !psp_offline_store_offer_recompile(&store, id)
          && psp_offline_store_offer_recompile(&store, id));
    (void) psp_offline_store_take_offer(&store, false);

    /* Open anyway still launches, compiling from the stored source. */
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(launch(&store, engine, profile, id));
    for (unsigned frame = 0; frame < 4u; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed));
    }
    CHECK(navigation->script_failed == 0
          && navigation->page.script_result
                 .external_script_bytecode_cache_hits == 0u
          && navigation->page.script_result
                 .external_script_bytecode_cache_misses == 7u);

    /* The library page marks it and links the action. */
    CHECK(psp_offline_store_open_library(&store, engine, false));
    char *page = NULL;
    size_t page_length = 0;
    CHECK(offline_library_build_page(&store.library, budget, &page,
                                     &page_length)
          && strstr(page, "needs recompile") != NULL
          && strstr(page, "/offline/recompile?id=") != NULL
          && strstr(page, "Open anyway") != NULL);
    budget_free(budget, page);

    /* Recompile through the library route, then launch from bytecode. */
    char route[96];
    snprintf(route, sizeof(route),
             "https://tilefinch.local/offline/recompile?id=%u", (unsigned) id);
    CHECK(psp_offline_store_handle_url(&store, engine, profile, route, NULL,
                                       false)
              == PSP_OFFLINE_ROUTE_STATE_CHANGED
          && strcmp(psp_offline_store_status(&store),
                    "OFFLINE APP RECOMPILED") == 0
          && store.library.count == 1u
          && offline_library_find(&store.library, id) == NULL);
    uint32_t fresh = store.library.items[0].id;
    CHECK(!offline_library_app_needs_recompile(&store.library, fresh, NULL)
          && !psp_offline_store_offer_recompile(&store, fresh));
    browser_session_cache_clear(session);
    CHECK(launch(&store, engine, profile, fresh));
    for (unsigned frame = 0; frame < 4u; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed));
    }
    CHECK(navigation->script_failed == 0
          && navigation->page.script_result
                 .external_script_bytecode_cache_hits == 7u);
    printf("OFFLINE-APP-STALE-FLOW ok compiles=%zu\n",
           store.library.script_compiles);
    psp_offline_store_destroy(&store);
    browser_profile_destroy(profile);
    CHECK(offline_library_remove(&store.library, fresh));
    browser_engine_destroy(engine);
    return true;
}

/* Page tools > Install offline app on an app opened from the library: the
   preview takes the manifest metadata and icon from the library entry (no
   manifest is cached and nothing is fetched), the install reads every
   deferred source back from the pack, and the new generation launches from
   bytecode. Navigating away ends that reuse. */
static bool reinstall_from_library(const char *directory)
{
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    NavigationSession *navigation = browser_engine_navigation(engine);
    OfflineLibrary library;
    offline_library_init(&library, budget, directory);
    uint32_t id = 0;
    CHECK(install_treadline(engine, &library, TREADLINE_BASE "index.html", false,
                            &id));
    /* Give the entry a recognizable stored icon. */
    unsigned char icon[OFFLINE_LIBRARY_APP_ICON_LIMIT];
    for (size_t at = 0; at < sizeof(icon); at++)
        icon[at] = (unsigned char) (at * 7u + 3u);
    char icon_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, id, ".app.icon", icon_path,
                                    sizeof(icon_path)));
    FILE *icon_file = fopen(icon_path, "wb");
    CHECK(icon_file != NULL
          && fwrite(icon, 1, sizeof(icon), icon_file) == sizeof(icon)
          && fclose(icon_file) == 0);
    OfflineLibraryItem *stored = offline_library_find_mutable(&library, id);
    CHECK(stored != NULL);
    stored->icon_bytes = (uint32_t) sizeof(icon);
    stored->icon_hash = fnv32(icon, sizeof(icon));
    CHECK(offline_library_save(&library));

    PspOfflineStore store;
    psp_offline_store_init(&store, budget, session, directory);
    BrowserProfile *profile = browser_profile_create(budget);
    CHECK(profile != NULL && offline_library_load(&store.library));
    unsigned char check_icon[OFFLINE_LIBRARY_APP_ICON_LIMIT];
    CHECK(offline_library_read_web_app_icon(&store.library, id, check_icon)
          && memcmp(check_icon, icon, sizeof(icon)) == 0);
    CHECK(launch(&store, engine, profile, id));
    for (unsigned frame = 0; frame < 4u; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed));
    }
    size_t deferred = browser_session_offline_deferred_pending(session);
    CHECK(navigation->page.script_result.external_script_bytecode_cache_hits
              == 7u && deferred >= 7u);

    bool prepared = psp_offline_store_prepare_current_app(&store, engine);
    if (!prepared)
        fprintf(stderr, "reinstall preview: %s\n",
                psp_offline_store_status(&store));
    const PspUiOfflineAppPreview *preview =
        psp_offline_store_app_preview(&store);
    CHECK(prepared && preview != NULL
          && strcmp(preview->name, stored->title) == 0
          /* The running DOM differs from the stored one, so this is an
             update of the same source rather than a fresh install. */
          && preview->operation != OFFLINE_WEB_APP_INSTALL
          && preview->display_mode == stored->app_display_mode
          && preview->captured_resources >= 8u);
    CHECK(psp_offline_store_install_current_app(&store, engine)
          && strncmp(psp_offline_store_status(&store), "OFFLINE APP ", 12) == 0
          && strcmp(psp_offline_store_status(&store),
                    "OFFLINE APP INSTALLED") != 0
          && store.library.count == 1u
          && browser_session_offline_deferred_pending(session) == 0u
          && session->offline_deferred_failures == 0u);
    uint32_t fresh = store.library.items[0].id;
    CHECK(fresh != id
          && !offline_library_app_needs_recompile(&store.library, fresh,
                                                  NULL)
          && offline_library_read_web_app_icon(&store.library, fresh,
                                               check_icon)
          && memcmp(check_icon, icon, sizeof(icon)) == 0);

    /* The fresh generation launches from bytecode without a source read,
       and its page can be installed again offline too. */
    browser_session_cache_clear(session);
    CHECK(launch(&store, engine, profile, fresh));
    for (unsigned frame = 0; frame < 4u; frame++) {
        bool changed = false;
        CHECK(browser_engine_advance_runtime(engine, 16, 8, &changed));
    }
    CHECK(navigation->script_failed == 0
          && navigation->page.script_result
                 .external_script_bytecode_cache_hits == 7u
          && browser_session_offline_deferred_pending(session) == deferred
          && psp_offline_store_prepare_current_app(&store, engine));
    psp_offline_store_discard_app_preparation(&store);

    /* Another document (here the library page) ends the reuse: a preview
       of it is refused rather than borrowing the app's metadata. */
    CHECK(psp_offline_store_open_library(&store, engine, false)
          && !psp_offline_store_prepare_current_app(&store, engine)
          && psp_offline_store_app_preview(&store) == NULL);
    printf("OFFLINE-APP-REINSTALL ok compiles=%zu\n",
           store.library.script_compiles);
    psp_offline_store_destroy(&store);
    browser_profile_destroy(profile);
    CHECK(offline_library_remove(&store.library, fresh));
    browser_engine_destroy(engine);
    return true;
}

static char shown_progress[80];

static void show_progress(void *context, const char *status)
{
    (void) context;
    snprintf(shown_progress, sizeof(shown_progress), "%s", status);
}

/* The store turns library progress into the busy-work status line. */
static bool store_progress_line(const char *directory)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 256u * KIB));
    PspOfflineStore store;
    psp_offline_store_init(&store, &budget, &session, directory);
    store.show_progress = show_progress;
    CHECK(store.library.progress != NULL);
    store.library.progress(store.library.progress_context, 2u, 4u);
    CHECK(strcmp(shown_progress,
                 "PREPARING 2 OF 4 SCRIPTS - CIRCLE STOPS") == 0
          && strcmp(psp_offline_store_status(&store), shown_progress) == 0);
    psp_offline_store_destroy(&store);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

int main(void)
{
    char directory[160];
    snprintf(directory, sizeof(directory),
             "/tmp/tilefinch-app-launch-%ld", (long) getpid());
    (void) mkdir(directory, 0700);
    bool okay = store_progress_line(directory)
        && installed_app_heap_scope(directory)
        && on_demand_triggers(directory)
        && stale_library_flow(directory)
        && reinstall_from_library(directory);
    script_test_remove_tree(directory);
    if (!okay) return 1;
    puts("offline-app-launch-tests: ok");
    return 0;
}
