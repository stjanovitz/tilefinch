/* Installed-app (offline library) JavaScript package limits.

   Builds synthetic same-origin apps in a BrowserSession cache, installs them
   through the real preview/save path, restores them into a fresh session and
   checks which classic scripts carry source-bound bytecode. It also reports
   Budget peaks for preview, install and restore so the package bound can be
   compared against the shipping PSP page budget, and drives the larger
   package through allocation refusals. */
#include "tilefinch/offline_library.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/platform.h"

#include <dirent.h>
#include <quickjs.h>
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

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static const char app_url[] = "https://games.test/big/index.html";

typedef struct {
    char name[48];
    char url[128];
    unsigned char *data;
    size_t length;
    bool script;
} AppFile;

typedef struct {
    AppFile files[12];
    size_t count;
    /* Model a live page whose runtime already stored each script's
       bytecode in the session cache, so installation compiles nothing. */
    bool warm;
} App;

/* Deterministic, structurally varied classic script. The shape (closures,
   loops, object literals, string building) is close enough to game code that
   its bytecode/source ratio is representative; the test also reports the
   ratio for the real Treadline sources. */
static unsigned char *synthetic_script(unsigned seed, size_t target,
                                       size_t *length)
{
    unsigned char *data = malloc(target + 1u);
    if (data == NULL) return NULL;
    size_t used = 0;
    unsigned unit = 0;
    uint32_t state = 0x9e3779b9u ^ seed;
    for (;;) {
        char chunk[1024];
        state = state * 1664525u + 1013904223u;
        unsigned a = (state >> 8) % 997u, b = (state >> 16) % 89u + 3u;
        int written = snprintf(
            chunk, sizeof(chunk),
            "var m%u_%u = (function () {\n"
            "  var table = [%u, %u, %u, %u, %u, %u];\n"
            "  function step(state, input) {\n"
            "    var x = state.x + input * %u, y = state.y - input;\n"
            "    for (var k = 0; k < table.length; k++) {\n"
            "      if (table[k] > x) y += table[k] %% %u; else x -= k;\n"
            "    }\n"
            "    return { x: x, y: y, tag: \"u%u\" + (x > y ? \"e\" : \"w\") };\n"
            "  }\n"
            "  function reset(o) { o.x = %u; o.y = %u; return o; }\n"
            "  return { step: step, reset: reset, id: %u };\n"
            "})();\n",
            seed, unit, a, b, a + b, a ^ b, a * 3u % 101u, b * 7u % 53u,
            b, b + 1u, unit, a % 17u, b % 13u, unit);
        if (written <= 0 || (size_t) written >= sizeof(chunk)) {
            free(data);
            return NULL;
        }
        if (used + (size_t) written > target) break;
        memcpy(data + used, chunk, (size_t) written);
        used += (size_t) written;
        unit++;
    }
    while (used < target) data[used++] = '\n';
    data[used] = 0;
    *length = used;
    return data;
}

static unsigned char *read_source(const char *relative, size_t *length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TILEFINCH_TEST_SOURCE_DIR,
             relative);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *data = malloc((size_t) size + 1u);
    bool okay = data != NULL
        && fread(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!okay) { free(data); return NULL; }
    data[size] = 0;
    *length = (size_t) size;
    return data;
}

static bool app_add(App *app, const char *name, unsigned char *data,
                    size_t length, bool script)
{
    if (data == NULL || app->count >= sizeof(app->files) / sizeof(app->files[0]))
        return false;
    AppFile *file = &app->files[app->count++];
    snprintf(file->name, sizeof(file->name), "%s", name);
    snprintf(file->url, sizeof(file->url), "https://games.test/big/%s", name);
    file->data = data;
    file->length = length;
    file->script = script;
    return true;
}

static void app_free(App *app)
{
    for (size_t at = 0; at < app->count; at++) free(app->files[at].data);
    memset(app, 0, sizeof(*app));
}

static size_t app_script_bytes(const App *app)
{
    size_t total = 0;
    for (size_t at = 0; at < app->count; at++)
        if (app->files[at].script) total += app->files[at].length;
    return total;
}

static size_t app_script_count(const App *app)
{
    size_t total = 0;
    for (size_t at = 0; at < app->count; at++)
        if (app->files[at].script) total++;
    return total;
}

static size_t app_bytes(const App *app)
{
    size_t total = 0;
    for (size_t at = 0; at < app->count; at++) total += app->files[at].length;
    return total;
}

static TilefinchRequestContext file_context(const AppFile *file)
{
    return (TilefinchRequestContext) {
        .target_url = file->url, .initiator_url = app_url,
        .top_level_url = app_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = file->script
            ? TILEFINCH_DESTINATION_SCRIPT : TILEFINCH_DESTINATION_STYLE
    };
}

static bool app_stage(const App *app, BrowserSession *session, Budget *budget)
{
    for (size_t at = 0; at < app->count; at++) {
        const AppFile *file = &app->files[at];
        unsigned char *copy = budget_malloc(budget, file->length + 1u);
        if (copy == NULL) return false;
        memcpy(copy, file->data, file->length);
        copy[file->length] = 0;
        BrowserSharedBody *body = browser_shared_body_take(
            budget, copy, file->length);
        if (body == NULL) {
            budget_free(budget, copy);
            return false;
        }
        TilefinchRequestContext context = file_context(file);
        TilefinchResourceGrant grant = {
            .destination = context.destination, .mode = context.mode,
            .credentials = context.credentials,
            .corp = TILEFINCH_CORP_UNSPECIFIED,
            .final_same_origin = true, .final_same_site = true,
            .mime_validated = true
        };
        bool stored = browser_session_cache_put_http_shared_resource(
            session, file->url, body, "", "",
            file->script ? "text/javascript" : "text/css",
            "public,max-age=31536000", "", 1u, &context, &grant);
        browser_shared_body_release(body);
        if (!stored) return false;
        if (!app->warm || !file->script) continue;
        unsigned char *bytecode = NULL;
        size_t bytecode_length = 0;
        bool warmed = script_compile_classic_bytecode(
                budget, (const char *) file->data, file->length, file->url,
                MIB, &bytecode, &bytecode_length)
            && browser_session_classic_script_bytecode_put(
                session, file->url, file->data, file->length, bytecode,
                bytecode_length);
        budget_free(budget, bytecode);
        if (!warmed) return false;
    }
    return true;
}

