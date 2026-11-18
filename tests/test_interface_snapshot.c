/* The interface snapshot on the real bootstrap.

   Each page under tests/fixtures/interface-snapshot wraps a family of
   built-ins with counting functions, runs one DOM workload that itself
   calls none of them (workload.js), and reports how many wrapper calls
   happened: every one came from the browser's own script. With the
   snapshot the count must be zero and the workload must not throw; with
   it off (ScriptRuntimeOptions.interface_snapshot_disabled) the same page
   must count calls, which proves the page still detects what the snapshot
   prevents. host-native.js checks a host native the page calls directly
   (run again under the profiling build's trampolines by
   tilefinch-interface-snapshot-profile-tests), intl.js the classes of a
   namespace a lazy module publishes, and key-path.js a browser
   function a page could steer into writing on Object.prototype. The
   lazy-*.js pages take over a lazily loaded global or member before its
   module loads, by assigning, redefining (also as non-configurable, so
   that the module's own changes are refused), deleting or wrapping it, or
   by calling its placeholder's setter on a receiver of their choosing.
   Some pages must count nothing with or without the snapshot (a fix that
   holds either way, or behaviour the snapshot must not change):
   async-callbacks.js counts the settlements of page async functions a
   zone.js-style patch misses, rejection-hook.js the window realm's
   rejection tracker calling a page function, lazy-namespace*.js a page's
   own namespace object (also one the module could not replace) marked as
   an interface when the module loads. Every realm ends with the
   ownership teardown check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_test_support.h"

static char *read_file(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path),
             TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/interface-snapshot/%s",
             name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    char *text = malloc(65536);
    size_t length = text == NULL ? 0 : fread(text, 1, 65535, file);
    fclose(file);
    if (text != NULL) text[length] = '\0';
    return text;
}

/* `page` with its workload marker replaced by `workload`. */
static char *splice(const char *page, const char *workload)
{
    static const char marker[] = "/*WORKLOAD*/";
    const char *at = strstr(page, marker);
    if (at == NULL) return NULL;
    size_t before = (size_t) (at - page), middle = strlen(workload),
        after = strlen(at + sizeof(marker) - 1u);
    char *source = malloc(before + middle + after + 1u);
    if (source == NULL) return NULL;
    memcpy(source, page, before);
    memcpy(source + before, workload, middle);
    memcpy(source + before + middle, at + sizeof(marker) - 1u, after + 1u);
    return source;
}

/* Runs the page in a fresh realm; the wrapper calls it counted, or -1. */
static long run_page(const char *name, const char *source, bool snapshot)
{
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return -1;
    fixture.options.interface_snapshot_disabled = !snapshot;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 2000,
        "https://interface-snapshot.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    for (unsigned turn = 0; ok && turn < 16; turn++)
        ok = script_runtime_advance(runtime, 16, 16, &result);
    long calls = -1;
    if (ok && result.success && strncmp(result.summary, "calls=", 6) == 0
        && strstr(result.summary, " THREW") == NULL)
        calls = strtol(result.summary + 6, NULL, 10);
    else
        fprintf(stderr, "%s (%s): summary=%s error=%s\n", name,
                snapshot ? "snapshot" : "no snapshot", result.summary,
                result.error);
    if (calls > 0 && snapshot)
        fprintf(stderr, "%s: %s\n", name, result.summary);
    script_runtime_destroy(runtime);
    if (!script_page_fixture_close(&fixture)) {
        fprintf(stderr, "%s: nonzero teardown ownership\n", name);
        return -1;
    }
    printf("%s: %s calls=%ld\n", name, snapshot ? "snapshot" : "no snapshot",
           calls);
    return calls;
}

/* With a file argument: runs that page script with and without the
   snapshot and prints both summaries (for investigating a difference). */
static int run_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 2;
    char *source = malloc(65536);
    size_t length = source == NULL ? 0 : fread(source, 1, 65535, file);
    fclose(file);
    if (source == NULL) return 2;
    source[length] = '\0';
    for (int snapshot = 1; snapshot >= 0; snapshot--) {
        ScriptPageFixture fixture;
        if (!script_page_fixture_open(&fixture, "<script></script>", 0))
            return 2;
        fixture.options.interface_snapshot_disabled = !snapshot;
        ScriptResult result = {0};
        ScriptRuntime *runtime = script_runtime_create_configured(
            &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 2000,
            "https://interface-snapshot.test/", &fixture.options, &result);
        bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
            runtime, source, path, &result);
        for (unsigned turn = 0; ok && turn < 16; turn++)
            ok = script_runtime_advance(runtime, 16, 16, &result);
        printf("%s: summary=%s error=%s\n",
               snapshot ? "snapshot" : "no snapshot", result.summary,
               result.error);
        script_runtime_destroy(runtime);
        script_page_fixture_close(&fixture);
    }
    free(source);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2) return run_file(argv[1]);
    /* The pages after `always_zero` must count nothing with or without
       the snapshot. */
    static const char *const pages[] = {
        "intrinsics.js", "platform.js", "globals.js", "instances.js",
        "host-native.js", "intl.js", "lazy-global-assign.js",
        "lazy-global-define.js", "lazy-global-delete.js", "lazy-receiver.js",
        "lazy-global-nc-define.js", "lazy-global-nc-accessor.js",
        "lazy-global-wrapped-setter.js", "lazy-global-configurable.js",
        "lazy-global-sibling.js", "lazy-global-readonly.js",
        "lazy-global-polyfill.js",
        "key-path.js", "lazy-wrapped-setter.js", "lazy-namespace.js",
        "lazy-namespace-nc.js", "lazy-host-globals.js", "async-callbacks.js",
        "rejection-hook.js"};
    const size_t always_zero = 17;
    char *workload = read_file("workload.js");
    if (workload == NULL) return 1;
    int failed = 0;
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        char *page = read_file(pages[i]);
        char *source = page == NULL ? NULL : splice(page, workload);
        /* a page with no workload of its own to splice */
        if (page != NULL && source == NULL && strstr(page, "/*WORKLOAD*/") == NULL) {
            source = page;
            page = NULL;
        }
        /* instances.js shadows one element's methods: the workload must
           use that element, not make its own */
        static const char own[] = "el=d.createElement('div');";
        char *made = source == NULL || page == NULL
                || strstr(page, "const _el") == NULL
            ? NULL : strstr(source, own);
        if (made != NULL) {
            memset(made, ' ', sizeof(own) - 1u);
            memcpy(made, "el=_el;", 7);
        }
        if (source == NULL) {
            fprintf(stderr, "%s: no workload marker\n", pages[i]);
            failed = 1;
        } else {
            if (run_page(pages[i], source, true) != 0) failed = 1;
            long without = run_page(pages[i], source, false);
            if (i >= always_zero ? without != 0 : without <= 0) {
                fprintf(stderr, i >= always_zero
                        ? "%s: counted calls without the snapshot\n"
                        : "%s: counted nothing without the snapshot\n",
                        pages[i]);
                failed = 1;
            }
        }
        free(source);
        free(page);
    }
    free(workload);
    return failed;
}