static uint32_t le32(const unsigned char *data)
{
    return (uint32_t) data[0] | (uint32_t) data[1] << 8
        | (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24;
}

typedef struct {
    size_t records;
    size_t scripts_with_bytecode;
    size_t bytecode_bytes;
    size_t pack_bytes;
} PackSummary;

/* Walk the installed TFAPP01 v2 pack independently of the library reader. */
static bool pack_summary(const OfflineLibrary *library, uint32_t id,
                         PackSummary *summary)
{
    memset(summary, 0, sizeof(*summary));
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(library, id, ".app.pack", path,
                                   sizeof(path))) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    rewind(file);
    unsigned char *pack = size > 16 ? malloc((size_t) size) : NULL;
    bool okay = pack != NULL
        && fread(pack, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!okay || memcmp(pack, "TFAPP01", 8) != 0 || le32(pack + 8) != 2u) {
        free(pack);
        return false;
    }
    size_t used = 16, length = (size_t) size;
    uint32_t count = le32(pack + 12);
    for (uint32_t at = 0; okay && at < count; at++) {
        if (length - used < 16u) { okay = false; break; }
        uint32_t body = le32(pack + used + 4u);
        uint32_t bytecode = le32(pack + used + 8u);
        used += 16u;
        for (unsigned text = 0; okay && text < 4u; text++) {
            if (length - used < 4u) { okay = false; break; }
            uint32_t text_length = le32(pack + used);
            used += 4u;
            if (length - used < text_length) okay = false;
            else used += text_length;
        }
        if (!okay || length - used < 14u * 4u) { okay = false; break; }
        used += 14u * 4u;
        if (length - used < (size_t) body + bytecode) { okay = false; break; }
        used += (size_t) body + bytecode;
        summary->records++;
        if (bytecode != 0) {
            summary->scripts_with_bytecode++;
            summary->bytecode_bytes += bytecode;
        }
    }
    summary->pack_bytes = length;
    free(pack);
    return okay && used == length;
}

/* Source bytes of `app` the session cache holds right now. */
static size_t resident_source_bytes(const App *app, BrowserSession *session)
{
    size_t bytes = 0;
    for (size_t at = 0; at < app->count; at++)
        for (size_t slot = 0; slot < BROWSER_CACHE_ENTRIES; slot++) {
            const BrowserCacheEntry *entry = &session->cache[slot];
            if (entry->data != NULL
                && strcmp(entry->url, app->files[at].url) == 0)
                bytes += entry->length;
        }
    return bytes;
}

/* Bytecode a launched app's script has, whether it was restored as
   bytecode only (its source left in the pack) or beside its source. */
static BrowserSharedBody *restored_bytecode(const AppFile *file,
                                            BrowserSession *session)
{
    BrowserSharedBody *bytecode = browser_session_offline_script_bytecode(
        session, file->url, file->length);
    return bytecode != NULL ? bytecode
        : browser_session_classic_script_bytecode_acquire(
              session, file->url, file->data, file->length);
}

/* Checks every member restored without reading deferred sources: a
   deferred script is a fresh hit with bytecode and no resident body. */
/* Model an engine update: rewrite every stored artifact's compiler ABI to
   `abi` and re-seal the pack checksum (in the in-memory index). */
static bool pack_set_abi(OfflineLibrary *library, uint32_t id, uint32_t abi)
{
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    OfflineLibraryItem *item = offline_library_find_mutable(library, id);
    if (item == NULL || !offline_library_item_path(library, id, ".app.pack",
                                                   path, sizeof(path)))
        return false;
    size_t length = (size_t) item->audio_bytes;
    unsigned char *pack = malloc(length);
    FILE *file = fopen(path, "rb");
    bool okay = pack != NULL && file != NULL
        && fread(pack, 1, length, file) == length;
    if (file != NULL) fclose(file);
    size_t used = 16;
    uint32_t count = okay ? le32(pack + 12) : 0;
    for (uint32_t at = 0; okay && at < count; at++) {
        uint32_t body = le32(pack + used + 4u);
        uint32_t bytecode = le32(pack + used + 8u);
        if (bytecode != 0)
            for (unsigned byte = 0; byte < 4u; byte++)
                pack[used + 12u + byte] = (unsigned char) (abi >> (8u * byte));
        used += 16u;
        for (unsigned text = 0; text < 4u; text++)
            used += 4u + le32(pack + used);
        used += 14u * 4u + body + bytecode;
    }
    uint32_t hash = UINT32_C(2166136261);
    for (size_t at = 0; okay && at < length; at++)
        hash = (hash ^ pack[at]) * UINT32_C(16777619);
    file = okay ? fopen(path, "wb") : NULL;
    okay = file != NULL && fwrite(pack, 1, length, file) == length;
    if (file != NULL && fclose(file) != 0) okay = false;
    free(pack);
    if (okay) item->auxiliary_hash = hash;
    return okay && used == length;
}

static uint32_t file_hash(const char *path)
{
    FILE *file = fopen(path, "rb");
    uint32_t hash = UINT32_C(2166136261);
    int byte;
    while (file != NULL && (byte = fgetc(file)) != EOF)
        hash = (hash ^ (unsigned char) byte) * UINT32_C(16777619);
    if (file != NULL) fclose(file);
    return hash;
}

static size_t restored_bytecode_scripts(const App *app,
                                        BrowserSession *session)
{
    size_t count = 0;
    for (size_t at = 0; at < app->count; at++) {
        const AppFile *file = &app->files[at];
        TilefinchRequestContext context = file_context(file);
        size_t deferred_length = 0;
        if (file->script
            && browser_session_offline_script_match(
                   session, file->url, &context, &deferred_length)) {
            BrowserSharedBody *bytecode = restored_bytecode(file, session);
            bool held = bytecode != NULL && deferred_length == file->length;
            browser_shared_body_release(bytecode);
            if (!held) return SIZE_MAX;
            count++;
            continue;
        }
        const BrowserCacheEntry *entry = NULL;
        if (browser_session_cache_match_resource(
                session, file->url, &context, 2u, &entry)
                != BROWSER_CACHE_FRESH
            || entry == NULL || entry->length != file->length
            || memcmp(entry->data, file->data, file->length) != 0)
            return SIZE_MAX;
        if (!file->script) continue;
        BrowserSharedBody *bytecode = restored_bytecode(file, session);
        if (bytecode != NULL) count++;
        browser_shared_body_release(bytecode);
    }
    return count;
}

/* QuickJS heap a launched app needs just to hold its restored code: every
   restored artifact is read into one 8 MiB-capped realm, or, for the
   side-effect-free synthetic apps, also evaluated (top-level closures and
   tables then exist). Reported against the shipping 5 MiB script heap; not
   a Budget figure. */
static bool restored_code_heap(const App *app, BrowserSession *session,
                               bool evaluate, size_t *heap_bytes)
{
    *heap_bytes = 0;
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *context = runtime == NULL ? NULL : JS_NewContext(runtime);
    if (context == NULL) {
        if (runtime != NULL) JS_FreeRuntime(runtime);
        return false;
    }
    JS_SetMemoryLimit(runtime, 8u * MIB);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(runtime, &usage);
    int64_t baseline = usage.malloc_size;
    JSValue loaded[12];
    size_t loaded_count = 0;
    bool okay = true;
    for (size_t at = 0; okay && at < app->count; at++) {
        const AppFile *file = &app->files[at];
        if (!file->script) continue;
        BrowserSharedBody *bytecode = restored_bytecode(file, session);
        if (bytecode == NULL) continue;
        JSValue function = JS_ReadObject(
            context, bytecode->data, bytecode->length,
            JS_READ_OBJ_BYTECODE);
        browser_shared_body_release(bytecode);
        if (JS_IsException(function)) {
            okay = false;
            break;
        }
        if (evaluate) {
            JSValue value = JS_EvalFunction(context, function);
            okay = !JS_IsException(value);
            JS_FreeValue(context, value);
        } else {
            loaded[loaded_count++] = function;
        }
    }
    JS_RunGC(runtime);
    JS_ComputeMemoryUsage(runtime, &usage);
    *heap_bytes = usage.malloc_size > baseline
        ? (size_t) (usage.malloc_size - baseline) : 0;
    for (size_t at = 0; at < loaded_count; at++)
        JS_FreeValue(context, loaded[at]);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return okay;
}

static size_t directory_entries(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) return 0;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            count++;
    closedir(dir);
    return count;
}

static const char page_html[] =
    "<!doctype html><title>Big game</title>"
    "<link rel=manifest href=manifest.webmanifest>"
    "<link rel=stylesheet href=game.css><canvas></canvas>";

typedef struct {
    unsigned done[16];
    unsigned total[16];
    size_t count;
} ProgressLog;

static void record_progress(void *context, unsigned done, unsigned total)
{
    ProgressLog *log = context;
    if (log->count < 16u) {
        log->done[log->count] = done;
        log->total[log->count] = total;
    }
    log->count++;
}

/* One call per compiled script: 1..n of the same n. */
static bool progress_in_order(const ProgressLog *log, size_t compiles)
{
    if (log->count != compiles || compiles > 16u) return false;
    for (size_t at = 0; at < log->count; at++)
        if (log->done[at] != at + 1u || log->total[at] < compiles
            || log->total[at] != log->total[0]) return false;
    return true;
}

typedef struct {
    size_t preview_compiles;
    size_t preview_retained;
    size_t preview_peak;
    size_t save_peak;
    size_t restore_peak;
    size_t restore_resident;
    size_t restore_source_resident;
    PackSummary pack;
    size_t restored_bytecode;
    size_t code_heap;
    size_t evaluated_heap;
    uint32_t id;
} InstallResult;

static size_t peak_since(Budget *budget, size_t baseline)
{
    return budget->peak > baseline ? budget->peak - baseline : 0;
}

/* Install `app` from a session sized for live browsing, then restore it into
   a fresh session at the shipping realistic 1 MiB live-cache preference. */
static int install_and_restore(const char *label, const App *app,
                               const char *directory, InstallResult *result)
{
    memset(result, 0, sizeof(*result));
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));

    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));

    OfflineWebAppPreview preview = {0};
    size_t baseline = budget.current;
    budget.peak = budget.current;
    ProgressLog progress = {0};
    library.progress = record_progress;
    library.progress_context = &progress;
    bool previewed = offline_library_preview_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0, &preview,
        error, sizeof(error));
    if (!previewed) fprintf(stderr, "%s preview: %s\n", label, error);
    CHECK(previewed && preview.resource_count == app->count);
    result->preview_peak = peak_since(&budget, baseline);
    /* The preview keeps exactly the bytecode it compiled (and reported, in
       order) for the confirming install. */
    result->preview_compiles = library.script_compiles;
    result->preview_retained = budget.current - baseline;
    CHECK(progress_in_order(&progress, library.script_compiles)
          && (library.script_compiles == 0) == (library.staged == NULL
              || result->preview_retained < 4u * KIB));

    budget.peak = budget.current;
    size_t install_baseline = budget.current;
    bool saved = offline_library_save_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0,
        &result->id, error, sizeof(error));
    if (!saved) fprintf(stderr, "%s save: %s\n", label, error);
    CHECK(saved && result->id != 0);
    result->save_peak = peak_since(&budget, install_baseline);
    /* Confirming compiled nothing more and released the staged bytecode. */
    CHECK(library.script_compiles == result->preview_compiles
          && library.staged == NULL && budget.current == baseline);
    library.progress = NULL;
    browser_session_destroy(&live);
    document_destroy(&document);
    CHECK(pack_summary(&library, result->id, &result->pack));
    CHECK(result->pack.records == app->count);
    CHECK(offline_library_find(&library, result->id)->audio_bytes
          == result->pack.pack_bytes);
    CHECK(preview.estimated_bytes
          >= result->pack.pack_bytes + sizeof(page_html) - 1u);

    BrowserSession restored;
    CHECK(browser_session_init(&restored, &budget, 1u * MIB));
    baseline = budget.current;
    budget.peak = budget.current;
    char *html = NULL;
    size_t html_length = 0;
    bool read = offline_library_read_web_app(
        &library, &budget, &restored, result->id, &html, &html_length,
        error, sizeof(error));
    if (!read) fprintf(stderr, "%s restore: %s\n", label, error);
    CHECK(read && html != NULL && strstr(html, "Big game") != NULL);
    result->restore_peak = peak_since(&budget, baseline);
    budget_free(&budget, html);
    result->restore_resident = budget.current - baseline;
    result->restore_source_resident = resident_source_bytes(app, &restored);
    result->restored_bytecode = restored_bytecode_scripts(app, &restored);
    /* Inspecting the restored app read no deferred source. */
    CHECK(resident_source_bytes(app, &restored)
          == result->restore_source_resident);
    CHECK(result->restored_bytecode != SIZE_MAX);
    CHECK(restored_code_heap(app, &restored, false, &result->code_heap));
    if (strcmp(label, "treadline") != 0)
        CHECK(restored_code_heap(app, &restored, true,
                                 &result->evaluated_heap));
    browser_session_destroy(&restored);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    printf("OFFLINE-APP-MEMORY %s scripts=%zu script_source=%zu "
           "resources=%zu pack=%zu bytecode=%zu bytecode_scripts=%zu "
           "restored_bytecode_scripts=%zu preview_peak=%zu save_peak=%zu "
           "restore_peak=%zu restore_resident=%zu source_resident=%zu "
           "code_heap=%zu evaluated_heap=%zu preview_compiles=%zu "
           "preview_retained=%zu\n",
           label, app_script_count(app), app_script_bytes(app),
           app_bytes(app), result->pack.pack_bytes,
           result->pack.bytecode_bytes, result->pack.scripts_with_bytecode,
           result->restored_bytecode, result->preview_peak,
           result->save_peak, result->restore_peak,
           result->restore_resident, result->restore_source_resident,
           result->code_heap,
           result->evaluated_heap, result->preview_compiles,
           result->preview_retained);
    return 0;
}

static int run_measured(const char *label, const App *app,
                        const char *directory, InstallResult *result)
{
    (void) mkdir(directory, 0700);
    int failed = install_and_restore(label, app, directory, result);
    script_test_remove_tree(directory);
    return failed;
}

/* The real Treadline package as a PSP installs it: game.css plus every
   deferred classic script index.html loads, in document order. The list is
   read from index.html so the model cannot drift from the shipped page.
   multiplayer-web.js is left out: index.html writes it only for browsers
   without Tilefinch's direct multiplayer API, so a PSP page never fetches it
   and it is not in the installed package. */
static const char treadline_script_tag[] = "<script defer src=\"";

static bool build_treadline(App *app, size_t *scripts)
{
    *scripts = 0;
    size_t html_length = 0;
    unsigned char *html = read_source("examples/treadline-arena/index.html",
                                      &html_length);
    size_t css_length = 0;
    unsigned char *css = read_source("examples/treadline-arena/game.css",
                                     &css_length);
    if (html == NULL || !app_add(app, "game.css", css, css_length, false)) {
        free(html);
        free(css);
        return false;
    }
    const char *at = (const char *) html;
    while ((at = strstr(at, treadline_script_tag)) != NULL) {
        at += sizeof(treadline_script_tag) - 1u;
        const char *end = strchr(at, '"');
        char name[48], relative[128];
        if (end == NULL || end == at || (size_t) (end - at) >= sizeof(name)) {
            free(html);
            return false;
        }
        memcpy(name, at, (size_t) (end - at));
        name[end - at] = 0;
        snprintf(relative, sizeof(relative), "examples/treadline-arena/%s",
                 name);
        size_t length = 0;
        unsigned char *data = read_source(relative, &length);
        if (!app_add(app, name, data, length, true)) {
            free(data);
            free(html);
            return false;
        }
        (*scripts)++;
        at = end;
    }
    free(html);
    return true;
}

/* Several scripts adding up to `total` bytes of classic source, each below
   the 384 KiB strict-realistic per-script admission ceilings used by games. */
static bool build_synthetic(App *app, size_t total, unsigned scripts)
{
    size_t length = 0;
    unsigned char *css = (unsigned char *) strdup("canvas{width:100%}");
    if (!app_add(app, "game.css", css, strlen((const char *) css), false))
        return false;
    for (unsigned at = 0; at < scripts; at++) {
        size_t share = total / scripts
            + (at == 0 ? total % scripts : 0u);
        char name[32];
        snprintf(name, sizeof(name), "part%u.js", at);
        unsigned char *data = synthetic_script(at + 1u, share, &length);
        if (!app_add(app, name, data, length, true)) {
            free(data);
            return false;
        }
    }
    return true;
}

static size_t app_payload_files(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) return 0;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strstr(entry->d_name, ".app") != NULL) count++;
    closedir(dir);
    return count;
}

/* Allocation refusal during install or restore must leave no partial
   package, no index entry, no restored cache entries, and no leaked bytes.
   A refused compiler allocation is not a refusal of the install: that
   script is packaged source-only, exactly like an over-budget script. */
static int refusal_sweep(const App *app, const char *directory)
{
    (void) mkdir(directory, 0700);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));
    const size_t scripts = app_script_count(app);

    /* A write failure after the whole pack has been streamed (the markup
       temporary cannot be created) rolls back the streamed pack too. */
    uint32_t next_id = library.next_id == 0 ? 1u : library.next_id;
    char blocked[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, next_id, ".app-html.tmp",
                                    blocked, sizeof(blocked))
          && mkdir(blocked, 0700) == 0);
    size_t baseline = budget.current;
    uint32_t id = 0;
    CHECK(!offline_library_save_web_app(
              &library, &document, &live, app_url, &manifest, NULL, 0, &id,
              error, sizeof(error))
          && library.count == 0 && library.next_id == next_id
          && budget.current == baseline);
    /* The rollback's remove() also takes the empty blocking directory. */
    (void) rmdir(blocked);
    CHECK(app_payload_files(directory) == 0);

    size_t save_refusals = 0, save_degraded = 0, attempts = 0;
    /* Arena-backed QuickJS allocations make exhaustive countdown sweeps
       impractical; a geometric sweep reaches every phase (markup
       serialization, compiler runtime, artifact copies) of the install. */
    for (size_t countdown = 0;; countdown = countdown < 4u
             ? countdown + 1u : countdown + countdown / 2u) {
        CHECK(++attempts < 200u);
        budget_inject_failure_after(&budget, countdown);
        bool saved = offline_library_save_web_app(
            &library, &document, &live, app_url, &manifest, NULL, 0, &id,
            error, sizeof(error));
        bool injected = !budget.failure_injection_enabled;
        budget_clear_failure_injection(&budget);
        CHECK(budget.current == baseline);
        if (!saved) {
            save_refusals++;
            /* No index entry and no stray payload or temporary file. */
            CHECK(injected && library.count == 0
                  && app_payload_files(directory) == 0);
            continue;
        }
        PackSummary pack;
        CHECK(library.count == 1 && id != 0
              && pack_summary(&library, id, &pack)
              && pack.records == app->count
              && pack.scripts_with_bytecode <= scripts);
        if (pack.scripts_with_bytecode == scripts && !injected) break;
        save_degraded++;
        CHECK(offline_library_remove(&library, id) && library.count == 0
              && app_payload_files(directory) == 0);
    }
    CHECK(save_refusals != 0 && library.count == 1 && id != 0);
    browser_session_destroy(&live);

    BrowserSession restored;
    CHECK(browser_session_init(&restored, &budget, 1u * MIB));
    baseline = budget.current;
    size_t restore_refusals = 0, restore_degraded = 0;
    attempts = 0;
    for (size_t countdown = 0;; countdown++) {
        CHECK(++attempts < 400u);
        budget_inject_failure_after(&budget, countdown);
        char *html = NULL;
        size_t html_length = 0;
        bool read = offline_library_read_web_app(
            &library, &budget, &restored, id, &html, &html_length,
            error, sizeof(error));
        bool injected = !budget.failure_injection_enabled;
        budget_clear_failure_injection(&budget);
        if (!read) {
            restore_refusals++;
            /* A refused restore leaves none of the package in the cache. */
            CHECK(injected && html == NULL && budget.current == baseline
                  && restored_bytecode_scripts(app, &restored) == SIZE_MAX);
            continue;
        }
        CHECK(html != NULL);
        budget_free(&budget, html);
        /* Every source is restored; a refused accelerator copy only costs
           that script its bytecode. */
        size_t restored_scripts = restored_bytecode_scripts(app, &restored);
        CHECK(restored_scripts <= scripts);
        if (restored_scripts == scripts && !injected) break;
        restore_degraded++;
        browser_session_cache_clear(&restored);
        CHECK(budget.current == baseline);
    }
    CHECK(restore_refusals != 0);
    browser_session_destroy(&restored);

    /* A page budget too small for the package's working set refuses the
       launch before or during admission, never leaving part of it behind. */
    size_t pack_bytes = (size_t) offline_library_find(&library, id)->audio_bytes;
    size_t tight_refusals = 0, tight_launches = 0;
    for (size_t limit = pack_bytes / 2u; limit <= pack_bytes * 3u;
         limit += pack_bytes / 4u) {
        Budget tight;
        budget_init(&tight, limit);
        BrowserSession small;
        CHECK(browser_session_init(&small, &tight, 256u * KIB));
        size_t tight_baseline = tight.current;
        char *tight_html = NULL;
        size_t tight_length = 0;
        if (offline_library_read_web_app(
                &library, &tight, &small, id, &tight_html, &tight_length,
                error, sizeof(error))) {
            tight_launches++;
            CHECK(restored_bytecode_scripts(app, &small) != SIZE_MAX);
            budget_free(&tight, tight_html);
        } else {
            tight_refusals++;
            CHECK(tight_html == NULL && tight.current == tight_baseline
                  && restored_bytecode_scripts(app, &small) == SIZE_MAX);
        }
        browser_session_destroy(&small);
        CHECK(tight.current == 0);
    }
    CHECK(tight_refusals != 0 && tight_launches != 0);

    /* A pack corrupted after installation is refused at the end of the
       streamed checksum, and everything restored before that is dropped. */
    char pack_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, id, ".app.pack", pack_path,
                                    sizeof(pack_path)));
    FILE *pack_file = fopen(pack_path, "r+b");
    CHECK(pack_file != NULL);
    long tail = (long) offline_library_find(&library, id)->audio_bytes - 2;
    CHECK(fseek(pack_file, tail, SEEK_SET) == 0);
    int byte = fgetc(pack_file);
    CHECK(byte != EOF && fseek(pack_file, tail, SEEK_SET) == 0
          && fputc(byte ^ 0x5a, pack_file) != EOF
          && fclose(pack_file) == 0);
    CHECK(browser_session_init(&restored, &budget, 1u * MIB));
    baseline = budget.current;
    char *html = NULL;
    size_t html_length = 0;
    CHECK(!offline_library_read_web_app(
              &library, &budget, &restored, id, &html, &html_length,
              error, sizeof(error))
          && html == NULL && budget.current == baseline
          && restored_bytecode_scripts(app, &restored) == SIZE_MAX);
    browser_session_destroy(&restored);

    printf("OFFLINE-APP-REFUSALS save=%zu save_degraded=%zu restore=%zu "
           "restore_degraded=%zu tight_refusals=%zu tight_launches=%zu\n",
           save_refusals, save_degraded, restore_refusals, restore_degraded,
           tight_refusals, tight_launches);
    document_destroy(&document);
    CHECK(offline_library_remove(&library, id));
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

static long pack_offset_of(const char *pack_path, const AppFile *file)
{
    FILE *pack = fopen(pack_path, "rb");
    if (pack == NULL) return -1;
    unsigned char *data = malloc(4u * MIB);
    size_t length = data == NULL ? 0 : fread(data, 1, 4u * MIB, pack);
    fclose(pack);
    const unsigned char *at = length < file->length ? NULL
        : memmem(data, length, file->data, file->length);
    long offset = at == NULL ? -1 : (long) (at - data);
    free(data);
    return offset;
}

/* Deferred sources are read on demand, exactly, at most once, and fail
   cleanly when the pack is gone or damaged. */
static int on_demand_sources(const App *app, const char *directory)
{
    (void) mkdir(directory, 0700);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));
    uint32_t id = 0;
    CHECK(offline_library_save_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0, &id,
        error, sizeof(error)));
    browser_session_destroy(&live);
    const AppFile *scripts[4];
    size_t script_count = 0;
    for (size_t at = 0; at < app->count && script_count < 4u; at++)
        if (app->files[at].script) scripts[script_count++] = &app->files[at];
    CHECK(script_count == 4u);

    BrowserSession restored;
    CHECK(browser_session_init(&restored, &budget, 1u * MIB));
    size_t empty = budget.current;
    char *html = NULL;
    size_t html_length = 0;
    CHECK(offline_library_read_web_app(&library, &budget, &restored, id,
                                       &html, &html_length, error,
                                       sizeof(error)));
    budget_free(&budget, html);
    CHECK(resident_source_bytes(app, &restored)
          == app_bytes(app) - app_script_bytes(app));

    /* A consumer that needs the bytes reads exactly that one source, bound
       to its bytecode, and only once. */
    TilefinchRequestContext context = file_context(scripts[0]);
    const BrowserCacheEntry *entry = NULL;
    CHECK(browser_session_cache_match_resource(
              &restored, scripts[0]->url, &context, 2u, &entry)
              == BROWSER_CACHE_FRESH
          && entry != NULL && entry->length == scripts[0]->length
          && memcmp(entry->data, scripts[0]->data, scripts[0]->length) == 0
          && !browser_session_offline_script_match(
                 &restored, scripts[0]->url, &context, NULL));
    BrowserSharedBody *bound = browser_session_classic_script_bytecode_acquire(
        &restored, scripts[0]->url, scripts[0]->data, scripts[0]->length);
    CHECK(bound != NULL);
    browser_shared_body_release(bound);
    CHECK(resident_source_bytes(app, &restored)
          == app_bytes(app) - app_script_bytes(app) + scripts[0]->length);

    /* A Budget refusal while reading keeps the record for a retry. */
    context = file_context(scripts[1]);
    budget_inject_failure_after(&budget, 0);
    BrowserSharedBody *source = browser_session_offline_script_source(
        &restored, scripts[1]->url, scripts[1]->length);
    budget_clear_failure_injection(&budget);
    CHECK(source == NULL && restored.offline_deferred_failures == 0
          && browser_session_offline_script_match(
                 &restored, scripts[1]->url, &context, NULL));
    source = browser_session_offline_script_source(
        &restored, scripts[1]->url, scripts[1]->length);
    CHECK(source != NULL && source->length == scripts[1]->length
          && memcmp(source->data, scripts[1]->data, source->length) == 0);
    browser_shared_body_release(source);

    /* Installing again from the running app captures every source (the
       rest are read from the pack) with its bytecode. */
    uint32_t again = 0;
    PackSummary repacked;
    CHECK(offline_library_save_web_app(
              &library, &document, &restored, app_url, &manifest, NULL, 0,
              &again, error, sizeof(error))
          && again != id && library.count == 1u
          && restored.offline_deferred == NULL
          && pack_summary(&library, again, &repacked)
          && repacked.records == app->count
          && repacked.scripts_with_bytecode == 4u);
    id = again;
    browser_session_cache_clear(&restored);
    CHECK(budget.current == empty);

    /* A pack damaged or removed after launch fails the read cleanly. */
    bool relaunched = offline_library_read_web_app(
        &library, &budget, &restored, id, &html, &html_length, error,
        sizeof(error));
    if (!relaunched) fprintf(stderr, "relaunch: %s count=%u pack=%llu\n", error,
        offline_library_find(&library, id)->resource_count,
        (unsigned long long) offline_library_find(&library, id)->audio_bytes);
    CHECK(relaunched);
    budget_free(&budget, html);
    /* Memory pressure takes the deferred bytecode but keeps each script's
       way back to its source: it is still a hit and reads from the pack. */
    context = file_context(scripts[0]);
    size_t pressured = budget.current;
    (void) browser_session_cache_reclaim(&restored, 64u * MIB);
    CHECK(budget.current < pressured
          && browser_session_offline_script_match(
                 &restored, scripts[0]->url, &context, NULL)
          && browser_session_offline_script_bytecode(
                 &restored, scripts[0]->url, scripts[0]->length) == NULL);
    source = browser_session_offline_script_source(
        &restored, scripts[0]->url, scripts[0]->length);
    CHECK(source != NULL
          && memcmp(source->data, scripts[0]->data, source->length) == 0);
    browser_shared_body_release(source);

    char pack_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, id, ".app.pack", pack_path,
                                    sizeof(pack_path)));
    long offset = pack_offset_of(pack_path, scripts[2]);
    FILE *pack = fopen(pack_path, "r+b");
    CHECK(offset > 0 && pack != NULL
          && fseek(pack, offset + 7, SEEK_SET) == 0
          && fputc('#', pack) != EOF && fclose(pack) == 0);
    context = file_context(scripts[2]);
    size_t before = budget.current;
    CHECK(browser_session_offline_script_source(
              &restored, scripts[2]->url, scripts[2]->length) == NULL
          && restored.offline_deferred_failures == 1u
          && !browser_session_offline_script_match(
                 &restored, scripts[2]->url, &context, NULL)
          && browser_session_cache_match_resource(
                 &restored, scripts[2]->url, &context, 2u, &entry)
                 == BROWSER_CACHE_MISS
          && budget.current < before);
    CHECK(remove(pack_path) == 0);
    context = file_context(scripts[3]);
    CHECK(browser_session_cache_match_resource(
              &restored, scripts[3]->url, &context, 2u, &entry)
              == BROWSER_CACHE_MISS
          && restored.offline_deferred_failures == 2u);
    browser_session_destroy(&restored);
    document_destroy(&document);
    CHECK(offline_library_remove(&library, id));
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    printf("OFFLINE-APP-ON-DEMAND ok\n");
    return 0;
}

typedef struct {
    size_t calls;
    size_t refuse_compile;   /* 1-based compile checkpoint to refuse */
} CancelAfter;

static bool cancel_compile(void *context, const char *phase,
                           size_t completed)
{
    CancelAfter *cancel = context;
    cancel->calls++;
    return phase == NULL || strcmp(phase, "offline-app-compile") != 0
        || completed < cancel->refuse_compile;
}

/* Circle during a preview or an install stops it between scripts with
   nothing published and nothing retained. */
static int cancellation(const App *app, const char *directory)
{
    (void) mkdir(directory, 0700);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));
    size_t baseline = budget.current;
    ProgressLog progress = {0};
    library.progress = record_progress;
    library.progress_context = &progress;
    CancelAfter cancel = {.refuse_compile = 2u};
    TilefinchPlatformServices services = {
        .context = &cancel, .cooperate = cancel_compile
    };
    tilefinch_platform_set_services(&services);
    OfflineWebAppPreview preview;
    bool previewed = offline_library_preview_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0, &preview,
        error, sizeof(error));
    CHECK(!previewed && strcmp(error, "offline app preview stopped") == 0
          && progress.count == 2u && progress.done[1] == 2u
          && library.script_compiles == 1u && library.staged == NULL
          && budget.current == baseline);
    progress.count = 0;
    uint32_t id = 0;
    bool saved = offline_library_save_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0, &id,
        error, sizeof(error));
    tilefinch_platform_set_services(NULL);
    CHECK(!saved && strcmp(error, "offline app install stopped") == 0
          && progress.count == 2u && library.count == 0
          && app_payload_files(directory) == 0
          && budget.current == baseline);
    /* An abandoned preview releases what it kept. */
    CHECK(offline_library_preview_web_app(
              &library, &document, &live, app_url, &manifest, NULL, 0,
              &preview, error, sizeof(error))
          && library.staged != NULL && budget.current > baseline);
    offline_library_discard_staged(&library);
    CHECK(library.staged == NULL && budget.current == baseline);
    browser_session_destroy(&live);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    printf("OFFLINE-APP-CANCEL ok cooperate_calls=%zu\n", cancel.calls);
    return 0;
}

/* After an engine (ABI) change the Library flags the app, and Recompile
   rewrites its bytecode in a new generation; every failure keeps the old
   generation and the index exactly as they were. */
static int stale_recompile(const App *app, const char *directory)
{
    (void) mkdir(directory, 0700);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));
    uint32_t id = 0;
    CHECK(offline_library_save_web_app(
        &library, &document, &live, app_url, &manifest, NULL, 0, &id,
        error, sizeof(error)));
    browser_session_destroy(&live);
    document_destroy(&document);
    size_t baseline = budget.current;
    unsigned scripts = 0;
    /* Fresh installs carry the current fingerprint: nothing to do. */
    CHECK(!offline_library_app_needs_recompile(&library, id, &scripts)
          && scripts == 4u);
    CHECK(offline_library_recompile_web_app(&library, 4040u, NULL, error,
                                            sizeof(error)) == false);

    /* An engine update: the stored artifacts no longer match. An entry
       from an older build has no fingerprint and is probed from headers. */
    CHECK(pack_set_abi(&library, id, UINT32_C(0x514a0001)));
    OfflineLibraryItem *item = offline_library_find_mutable(&library, id);
    item->duration_ms = 0;
    CHECK(offline_library_app_needs_recompile(&library, id, &scripts)
          && scripts == 4u && item->duration_ms != 0
          && budget.current == baseline);
    CHECK(offline_library_save(&library));

    /* Launch anyway: every source restores, no stale bytecode is used. */
    BrowserSession restored;
    CHECK(browser_session_init(&restored, &budget, 1u * MIB));
    char *html = NULL;
    size_t html_length = 0;
    CHECK(offline_library_read_web_app(&library, &budget, &restored, id,
                                       &html, &html_length, error,
                                       sizeof(error)));
    budget_free(&budget, html);
    CHECK(restored_bytecode_scripts(app, &restored) == 0
          && resident_source_bytes(app, &restored) == app_bytes(app));
    browser_session_cache_clear(&restored);
    baseline = budget.current;

    char pack_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, id, ".app.pack", pack_path,
                                    sizeof(pack_path)));
    uint32_t stale_pack = file_hash(pack_path);
    OfflineLibraryItem stale_item = *item;
    size_t files_before = app_payload_files(directory);
    #define UNCHANGED() (library.count == 1u \
        && memcmp(offline_library_find(&library, id), &stale_item, \
                  sizeof(stale_item)) == 0 \
        && file_hash(pack_path) == stale_pack \
        && app_payload_files(directory) == files_before \
        && budget.current == baseline \
        && offline_library_app_needs_recompile(&library, id, NULL))

    /* Circle between scripts. */
    ProgressLog progress = {0};
    library.progress = record_progress;
    library.progress_context = &progress;
    CancelAfter cancel = {.refuse_compile = 3u};
    TilefinchPlatformServices services = {
        .context = &cancel, .cooperate = cancel_compile
    };
    tilefinch_platform_set_services(&services);
    uint32_t fresh = 0;
    bool recompiled = offline_library_recompile_web_app(
        &library, id, &fresh, error, sizeof(error));
    tilefinch_platform_set_services(NULL);
    CHECK(!recompiled && strcmp(error, "offline app recompile stopped") == 0
          && progress.count == 3u && UNCHANGED());

    /* The new generation's pack cannot be created. */
    char blocked[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&library, library.next_id,
                                    ".app-pack.tmp", blocked,
                                    sizeof(blocked))
          && mkdir(blocked, 0700) == 0);
    CHECK(!offline_library_recompile_web_app(&library, id, &fresh, error,
                                             sizeof(error)));
    (void) rmdir(blocked);
    CHECK(UNCHANGED());

    /* Budget refusals anywhere: a clean failure, or a success in which a
       refused compile left that script source-only. */
    size_t refusals = 0, degraded = 0, attempts = 0;
    PackSummary pack;
    for (size_t countdown = 0;; countdown = countdown < 8u
             ? countdown + 1u : countdown + countdown / 2u) {
        CHECK(++attempts < 200u);
        progress.count = 0;
        size_t compiles_before = library.script_compiles;
        budget_inject_failure_after(&budget, countdown);
        recompiled = offline_library_recompile_web_app(
            &library, id, &fresh, error, sizeof(error));
        bool injected = !budget.failure_injection_enabled;
        budget_clear_failure_injection(&budget);
        if (!recompiled) {
            CHECK(injected && UNCHANGED());
            refusals++;
            continue;
        }
        CHECK(pack_summary(&library, fresh, &pack)
              && pack.records == app->count
              && offline_library_find(&library, id) == NULL);
        if (!injected && pack.scripts_with_bytecode == 4u) {
            /* Every stale script was compiled once, reported in order. */
            CHECK(library.script_compiles - compiles_before == 4u
                  && progress_in_order(&progress, 4u));
            break;
        }
        /* Make the published generation stale again and keep sweeping. */
        degraded++;
        id = fresh;
        CHECK(offline_library_item_path(&library, id, ".app.pack",
                                        pack_path, sizeof(pack_path))
              && pack_set_abi(&library, id, UINT32_C(0x514a0001)));
        offline_library_find_mutable(&library, id)->duration_ms = 0;
        CHECK(offline_library_app_needs_recompile(&library, id, NULL));
        stale_item = *offline_library_find(&library, id);
        stale_pack = file_hash(pack_path);
    }
    CHECK(refusals != 0 && degraded != 0);
    library.progress = NULL;
    /* The new generation replaced the old one in the same slot. */
    CHECK(fresh != id && library.count == 1u
          && offline_library_find(&library, id) == NULL
          && !offline_library_app_needs_recompile(&library, fresh, &scripts)
          && scripts == 4u && pack_summary(&library, fresh, &pack)
          && pack.scripts_with_bytecode == 4u && pack.records == app->count
          && app_payload_files(directory) == files_before
          && budget.current == baseline);
    OfflineLibrary reloaded;
    offline_library_init(&reloaded, &budget, directory);
    CHECK(offline_library_load(&reloaded)
          && offline_library_find(&reloaded, fresh) != NULL
          && !offline_library_app_needs_recompile(&reloaded, fresh, NULL));
    CHECK(offline_library_read_web_app(&reloaded, &budget, &restored, fresh,
                                       &html, &html_length, error,
                                       sizeof(error)));
    budget_free(&budget, html);
    CHECK(restored_bytecode_scripts(app, &restored) == 4u
          && resident_source_bytes(app, &restored)
                 == app_bytes(app) - app_script_bytes(app));
    browser_session_destroy(&restored);

    /* A damaged pack is reported, never laundered into a new one. */
    CHECK(pack_set_abi(&reloaded, fresh, UINT32_C(0x514a0001)));
    char fresh_pack[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(offline_library_item_path(&reloaded, fresh, ".app.pack",
                                    fresh_pack, sizeof(fresh_pack)));
    FILE *damage = fopen(fresh_pack, "r+b");
    CHECK(damage != NULL && fseek(damage, 200, SEEK_SET) == 0
          && fputc('#', damage) != EOF && fclose(damage) == 0);
    uint32_t damaged = file_hash(fresh_pack);
    CHECK(!offline_library_recompile_web_app(&reloaded, fresh, NULL, error,
                                             sizeof(error))
          && strcmp(error, "offline app could not be recompiled") == 0
          && file_hash(fresh_pack) == damaged && reloaded.count == 1u
          && offline_library_find(&reloaded, fresh) != NULL);
    #undef UNCHANGED
    CHECK(offline_library_remove(&reloaded, fresh));
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    printf("OFFLINE-APP-RECOMPILE ok refusals=%zu degraded=%zu\n",
           refusals, degraded);
    return 0;
}

static int oversized_refused(const App *app, const char *directory)
{
    (void) mkdir(directory, 0700);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 4096));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    static const char manifest_json[] = "{\"name\":\"Big game\"}";
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, sizeof(manifest_json) - 1u,
        "https://games.test/big/manifest.webmanifest", app_url, &manifest,
        error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    BrowserSession live;
    CHECK(browser_session_init(&live, &budget, 4u * MIB));
    CHECK(app_stage(app, &live, &budget));
    size_t baseline = budget.current;
    OfflineWebAppPreview preview;
    uint32_t id = 0;
    CHECK(!offline_library_preview_web_app(
              &library, &document, &live, app_url, &manifest, NULL, 0,
              &preview, error, sizeof(error))
          && strstr(error, "offline package bound") != NULL);
    CHECK(!offline_library_save_web_app(
              &library, &document, &live, app_url, &manifest, NULL, 0, &id,
              error, sizeof(error))
          && strstr(error, "offline package bound") != NULL
          && library.count == 0 && app_payload_files(directory) == 0
          && budget.current == baseline);
    browser_session_destroy(&live);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

int main(void)
{
    char directory[160];
    snprintf(directory, sizeof(directory), "/tmp/tilefinch-app-limits-%ld",
             (long) getpid());

    /* The motivating game: every deferred classic script is precompiled.
       Seven (arena-generator, practice, campaign, controls, music, bots,
       game); a qualification package adds qualification.js for eight
       (examples/treadline-arena/package-files.txt). The package may use at
       most the eight bytecode slots; a ninth script would launch from
       source. */
    InstallResult result;
    App treadline = {0};
    size_t treadline_scripts = 0;
    CHECK(build_treadline(&treadline, &treadline_scripts));
    CHECK(treadline_scripts >= 7u && treadline_scripts <= 8u
          && app_script_count(&treadline) == treadline_scripts);
    CHECK(run_measured("treadline", &treadline, directory, &result) == 0);
    CHECK(result.pack.scripts_with_bytecode == treadline_scripts
          && result.restored_bytecode == treadline_scripts
          /* Only the stylesheet's body is resident after launch. */
          && result.restore_source_resident
                 == app_bytes(&treadline) - app_script_bytes(&treadline));
    app_free(&treadline);

    /* ~1 MiB of classic source in four files: every script is precompiled,
       stored, and restored with its bytecode. */
    App full = {0};
    CHECK(build_synthetic(&full, 1000u * KIB, 4u));
    CHECK(app_script_bytes(&full) <= MIB);
    CHECK(run_measured("one-mib", &full, directory, &result) == 0);
    CHECK(result.pack.scripts_with_bytecode == 4u
          && result.restored_bytecode == 4u
          && result.pack.bytecode_bytes <= OFFLINE_LIBRARY_APP_BYTECODE_LIMIT
          && result.restore_source_resident
                 == app_bytes(&full) - app_script_bytes(&full)
          /* Bytecode, metadata and the stylesheet: no script source. */
          && result.restore_resident
                 < result.pack.bytecode_bytes + 64u * KIB);
    /* With the live runtime's artifacts already cached nothing is compiled,
       and the pack is streamed: preview and install hold no copy of it
       (only the serialized markup and stdio state are transient). */
    full.warm = true;
    InstallResult warm;
    CHECK(run_measured("one-mib-warm", &full, directory, &warm) == 0);
    full.warm = false;
    CHECK(warm.pack.scripts_with_bytecode == 4u
          && warm.restored_bytecode == 4u
          && warm.preview_peak < 64u * KIB && warm.save_peak < 64u * KIB);
    /* A launch holds the package once (in the cache) plus one member. */
    CHECK(result.restore_peak
          < result.restore_resident + 512u * KIB);

    /* Beyond the 1 MiB precompile ceiling: the package still installs and
       every source restores, but scripts past the ceiling stay
       source-only (newest responses are packed first). */
    App over = {0};
    CHECK(build_synthetic(&over, 1300u * KIB, 5u));
    CHECK(app_script_bytes(&over) > MIB
          && app_bytes(&over) <= OFFLINE_LIBRARY_APP_RESOURCE_LIMIT);
    CHECK(run_measured("over-one-mib", &over, directory, &result) == 0);
    CHECK(result.pack.scripts_with_bytecode == 3u
          && result.restored_bytecode == 3u
          /* The source-only script stays resident; the others do not. */
          && result.restore_source_resident > 0
          && result.restore_source_resident < app_script_bytes(&over));
    app_free(&over);

    CHECK(on_demand_sources(&full, directory) == 0);
    CHECK(cancellation(&full, directory) == 0);
    CHECK(stale_recompile(&full, directory) == 0);
    CHECK(refusal_sweep(&full, directory) == 0);
    app_free(&full);

    /* Captured responses beyond the package bound are refused outright,
       before anything is compiled or written. */
    App oversized = {0};
    CHECK(build_synthetic(&oversized, OFFLINE_LIBRARY_APP_RESOURCE_LIMIT
                                          + 4u * KIB, 6u));
    CHECK(oversized_refused(&oversized, directory) == 0);
    app_free(&oversized);
    puts("offline-app-limits-tests: ok");
    return 0;
}
